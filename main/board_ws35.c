// The Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5, and nothing else: an ST7796
// over SPI, its 320x480 glass turned to 480x320 landscape in the controller,
// an FT5x06 on the shared I2C bus, the backlight PWM, and the same C6 held in
// reset on the same pad as the Guition. Facts
// from the board's schematic and Waveshare's own BSP, checked against Kern,
// which runs on this exact board.
//
// What is different from board_guition.c, and why it is a second file rather
// than a second arm: there is no framebuffer here. A MIPI-DSI panel scans a
// buffer in PSRAM and the camera writes straight into it; an SPI panel holds
// its picture in its own GRAM and the only way in is esp_lcd_panel_draw_bitmap.
// So the canvas is the panel (no rotation, no rotate buffer), every pixel
// crosses one SPI bus, and LVGL and the camera have to share that bus
// politely. kiss_board_blit is the one door; both go through it.
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_st7796.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "kiss_board.h"
#include "kiss_panel.h"
#include "kiss_scan.h"
#include "camera_spike.h"
#include "main.h"   // radio_is_held

static const char *TAG = "kiss";

// ST7796 on SPI2. 80 MHz is what the BSP drives it at; mode 3 likewise.
#define LCD_SPI_HOST   SPI2_HOST
#define LCD_MOSI_GPIO  20
#define LCD_CLK_GPIO   21
#define LCD_CS_GPIO    23
#define LCD_DC_GPIO    26
#define LCD_RST_GPIO   27
#define LCD_PCLK_HZ    (80 * 1000 * 1000)
#define LCD_SPI_MODE   3
#define LCD_BL_GPIO    28
#define LCD_BL_PWM_FREQ 20000
// FT5x06 (an FT6336 on this board) on the same bus the camera's SCCB uses.
#define TOUCH_I2C_SCL  8
#define TOUCH_I2C_SDA  7
#define TOUCH_RST_GPIO 29
#define TOUCH_INT_GPIO 50   // wired, unused: the reader polls, as it does for the GT911

// Landscape, in the controller. The BSP's correct PORTRAIT is MX=1 (mirror x)
// with no swap; a rotation is that transposed (swap x/y) with one axis
// flipped, which leaves exactly two upright landscapes: swap with neither
// mirror, or swap with both. The other two swap combinations are mirrored
// text. This is the first of the two; if the picture comes up upside down,
// both mirrors go to 1 together.
//
// The touch controller reads the glass in its own portrait frame (x across
// the short side, y down the long one), the frame the BSP's portrait picture
// is drawn in. The same rotation in touch terms: canvas x is the glass's y,
// canvas y is the glass's x flipped. The driver applies the mirrors FIRST, in
// the glass's frame, and swaps AFTER (esp_lcd_touch.c), so the flip of the
// canvas's y axis is spelled as a mirror of the glass's x. Spelled as
// mirror_y it flips the other axis, and after the swap every touch lands
// mirrored through the centre of the screen: that was the first flash of
// this layout, where no tap or swipe found its target.
#define LCD_SWAP_XY    1
#define LCD_MIRROR_X   0
#define LCD_MIRROR_Y   0
#define TOUCH_SWAP_XY  1
#define TOUCH_MIRROR_X 1
#define TOUCH_MIRROR_Y 0
// ...and UPSIDE DOWN is the other upright landscape the paragraph above
// names: the swap stays, both mirrors go together. So the flip on this board
// is one register, and the same fact in touch terms is the two flags
// exchanging their values -- a reflection of both canvas axes is a reflection
// of both glass axes, whichever order the driver applies them in.
//
// To one pixel, which is worth knowing before anybody measures it on glass.
// The driver mirrors as `x_max - x`, not `x_max - 1 - x`, so the map here has
// always handed back canvas y = 320 at the glass's x = 0 -- one past a 0..319
// canvas. Flipped, the same single-pixel overhang moves to canvas x = 480 at
// the glass's y = 0. It is relocated, not introduced, and the flip is exact
// everywhere else.
#define LCD_FLIP_MIRROR_X   1
#define LCD_FLIP_MIRROR_Y   1
#define TOUCH_FLIP_MIRROR_X 0
#define TOUCH_FLIP_MIRROR_Y 1

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static i2c_master_bus_handle_t s_i2c_bus;   // shared touch bus; camera SCCB probes it too
static lv_display_t *s_disp;                // for flush_ready from the DMA-done callback

