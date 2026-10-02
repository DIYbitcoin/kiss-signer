// The Waveshare ESP32-P4-WiFi6-Touch-LCD-4.3, and nothing else: the pins, the
// ST7701 over MIPI-DSI, the software rotation that turns the 800x480 canvas
// into the 480x800 panel, the GT911 on the shared I2C bus, the backlight PWM,
// the two DPI framebuffers the camera writes into, and the C6 held in reset.
// It is the Guition 4.3in's twin in everything a screen can see: the same
// controller, the same portrait glass under the same landscape UI, so the same
// quarter turn on the way out and the same reflection on the way in, and the
// image draws the Guition's art and type. What is this board's own is wiring
// and one table: the panel's reset and backlight pins, a backlight driven
// through an INVERTING buffer, a GT911 with a reset line and no interrupt,
// which comes up at either of its two addresses, the panel's timing, and
// Waveshare's init table for this glass. The camera is the 3.5in's OV5647, on
// the touch bus as the Guition's own sensor is.
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

#define LCD_H_RES KISS_PANEL_W   // PHYSICAL panel (native portrait): DPI timings, framebuffer, raw touch
#define LCD_V_RES KISS_PANEL_H
#define LCD_BITS_PER_PIXEL 16
#define DSI_LANES 2
// Waveshare's own board package runs this panel at 500 Mbps a lane and a 30 MHz
// pixel clock, with the porches below; Kern drives the same board with the
// same clock and the same porches.
#define DSI_LANE_BITRATE_MBPS 500
#define DPI_CLOCK_MHZ 30
#define DSI_PHY_LDO_CHAN 3       // LDO_VO3 feeds VDD_MIPI_DPHY, as on the other DSI boards
#define DSI_PHY_LDO_MV 2500
#define LCD_RST_GPIO 27
#define LCD_BL_GPIO 26           // through an inverting buffer: see backlight_on
#define LCD_BL_PWM_FREQ 5000     // the vendor's rate for the driver behind this pin
#define TOUCH_I2C_SCL 8
#define TOUCH_I2C_SDA 7
#define TOUCH_RST_GPIO 23        // the GT911's reset; its interrupt is not wired

// Waveshare's init table for this glass, byte for byte from their board
// package (esp32_p4_wifi6_touch_lcd_4_3, Apache-2.0), which is also the table
// Kern sends. Not the Guition's: six rows differ, the gamma pair, two of the
// voltage rows and the two that end the sequence.
static const st7701_lcd_init_cmd_t st7701_lcd_cmds[] = {
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, (uint8_t[]){0x08}, 1, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x63, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x17, 0x08}, 2, 0},
    {0xCC, (uint8_t[]){0x10}, 1, 0},
    {0xB0, (uint8_t[]){0x40, 0xC9, 0x94, 0x0E, 0x10, 0x05, 0x0B, 0x09, 0x08, 0x26, 0x04, 0x52, 0x10, 0x69, 0x6B, 0x69}, 16, 0},
    {0xB1, (uint8_t[]){0x40, 0xD2, 0x98, 0x0C, 0x92, 0x07, 0x09, 0x08, 0x07, 0x25, 0x02, 0x0E, 0x0C, 0x6E, 0x78, 0x55}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x5D}, 1, 0},
    {0xB1, (uint8_t[]){0x4E}, 1, 0},
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
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x29, (uint8_t[]){0x00}, 0, 0},
};

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static esp_lcd_panel_io_handle_t s_tp_io;  // the GT911's own IO: see the reader's pre-check

static i2c_master_bus_handle_t s_i2c_bus;  // shared: touch and the camera's SCCB

// The board pairs the radio-less ESP32-P4 with an ESP32-C6 WiFi/Bluetooth
// coprocessor (SDIO, CHIP_EN on GPIO54 per Waveshare's board package, the
// same pin as the Guition boards). KISS never uses it: hold its reset low
// from the first code we run so whatever firmware shipped on the C6 never
// executes, and latch the pad so the level survives soft resets. Logged at W
// because release builds strip INFO. Settings/home read the pad back via
// radio_is_held().
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
  // INVERTED at the pin. The backlight sits behind an inverting buffer on this
  // board, so a high pin is a dark panel; Waveshare's package and Kern both set
  // output_invert and keep their duty meaning brightness. So does this, and
  // that is the whole difference: every duty written below still reads 1023
  // for full and 0 for dark, on this board as on the others.
  ledc_channel_config_t c = {.gpio_num = LCD_BL_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE,
                             .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 1023,
                             .flags.output_invert = 1};
  ESP_ERROR_CHECK(ledc_channel_config(&c));
}

// The panel while flash is being written: dark for the length of the write,
// and the backlight as the only honest progress indicator. board_guition.c
// has the whole account (the framebuffers live in PSRAM behind the cache a
// flash write disables, so the panel cannot be fed anything true meanwhile);
// the same holds here.
void kiss_backlight_set(int on)
{
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, on ? 1023 : 0);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

