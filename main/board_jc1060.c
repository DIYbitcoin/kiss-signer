// The Guition JC1060P470C, 7.0in, and nothing else: the JD9165 over MIPI-DSI,
// the GT911 on the shared I2C bus, the backlight PWM, the two DPI framebuffers
// and the C6 held in reset. The same board family as the 4.3in and most of the
// same parts, so this file is board_guition.c's shape with one difference that
// matters: the glass is 1024x600 and LANDSCAPE, so the canvas is the panel and
// nothing turns a quarter on the way out.
//
// Pins and panel from Guition's own JC1060P470C_I_W sources (schematic, BSP,
// the JD9165BA panel file) and from this board's factory image, which was read
// on the bench before anything was written to it. That image carries the
// first panel revision's init table byte for byte, and not the second
// revision's (a 2026 "V2" panel with a different table, timing and reset pin),
// so this is the first revision. The wrong one shows a white screen with a
// vertical band of noise.
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
#include "esp_lcd_jd9165.h"
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

#define LCD_H_RES KISS_PANEL_W   // the glass, which is the canvas
#define LCD_V_RES KISS_PANEL_H
#define LCD_BITS_PER_PIXEL 16
// Two lanes. The vendor's own sources disagree on the lane rate (550, 750 and
// 900 all appear); anything above ~430 Mbps carries 54 MHz of RGB565, and 750
// is the driver's default and the rate the widest-used community profile runs.
#define DSI_LANES 2
#define DSI_LANE_BITRATE_MBPS 750
#define DPI_CLOCK_MHZ 54
#define DSI_PHY_LDO_CHAN 3       // LDO_VO3 feeds VDD_MIPI_DPHY, as on the 4.3in
#define DSI_PHY_LDO_MV 2500
#define LCD_RST_GPIO 27
#define LCD_BL_GPIO 23           // enable of the backlight boost, active high
#define LCD_BL_PWM_FREQ 20000
#define TOUCH_I2C_SCL 8
#define TOUCH_I2C_SDA 7
#define TOUCH_RST_GPIO 22
#define TOUCH_INT_GPIO 21

