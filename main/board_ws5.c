// The Waveshare ESP32-P4-WiFi6-Touch-LCD-5, and nothing else: the pins, the
// HX8394 over MIPI-DSI, the software rotation that turns the 1280x720 canvas
// into the 720x1280 panel, the GT911 on the shared I2C bus, the backlight PWM,
// the two DPI framebuffers the camera writes into, and the C6 held in reset.
// It is board_guition.c with this board's numbers: the same portrait glass
// under a landscape UI, so the same quarter turn on the way out and the same
// reflection on the way in, at 720x1280 instead of 480x800. What is this
// board's own: a power rail chip for the panel on the I2C bus, which has to be
// programmed before the panel is reset, so the bus is created here rather than
// in touch_start; a GT911 with neither reset nor interrupt wired, which comes
// up at either of its two addresses; and a panel reset that is active HIGH.
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
#include "esp_lcd_hx8394.h"
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
// Waveshare's own board package runs this panel at 700 Mbps a lane and a 58 MHz
// pixel clock, with the porches below; Kern drives the same board with the
// same clock and the same porches.
#define DSI_LANE_BITRATE_MBPS 700
#define DPI_CLOCK_MHZ 58
#define DSI_PHY_LDO_CHAN 3       // LDO_VO3 feeds VDD_MIPI_DPHY, as on the other DSI boards
#define DSI_PHY_LDO_MV 2500
#define LCD_RST_GPIO 27          // active HIGH on this board: panel_cfg says so
#define LCD_BL_GPIO 26
#define LCD_BL_PWM_FREQ 5000     // the vendor's rate for the driver behind this pin
#define TOUCH_I2C_SCL 8
#define TOUCH_I2C_SDA 7
// The panel's power rail chip. Waveshare's HX8394 driver writes these four
// bytes to it over I2C before it creates the panel, Kern's board file sends the
// same four on the shared bus and shrugs off a NAK, and neither names the part.
// The sequence is theirs byte for byte.
#define RAIL_ADDR 0x45

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static esp_lcd_panel_io_handle_t s_tp_io;  // the GT911's own IO: see the reader's pre-check

static i2c_master_bus_handle_t s_i2c_bus;  // shared: touch, the camera's SCCB, the rail chip

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
  ledc_channel_config_t c = {.gpio_num = LCD_BL_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE,
                             .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 1023};
  ESP_ERROR_CHECK(ledc_channel_config(&c));
}

// The panel while flash is being written: dark for the length of the write,
// and the backlight as the only honest progress indicator. board_guition.c
// has the whole account (the framebuffers live in PSRAM behind the cache a
// flash write disables, so the panel cannot be fed anything true meanwhile);
// the same holds here, pixel count aside.
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

static uint16_t *s_fb;       // native 720x1280 panel framebuffer (used only to clear it once)
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

// Landscape on a portrait panel, the Guition's way: LVGL renders the 1280x720
// logical canvas, each region is turned a quarter into s_rotbuf, and the DMA
// blit pushes it to the panel. Mapping (90deg CW): logical (lx,ly) -> panel
// (px,py) = (719-ly, lx). UPSIDE DOWN is the same quarter turn the other way:
// logical (lx,ly) -> (ly, 1279-lx). board_guition.c says why this is a
// software turn and not the panel's own mirror; the HX8394 is no better
// documented in DPI mode than the ST7701 was.
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
    // panel rect: x in [y1 .. y2], y in [1279-x2 .. 1279-x1]
    esp_lcd_panel_draw_bitmap(s_panel, area->y1, (LCD_V_RES - 1) - area->x2,
                              area->y2 + 1, LCD_V_RES - area->x1, s_rotbuf);
    return;                 // flush_ready in dpi_trans_done, as below
  }
  for (int ly = area->y1; ly <= area->y2; ly++) {
    int j = area->y2 - ly;
    for (int lx = area->x1; lx <= area->x2; lx++)
      s_rotbuf[(lx - area->x1) * ah + j] = *src++;
  }
  // panel rect: x in [719-y2 .. 719-y1], y in [x1 .. x2]
  esp_lcd_panel_draw_bitmap(s_panel, (LCD_H_RES - 1) - area->y2, area->x1,
                            LCD_H_RES - area->y1, area->x2 + 1, s_rotbuf);
  // flush_ready happens in dpi_trans_done when the DMA completes
}

// The shared I2C bus. Created by whichever of display_start and touch_start
// runs first, which on this board is display_start: the panel's rail chip is
// on this bus and has to be written before the panel is reset. The scanner
// and the camera get the handle here as well, so nothing depends on the order
// after this.
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