#define BL_FLOOR 80          // ~8%, awake but clearly not finished
void kiss_backlight_level(int pct)
{
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  uint32_t shaped = (uint32_t)pct * (uint32_t)pct;        // 0..10000, gamma corrected
  uint32_t duty = BL_FLOOR + (uint32_t)((1023 - BL_FLOOR) * shaped / 10000);
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

// Both DPI framebuffers to black, with the C2M flush that makes the black
// reach the panel and not just the cache (board_guition.c, camera_spike.c's
// blank_fb).
void kiss_panel_black(void)
{
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

// Landscape on a portrait panel, the Guition's way and at the Guition's size:
// LVGL renders the 800x480 logical canvas, each region is turned a quarter into
// s_rotbuf, and the DMA blit pushes it to the panel. Mapping (90deg CW):
// logical (lx,ly) -> panel (px,py) = (479-ly, lx). UPSIDE DOWN is the same
// quarter turn the other way: logical (lx,ly) -> (ly, 799-lx). board_guition.c
// says why this is a software turn and not the panel's own mirror.
//
// While the camera owns the WHOLE panel LVGL must not paint over the live
// video; in two-column mode it paints everything and the video takes its rect
// back on the next frame. board_guition.c has the history of why the flush is
// dropped whole or not at all.
static void rot_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  (void)disp;
  if (camera_spike_owns_panel()) {
    lv_display_flush_ready(disp);
    return;
  }
  uint16_t *src = (uint16_t *)px_map;
  int ah = area->y2 - area->y1 + 1;                 // rotated rect width (panel x)
  // Read ONCE per flush, not per pixel (board_guition.c).
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

// The shared I2C bus: the GT911, and the camera's SCCB at 0x36. The scanner and
// the camera get the handle here as well.
static void i2c_bus_start(void) {
  if (s_i2c_bus) return;
  i2c_master_bus_config_t i2c_cfg = {.i2c_port = I2C_NUM_0, .sda_io_num = TOUCH_I2C_SDA,
                                     .scl_io_num = TOUCH_I2C_SCL, .clk_source = I2C_CLK_SRC_DEFAULT,
                                     .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true};
  i2c_master_bus_handle_t bus = NULL;
  if (i2c_new_master_bus(&i2c_cfg, &bus) != ESP_OK) { ESP_LOGE(TAG, "i2c failed"); return; }
  s_i2c_bus = bus;
  kiss_scan_set_bus(bus);                // the QR scanner shares the camera bus
  camera_spike_set_bus(bus);             // the entropy page starts the camera too
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
                       .hsync_back_porch = 42, .hsync_front_porch = 42, .vsync_pulse_width = 8,
                       .vsync_back_porch = 2, .vsync_front_porch = 60}};
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

  // Clear the native framebuffers once (else un-painted regions show
  // uninitialized noise), and register the DMA-done callback so flush_ready
  // fires when each blit completes.
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
  // Checked, for the reason board_guition.c gives: LVGL asserts on a NULL
  // buffer with logging compiled out, which is a silent hang, and a hang on an
  // update's first boot strands the user on the broken image where a panic
  // would roll back. PSRAM is fine as a render target here: rot_flush copies
  // into s_rotbuf before DMA.
  void *b1 = heap_caps_malloc(bufsz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  void *b2 = heap_caps_malloc(bufsz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!b1) b1 = heap_caps_aligned_alloc(64, bufsz, MALLOC_CAP_SPIRAM);
  if (!b2) b2 = heap_caps_aligned_alloc(64, bufsz, MALLOC_CAP_SPIRAM);
  if (!b1 || !b2) {
    ESP_LOGE(TAG, "display buffers: %u bytes x2 unavailable in any heap",
             (unsigned)bufsz);
    abort();
  }
  // DMA source for the rotated region: internal first, because the blitter is
  // fastest reading internal RAM; PSRAM second, because the DPI panel reads it
  // perfectly well; and then a hard check, so the next time the internal heap
  // gets tight this says so on the console instead of storing through a null
  // pointer (board_guition.c has the panic that taught it).
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
  ESP_LOGI(TAG, "display buffers: 3 x %u bytes, free internal %u",
           (unsigned)bufsz, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  lv_display_set_buffers(disp, b1, b2, bufsz, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, rot_flush);
  s_disp = disp;
  return disp;
}

// One try at the GT911: find its address, open it. With `rst` a real pin the
// driver pulses it first; with GPIO_NUM_NC it takes the controller as it is.
static bool gt911_open(gpio_num_t rst) {
  // No interrupt line reaches the P4 on this board, so nothing here picks the
  // GT911's address: it comes up at 0x5D or 0x14 as the board's own pull on
  // that pin decides, at power on and after every reset alike. Waveshare's
  // package probes both before it opens the device, and so does this.
  uint8_t addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
  if (i2c_master_probe(s_i2c_bus, addr, 100) != ESP_OK) {
    addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
    if (i2c_master_probe(s_i2c_bus, addr, 100) != ESP_OK) {
      ESP_LOGE(TAG, "GT911: no answer at 0x5d or 0x14");
      return false;
    }
  }
  esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
  tp_io_cfg.dev_addr = addr;
  esp_lcd_panel_io_handle_t tp_io = NULL;
  if (esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io) != ESP_OK) return false;
  esp_lcd_touch_config_t tp_cfg = {.x_max = LCD_H_RES, .y_max = LCD_V_RES,
                                   .rst_gpio_num = rst, .int_gpio_num = GPIO_NUM_NC,
                                   .levels = {.reset = 0},
                                   .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0}};
  if (esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_touch) != ESP_OK) {
    s_touch = NULL;
    esp_lcd_panel_io_del(tp_io);
    return false;
  }
  s_tp_io = tp_io;                       // kept for the pre-check in the reader
  ESP_LOGI(TAG, "GT911 ready @0x%02x%s", (unsigned)addr,
           rst == GPIO_NUM_NC ? " (no reset)" : "");
  return true;
}