// The C6 CHIP_PU pad: GPIO54 through a 0R link on this board's schematic,
// exactly the Guition's wiring, so the hold is the same code. Level first so
// config never glitches the pad high, then latched so it survives soft resets.
// Logged at W because release builds strip INFO; the release lane greps for
// this line. Settings/home read the pad back via radio_is_held().
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

// The backlight during a firmware write, same contract as the Guition's: dark
// for the length of the write, then brightness as the only honest progress
// indicator. See board_guition.c for the reasoning; the ramp is the same.
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
  uint32_t shaped = (uint32_t)pct * (uint32_t)pct;        // 0..10000
  uint32_t duty = BL_FLOOR + (uint32_t)((1023 - BL_FLOOR) * shaped / 10000);
  static uint32_t last = UINT32_MAX;
  if (last != UINT32_MAX && (duty > last ? duty - last : last - duty) < 8 &&
      pct != 0 && pct != 100)
    return;
  last = duty;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// ---- the transport: one SPI bus, two clients ----
//
// LVGL flushes are asynchronous, as on the Guition: the DMA-done callback
// hands the buffer back with lv_display_flush_ready. The camera's blits are
// synchronous, so its scratch buffer is free again when the call returns and
// it can never run ahead of the panel. The SPI completes transfers in the
// order they were issued, so a small ring remembering who issued each one is
// enough to route the completion; it is deeper than the driver's queue, so it
// cannot wrap.
static SemaphoreHandle_t s_issue;      // one issuer at a time
static SemaphoreHandle_t s_cam_done;   // the camera's transfer completed
static uint8_t s_ring[16];
static volatile uint8_t s_rd, s_wr;
enum { WHO_LVGL = 1, WHO_CAM = 2 };

static bool spi_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e,
                           void *u) {
  (void)io; (void)e; (void)u;
  uint8_t who = s_ring[s_rd++ & 15];
  BaseType_t hp = pdFALSE;
  if (who == WHO_LVGL) {
    if (s_disp) lv_display_flush_ready(s_disp);
  } else {
    xSemaphoreGiveFromISR(s_cam_done, &hp);
  }
  return hp == pdTRUE;
}

// x2, y2 exclusive: esp_lcd's convention. `px` must already be in the panel's
// byte order and written back from the cache.
//
// A transfer that was never queued never completes, so its ring entry is taken
// back before anyone else can issue, and the caller is told. Left in the ring,
// it would hand the NEXT completion to the wrong client: LVGL waiting forever
// on a flush_ready the camera swallowed, or the camera waking on LVGL's. The
// likeliest refusal is the SPI driver failing to copy a PSRAM buffer into
// internal DMA memory, which is why the camera sends in bands.
bool kiss_board_blit(int x1, int y1, int x2, int y2, const void *px, bool cam) {
  xSemaphoreTake(s_issue, portMAX_DELAY);
  s_ring[s_wr++ & 15] = cam ? WHO_CAM : WHO_LVGL;
  esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2, y2, px);
  if (err != ESP_OK) s_wr--;
  xSemaphoreGive(s_issue);
  if (err != ESP_OK) return false;
  if (cam) xSemaphoreTake(s_cam_done, portMAX_DELAY);
  return true;
}

// The panel keeps its own picture, so a flash write cannot starve it the way
// it starves the Guition's scanned-out framebuffers. Black is still what the
// update shows -- deliberate dark reads as "working" -- and it is one
// synchronous pass of a zeroed strip, ten kilobytes of .bss.
void kiss_panel_black(void)
{
  static uint16_t s_black[SCREEN_W * 16] __attribute__((aligned(64)));
  if (!s_panel) return;
  for (int y = 0; y < SCREEN_H; y += 16)
    (void)kiss_board_blit(0, y, SCREEN_W, y + 16, s_black, true);
}