// The rail chip, probed first: Waveshare's own board package ships without
// this write, so some boards may not carry the part at all, and a missing
// chip must read as a log line, never as a panel that is never created.
static void panel_rails_init(void) {
  if (!s_i2c_bus) return;
  if (i2c_master_probe(s_i2c_bus, RAIL_ADDR, 100) != ESP_OK) {
    ESP_LOGW(TAG, "panel rail chip: no answer at 0x%02x", RAIL_ADDR);
    return;
  }
  i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
                             .device_address = RAIL_ADDR, .scl_speed_hz = 100000};
  i2c_master_dev_handle_t dev = NULL;
  if (i2c_master_bus_add_device(s_i2c_bus, &cfg, &dev) != ESP_OK) {
    ESP_LOGW(TAG, "panel rail chip: add_device failed");
    return;
  }
  static const uint8_t seq[4][2] = {{0x95, 0x11}, {0x95, 0x17}, {0x96, 0x00}, {0x96, 0xFF}};
  int refused = 0;
  for (int i = 0; i < 4; i++) {
    if (i == 3) vTaskDelay(pdMS_TO_TICKS(100));   // the vendor's pause before the last write
    if (i2c_master_transmit(dev, seq[i], 2, 50) != ESP_OK) refused++;
  }
  i2c_master_bus_rm_device(dev);
  vTaskDelay(pdMS_TO_TICKS(100));
  ESP_LOGI(TAG, "panel rail chip at 0x%02x programmed, %d of 4 writes refused",
           RAIL_ADDR, refused);
}

lv_display_t *kiss_board_display_start(void) {
  i2c_bus_start();
  panel_rails_init();
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
      .video_timing = {.h_size = LCD_H_RES, .v_size = LCD_V_RES, .hsync_pulse_width = 20,
                       .hsync_back_porch = 20, .hsync_front_porch = 40, .vsync_pulse_width = 4,
                       .vsync_back_porch = 10, .vsync_front_porch = 24}};
  // No init table of our own: the driver's default IS Waveshare's table for
  // this glass (components/esp_lcd_hx8394/VENDOR.kiss.md), and both vendors
  // run it unchanged.
  hx8394_vendor_config_t vendor_cfg = {
      .mipi_config = {.dsi_bus = dsi_bus, .dpi_config = &dpi_cfg, .lane_num = DSI_LANES}};
  esp_lcd_panel_dev_config_t panel_cfg = {.reset_gpio_num = LCD_RST_GPIO,
                                          .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
                                          .bits_per_pixel = LCD_BITS_PER_PIXEL,
                                          .flags.reset_active_high = 1,
                                          .vendor_config = &vendor_cfg};
  ESP_ERROR_CHECK(esp_lcd_new_panel_hx8394(s_io, &panel_cfg, &s_panel));
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

  // Manual LVGL setup (no esp_lvgl_port display): logical canvas is 1280x720 landscape.
  lv_init();
  kiss_lv_pool_extend();           // the second LVGL pool (kiss_board.h says why)
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
  // 32-line partial buffers, not the other boards' 48. This canvas is 1280
  // wide, so a 48-line band is 123 KB and three of them (two render targets
  // and the rotation buffer below) would want 369 KB of the roughly 340 KB of
  // internal RAM the 4.3in's board file counts. 32 lines is 82 KB each. The
  // boot log's free internal RAM line says whether 24 is needed.
  size_t bufsz = (size_t)SCREEN_W * 32 * 2;
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

void kiss_board_touch_start(void) {
  i2c_bus_start();            // normally already up: display_start needed the bus first
  if (!s_i2c_bus) return;
  // Neither reset nor interrupt is wired to the GT911 on this board, so
  // nothing picks its address: it comes up at 0x5D or 0x14 as its own pins
  // decide, and Waveshare's package probes both before it opens the device.
  // So does this. The 7in pins the address through its reset instead, and the
  // 4.3in trusts the default; neither option exists here.
  uint8_t addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
  if (i2c_master_probe(s_i2c_bus, addr, 100) != ESP_OK) {
    addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
    if (i2c_master_probe(s_i2c_bus, addr, 100) != ESP_OK) {
      ESP_LOGE(TAG, "GT911: no answer at 0x5d or 0x14");
      return;
    }
  }
  esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
  tp_io_cfg.dev_addr = addr;
  esp_lcd_panel_io_handle_t tp_io = NULL;
  if (esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io) != ESP_OK) return;
  s_tp_io = tp_io;                       // kept for the pre-check in the reader
  esp_lcd_touch_config_t tp_cfg = {.x_max = LCD_H_RES, .y_max = LCD_V_RES,
                                   .rst_gpio_num = GPIO_NUM_NC, .int_gpio_num = GPIO_NUM_NC,
                                   .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0}};
  if (esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_touch) != ESP_OK) {
    ESP_LOGE(TAG, "GT911 init failed");
    s_touch = NULL;
    return;
  }
  ESP_LOGI(TAG, "GT911 ready @0x%02x", (unsigned)addr);
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
    // raw GT911 is portrait (x:0..719, y:0..1279); map to the logical
    // 1280x720 landscape. Must match the 90deg mapping in rot_flush, and
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