void kiss_board_touch_start(void) {
  i2c_bus_start();
  if (!s_i2c_bus) return;
  // Waveshare's way first: the controller's reset is on GPIO23 and their
  // package hands it to the driver, which pulses it and reads the chip's ID
  // 10 ms later. If that read fails -- a controller still waking up -- the
  // second try waits, looks for the address again and opens it without a
  // reset, which is how Kern runs this board. Either leaves a working touch;
  // the boot log says which.
  if (gt911_open(TOUCH_RST_GPIO)) return;
  ESP_LOGW(TAG, "GT911: open with reset failed, trying without");
  vTaskDelay(pdMS_TO_TICKS(100));
  if (!gt911_open(GPIO_NUM_NC)) ESP_LOGE(TAG, "GT911 init failed");
}

bool kiss_board_touch_ok(void) { return s_touch != NULL; }
i2c_master_bus_handle_t kiss_board_i2c_bus(void) { return s_i2c_bus; }

// ONE read of the GT911, asked through its ready flag first, for the reasons
// board_guition.c sets out at length: the controller's ready flag is a one
// shot that every read clears, the driver acknowledges a frame it never read
// when it writes the status register back, and the frame most often eaten is
// the lift. kiss_touch.c's sampler is the only caller.
#define GT911_BUF_STATUS_REG 0x814E   // bit 7: a frame is ready, host clears it
bool kiss_board_touch_point(int *x, int *y) {
  if (!s_touch) return false;
  bool fresh = true;
  if (s_tp_io) {
    uint8_t st = 0;
    if (esp_lcd_panel_io_rx_param(s_tp_io, GT911_BUF_STATUS_REG, &st, 1) == ESP_OK)
      fresh = (st & 0x80) != 0;
  }
  if (fresh) esp_lcd_touch_read_data(s_touch);
  esp_lcd_touch_point_data_t pt[1];
  uint8_t cnt = 0;
  if (esp_lcd_touch_get_data(s_touch, pt, &cnt, 1) == ESP_OK && cnt > 0) {
    // raw GT911 is portrait (x:0..479, y:0..799); map to the logical
    // 800x480 landscape. Must match the 90deg mapping in rot_flush, and
    // upside down is rot_flush's other quarter turn read backwards, which is
    // both canvas axes reflected. Written HERE rather than in the GT911's
    // flags because the driver mirrors as `x_max - x` and not
    // `x_max - 1 - x`, so a flag flip would hand back a point one pixel
    // outside the canvas.
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
// Three surfaces, one bool: the glass is rot_flush's direction, the touch map
// is the reflection above, and the camera reads the flag itself and turns its
// own three parts. No serialisation is needed: the flip arrives from a click
// handler on the LVGL task, the only task that runs rot_flush, and the camera
// task reads the flag once per frame into a local. board_guition.c has the
// account of the one stale touch sample either side of the tap.
bool kiss_flip_get(void) { return s_flip; }

void kiss_flip_set(bool on, bool repaint)
{
  s_flip = on;
  camera_spike_flip_refresh();     // the preview rect is mapped, so it moves
  if (!repaint) return;
  for (lv_indev_t *d = lv_indev_get_next(NULL); d; d = lv_indev_get_next(d))
    lv_indev_wait_release(d);
  // Both framebuffers, because the flush alternates between them and the one
  // not being painted still holds the old picture the right way up.
  kiss_panel_black();
  lv_obj_invalidate(lv_screen_active());
}