// ---- upside down (kiss_board.h) ----
//
// The whole turn is MADCTL, which is why this board's half of the feature is
// four lines and the Guition's is not. MX/MY are a global map on the address
// space rather than a per-region transform, so a window at canvas x1..x2
// lands at W-1-x2 .. W-1-x1 and is filled from its far end: LVGL's 48-line
// bands and the camera's preview bands reassemble into one coherent 180 with
// no arithmetic anywhere downstream. Nothing in camera_spike.c changes on
// this board for exactly that reason -- the preview, its reticle and its
// overlay text all cross the mirror on their way out.
//
// It matters that the factory table's MADCTL 0x48 was left out of
// s_panel_init (see the note there): the driver latches any MADCTL it sees in
// a vendor table into its saved value, so with 0x48 in place the base would
// be MX=1 and this delta would compose from a mirrored landscape.
//
// SERIALISED ON s_issue, and not for tidiness. The mirror is sent with
// tx_param over the same SPI device as the pixels, and a MADCTL that lands
// between a queued CASET/RASET and its RAMWR reinterprets the address window
// underneath the data that is being written -- a garbage rectangle, not a
// tear. Holding s_issue makes it atomic against any blit's issue, and the
// io's own tx_param drains the queued transfers before it sends, so the
// in-flight RAMWR is waited out inside the call. Two tasks can be issuing
// here (LVGL's flush and the camera's bands on core 1), which is the other
// half of the same reason.
static bool s_flip;

bool kiss_flip_get(void) { return s_flip; }

void kiss_flip_set(bool on, bool repaint)
{
  s_flip = on;
  if (s_panel) {
    xSemaphoreTake(s_issue, portMAX_DELAY);
    esp_lcd_panel_mirror(s_panel, on ? LCD_FLIP_MIRROR_X : LCD_MIRROR_X,
                         on ? LCD_FLIP_MIRROR_Y : LCD_MIRROR_Y);
    xSemaphoreGive(s_issue);
  }
  if (s_touch) {
    // Software flags, not registers: the FT5x06 registers no hardware setter,
    // so these two calls write tp->config.flags and return. That also honours
    // the rule at the touch config below -- an axis flip lives in the flags,
    // never in platform_read_touch's map.
    esp_lcd_touch_set_mirror_x(s_touch, on ? TOUCH_FLIP_MIRROR_X : TOUCH_MIRROR_X);
    esp_lcd_touch_set_mirror_y(s_touch, on ? TOUCH_FLIP_MIRROR_Y : TOUCH_MIRROR_Y);
  }
  // A no-op on this board, and called anyway so the two board files keep the
  // same shape: whatever the camera holds in panel coordinates is re-derived
  // from one place, whichever board is holding it.
  camera_spike_flip_refresh();
  if (!repaint) return;
  // The GRAM holds the ONLY copy of the picture, so until every pixel is
  // rewritten the owner is looking at the last frame upside down. Black
  // first, then a full invalidate: a partial repaint of the control that was
  // tapped would leave the rest of the page inverted, which is the same
  // argument camera_spike_owns_panel() makes about a skipped flush.
  for (lv_indev_t *d = lv_indev_get_next(NULL); d; d = lv_indev_get_next(d))
    lv_indev_wait_release(d);
  kiss_panel_black();
  lv_obj_invalidate(lv_screen_active());
}

// Identity geometry: the canvas is the panel. The one transformation is the
// byte order -- the ST7796 wants big-endian RGB565 and LVGL renders little --
// swapped in place before the DMA reads the buffer.
static void spi_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  uint32_t n = (uint32_t)lv_area_get_width(area) * (uint32_t)lv_area_get_height(area);
  lv_draw_sw_rgb565_swap(px_map, n);
  esp_cache_msync(px_map, n * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  // flush_ready happens in spi_trans_done when the DMA completes, or here when
  // nothing was sent and nothing will
  if (!kiss_board_blit(area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map, false))
    lv_display_flush_ready(disp);
}

static void lv_tick_cb(void *a) { (void)a; lv_tick_inc(2); }

