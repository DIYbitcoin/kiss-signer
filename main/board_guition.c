// The Guition JC4880P443C, and nothing else: the pins, the ST7701 over
// MIPI-DSI, the software rotation that turns the 800x480 canvas into the
// 480x800 panel, the GT911 on the shared I2C bus, the backlight PWM, the two
// DPI framebuffers the camera writes into, and the C6 held in reset. It was
// the top and bottom of main.c until the second board arrived; main.c now
// knows a board only through kiss_board.h, and this file is the Guition's
// answer to it. Every function here moved verbatim; only the names on the
// seam changed.
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "esp_ldo_regulator.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "kiss_board.h"
#include "kiss_panel.h"
#include "kiss_scan.h"
#include "camera_spike.h"
#include "main.h"   // radio_is_held

static const char *TAG = "kiss";

#define LCD_H_RES 480   // PHYSICAL panel (native portrait): DPI timings, framebuffer, raw touch
#define LCD_V_RES 800
#define LCD_BITS_PER_PIXEL 16
#define DSI_LANES 2
#define DSI_LANE_BITRATE_MBPS 500
#define DPI_CLOCK_MHZ 34
#define DSI_PHY_LDO_CHAN 3
#define DSI_PHY_LDO_MV 2500
#define LCD_RST_GPIO 5
#define LCD_BL_GPIO 23
#define LCD_BL_PWM_FREQ 20000
#define TOUCH_I2C_SCL 8
#define TOUCH_I2C_SDA 7

static const st7701_lcd_init_cmd_t st7701_lcd_cmds[] = {
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, (uint8_t[]){0x08}, 1, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x63, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x10, 0x08}, 2, 0},
    {0xCC, (uint8_t[]){0x10}, 1, 0},
    {0xB0, (uint8_t[]){0x80, 0x09, 0x53, 0x0C, 0xD0, 0x07, 0x0C, 0x09, 0x09, 0x28, 0x06, 0xD4, 0x13, 0x69, 0x2B, 0x71}, 16, 0},
    {0xB1, (uint8_t[]){0x80, 0x94, 0x5A, 0x10, 0xD3, 0x06, 0x0A, 0x08, 0x08, 0x25, 0x03, 0xD3, 0x12, 0x66, 0x6A, 0x0D}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x5D}, 1, 0},
    {0xB1, (uint8_t[]){0x58}, 1, 0},
    {0xB2, (uint8_t[]){0x87}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x4E}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x21}, 1, 0},
    {0xB9, (uint8_t[]){0x10, 0x1F}, 2, 0},
    {0xBB, (uint8_t[]){0x03}, 1, 0},
    {0xBC, (uint8_t[]){0x00}, 1, 0},
    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 0},
    {0xD0, (uint8_t[]){0x88}, 1, 0},
    {0xE0, (uint8_t[]){0x00, 0x3A, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0, 0x00, 0x40, 0x40}, 11, 0},
    {0xE2, (uint8_t[]){0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00}, 13, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0, 0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0}, 16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0, 0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0}, 16, 0},
    {0xEB, (uint8_t[]){0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}, 7, 0},
    {0xEC, (uint8_t[]){0x08, 0x01}, 2, 0},
    {0xED, (uint8_t[]){0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B}, 16, 0},
    {0xEF, (uint8_t[]){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 20},
};

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;

static i2c_master_bus_handle_t s_i2c_bus;  // shared touch bus; camera SCCB probes it too