// The first panel revision's table, exactly as this board's factory image
// sends it: the JD9165BA page writes, both gamma pages, RGB565, then sleep out
// and display on with the vendor's delays.
static const jd9165_lcd_init_cmd_t jd9165_lcd_cmds[] = {
    {0x30, (uint8_t[]){0x00}, 1, 0},
    {0xF7, (uint8_t[]){0x49, 0x61, 0x02, 0x00}, 4, 0},
    {0x30, (uint8_t[]){0x01}, 1, 0},
    {0x04, (uint8_t[]){0x0C}, 1, 0},
    {0x05, (uint8_t[]){0x00}, 1, 0},
    {0x06, (uint8_t[]){0x00}, 1, 0},
    {0x0B, (uint8_t[]){0x11}, 1, 0},
    {0x17, (uint8_t[]){0x00}, 1, 0},
    {0x20, (uint8_t[]){0x04}, 1, 0},
    {0x1F, (uint8_t[]){0x05}, 1, 0},
    {0x23, (uint8_t[]){0x00}, 1, 0},
    {0x25, (uint8_t[]){0x19}, 1, 0},
    {0x28, (uint8_t[]){0x18}, 1, 0},
    {0x29, (uint8_t[]){0x04}, 1, 0},
    {0x2A, (uint8_t[]){0x01}, 1, 0},
    {0x2B, (uint8_t[]){0x04}, 1, 0},
    {0x2C, (uint8_t[]){0x01}, 1, 0},
    {0x30, (uint8_t[]){0x02}, 1, 0},
    {0x01, (uint8_t[]){0x22}, 1, 0},
    {0x03, (uint8_t[]){0x12}, 1, 0},
    {0x04, (uint8_t[]){0x00}, 1, 0},
    {0x05, (uint8_t[]){0x64}, 1, 0},
    {0x0A, (uint8_t[]){0x08}, 1, 0},
    {0x0B, (uint8_t[]){0x0A, 0x1A, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x06, 0x08, 0x1F, 0x1D}, 11, 0},
    {0x0C, (uint8_t[]){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x0D, (uint8_t[]){0x16, 0x1B, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x07, 0x09, 0x1E, 0x1C}, 11, 0},
    {0x0E, (uint8_t[]){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x0F, (uint8_t[]){0x16, 0x1B, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1C, 0x1E, 0x09, 0x07}, 11, 0},
    {0x10, (uint8_t[]){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x11, (uint8_t[]){0x0A, 0x1A, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1D, 0x1F, 0x08, 0x06}, 11, 0},
    {0x12, (uint8_t[]){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x14, (uint8_t[]){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0x18, (uint8_t[]){0x99}, 1, 0},
    {0x30, (uint8_t[]){0x06}, 1, 0},
    {0x12, (uint8_t[]){0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29}, 14, 0},
    {0x13, (uint8_t[]){0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29}, 14, 0},
    {0x30, (uint8_t[]){0x0A}, 1, 0},
    {0x02, (uint8_t[]){0x4F}, 1, 0},
    {0x0B, (uint8_t[]){0x40}, 1, 0},
    {0x12, (uint8_t[]){0x3E}, 1, 0},
    {0x13, (uint8_t[]){0x78}, 1, 0},
    {0x30, (uint8_t[]){0x0D}, 1, 0},
    {0x0D, (uint8_t[]){0x04}, 1, 0},
    {0x10, (uint8_t[]){0x0C}, 1, 0},
    {0x11, (uint8_t[]){0x0C}, 1, 0},
    {0x12, (uint8_t[]){0x0C}, 1, 0},
    {0x13, (uint8_t[]){0x0C}, 1, 0},
    {0x30, (uint8_t[]){0x00}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 20},
};

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static esp_lcd_panel_io_handle_t s_tp_io;  // the GT911's own IO: see the reader's pre-check

static i2c_master_bus_handle_t s_i2c_bus;  // shared touch bus; camera SCCB probes it too

// The C6 coprocessor's CHIP_PU is on GPIO54 here as on the other two boards.
// board_guition.c says why it is held from the first code that runs.
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
  // Three band buffers of 1024x48 are the first large internal allocations;
  // on this canvas they are 98 KB each against the 4.3in's 75 KB, so say what
  // is left before they are made.
  ESP_LOGI(TAG, "internal heap free %u, largest block %u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
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

// Dark for the length of a flash write, and brightness as its progress: the
// same reasons as board_guition.c, which has them in full. The framebuffers
// live in PSRAM behind the cache the write disables, on this board as there.
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
  uint32_t shaped = (uint32_t)pct * (uint32_t)pct;        // 0..10000, gamma
  uint32_t duty = BL_FLOOR + (uint32_t)((1023 - BL_FLOOR) * shaped / 10000);
  static uint32_t last = UINT32_MAX;
  if (last != UINT32_MAX && (duty > last ? duty - last : last - duty) < 8 &&
      pct != 0 && pct != 100)
    return;
  last = duty;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static uint16_t *s_fb;       // the two DPI framebuffers, 1024x600 each
static void *s_fb2;

// Both framebuffers to black, written back through the cache so the panel is
// fed the black the CPU wrote (board_guition.c has the account).
void kiss_panel_black(void)
{
  const size_t n = (size_t)LCD_H_RES * LCD_V_RES * 2;
  if (s_fb)  { memset(s_fb,  0, n); esp_cache_msync(s_fb,  n, ESP_CACHE_MSYNC_FLAG_DIR_C2M); }
  if (s_fb2) { memset(s_fb2, 0, n); esp_cache_msync(s_fb2, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M); }
}
static uint16_t *s_blitbuf;  // the band handed to the blitter (DMA source)
static lv_display_t *s_disp; // for flush_ready from the DMA-done callback
static bool s_flip;          // upside down (kiss_board.h)

static void lv_tick_cb(void *a) { (void)a; lv_tick_inc(2); }

static bool dpi_trans_done(esp_lcd_panel_handle_t p, esp_lcd_dpi_panel_event_data_t *e, void *u) {
  (void)p; (void)e; (void)u;
  if (s_disp) lv_display_flush_ready(s_disp);
  return false;
}

// The canvas is the panel, so a band goes out where LVGL drew it. It still goes
// through s_blitbuf, for two reasons: upside down is the same band written
// backwards -- canvas (x,y) to panel (1023-x, 599-y), a 180 that is simply the
// band's pixels in reverse order -- and the blitter reads the one buffer this
// file made DMA-capable, whatever heap LVGL's band buffers came from.
//
// Not esp_lcd_panel_mirror: the JD9165 driver has one, but nothing establishes
// that this glass honours the scan-direction bits in DPI mode, and the 4.3in
// made the same call for the same reason (board_guition.c).
static void jc_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  // While the camera owns the whole panel, LVGL must not paint (board_guition.c).
  if (camera_spike_owns_panel()) {
    lv_display_flush_ready(disp);
    return;
  }
  const uint16_t *src = (const uint16_t *)px_map;
  const int w = area->x2 - area->x1 + 1;
  const int h = area->y2 - area->y1 + 1;
  const size_t n = (size_t)w * (size_t)h;
  if (s_flip) {
    for (size_t i = 0; i < n; i++) s_blitbuf[n - 1 - i] = src[i];
    esp_lcd_panel_draw_bitmap(s_panel, (LCD_H_RES - 1) - area->x2, (LCD_V_RES - 1) - area->y2,
                              LCD_H_RES - area->x1, LCD_V_RES - area->y1, s_blitbuf);
    return;                 // flush_ready in dpi_trans_done
  }
  memcpy(s_blitbuf, src, n * sizeof(uint16_t));
  esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, s_blitbuf);
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
  // The vendor's first-revision timing (Guition's JD9165 driver and BSP).
  esp_lcd_dpi_panel_config_t dpi_cfg = {
      .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT, .dpi_clock_freq_mhz = DPI_CLOCK_MHZ,
      .virtual_channel = 0, .in_color_format = LCD_COLOR_FMT_RGB565,
      .out_color_format = LCD_COLOR_FMT_RGB565, .num_fbs = 2,  // FB #2 = camera flip target
      .video_timing = {.h_size = LCD_H_RES, .v_size = LCD_V_RES, .hsync_pulse_width = 40,
                       .hsync_back_porch = 160, .hsync_front_porch = 160, .vsync_pulse_width = 10,
                       .vsync_back_porch = 23, .vsync_front_porch = 12}};
  jd9165_vendor_config_t vendor_cfg = {
      .init_cmds = jd9165_lcd_cmds,
      .init_cmds_size = sizeof(jd9165_lcd_cmds) / sizeof(jd9165_lcd_cmds[0]),
      .mipi_config = {.dsi_bus = dsi_bus, .dpi_config = &dpi_cfg}};
  esp_lcd_panel_dev_config_t panel_cfg = {.reset_gpio_num = LCD_RST_GPIO,
                                          .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
                                          .bits_per_pixel = LCD_BITS_PER_PIXEL,
                                          .vendor_config = &vendor_cfg};
  ESP_ERROR_CHECK(esp_lcd_new_panel_jd9165(s_io, &panel_cfg, &s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
  ESP_LOGI(TAG, "JD9165 ready");

  // Both framebuffers black before anything is shown (un-painted regions are
  // otherwise power-on noise), and the DMA-done callback that returns bands.
  void *fb2 = NULL;
  ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2, (void **)&s_fb, &fb2));
  s_fb2 = fb2;
  kiss_panel_black();
  camera_spike_set_panel(s_panel, s_fb, fb2);
  esp_lcd_dpi_panel_event_callbacks_t dpi_cbs = {.on_color_trans_done = dpi_trans_done};
  ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(s_panel, &dpi_cbs, NULL));

  lv_init();
  const esp_timer_create_args_t tcfg = {.callback = lv_tick_cb, .name = "lvtick"};
  esp_timer_handle_t tick;
  ESP_ERROR_CHECK(esp_timer_create(&tcfg, &tick));
  ESP_ERROR_CHECK(esp_timer_start_periodic(tick, 2000));   // 2 ms LVGL tick

  lv_display_t *disp = lv_display_create(SCREEN_W, SCREEN_H);
  if (!disp) {
    ESP_LOGE(TAG, "lv_display_create failed");
    abort();                       // a panic reboots into last-known-good
  }
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  // 48-line bands, internal RAM first and PSRAM second, and a hard stop if
  // neither has room: board_guition.c says what an unchecked NULL did there.
  size_t bufsz = (size_t)SCREEN_W * 48 * 2;
  void *b1 = heap_caps_malloc(bufsz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  void *b2 = heap_caps_malloc(bufsz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!b1) b1 = heap_caps_aligned_alloc(64, bufsz, MALLOC_CAP_SPIRAM);
  if (!b2) b2 = heap_caps_aligned_alloc(64, bufsz, MALLOC_CAP_SPIRAM);
  if (!b1 || !b2) {
    ESP_LOGE(TAG, "display buffers: %u bytes x2 unavailable in any heap", (unsigned)bufsz);
    abort();
  }
  s_blitbuf = heap_caps_malloc(bufsz, MALLOC_CAP_DMA);
  if (!s_blitbuf) {
    ESP_LOGW(TAG, "blitbuf: no internal DMA heap for %u bytes, using PSRAM", (unsigned)bufsz);
    s_blitbuf = heap_caps_aligned_alloc(64, bufsz, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
  }
  if (!s_blitbuf) {
    ESP_LOGE(TAG, "blitbuf: %u bytes unavailable in any heap", (unsigned)bufsz);
    abort();
  }
  lv_display_set_buffers(disp, b1, b2, bufsz, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, jc_flush);
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
  kiss_scan_set_bus(bus);
  camera_spike_set_bus(bus);
  esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
  esp_lcd_panel_io_handle_t tp_io = NULL;
  if (esp_lcd_new_panel_io_i2c(bus, &tp_io_cfg, &tp_io) != ESP_OK) return;
  s_tp_io = tp_io;
  // Reset and interrupt are both wired on this board (the 4.3in wires
  // neither, and its GT911 is never reset). The level INT holds as reset lets
  // go is what picks the GT911's address, so the driver is handed the address
  // the IO above was opened on: with that and both pins it holds INT at the
  // matching level through the reset and waits 60 ms before the first read.
  // Without driver_data it would pulse reset with INT left floating and read
  // 10 ms later, and a chip that came up at the other address is a touch
  // panel that never answers.
  esp_lcd_touch_io_gt911_config_t gt911_addr = {.dev_addr = tp_io_cfg.dev_addr};
  esp_lcd_touch_config_t tp_cfg = {.x_max = LCD_H_RES, .y_max = LCD_V_RES,
                                   .rst_gpio_num = TOUCH_RST_GPIO, .int_gpio_num = TOUCH_INT_GPIO,
                                   .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
                                   .driver_data = &gt911_addr};
  if (esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_touch) != ESP_OK) {
    ESP_LOGE(TAG, "GT911 init failed");
    s_touch = NULL;
    return;
  }
  ESP_LOGI(TAG, "GT911 ready @0x%02x", (unsigned)tp_io_cfg.dev_addr);
}

bool kiss_board_touch_ok(void) { return s_touch != NULL; }
i2c_master_bus_handle_t kiss_board_i2c_bus(void) { return s_i2c_bus; }

// ONE read of the GT911, asked through its ready flag first. board_guition.c
// has the whole account of why both of those matter; it is the same controller
// and the same driver.
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
    // The panel's own frame is the canvas. The first point is logged once so
    // the glass can confirm the axes; a controller configured the other way
    // round shows up here, not as a tap that lands somewhere else.
    static bool logged;
    if (!logged) {
      logged = true;
      ESP_LOGI(TAG, "touch: first point raw %d,%d", (int)pt[0].x, (int)pt[0].y);
    }
    // Upside down is both axes reflected, written here and not in the
    // driver's flags, which mirror as x_max - x and so land one pixel outside
    // the canvas (board_guition.c).
    if (s_flip) {
      *x = (LCD_H_RES - 1) - pt[0].x;
      *y = (LCD_V_RES - 1) - pt[0].y;
      return true;
    }
    *x = pt[0].x;
    *y = pt[0].y;
    return true;
  }
  return false;
}

// ---- upside down (kiss_board.h) ----
// The glass is jc_flush's direction and the touch map is the reflection
// above; both are read on the LVGL task and the touch sampler's, as on the
// 4.3in, where the same single-point window is described.
bool kiss_flip_get(void) { return s_flip; }

void kiss_flip_set(bool on, bool repaint)
{
  s_flip = on;
  camera_spike_flip_refresh();
  if (!repaint) return;
  for (lv_indev_t *d = lv_indev_get_next(NULL); d; d = lv_indev_get_next(d))
    lv_indev_wait_release(d);
  kiss_panel_black();
  lv_obj_invalidate(lv_screen_active());
}