// THE PANEL'S OWN TUNING, as Waveshare's factory firmware for this board sends
// it. The ST7796 component's built-in sequence is a generic one, and it is what
// every published source for this board runs, Waveshare's BSP included: that
// BSP carries the table below but compiles it out (#if 0), with no
// vendor_config. The factory image Waveshare flashes onto the board, the demo
// the glass ships with, does pass it: the 18-entry table was read out of that
// image's app (firmware/ESP32-P4-WiFi6-LCD-3in5.bin in the board's repository,
// the same bytes in its March builds) and is identical to the BSP's disabled
// array plus one MADCTL entry. Against the generic sequence it adds power
// control 1 (C0), and changes power control 2 (C1), VCOM (C5) and both gamma
// curves (E0, E1): the curves this glass is shipped with.
//
// Two of its entries are left out on purpose. MADCTL 0x48 is the factory
// demo's PORTRAIT orientation, and this board is turned landscape below by
// swap_xy/mirror, which build the register from the driver's saved value; and
// COLMOD 0x05 is what the driver already sends for 16 bpp, so repeating it
// only draws the driver's "overwritten" warning. Everything else is in the
// factory order, including its sleep-out and display-on, with its delays.
static const st7796_lcd_init_cmd_t s_panel_init[] = {
    {0x11, NULL, 0, 120},
    {0xF0, (uint8_t[]){0xC3}, 1, 0},
    {0xF0, (uint8_t[]){0x96}, 1, 0},
    {0xB4, (uint8_t[]){0x01}, 1, 0},
    {0xB7, (uint8_t[]){0xC6}, 1, 0},
    {0xC0, (uint8_t[]){0x80, 0x45}, 2, 0},
    {0xC1, (uint8_t[]){0x13}, 1, 0},
    {0xC2, (uint8_t[]){0xA7}, 1, 0},
    {0xC5, (uint8_t[]){0x0A}, 1, 0},
    {0xE8, (uint8_t[]){0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33}, 8, 0},
    {0xE0, (uint8_t[]){0xD0, 0x08, 0x0F, 0x06, 0x06, 0x33, 0x30, 0x33, 0x47, 0x17,
                       0x13, 0x13, 0x2B, 0x31}, 14, 0},
    {0xE1, (uint8_t[]){0xD0, 0x0A, 0x11, 0x0B, 0x09, 0x07, 0x2F, 0x33, 0x47, 0x38,
                       0x15, 0x16, 0x2C, 0x32}, 14, 0},
    {0xF0, (uint8_t[]){0x3C}, 1, 0},
    {0xF0, (uint8_t[]){0x69}, 1, 120},
    {0x21, NULL, 0, 0},
    {0x29, NULL, 0, 0},
};