// The board pairs the radio-less ESP32-P4 with an ESP32-C6 WiFi/Bluetooth
// coprocessor (SDIO, reset line on GPIO54 per Guition's EV-board-derived
// BSP). KISS never uses it: hold its reset low from the first code we run
// so whatever firmware shipped on the C6 never executes, and latch the pad
// so the level survives soft resets. Logged at W because release builds
// strip INFO. Settings/home read the pad back via radio_is_held().
#define C6_RESET_GPIO GPIO_NUM_54
void kiss_board_radio_hold(void) {
  gpio_set_level(C6_RESET_GPIO, 0);   // level first: no high glitch on config
  gpio_config_t io = {.pin_bit_mask = 1ULL << C6_RESET_GPIO,
                      .mode = GPIO_MODE_INPUT_OUTPUT};
  ESP_ERROR_CHECK(gpio_config(&io));
  gpio_set_level(C6_RESET_GPIO, 0);
  gpio_hold_en(C6_RESET_GPIO);
  ESP_LOGW(TAG, "C6 radio held in reset (GPIO54 low)");
}
bool radio_is_held(void) { return gpio_get_level(C6_RESET_GPIO) == 0; }

void kiss_board_log_info(void) {
  esp_chip_info_t chip;
  uint32_t fs = 0;
  esp_chip_info(&chip);
  if (esp_flash_get_size(NULL, &fs) != ESP_OK) fs = 0;
  ESP_LOGI(TAG, "KISS - " KISS_BOARD_NAME " ESP32-P4 rev%d flash=%luMB", chip.revision,
           (unsigned long)(fs / (1024 * 1024)));
}

void kiss_board_backlight_on(void) {
  ledc_timer_config_t t = {.speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_0,
                           .duty_resolution = LEDC_TIMER_10_BIT, .freq_hz = LCD_BL_PWM_FREQ,
                           .clk_cfg = LEDC_AUTO_CLK};
  ESP_ERROR_CHECK(ledc_timer_config(&t));
  ledc_channel_config_t c = {.gpio_num = LCD_BL_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE,
                             .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 1023};
  ESP_ERROR_CHECK(ledc_channel_config(&c));
}

// The panel while flash is being written. Every esp_ota_write disables the
// cache, and both things that keep this screen alive sit on the wrong side of
// that: the two DPI framebuffers live in PSRAM and are reached through the
// cache, and the LVGL refresh the progress callback forces runs from flash. So
// the panel is fed garbage for the whole update and the owner watches their
// signer strobe while it rewrites itself.
//
// Two config routes were tried on the board and both are dead ends.
// CONFIG_SPI_FLASH_AUTO_SUSPEND asserts at init on this board's Boya flash
// chip; CONFIG_SPIRAM_XIP_FROM_PSRAM takes a store access fault in early
// init. Both produced a boot loop and a black screen, so the answer is not a
// Kconfig symbol, it is not driving the panel while the cache is gone.
//
// Dark for the length of the write. Deliberate dark reads as "it is working";
// a strobing screen reads as "it is broken", and on a device rewriting its own
// firmware that is the difference between waiting and pulling the power.
void kiss_backlight_set(int on)
{
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, on ? 1023 : 0);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// Brightness as the only progress indicator an update can honestly show.
//
// The panel cannot be redrawn while the write holds the cache, but the
// backlight is a PWM peripheral and does not care: progress_cb runs BETWEEN
// esp_ota_write calls, when the cache is back, so setting a duty there is
// free and safe. The device starts at a floor rather than at nothing, because
// a signer that is rewriting itself should read as awake, and climbs to full
// as the write completes.
//
// This is only watchable if the panel is showing black while it starves,
// which is what kiss_panel_black is for. Brightening a torn framebuffer would
// reveal the tearing instead of hiding it.
// The ramp is gamma corrected, and that is not a polish detail. LED luminance
// is near enough linear in PWM duty, but perceived brightness goes as roughly
// the 0.43 power of luminance, so a linear duty ramp is SEEN as racing to
// almost-full in the first third and then crawling. On a progress indicator
// that is not a cosmetic complaint, it is the light telling the owner the
// write is nearly done when a quarter of it has landed, and then appearing to
// stall for twenty seconds. Squaring pct undoes most of that and costs one
// multiply.
#define BL_FLOOR 80          // ~8%, awake but clearly not finished
void kiss_backlight_level(int pct)
{
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  uint32_t shaped = (uint32_t)pct * (uint32_t)pct;        // 0..10000
  uint32_t duty = BL_FLOOR + (uint32_t)((1023 - BL_FLOOR) * shaped / 10000);
  // One write per meaningful change. The install path calls this from a
  // tight progress loop, and a stream of same-or-nearly-same duty writes is
  // jitter the eye reads as flicker on a light that is the only indicator.
  static uint32_t last = UINT32_MAX;
  if (last != UINT32_MAX && (duty > last ? duty - last : last - duty) < 8 &&
      pct != 0 && pct != 100)
    return;
  last = duty;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static uint16_t *s_fb;       // native 480x800 panel framebuffer (used only to clear it once)
static void *s_fb2;          // the second one, kept for kiss_panel_black()

// Both DPI framebuffers to black, so that when the DMA starves mid write the
// panel is fed black where it expected black. The tearing does not stop, it
// stops being visible -- which is the only version of "stops" available while
// the framebuffers live in PSRAM behind the cache a flash write disables.
void kiss_panel_black(void)
{
  // The msync is the fix for the rave. These framebuffers live in PSRAM
  // behind the cache: a memset that stays in cache lines is black the CPU
  // can see and garbage the DPI panel keeps scanning out -- and once the
  // flash write disables the cache, nothing ever writes the black back.
  // The camera code learned this first (camera_spike.c blank_fb); same
  // C2M flush here, so the panel is fed the black we think we wrote.
  const size_t n = (size_t)LCD_H_RES * LCD_V_RES * 2;
  if (s_fb)  { memset(s_fb,  0, n); esp_cache_msync(s_fb,  n, ESP_CACHE_MSYNC_FLAG_DIR_C2M); }
  if (s_fb2) { memset(s_fb2, 0, n); esp_cache_msync(s_fb2, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M); }
}
static uint16_t *s_rotbuf;   // pre-rotated region, handed to the hardware blitter (DMA source)
static lv_display_t *s_disp; // for flush_ready from the DMA-done callback
static bool s_flip;          // upside down: rot_flush turns the other way (kiss_board.h)

static void lv_tick_cb(void *a) { (void)a; lv_tick_inc(2); }

// the hardware blit (esp_lcd_panel_draw_bitmap) finished copying -> let LVGL render the next region
static bool dpi_trans_done(esp_lcd_panel_handle_t p, esp_lcd_dpi_panel_event_data_t *e, void *u) {
  (void)p; (void)e; (void)u;
  if (s_disp) lv_display_flush_ready(s_disp);
  return false;
}

// Lag triage, kept dormant: uncomment (or -DKISS_FLUSH_STATS) to log pixels
// rotated/s, flush count/s and the largest dirty rect once per second. The CPU
// rotate below is the only per-pixel display cost, so if the UI ever feels
// slow this turns "feels laggy" into a number BEFORE anyone optimizes anything
// (idle screens should be ~0; the sim's redraw-px diff is the desktop twin).
// #define KISS_FLUSH_STATS 1

// Landscape WITHOUT the driver's rotation (which panics): LVGL renders the 800x480 logical canvas;
// we rotate each region 90deg into s_rotbuf, then let the SAME fast DMA blit the portrait build used
// push it to the panel (CPU pixel writes to the live framebuffer tore on moving content).
// Mapping (90deg CW): logical (lx,ly) -> panel (px,py) = (479-ly, lx).
//
// UPSIDE DOWN (kiss_board.h) is the same quarter turn the OTHER way: logical
// (lx,ly) -> (ly, 799-lx). The 180 composes into the rotation that was
// already happening rather than adding a pass, so it costs nothing
// measurable -- the same store count into the same buffer, the inner loop
// striding by -ah instead of +ah -- and nothing new is allocated.
//
// The alternative was esp_lcd_panel_mirror: this board's vendor table never
// sends 0x36, so the ST7701 component's SDIR/ML write would be a clean delta,
// and it would turn the camera's PPA output and its CPU overlays for free
// because the beam would read the framebuffer backwards. It is not taken
// because nothing establishes that this glass honours SDIR/ML in DPI mode at
// all (the vendor init writes 0xCC, a BK0 register some sequences use for
// scan direction and this driver never touches), and reversing the gate scan
// against an 8/166 porch pair could shift the picture by a line or two. The
// software route is the one that can be reasoned about away from the glass.
static void rot_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  (void)disp;
  // While the camera owns the WHOLE panel (the dev preview, and any mode with no
  // preview rect set), LVGL must not paint: menu animations under the wizard kept
  // dirtying regions, and every repair flush flashed black boxes over the live
  // video. Video ends with a full-screen invalidate, so dropping those flushes
  // loses nothing.
  //
  // In two-column mode (ADDENDUM-01) LVGL paints EVERYTHING, including over the
  // preview rect, and the video simply takes its rect back on the next frame.
  //
  // This used to ask camera_spike_ui_rect_free() whether the flush touched the
  // preview and drop the whole flush if it did, which broke the scan and entropy
  // screens on hardware and looked perfect in the simulator, where there is no
  // camera and this function never runs. LVGL renders PARTIAL into a 48-LINE
  // FULL-WIDTH buffer (see display_start), so a screen build arrives as ten bands
  // of 800x48. Every band whose y range crossed the preview was discarded across
  // its entire width, right column included: on the scan screen the preview spans
  // y112..299, so everything from y96 to y335 never reached the panel. The owner
  // saw a thick black band with camera on the left and nothing on the right.
  //
  // Painting over the video instead costs at most one frame of the layout showing
  // through the preview, at 30fps, and only while something is actually being
  // repainted. In steady state LVGL is not flushing at all. A rect test cannot be
  // made to work here without splitting each band into sub-rectangles, and
  // rot_flush has ONE rotation buffer and owes exactly one flush_ready per flush.
  if (camera_spike_owns_panel()) {
    lv_display_flush_ready(disp);
    return;
  }
  uint16_t *src = (uint16_t *)px_map;
  int ah = area->y2 - area->y1 + 1;                 // rotated rect width (panel x)
#ifdef KISS_FLUSH_STATS
  static uint32_t st_px, st_n, st_max;
  static int64_t st_t0;
  uint32_t px = (uint32_t)ah * (uint32_t)(area->x2 - area->x1 + 1);
  st_px += px; st_n++;
  if (px > st_max) st_max = px;
  int64_t now = esp_timer_get_time();
  if (st_t0 == 0) st_t0 = now;
  if (now - st_t0 >= 1000000) {
    ESP_LOGI(TAG, "flush: %u px/s in %u flushes, max rect %u px",
             (unsigned)st_px, (unsigned)st_n, (unsigned)st_max);
    st_px = st_n = st_max = 0;
    st_t0 = now;
  }
#endif
  // Read ONCE per flush, not per pixel: the flip is applied from a click
  // handler on the LVGL task, the same task this runs on, so it cannot change
  // underneath the loop -- but a global in the inner loop is a reload the
  // compiler cannot drop.
  const bool flip = s_flip;
  if (flip) {
    for (int ly = area->y1; ly <= area->y2; ly++) {
      int j = ly - area->y1;
      for (int lx = area->x1; lx <= area->x2; lx++)
        s_rotbuf[(area->x2 - lx) * ah + j] = *src++;
    }
    // panel rect: x in [y1 .. y2], y in [799-x2 .. 799-x1]
    esp_lcd_panel_draw_bitmap(s_panel, area->y1, (LCD_V_RES - 1) - area->x2,
                              area->y2 + 1, LCD_V_RES - area->x1, s_rotbuf);
    return;                 // flush_ready in dpi_trans_done, as below
  }
  for (int ly = area->y1; ly <= area->y2; ly++) {
    int j = area->y2 - ly;
    for (int lx = area->x1; lx <= area->x2; lx++)
      s_rotbuf[(lx - area->x1) * ah + j] = *src++;
  }
  // panel rect: x in [479-y2 .. 479-y1], y in [x1 .. x2]
  esp_lcd_panel_draw_bitmap(s_panel, (LCD_H_RES - 1) - area->y2, area->x1,
                            LCD_H_RES - area->y1, area->x2 + 1, s_rotbuf);
  // flush_ready happens in dpi_trans_done when the DMA completes
}