lv_display_t *kiss_board_display_start(void) {
  spi_bus_config_t bus = {.sclk_io_num = LCD_CLK_GPIO, .mosi_io_num = LCD_MOSI_GPIO,
                          .miso_io_num = -1, .quadwp_io_num = -1, .quadhd_io_num = -1,
                          // a whole canvas in one transfer, the most either
                          // client could ask for; the camera sends its preview
                          // in bands anyway (camera_spike.c says why)
                          .max_transfer_sz = SCREEN_W * SCREEN_H * 2};
  ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));
  esp_lcd_panel_io_spi_config_t io_cfg = {.dc_gpio_num = LCD_DC_GPIO, .cs_gpio_num = LCD_CS_GPIO,
                                          .pclk_hz = LCD_PCLK_HZ, .lcd_cmd_bits = 8,
                                          .lcd_param_bits = 8, .spi_mode = LCD_SPI_MODE,
                                          .trans_queue_depth = 10,
                                          .on_color_trans_done = spi_trans_done};
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &s_io));
  // The factory tuning above, not the component's generic sequence. BGR and
  // the colour inversion are the BSP's values for this glass.
  static const st7796_vendor_config_t vendor = {
      .init_cmds = s_panel_init,
      .init_cmds_size = sizeof s_panel_init / sizeof s_panel_init[0],
  };
  esp_lcd_panel_dev_config_t panel_cfg = {.reset_gpio_num = LCD_RST_GPIO,
                                          .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
                                          .bits_per_pixel = 16,
                                          .vendor_config = (void *)&vendor};
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(s_io, &panel_cfg, &s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
  ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, LCD_SWAP_XY));
  ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, LCD_MIRROR_X, LCD_MIRROR_Y));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
  ESP_LOGI(TAG, "ST7796 ready (SPI2 80MHz, %dx%d landscape, factory panel tuning)", SCREEN_W,
           SCREEN_H);

  s_issue = xSemaphoreCreateMutex();
  s_cam_done = xSemaphoreCreateBinary();
  // The GRAM holds power-on noise, the SPI analogue of clearing the DPI
  // framebuffers once.
  kiss_panel_black();
  // No framebuffers to hand over. The camera composes each frame in its own
  // scratch and sends the preview rect through kiss_board_blit; the handle
  // only tells it the display is up.
  camera_spike_set_panel(s_panel, NULL, NULL);

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
  // 48-line partial buffers, 30 KB each here against the Guition's 75, and no
  // third buffer: nothing is rotated on the way out. Internal DMA memory
  // first, PSRAM second, and the same hard check as the Guition's, so a tight
  // heap says so on the console instead of storing through a null pointer.
  size_t bufsz = (size_t)SCREEN_W * 48 * 2;
  void *b1 = heap_caps_aligned_alloc(64, bufsz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  void *b2 = heap_caps_aligned_alloc(64, bufsz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!b1) b1 = heap_caps_aligned_alloc(64, bufsz, MALLOC_CAP_SPIRAM);
  if (!b2) b2 = heap_caps_aligned_alloc(64, bufsz, MALLOC_CAP_SPIRAM);
  if (!b1 || !b2) {
    ESP_LOGE(TAG, "display buffers: %u bytes x2 unavailable in any heap",
             (unsigned)bufsz);
    abort();
  }
  lv_display_set_buffers(disp, b1, b2, bufsz, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, spi_flush);
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
  kiss_scan_set_bus(bus);              // the QR scanner shares the camera bus
  camera_spike_set_bus(bus);           // the entropy page starts the camera too
  // 400 kHz for the touch device; the sensor's SCCB sets its own, slower, speed
  // per device, so both are legal on one bus.
  esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
  tp_io_cfg.scl_speed_hz = 400000;
  esp_lcd_panel_io_handle_t tp_io = NULL;
  if (esp_lcd_new_panel_io_i2c(bus, &tp_io_cfg, &tp_io) != ESP_OK) return;
  // The reset is a real pin here (the GT911's was not wired); INT stays
  // unused because the reader polls. Any axis flip found on glass goes into
  // these flags, never into platform_read_touch's map.
  // x_max/y_max are the glass's own frame (portrait), which is the frame the
  // driver mirrors in before it swaps; the flags are explained where they
  // are defined.
  esp_lcd_touch_config_t tp_cfg = {.x_max = KISS_PANEL_W, .y_max = KISS_PANEL_H,
                                   .rst_gpio_num = TOUCH_RST_GPIO, .int_gpio_num = GPIO_NUM_NC,
                                   .levels = {.reset = 0, .interrupt = 0},
                                   .flags = {.swap_xy = TOUCH_SWAP_XY, .mirror_x = TOUCH_MIRROR_X,
                                             .mirror_y = TOUCH_MIRROR_Y}};
  if (esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &s_touch) != ESP_OK) {
    ESP_LOGE(TAG, "FT5x06 init failed");
    s_touch = NULL;
    return;
  }
  ESP_LOGI(TAG, "FT5x06 ready");
}

bool kiss_board_touch_ok(void) { return s_touch != NULL; }
i2c_master_bus_handle_t kiss_board_i2c_bus(void) { return s_i2c_bus; }

// Identity map: the driver's flags already turned the point into the canvas,
// and the flip is in those flags too (kiss_flip_set), so this stays identity
// both ways up.
// The first point is logged once so the orientation can be read off the
// serial log at the bench.
bool platform_read_touch(int *x, int *y) {
  if (!s_touch) return false;
  esp_lcd_touch_read_data(s_touch);
  esp_lcd_touch_point_data_t pt[1];
  uint8_t cnt = 0;
  if (esp_lcd_touch_get_data(s_touch, pt, &cnt, 1) == ESP_OK && cnt > 0) {
    static bool logged;
    if (!logged) {
      logged = true;
      ESP_LOGI(TAG, "touch: first point %u,%u on a %dx%d canvas", (unsigned)pt[0].x,
               (unsigned)pt[0].y, SCREEN_W, SCREEN_H);
    }
    *x = pt[0].x;
    *y = pt[0].y;
    return true;
  }
  return false;
}