lv_display_t *kiss_board_display_start(void) {
  esp_ldo_channel_handle_t ldo = NULL;
  esp_ldo_channel_config_t ldo_cfg = {.chan_id = DSI_PHY_LDO_CHAN, .voltage_mv = DSI_PHY_LDO_MV};
  ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo));
  esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
  esp_lcd_dsi_bus_config_t bus_cfg = {.bus_id = 0, .num_data_lanes = DSI_LANES,
                                      .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
                                      .lane_bit_rate_mbps = DSI_LANE_BITRATE_MBPS};
  ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus));
  vTaskDelay(pdMS_TO_TICKS(50));
  esp_lcd_dbi_io_config_t dbi_cfg = {.virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8};
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &s_io));
  esp_lcd_dpi_panel_config_t dpi_cfg = {
      .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT, .dpi_clock_freq_mhz = DPI_CLOCK_MHZ,
      .virtual_channel = 0, .in_color_format = LCD_COLOR_FMT_RGB565,
      .out_color_format = LCD_COLOR_FMT_RGB565, .num_fbs = 2,  // FB #2 = camera flip target
      .video_timing = {.h_size = LCD_H_RES, .v_size = LCD_V_RES, .hsync_pulse_width = 12,
                       .hsync_back_porch = 42, .hsync_front_porch = 42, .vsync_pulse_width = 2,
                       .vsync_back_porch = 8, .vsync_front_porch = 166}};
  st7701_vendor_config_t vendor_cfg = {
      .mipi_config = {.dsi_bus = dsi_bus, .dpi_config = &dpi_cfg},
      .init_cmds = st7701_lcd_cmds,
      .init_cmds_size = sizeof(st7701_lcd_cmds) / sizeof(st7701_lcd_cmds[0]),
      .flags.use_mipi_interface = 1};
  esp_lcd_panel_dev_config_t panel_cfg = {.reset_gpio_num = LCD_RST_GPIO,
                                          .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
                                          .bits_per_pixel = LCD_BITS_PER_PIXEL,
                                          .vendor_config = &vendor_cfg};
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(s_io, &panel_cfg, &s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

  // Clear the native framebuffer once (else un-painted regions show uninitialized "barcode" noise),
  // and register the DMA-done callback so flush_ready fires when each blit completes.
  void *fb2 = NULL;
  ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2, (void **)&s_fb, &fb2));
  s_fb2 = fb2;                 // kept, so the update can black both out again
  memset(s_fb, 0, (size_t)LCD_H_RES * LCD_V_RES * 2);
  esp_cache_msync(s_fb, (size_t)LCD_H_RES * LCD_V_RES * 2,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  memset(fb2, 0, (size_t)LCD_H_RES * LCD_V_RES * 2);
  camera_spike_set_panel(s_panel, s_fb, fb2);
  esp_lcd_dpi_panel_event_callbacks_t dpi_cbs = {.on_color_trans_done = dpi_trans_done};
  ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(s_panel, &dpi_cbs, NULL));

  // Manual LVGL setup (no esp_lvgl_port display): logical canvas is 800x480 landscape.
  lv_init();
  const esp_timer_create_args_t tcfg = {.callback = lv_tick_cb, .name = "lvtick"};
  esp_timer_handle_t tick;
  ESP_ERROR_CHECK(esp_timer_create(&tcfg, &tick));
  ESP_ERROR_CHECK(esp_timer_start_periodic(tick, 2000));   // 2 ms LVGL tick

  lv_display_t *disp = lv_display_create(SCREEN_W, SCREEN_H);
  if (!disp) {
    ESP_LOGE(TAG, "lv_display_create failed");
    abort();                       // a panic reboots into last-known-good;
  }                                // LVGL's own fallback is a silent while(1)
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  size_t bufsz = (size_t)SCREEN_W * 48 * 2;                  // 48-line partial buffers
  // Same treatment as s_rotbuf below, for the same reason: these are the FIRST
  // two 75KB internal allocations of the three, and they were the unchecked
  // ones -- LVGL asserts on a NULL buffer with logging compiled out, which on
  // this board is a silent hang, and a hang on an update's first boot strands
  // the user on the broken image where a panic would roll back. PSRAM is fine
  // as a render target here: rot_flush copies into s_rotbuf before DMA, and
  // 64-byte alignment satisfies lv_draw_buf_align.
  void *b1 = heap_caps_malloc(bufsz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  void *b2 = heap_caps_malloc(bufsz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!b1) b1 = heap_caps_aligned_alloc(64, bufsz, MALLOC_CAP_SPIRAM);
  if (!b2) b2 = heap_caps_aligned_alloc(64, bufsz, MALLOC_CAP_SPIRAM);
  if (!b1 || !b2) {
    ESP_LOGE(TAG, "display buffers: %u bytes x2 unavailable in any heap",
             (unsigned)bufsz);
    abort();
  }
  // DMA source for the rotated region. This is the THIRD 75KB allocation in a
  // row -- b1, b2, then this -- out of roughly 340KB of internal RAM, and it
  // was unchecked. When it finally came back NULL the first flush stored to
  // NULL + 94 and the device panicked in rot_flush before it had drawn a
  // single frame: "Store access fault, MTVAL 0x5e", three seconds after boot,
  // forever. Nothing in the gate suite can reach this. rot_flush does not run
  // in the simulator at all, and a build that never allocates cannot fail to.
  //
  // Internal first, because the blitter is fastest reading internal RAM. PSRAM
  // second, because on the P4 the DPI panel reads it perfectly well and 75KB
  // of headroom is worth more than the margin: this buffer is written once per
  // band and read once by DMA. And then a hard check, so the next time the
  // internal heap gets tight this says so on the console instead of storing
  // through a null pointer.
  s_rotbuf = heap_caps_malloc(bufsz, MALLOC_CAP_DMA);
  if (!s_rotbuf) {
    ESP_LOGW(TAG, "rotbuf: no internal DMA heap for %u bytes, using PSRAM",
             (unsigned)bufsz);
    s_rotbuf = heap_caps_aligned_alloc(64, bufsz, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
  }
  if (!s_rotbuf) {
    ESP_LOGE(TAG, "rotbuf: %u bytes unavailable in any heap", (unsigned)bufsz);
    abort();
  }
  lv_display_set_buffers(disp, b1, b2, bufsz, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, rot_flush);
  s_disp = disp;
  return disp;
}

void kiss_board_touch_start(void) {
  i2c_master_bus_config_t i2c_cfg = {.i2c_port = I2C_NUM_0, .sda_io_num = TOUCH_I2C_SDA,
                                     .scl_io_num = TOUCH_I2C_SCL, .clk_source = I2C_CLK_SRC_DEFAULT,
                                     .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true};
  i2c_master_bus_handle_t bus = NULL;
  if (i2c_new_master_bus(&i2c_cfg, &bus) != ESP_OK) { ESP_LOGE(TAG, "i2c failed"); return; }
  s_i2c_bus = bus;
  kiss_scan_set_bus(bus);              // step 6: QR scanner shares the camera bus
  camera_spike_set_bus(bus);             // step 7: entropy page starts the camera too
  esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
  esp_lcd_panel_io_handle_t tp_io = NULL;
  if (esp_lcd_new_panel_io_i2c(bus, &tp_io_cfg, &tp_io) != ESP_OK) return;
  esp_lcd_touch_config_t tp_cfg = {.x_max = LCD_H_RES, .y_max = LCD_V_RES,
                                   .rst_gpio_num = GPIO_NUM_NC, .int_gpio_num = GPIO_NUM_NC,
                                   .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0}};
  if (esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_touch) != ESP_OK) {
    ESP_LOGE(TAG, "GT911 init failed");
    s_touch = NULL;
    return;
  }
  ESP_LOGI(TAG, "GT911 ready");
}

bool kiss_board_touch_ok(void) { return s_touch != NULL; }
i2c_master_bus_handle_t kiss_board_i2c_bus(void) { return s_i2c_bus; }

// device touch: read the GT911 controller
bool platform_read_touch(int *x, int *y) {
  if (!s_touch) return false;
  esp_lcd_touch_read_data(s_touch);
  // esp_lcd_touch_get_coordinates is deprecated (removed in component v2.0.0);
  // esp_lcd_touch_get_data returns the same points in a struct array.
  esp_lcd_touch_point_data_t pt[1];
  uint8_t cnt = 0;
  if (esp_lcd_touch_get_data(s_touch, pt, &cnt, 1) == ESP_OK && cnt > 0) {
    // raw GT911 is portrait (x:0..479, y:0..799); map to the logical 800x480 landscape.
    // Must match the 90deg mapping in rot_flush. Flip if it feels mirrored.
    //
    // ...and upside down is rot_flush's other quarter turn read backwards,
    // which is both canvas axes reflected. Written HERE rather than in the
    // GT911's flags because the driver mirrors as `x_max - x` and not
    // `x_max - 1 - x`, so a flag flip would hand back a point one pixel
    // outside the canvas on whichever axis it was spelled against.
    if (s_flip) {
      *x = (LCD_V_RES - 1) - pt[0].y;
      *y = pt[0].x;
      return true;
    }
    *x = pt[0].y;
    *y = (LCD_H_RES - 1) - pt[0].x;
    return true;
  }
  return false;
}

// ---- upside down (kiss_board.h) ----
//
// Three surfaces, one bool. The glass is rot_flush's direction; the touch map
// is the reflection above; the camera is camera_spike.c, which does not go
// through rot_flush at all -- the PPA writes the framebuffer by DMA and the
// CPU overlays write it directly -- so it reads this flag itself and turns
// its own three parts (the rotation index, the preview rect's UI-to-panel
// map, and the single door every overlay pixel already passes through).
//
// No serialisation is needed on this board, unlike the 3.5in's MADCTL: there
// is no command to land in the middle of a transfer. The flip arrives from a
// click handler on the LVGL task, which is the only task that runs rot_flush,
// and the camera task reads the flag once per frame into a local.
bool kiss_flip_get(void) { return s_flip; }

void kiss_flip_set(bool on, bool repaint)
{
  s_flip = on;
  camera_spike_flip_refresh();     // the preview rect is mapped, so it moves
  if (!repaint) return;
  // The finger is up by the time a CLICKED handler runs, but LVGL's indev
  // still holds the press point and the gesture accumulator, both measured in
  // canvas coordinates that have just been reflected. main.c arms the same
  // guard for the same class of problem when a screen opens under a finger.
  for (lv_indev_t *d = lv_indev_get_next(NULL); d; d = lv_indev_get_next(d))
    lv_indev_wait_release(d);
  // Both framebuffers, because the flush alternates between them and the one
  // not being painted still holds the old picture the right way up.
  kiss_panel_black();
  lv_obj_invalidate(lv_screen_active());
}
