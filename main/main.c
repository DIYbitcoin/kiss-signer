#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifndef SIMULATOR  // ESP-only hardware bring-up; the desktop simulator provides its own platform
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_ldo_regulator.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#include "lvgl.h"
#include "sprites.h"
#include "menu_img.h"
#include "menu_logo.h"
#include "gameover_img.h"
#include "wallet_img.h"
#include "mote_img.h"
#include "wallet_ui.h"
#include "wallet_recv.h"
#include "wallet_sign.h"
#include "wallet_scan.h"
#include "wallet_settings.h"
#include "wallet_setup.h"
#include "wallet_seed.h"
#include "wallet_crypto.h"
#ifndef SIMULATOR
#include "wallet_crypto.h"
#include "camera_spike.h"
#include "platform_sd.h"
#endif

// touch read is the platform seam: device reads GT911, simulator feeds scripted input
extern bool platform_read_touch(int *x, int *y);

#ifndef SIMULATOR
static const char *TAG = "kiss";
#endif

#define LCD_H_RES 480   // PHYSICAL panel (native portrait): DPI timings, framebuffer, raw touch
#define LCD_V_RES 800
// LOGICAL UI canvas: the whole game is LANDSCAPE. Device reaches this via a one-time boot
// rotation (display_start); the sim creates an 800x480 display directly. All UI/gameplay/art
// is authored in these coordinates.
#define SCREEN_W 800   // LOGICAL landscape canvas; rotated into the 480x800 panel by rot_flush
#define SCREEN_H 480
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
#define SPRITE_SRC 132  // source sprite size

#ifndef SIMULATOR
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
#endif  // !SIMULATOR

// ---------------- game ----------------
#define MAX_ENT 28
#define TRAIL_LEN 6       // trail length (per-frame redraw is bounded by the local-origin fix below)
#ifndef BLADE_MAX_SPAN
#define BLADE_MAX_SPAN 150  // px: hard cap on blade length so a fast swipe / dropped frame can't
                            // stretch the redraw box across the screen (breaks the lag feedback loop)
#endif
#define GRAVITY 0.5f
#define TICK_MS 16

typedef struct {
  const lv_image_dsc_t *whole, *hl, *hr;
  int size, hsize;
  bool bomb, burst;
  uint32_t juice;
} def_t;

static const def_t DEFS[] = {
    {&img_watermelon, &img_watermelon_half, &img_watermelon_halfr, 104, 104, false, false, 0xF0364C},
    {&img_apple, &img_apple_half, &img_apple_halfr, 94, 94, false, false, 0xF3E8C6},
    {&img_orange, &img_orange_half, &img_orange_halfr, 94, 94, false, false, 0xFF9E1B},
    {&img_pineapple, &img_pineapple_half, &img_pineapple_halfr, 112, 112, false, false, 0xFFD23A},
    {&img_strawberry, NULL, NULL, 100, 0, false, true, 0xFF466E},
    {&img_cherries, NULL, NULL, 90, 0, false, true, 0xE01F2A},
    {&img_grapes, NULL, NULL, 94, 0, false, true, 0x9C4DCC},
    {&img_bomb, NULL, NULL, 92, 0, true, false, 0},
};
#define NUM_DEFS (sizeof(DEFS) / sizeof(DEFS[0]))
#define BOMB_IDX (NUM_DEFS - 1)

typedef enum { K_NONE, K_FRUIT, K_HALF, K_JUICE } kind_t;
typedef struct {
  bool active;
  kind_t kind;
  bool bomb;
  int defi;
  float x, y, vx, vy;
  int size;
  lv_obj_t *obj;
} ent_t;

static ent_t s_ent[MAX_ENT];
static lv_obj_t *s_blade, *s_blade_glow;
static lv_point_precise_t s_trail[TRAIL_LEN];
static int s_trail_count;
static lv_obj_t *s_score_lbl;
static lv_obj_t *s_hearts[3];
static lv_obj_t *s_menu_panel;
static lv_obj_t *s_logo_lt[LOGO_LT_N];  // logo letters (children of the menu panel; drop in on entry)
static lv_obj_t *s_menu_fruit[MENU_FRUIT_N];  // accent fruit: hop in after the letters, then float
static lv_obj_t *s_over_panel, *s_over_lbl, *s_best_lbl, *s_newbest;
static int s_score, s_best, s_lives = 3;
static int s_swipe_n;        // fruit sliced in the current swipe (for combos)
static int s_life_milestone; // highest 50-pt mark a bonus life was granted for
enum { ST_MENU, ST_PLAY, ST_OVER };
static int s_state = ST_MENU;
static bool s_prev_press;
// Wallet home tiles arm on press but OPEN on release: opening under a
// still-pressed finger lets the release "click through" onto whatever LVGL
// button the new screen put beneath it (the sim caught this on the Sign
// chooser — the finger sat exactly on a chooser pill).
static int s_tile_pend;   // 0 none, 1 Sign, 2 Receive, 3 Wallet/export
static lv_timer_t *s_spawn_timer;  // handle so start_game can reset the difficulty ramp

// ---- hidden KISS wallet: revealed by drawing a "K" on the game menu (cover -> wallet) ----
static lv_obj_t *s_wallet;         // baked KISS wallet menu (visual shell only, for now)
#define N_MOTES 5
static lv_obj_t *s_mote[N_MOTES];  // ambient idle life: dim motes drifting up
static bool s_wallet_on;
// dev-seed fingerprint for the top-right chip; filled from the boot selftest on
// device (sim build has no libwally, keeps the placeholder)
static char s_fp_hex[12] = "--------";
static lv_obj_t *s_fp_chip;              // home fingerprint chip (updated at unlock)
static lv_obj_t *s_fp_cap;               // "fingerprint" caption under the chip frame
static lv_obj_t *s_fp_fly;               // transient: the code flying from the reveal card
static lv_obj_t *s_cam_lbl;              // bottom-center status/error slot
static lv_obj_t *s_net_lbl;              // top-center TESTNET badge (hidden on mainnet)
static uint32_t s_wallet_act_t;          // idle auto-lock: last touch while unlocked
#ifndef SIMULATOR
static i2c_master_bus_handle_t s_i2c_bus;  // shared touch bus; camera SCCB probes it too
#endif
#define GEST_MAX 160               // accumulated points across the strokes of the unlock draw
static lv_point_t s_gpt[GEST_MAX];
static uint8_t s_gid[GEST_MAX];     // stroke id per point (for same-stroke gap filling)
static lv_point_t s_gsub[GEST_MAX]; // scratch: the left-letter subset, for the K check
static int s_gn;
static int s_strokes;              // number of strokes in the current draw (KISS is many)
static int s_stroke_n0;            // index where the current stroke began (tap vs draw test)
static uint32_t s_gest_idle;       // ms since the last gesture activity (abandon timeout)
static bool s_gest_swallow;        // ignore the touch that just woke the screensaver

// ---- idle attract-mode screensaver ----
#define IDLE_MS 60000              // show the screensaver after 1min with no touch (menu/game-over)
#define SAVER_N 6
static lv_obj_t *s_saver;          // full-screen backdrop (img_saver)
static lv_obj_t *s_saver_fruit[SAVER_N];
static lv_obj_t *s_saver_hint;     // pulsing "tap to play" prompt
static bool s_saver_on;
static uint32_t s_idle_ms;
static void saver_hide(void);      // defined below; start_game (above it) needs it

// portable xorshift RNG (was esp_random) so the game compiles for device + simulator
static uint32_t s_rng = 0x9e3779b9u;
static uint32_t rnd(uint32_t n) {
  s_rng ^= s_rng << 13;
  s_rng ^= s_rng >> 17;
  s_rng ^= s_rng << 5;
  return n ? s_rng % n : 0;
}
static int rnd_range(int a, int b) { return a + (int)rnd(b - a + 1); }

#ifndef SIMULATOR
static void log_board_info(void) {
  esp_chip_info_t chip;
  uint32_t fs = 0;
  esp_chip_info(&chip);
  if (esp_flash_get_size(NULL, &fs) != ESP_OK) fs = 0;
  ESP_LOGI(TAG, "KISS - Guition JC4880P443C ESP32-P4 rev%d flash=%luMB", chip.revision,
           (unsigned long)(fs / (1024 * 1024)));
}

static void backlight_on(void) {
  ledc_timer_config_t t = {.speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_0,
                           .duty_resolution = LEDC_TIMER_10_BIT, .freq_hz = LCD_BL_PWM_FREQ,
                           .clk_cfg = LEDC_AUTO_CLK};
  ESP_ERROR_CHECK(ledc_timer_config(&t));
  ledc_channel_config_t c = {.gpio_num = LCD_BL_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE,
                             .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 1023};
  ESP_ERROR_CHECK(ledc_channel_config(&c));
}

static uint16_t *s_fb;       // native 480x800 panel framebuffer (used only to clear it once)
static uint16_t *s_rotbuf;   // pre-rotated region, handed to the hardware blitter (DMA source)
static lv_display_t *s_disp; // for flush_ready from the DMA-done callback

static void lv_tick_cb(void *a) { (void)a; lv_tick_inc(2); }

// the hardware blit (esp_lcd_panel_draw_bitmap) finished copying -> let LVGL render the next region
static bool dpi_trans_done(esp_lcd_panel_handle_t p, esp_lcd_dpi_panel_event_data_t *e, void *u) {
  (void)p; (void)e; (void)u;
  if (s_disp) lv_display_flush_ready(s_disp);
  return false;
}

// Landscape WITHOUT the driver's rotation (which panics): LVGL renders the 800x480 logical canvas;
// we rotate each region 90deg into s_rotbuf, then let the SAME fast DMA blit the portrait build used
// push it to the panel (CPU pixel writes to the live framebuffer tore on moving content).
// Mapping (90deg CW): logical (lx,ly) -> panel (px,py) = (479-ly, lx).
static void rot_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  (void)disp;
  // While the camera owns the panel (scan/entropy/preview), LVGL must not
  // paint: menu animations under the wizard kept dirtying regions, and every
  // repair flush flashed black boxes over the live video. Video ends with a
  // full-screen invalidate, so dropping these flushes loses nothing.
  if (camera_spike_is_on()) {
    lv_display_flush_ready(disp);
    return;
  }
  uint16_t *src = (uint16_t *)px_map;
  int ah = area->y2 - area->y1 + 1;                 // rotated rect width (panel x)
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

static lv_display_t *display_start(void) {
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
  memset(s_fb, 0, (size_t)LCD_H_RES * LCD_V_RES * 2);
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
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  size_t bufsz = (size_t)SCREEN_W * 48 * 2;                  // 48-line partial buffers
  void *b1 = heap_caps_malloc(bufsz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  void *b2 = heap_caps_malloc(bufsz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  s_rotbuf = heap_caps_malloc(bufsz, MALLOC_CAP_DMA);   // DMA source for the rotated region
  lv_display_set_buffers(disp, b1, b2, bufsz, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, rot_flush);
  s_disp = disp;
  return disp;
}

static void touch_start(void) {
  i2c_master_bus_config_t i2c_cfg = {.i2c_port = I2C_NUM_0, .sda_io_num = TOUCH_I2C_SDA,
                                     .scl_io_num = TOUCH_I2C_SCL, .clk_source = I2C_CLK_SRC_DEFAULT,
                                     .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true};
  i2c_master_bus_handle_t bus = NULL;
  if (i2c_new_master_bus(&i2c_cfg, &bus) != ESP_OK) { ESP_LOGE(TAG, "i2c failed"); return; }
  s_i2c_bus = bus;
  wallet_scan_set_bus(bus);              // step 6: QR scanner shares the camera bus
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
#endif  // !SIMULATOR

// ---------------- entities ----------------
static lv_obj_t *make_sprite(const lv_image_dsc_t *dsc) {
  lv_obj_t *o = lv_image_create(lv_screen_active());
  lv_image_set_src(o, dsc);  // pre-sized sprite -> no runtime scaling (fast path)
  return o;
}

static lv_obj_t *make_droplet(uint32_t col) {
  lv_obj_t *o = lv_image_create(lv_screen_active());
  lv_image_set_src(o, &img_droplet);
  lv_obj_set_style_image_recolor(o, lv_color_hex(col), 0);
  lv_obj_set_style_image_recolor_opa(o, LV_OPA_COVER, 0);
  return o;
}

// short-lived animated effects
static void anim_scale_cb(void *o, int32_t v) { lv_image_set_scale((lv_obj_t *)o, v); }
static void anim_opa_cb(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, v, 0); }
static void anim_bgopa_cb(void *o, int32_t v) { lv_obj_set_style_bg_opa((lv_obj_t *)o, v, 0); }
static void saver_set_x(void *o, int32_t v) { lv_obj_set_x((lv_obj_t *)o, v); }
static void saver_set_y(void *o, int32_t v) { lv_obj_set_y((lv_obj_t *)o, v); }
static void anim_del_cb(lv_anim_t *a) { lv_obj_delete((lv_obj_t *)a->var); }
static void anim_y_cb(void *o, int32_t v) { lv_obj_set_y((lv_obj_t *)o, v); }

// small "+1" / "COMBO xN" text that drifts up and fades, then self-deletes (Fruit-Ninja juice)
static void score_popup(int x, int y, const char *txt, uint32_t color) {
  lv_obj_t *l = lv_label_create(lv_screen_active());
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
  if (x < 6) x = 6;
  if (x > SCREEN_W - 120) x = SCREEN_W - 120;
  if (y < 28) y = 28;
  lv_obj_set_pos(l, x, y);
  lv_anim_t a;
  lv_anim_init(&a); lv_anim_set_var(&a, l);
  lv_anim_set_exec_cb(&a, anim_y_cb);
  lv_anim_set_values(&a, y, y - 54);
  lv_anim_set_duration(&a, 600);
  lv_anim_set_ready_cb(&a, anim_del_cb);
  lv_anim_start(&a);
  lv_anim_t b;
  lv_anim_init(&b); lv_anim_set_var(&b, l);
  lv_anim_set_exec_cb(&b, anim_opa_cb);
  lv_anim_set_values(&b, 255, 0);
  lv_anim_set_duration(&b, 560);
  lv_anim_start(&b);
}

// subtle "you earned a life" feedback: pop the gained heart in + a soft green "+1 LIFE"
static void life_gain_fx(lv_obj_t *heart) {
  lv_image_set_pivot(heart, 20, 20);
  lv_anim_t a;
  lv_anim_init(&a); lv_anim_set_var(&a, heart);
  lv_anim_set_exec_cb(&a, anim_scale_cb);
  lv_anim_set_values(&a, 40, 256);                 // strong pop-in with bounce, then settle
  lv_anim_set_duration(&a, 380);
  lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
  lv_anim_start(&a);
  score_popup(SCREEN_W - 176, 44, "+1 LIFE", 0x8CF09A);
}

static void explosion(float x, float y) {
  lv_obj_t *o = lv_image_create(lv_screen_active());
  lv_image_set_src(o, &img_explosion);
  lv_image_set_pivot(o, 72, 72);
  lv_obj_set_pos(o, (int)x - 72, (int)y - 72);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, o);
  lv_anim_set_exec_cb(&a, anim_scale_cb);
  lv_anim_set_values(&a, 90, 320);  // smaller peak -> far fewer pixels to rasterize
  lv_anim_set_duration(&a, 360);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
  lv_anim_t b;
  lv_anim_init(&b);
  lv_anim_set_var(&b, o);
  lv_anim_set_exec_cb(&b, anim_opa_cb);
  lv_anim_set_values(&b, 255, 0);
  lv_anim_set_duration(&b, 360);
  lv_anim_set_ready_cb(&b, anim_del_cb);
  lv_anim_start(&b);
}

static void screen_flash(uint32_t col) {
  lv_obj_t *f = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(f);
  lv_obj_set_size(f, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(f, 0, 0);
  lv_obj_set_style_bg_color(f, lv_color_hex(col), 0);
  lv_obj_set_style_bg_opa(f, 150, 0);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, f);
  lv_anim_set_exec_cb(&a, anim_bgopa_cb);
  lv_anim_set_values(&a, 150, 0);
  lv_anim_set_duration(&a, 260);
  lv_anim_set_ready_cb(&a, anim_del_cb);
  lv_anim_start(&a);
}

// juicy splash at the cut: a recolored splat that fades out.
// NOTE: fade-only (no scale animation) on purpose - image scaling is the most
// expensive LVGL op and was the main cause of swipe lag.
static void juice_splat(float x, float y, uint32_t col) {
  lv_obj_t *o = lv_image_create(lv_screen_active());
  lv_image_set_src(o, &img_splat);  // 100px, white -> recolored to juice
  lv_obj_set_style_image_recolor(o, lv_color_hex(col), 0);
  lv_obj_set_style_image_recolor_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_pos(o, (int)x - 50, (int)y - 50);
  lv_anim_t b;
  lv_anim_init(&b);
  lv_anim_set_var(&b, o);
  lv_anim_set_exec_cb(&b, anim_opa_cb);
  lv_anim_set_values(&b, 240, 0);
  lv_anim_set_duration(&b, 240);
  lv_anim_set_ready_cb(&b, anim_del_cb);
  lv_anim_start(&b);
}

static ent_t *alloc_ent(void) {
  for (int i = 0; i < MAX_ENT; i++)
    if (!s_ent[i].active) return &s_ent[i];
  return NULL;
}

static void place(ent_t *e) {
  if (e->obj) lv_obj_set_pos(e->obj, (int)e->x - e->size / 2, (int)e->y - e->size / 2);
}

static void clear_all(void) {
  for (int i = 0; i < MAX_ENT; i++) {
    if (s_ent[i].active && s_ent[i].obj) lv_obj_delete(s_ent[i].obj);
    s_ent[i].active = false;
    s_ent[i].obj = NULL;
  }
}

// Difficulty steps up GENTLY every 50 points (one small notch per milestone, +10% each),
// reaching full only at 500. Easy early; never crazy-hard right away. Keyed to score so it
// tracks the milestones the player sees, but the steps are small enough that combos (which
// inflate the score) can't spike it.
static float diff_progress(void) {
  int lvl = s_score / 50;
  if (lvl > 10) lvl = 10;
  return lvl / 10.0f;
}

// Uniform rnd() produces visible runs (e.g. three oranges in a row). Pick a
// fruit that differs from the last two -> feels varied without being rigged.
static int pick_fruit(void) {
  static int last1 = -1, last2 = -1;
  int idx;
  do { idx = (int)rnd(NUM_DEFS - 1); } while (idx == last1 || idx == last2);  // never the bomb
  last2 = last1; last1 = idx;
  return idx;
}

// Keep objects spawned in the same wave horizontally separated, so a fruit can never
// ride directly behind a bomb for its whole arc (they may still cross paths). Reset per wave.
static float s_wave_x[8];
static int s_wave_xn;
static float pick_spawn_x(void) {
  float x = rnd_range(70, SCREEN_W - 70);
  for (int tries = 0; tries < 10; tries++) {
    bool ok = true;
    for (int i = 0; i < s_wave_xn; i++)
      if (fabsf(x - s_wave_x[i]) < 96.0f) { ok = false; break; }  // lanes >= 96px apart
    if (ok) break;
    x = rnd_range(70, SCREEN_W - 70);
  }
  if (s_wave_xn < 8) s_wave_x[s_wave_xn++] = x;
  return x;
}

static void spawn_fruit_idx(int idx) {
  ent_t *e = alloc_ent();
  if (!e) return;
  const def_t *d = &DEFS[idx];
  float p = diff_progress();  // arcs get a little faster/wider as the game goes on (gentle, smooth)
  e->active = true;
  e->kind = K_FRUIT;
  e->bomb = d->bomb;
  e->defi = idx;
  e->size = d->size;
  e->x = pick_spawn_x();
  e->y = SCREEN_H + d->size;
  e->vx = (rnd_range(0, 100 + (int)(p * 50)) - (50 + (int)(p * 25))) / 26.0f;  // tighter spread -> fruit stay on screen
  e->vy = -(float)rnd_range(20, 23 + (int)(p * 4));
  e->obj = make_sprite(d->whole);
  place(e);
}

static void spawn_half(const lv_image_dsc_t *dsc, int size, float x, float y, float vx, float vy) {
  ent_t *e = alloc_ent();
  if (!e) return;
  e->active = true;
  e->kind = K_HALF;
  e->bomb = false;
  e->size = size;
  e->x = x;
  e->y = y;
  e->vx = vx;
  e->vy = vy;
  e->obj = make_sprite(dsc);
  place(e);
}

static void spawn_juice(float x, float y, uint32_t col, int n) {
  for (int i = 0; i < n; i++) {
    ent_t *e = alloc_ent();
    if (!e) return;
    float a = rnd(628) / 100.0f, sp = rnd_range(8, 16);  // faster spray = reads as a burst, clears quickly
    e->active = true;
    e->kind = K_JUICE;
    e->bomb = false;
    e->size = 16;
    e->x = x;
    e->y = y;
    e->vx = cosf(a) * sp;
    e->vy = sinf(a) * sp - 3.0f;
    e->obj = make_droplet(col);
    place(e);
  }
}

static void update_hearts(void) {
  for (int i = 0; i < 3; i++)
    lv_image_set_src(s_hearts[i], i < s_lives ? &img_heart : &img_heart_empty);
}

static void show_game_over(void) {
  s_state = ST_OVER;
  bool newbest = s_score > s_best;
  if (newbest) s_best = s_score;
  clear_all();
  s_trail_count = 0;
  lv_obj_add_flag(s_blade, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_blade_glow, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text_fmt(s_over_lbl, "%d", s_score);
  lv_label_set_text_fmt(s_best_lbl, "BEST  %d", s_best);
  lv_obj_align(s_over_lbl, LV_ALIGN_TOP_MID, 0, 240);   // re-center for digit count (landscape card)
  lv_obj_align(s_best_lbl, LV_ALIGN_TOP_MID, 0, 316);
  if (newbest) lv_obj_clear_flag(s_newbest, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(s_newbest, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_over_panel);
  lv_obj_clear_flag(s_over_panel, LV_OBJ_FLAG_HIDDEN);
}

static void start_game(void) {
  clear_all();
  s_trail_count = 0;
  saver_hide();
  s_idle_ms = 0;
  s_score = 0;
  s_lives = 3;
  s_swipe_n = 0;
  s_life_milestone = 0;
  s_state = ST_PLAY;
  if (s_spawn_timer) lv_timer_set_period(s_spawn_timer, 800);  // back to easy for a fresh run
  lv_label_set_text(s_score_lbl, "0");
  update_hearts();
  lv_obj_add_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_over_panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_score_lbl, LV_OBJ_FLAG_HIDDEN);
  for (int i = 0; i < 3; i++) lv_obj_clear_flag(s_hearts[i], LV_OBJ_FLAG_HIDDEN);
}

// menu intro: the logo letters (each carrying its own shadow) drop in from above,
// staggered left->right, and land with an overshoot bounce; the accent fruit then
// hop up into place and keep floating gently (the only idle motion on the menu)
static void logo_y_cb(void *var, int32_t v) { lv_obj_set_y((lv_obj_t *)var, v); }
static void fruit_hop_done(lv_anim_t *a) {
  lv_obj_t *o = (lv_obj_t *)a->var;
  int i = 0;
  while (i < MENU_FRUIT_N && s_menu_fruit[i] != o) i++;
  if (i >= MENU_FRUIT_N) return;
  // hand off to the idle drift: slow two-axis float like the screensaver fruit
  // (y and x periods deliberately co-prime-ish so the path never looks scripted)
  lv_anim_t f;
  lv_anim_init(&f);
  lv_anim_set_var(&f, o);
  lv_anim_set_exec_cb(&f, logo_y_cb);
  lv_anim_set_values(&f, menu_fruit_y[i], menu_fruit_y[i] - 12);
  lv_anim_set_duration(&f, 2600 + i * 300);
  lv_anim_set_reverse_duration(&f, 2600 + i * 300);
  lv_anim_set_repeat_count(&f, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&f, lv_anim_path_ease_in_out);
  lv_anim_start(&f);
  lv_anim_set_exec_cb(&f, saver_set_x);
  lv_anim_set_values(&f, menu_fruit_x[i] - 7, menu_fruit_x[i] + 7);
  lv_anim_set_duration(&f, 3400 + i * 370);
  lv_anim_set_reverse_duration(&f, 3400 + i * 370);
  lv_anim_start(&f);
}
static void menu_intro(void) {
  for (int i = 0; i < LOGO_LT_N; i++) {
    if (!s_logo_lt[i]) return;
    lv_obj_set_y(s_logo_lt[i], logo_lt_y[i] - 320);  // park off-screen until its delay is up
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_logo_lt[i]);
    lv_anim_set_exec_cb(&a, logo_y_cb);
    lv_anim_set_values(&a, logo_lt_y[i] - 320, logo_lt_y[i]);
    lv_anim_set_duration(&a, 420);
    lv_anim_set_delay(&a, 40 + i * 55);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_start(&a);
  }
  for (int i = 0; i < MENU_FRUIT_N; i++) {           // fruit wait for the last letter to land
    if (!s_menu_fruit[i]) return;
    lv_anim_delete(s_menu_fruit[i], NULL);           // stop a previous run's idle drift
    lv_obj_set_x(s_menu_fruit[i], menu_fruit_x[i]);
    lv_obj_set_y(s_menu_fruit[i], menu_fruit_y[i] + 26);
    lv_obj_set_style_opa(s_menu_fruit[i], 0, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_menu_fruit[i]);
    lv_anim_set_exec_cb(&a, logo_y_cb);
    lv_anim_set_values(&a, menu_fruit_y[i] + 26, menu_fruit_y[i]);
    lv_anim_set_duration(&a, 300);
    lv_anim_set_delay(&a, 1060 + i * 90);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_set_ready_cb(&a, fruit_hop_done);
    lv_anim_start(&a);
    lv_anim_set_ready_cb(&a, NULL);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
  }
}

// Back to the FRUIT ISLAND menu (from game over): lets you retry the KISS draw without
// having to play a whole round — a stray tap at game over lands here, not in a new game.
static void go_menu(void) {
  clear_all();
  s_trail_count = 0;
  s_state = ST_MENU;
  lv_obj_add_flag(s_over_panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_score_lbl, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_blade, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_blade_glow, LV_OBJ_FLAG_HIDDEN);
  for (int i = 0; i < 3; i++) lv_obj_add_flag(s_hearts[i], LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_menu_panel);
  menu_intro();
}

static void lose_life(void) {
  if (s_lives > 0) s_lives--;
  update_hearts();
  if (s_lives == 0) show_game_over();
}

// quick damped screen shake (whole screen translates; black edges hide the gap)
static void anim_shake_cb(void *var, int32_t t) {
  (void)var;
  float p = t / 1000.0f, amp = 8.0f * (1.0f - p);
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_translate_x(scr, (int)(sinf(p * 31.4f) * amp), 0);
  lv_obj_set_style_translate_y(scr, (int)(cosf(p * 25.1f) * amp * 0.6f), 0);
}
static void anim_shake_done(lv_anim_t *a) {
  (void)a;
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_translate_x(scr, 0, 0);
  lv_obj_set_style_translate_y(scr, 0, 0);
}
static void screen_shake(void) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, lv_screen_active());
  lv_anim_set_exec_cb(&a, anim_shake_cb);
  lv_anim_set_values(&a, 0, 1000);
  lv_anim_set_duration(&a, 280);
  lv_anim_set_ready_cb(&a, anim_shake_done);
  lv_anim_start(&a);
}

static void slice(ent_t *e) {
  if (e->bomb) {
    explosion(e->x, e->y);
    screen_flash(0xFF5A00);
    screen_shake();
    if (e->obj) lv_obj_delete(e->obj);
    e->obj = NULL;
    e->active = false;
    lose_life();  // bomb: real explosion burst + flash + shake, costs a life
    return;
  }
  s_score++;
  s_swipe_n++;
  if (s_swipe_n >= 2) {                       // 2+ fruit in one swipe = combo: bonus point + gold popup
    s_score++;
    char buf[20];
    snprintf(buf, sizeof buf, "COMBO x%d", s_swipe_n);
    score_popup((int)e->x - 36, (int)e->y - 24, buf, 0xFFD23A);
  } else {
    score_popup((int)e->x - 6, (int)e->y - 24, "+1", 0xFFFFFF);
  }
  lv_label_set_text_fmt(s_score_lbl, "%d", s_score);
  if (s_score / 50 > s_life_milestone) {       // every 50 pts: earn a life back (handles combo jumps)
    s_life_milestone = s_score / 50;
    if (s_lives < 3) {
      s_lives++;
      update_hearts();
      life_gain_fx(s_hearts[s_lives - 1]);     // subtle heart pop, not a screen flash
    }
  }
  const def_t *d = &DEFS[e->defi];
  juice_splat(e->x, e->y, d->juice);  // big juicy splash on every slice
  if (d->burst) {
    spawn_juice(e->x, e->y, d->juice, 3);  // cherries/grapes: burst, no halves
  } else {
    spawn_half(d->hl, d->hsize, e->x - 8, e->y, -8.0f, -5.0f);  // halves fly apart fast so they clear quickly
    spawn_half(d->hr, d->hsize, e->x + 8, e->y, 8.0f, -4.5f);
    spawn_juice(e->x, e->y, d->juice, 2);
  }
  if (e->obj) lv_obj_delete(e->obj);
  e->obj = NULL;
  e->active = false;
}

static float seg_dist(float ax, float ay, float bx, float by, float px, float py) {
  float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
  float t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
  t = t < 0 ? 0 : (t > 1 ? 1 : t);
  float ex = px - (ax + t * dx), ey = py - (ay + t * dy);
  return sqrtf(ex * ex + ey * ey);
}

static void check_slices(void) {
  if (s_trail_count < 2) return;
  lv_point_precise_t *a = &s_trail[s_trail_count - 2], *b = &s_trail[s_trail_count - 1];
  for (int i = 0; i < MAX_ENT; i++) {
    ent_t *e = &s_ent[i];
    if (!e->active || e->kind != K_FRUIT) continue;
    if (seg_dist(a->x, a->y, b->x, b->y, e->x, e->y) < e->size / 2 + 4) slice(e);
  }
}

static bool read_touch(int *x, int *y) { return platform_read_touch(x, y); }

static void update_blade(int tx, int ty, bool pressed) {
  if (pressed) {
    if (s_trail_count < TRAIL_LEN) {
      s_trail[s_trail_count].x = tx;
      s_trail[s_trail_count].y = ty;
      s_trail_count++;
    } else {
      for (int i = 1; i < TRAIL_LEN; i++) s_trail[i - 1] = s_trail[i];
      s_trail[TRAIL_LEN - 1].x = tx;
      s_trail[TRAIL_LEN - 1].y = ty;
    }
  } else if (s_trail_count > 0) {
    for (int i = 1; i < s_trail_count; i++) s_trail[i - 1] = s_trail[i];
    s_trail_count--;
  }
  // Drop oldest points while the blade is longer than the cap. Without this, a
  // dropped frame spaces touch samples farther apart -> bigger blade -> slower
  // still: lag feeding on itself. Capping length bounds the worst-case redraw box.
  while (s_trail_count > 2) {
    lv_value_precise_t dx = s_trail[s_trail_count - 1].x - s_trail[0].x;
    lv_value_precise_t dy = s_trail[s_trail_count - 1].y - s_trail[0].y;
    if (dx * dx + dy * dy <= (lv_value_precise_t)BLADE_MAX_SPAN * BLADE_MAX_SPAN) break;
    for (int i = 1; i < s_trail_count; i++) s_trail[i - 1] = s_trail[i];
    s_trail_count--;
  }
  if (s_trail_count >= 2) {
    // lv_line sizes its object as the bbox of its points measured FROM the object's
    // (0,0). Our trail holds absolute screen coords, so the object stretched from the
    // top-left corner to the blade tip -> a huge phantom rect redrawn every frame (the
    // real swipe-lag cause). Fix: offset the points to a local origin and move the
    // object there, so the invalidation is just the tight blade box.
    lv_value_precise_t minx = s_trail[0].x, miny = s_trail[0].y;
    for (int i = 1; i < s_trail_count; i++) {
      if (s_trail[i].x < minx) minx = s_trail[i].x;
      if (s_trail[i].y < miny) miny = s_trail[i].y;
    }
    static lv_point_precise_t local[TRAIL_LEN];
    for (int i = 0; i < s_trail_count; i++) {
      local[i].x = s_trail[i].x - minx;
      local[i].y = s_trail[i].y - miny;
    }
    lv_obj_set_pos(s_blade_glow, (int32_t)minx, (int32_t)miny);
    lv_obj_set_pos(s_blade, (int32_t)minx, (int32_t)miny);
    lv_line_set_points(s_blade_glow, local, s_trail_count);
    lv_line_set_points(s_blade, local, s_trail_count);
    lv_obj_clear_flag(s_blade_glow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_blade, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_blade_glow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_blade, LV_OBJ_FLAG_HIDDEN);
  }
}

// idle attract mode: gorgeous fruit drift up over the sunset backdrop; any touch wakes it.
static void saver_show(void) {
  if (s_saver_on) return;
  s_saver_on = true;
  // backdrop first: hide menu/over, bring the static backdrop up...
  lv_obj_add_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_over_panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_saver, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_saver);
  // ...then float the fruit as children of the SCREEN (scroll-disabled), NOT the backdrop image
  // (animated children on an lv_image were making the whole thing drift up/down).
  lv_obj_t *scr = lv_screen_active();
  for (int i = 0; i < SAVER_N; i++) {
    lv_obj_t *f = lv_image_create(scr);
    lv_image_set_src(f, DEFS[pick_fruit()].whole);  // varied fruit, never the bomb, no triple-repeats
    int bx = rnd_range(24, SCREEN_W - 96);
    lv_obj_set_pos(f, bx, SCREEN_H + 60);
    s_saver_fruit[i] = f;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, f);
    lv_anim_set_exec_cb(&a, saver_set_y);
    lv_anim_set_values(&a, SCREEN_H + 90, -140);            // rise off-bottom to off-top (loops unseen)
    lv_anim_set_duration(&a, rnd_range(7000, 12000));
    lv_anim_set_delay(&a, i * 1100);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, f);
    lv_anim_set_exec_cb(&b, saver_set_x);
    lv_anim_set_values(&b, bx - 26, bx + 26);                // gentle horizontal sway
    lv_anim_set_duration(&b, rnd_range(2600, 4200));
    lv_anim_set_reverse_duration(&b, rnd_range(2600, 4200));
    lv_anim_set_repeat_count(&b, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_in_out);
    lv_anim_start(&b);
  }
  lv_obj_clear_flag(s_saver_hint, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_saver_hint);                      // prompt on top of everything
}

static void saver_hide(void) {
  if (!s_saver_on) return;
  s_saver_on = false;
  for (int i = 0; i < SAVER_N; i++) {
    if (s_saver_fruit[i]) {
      lv_anim_delete(s_saver_fruit[i], NULL);                // stop its float/sway anims first
      lv_obj_delete(s_saver_fruit[i]);
      s_saver_fruit[i] = NULL;
    }
  }
  lv_obj_add_flag(s_saver, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_saver_hint, LV_OBJ_FLAG_HIDDEN);
  // restore the panel for whatever state we returned to
  if (s_state == ST_MENU) { lv_obj_clear_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN); menu_intro(); }
  else if (s_state == ST_OVER) lv_obj_clear_flag(s_over_panel, LV_OBJ_FLAG_HIDDEN);
}

// Secret unlock: recognise a one-stroke "K" — a left vertical spine, an upper-right
// arm, a lower-right arm, and a mid-left waist. Forgiving on size/position; silent.
static bool detect_K(const lv_point_t *p, int n) {
  if (n < 8) return false;
  int minx = p[0].x, maxx = p[0].x, miny = p[0].y, maxy = p[0].y;
  for (int i = 1; i < n; i++) {
    if (p[i].x < minx) minx = p[i].x;
    if (p[i].x > maxx) maxx = p[i].x;
    if (p[i].y < miny) miny = p[i].y;
    if (p[i].y > maxy) maxy = p[i].y;
  }
  int w = maxx - minx, h = maxy - miny;
  if (w < 40 || h < 50) return false;        // needs a bit of size, but easy to meet
  int spine_top = 0, spine_bot = 0;
  bool arm = false;
  for (int i = 0; i < n; i++) {
    float nx = (float)(p[i].x - minx) / w, ny = (float)(p[i].y - miny) / h;
    if (nx < 0.50f) { if (ny < 0.5f) spine_top++; else spine_bot++; }   // a vertical-ish left stroke
    if (nx > 0.45f) arm = true;                                         // ...that reaches to the right
  }
  // Deliberately LENIENT (this is cover, not the lock): a left vertical spine plus anything
  // reaching to the right counts as a "K". Easy to draw; a plain tap or flat swipe still won't match.
  return spine_top >= 1 && spine_bot >= 1 && arm;
}

// The secret unlock is the word "KISS": a wide, multi-stroke drawing whose LEFT letter is a
// K and which extends well to the right. Lenient on the I/S/S shapes (it's cover, not the lock).
static bool detect_KISS(const lv_point_t *p, int n, int strokes) {
  (void)strokes;                                      // reliability first: match on shape, not lift count
  if (n < 12) return false;
  int minx = p[0].x, maxx = p[0].x, miny = p[0].y, maxy = p[0].y;
  for (int i = 1; i < n; i++) {
    if (p[i].x < minx) minx = p[i].x;
    if (p[i].x > maxx) maxx = p[i].x;
    if (p[i].y < miny) miny = p[i].y;
    if (p[i].y > maxy) maxy = p[i].y;
  }
  int w = maxx - minx, h = maxy - miny;
  if (w < 170 || h < 45) return false;                // a wide-ish word
  if (w * 5 < h * 6) return false;                     // width >= ~1.2 * height
  // Require FOUR letter clusters along x (K-I-S-S) so "KIS" (3) won't unlock — both S's must be
  // drawn. Letters are continuous in x; a >=2-bin empty gap marks a letter break. Only the K
  // (leftmost cluster) is shape-checked; I/S/S just need to be there and separated.
  // Bins are filled along same-stroke segments (not just at touch samples): fast strokes leave
  // >1-bin gaps between samples, which used to split one letter into two phantom clusters and
  // let a fast "KIS" count as 4. Pen-lifts still separate letters.
  enum { KB = 40 };
  bool occ[KB];
  for (int b = 0; b < KB; b++) occ[b] = false;
  for (int i = 0; i < n; i++) {
    int b1 = (p[i].x - minx) * KB / (w + 1);
    occ[b1] = true;
    if (i > 0 && s_gid[i] == s_gid[i - 1]) {
      int b0 = (p[i - 1].x - minx) * KB / (w + 1);
      for (int b = b0 < b1 ? b0 : b1; b <= (b0 < b1 ? b1 : b0); b++) occ[b] = true;
    }
  }
  int clusters = 0, gap = 0, k_hi = 0; bool in = false;
  for (int b = 0; b < KB; b++) {
    if (occ[b]) {
      if (!in) { clusters++; in = true; }
      if (clusters == 1) k_hi = b;                     // right edge of the leftmost letter (the K)
      gap = 0;
    } else if (in && ++gap >= 1) {
      // 1-bin gaps are trustworthy letter breaks now that bins are segment-filled
      // (no phantom splits); 2 bins (~w/20 px) was too strict for fat-finger drawing,
      // which blurs the letters together and forced people to draw with a fingernail
      in = false;
    }
  }
  if (clusters < 4) return false;                      // need K, I, S, S — not just "KIS"
  int kcut = minx + (k_hi + 1) * (w + 1) / KB;         // isolate the leftmost letter
  int ln = 0;
  for (int i = 0; i < n; i++)
    if (p[i].x <= kcut && ln < GEST_MAX) s_gsub[ln++] = p[i];
  return detect_K(s_gsub, ln);                         // ...which must be a (lenient) K
}

// center the code + caption on the baked chip frame (566..760 x 39..86 in wallet_mock.py)
static void fp_chip_place(void) {
  lv_obj_update_layout(s_fp_chip);
  lv_obj_set_pos(s_fp_chip, 663 - lv_obj_get_width(s_fp_chip) / 2, 46);
  lv_obj_update_layout(s_fp_cap);
  lv_obj_set_pos(s_fp_cap, 663 - lv_obj_get_width(s_fp_cap) / 2, 92);
}

// unlock hand-off, in two acts on the freshly-revealed home screen:
//   1) DECRYPT: the code appears center-screen as random hex noise and locks in
//      character by character, left to right, with an electric blue flicker.
//   2) GLIDE: the locked code flies into the top-right chip and crossfades into it.
// No transform_scale anywhere (a scaled label forces a draw layer the LVGL pool
// can't hold -> lv_draw_dispatch retries the alloc forever = hard hang); the
// "shrink" is faked by crossfading the mont_48 flier into the mont_28 chip.
static void fly_x_cb(void *v, int32_t x) { lv_obj_set_x((lv_obj_t *)v, x); }
static void fly_y_cb(void *v, int32_t y) { lv_obj_set_y((lv_obj_t *)v, y); }
static void fly_del_cb(lv_anim_t *a) {
  (void)a;
  if (s_fp_fly) { lv_obj_delete_async(s_fp_fly); s_fp_fly = NULL; }
}
static void fp_fly_glide(void) {
  int fw = lv_obj_get_width(s_fp_fly), fh = lv_obj_get_height(s_fp_fly);
  int sx = lv_obj_get_x(s_fp_fly), sy = lv_obj_get_y(s_fp_fly);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_fp_fly);
  lv_anim_set_duration(&a, 560);
  lv_anim_set_delay(&a, 140);                 // a beat to read the locked code
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_set_exec_cb(&a, fly_x_cb);
  lv_anim_set_values(&a, sx, 663 - fw / 2);   // land centered on the chip frame (663,61)
  lv_anim_start(&a);
  lv_anim_set_exec_cb(&a, fly_y_cb);
  lv_anim_set_values(&a, sy, 61 - fh / 2);
  lv_anim_start(&a);
  // last stretch: flier fades out...
  lv_anim_set_exec_cb(&a, anim_opa_cb);
  lv_anim_set_values(&a, 255, 0);
  lv_anim_set_duration(&a, 200);
  lv_anim_set_delay(&a, 140 + 400);
  lv_anim_set_path_cb(&a, lv_anim_path_linear);
  lv_anim_set_ready_cb(&a, fly_del_cb);
  lv_anim_start(&a);
  // ...while the chip + caption fade in underneath it (then hold still: an
  // opacity pulse on the chip read as a shake on the real panel — reverted)
  lv_anim_set_ready_cb(&a, NULL);
  lv_anim_set_var(&a, s_fp_chip);
  lv_anim_set_values(&a, 0, 255);
  lv_anim_set_duration(&a, 240);
  lv_anim_set_delay(&a, 140 + 380);
  lv_anim_start(&a);
  lv_anim_set_var(&a, s_fp_cap);
  lv_anim_start(&a);
}
static lv_timer_t *s_fp_scr_tmr;
static int s_fp_scr_step;
static void fp_scramble_cb(lv_timer_t *t) {
  (void)t;
  if (!s_fp_fly) {                            // flier died under us: stop scrambling
    lv_timer_delete(s_fp_scr_tmr);
    s_fp_scr_tmr = NULL;
    return;
  }
  static const char HEXD[] = "0123456789ABCDEF";
  int resolved = s_fp_scr_step - 4;           // first 4 ticks: pure noise
  if (resolved < 0) resolved = 0;
  if (resolved > 8) resolved = 8;
  char buf[9];
  for (int i = 0; i < 8; i++) buf[i] = (i < resolved) ? s_fp_hex[i] : HEXD[rnd(16)];
  buf[8] = 0;
  lv_label_set_text(s_fp_fly, buf);
  lv_obj_set_style_text_color(s_fp_fly,       // electric flicker while unresolved
      lv_color_hex((s_fp_scr_step & 1) ? 0x9FE8FF : 0xE8EEF7), 0);
  s_fp_scr_step++;
  if (resolved >= 8) {                        // locked: settle to ink and glide away
    lv_timer_delete(s_fp_scr_tmr);
    s_fp_scr_tmr = NULL;
    lv_obj_set_style_text_color(s_fp_fly, lv_color_hex(0xE8EEF7), 0);
    fp_fly_glide();
  }
}
static void fp_fly_start(void) {
  s_fp_fly = lv_label_create(lv_layer_top());
  lv_label_set_text(s_fp_fly, s_fp_hex);      // real code first: size the label off it
  lv_obj_set_style_text_color(s_fp_fly, lv_color_hex(0xE8EEF7), 0);
  lv_obj_set_style_text_font(s_fp_fly, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_letter_space(s_fp_fly, 4, 0);
  lv_obj_update_layout(s_fp_fly);
  // start where the reveal card showed the code (box center 400,163 in wallet_ui.c)
  lv_obj_set_pos(s_fp_fly, 400 - lv_obj_get_width(s_fp_fly) / 2,
                 163 - lv_obj_get_height(s_fp_fly) / 2);
  lv_obj_set_style_opa(s_fp_chip, 0, 0);      // chip appears only when the glide lands
  lv_obj_set_style_opa(s_fp_cap, 0, 0);
  lv_obj_clear_flag(s_fp_chip, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_fp_cap, LV_OBJ_FLAG_HIDDEN);
  s_fp_scr_step = 0;
  if (s_fp_scr_tmr) lv_timer_delete(s_fp_scr_tmr);
  s_fp_scr_tmr = lv_timer_create(fp_scramble_cb, 45, NULL);  // ~540ms decrypt, then glide
}

static void wallet_start(void);
static void setup_done_login(void) {       // wizard stored the seed: first login,
  wallet_login_open_setup(wallet_start);   // passphrase typed twice (safety net)
}

// Settings -> "REPLACE WALLET": run the wizard on demand (not just first boot).
// Completing it overwrites the stored seed; cancelling leaves it untouched.
void wallet_begin_setup(void) {
  wallet_setup_open(lv_screen_active(), setup_done_login);
}

// Sync the home TESTNET badge to the current network. Called on unlock and by
// Settings when it closes, so flipping the network updates the home immediately.
void wallet_home_refresh(void) {
  if (!s_net_lbl) return;
  if (wallet_testnet()) lv_obj_clear_flag(s_net_lbl, LV_OBJ_FLAG_HIDDEN);
  else                  lv_obj_add_flag(s_net_lbl, LV_OBJ_FLAG_HIDDEN);
}

#ifdef SIMULATOR
// sim-only: drive the bottom-center status slot so the SD-insert indicator can
// be previewed (the real poll is device-only — no SDMMC in the sim).
void sim_home_status(const char *msg) {
  if (!s_cam_lbl) return;
  lv_label_set_text(s_cam_lbl, msg);
  lv_obj_set_style_text_color(s_cam_lbl, lv_color_hex(0x35D07F), 0);
}
#endif

// Ambient life while the home idles: dim ink motes rising slowly through the
// grid. Each lane has its own period + fade so the pattern never visibly
// repeats; 4px dots = tiny dirty rects, translate/opa only.
static void motes_start(void) {
  static const int mx[N_MOTES]  = {150, 260, 430, 590, 735};
  static const int mms[N_MOTES] = {9000, 12400, 7600, 10800, 14200};
  for (int i = 0; i < N_MOTES; i++) {
    if (!s_mote[i]) return;
    lv_anim_delete(s_mote[i], NULL);
    lv_obj_set_pos(s_mote[i], mx[i], 488);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_mote[i]);
    lv_anim_set_exec_cb(&a, fly_y_cb);
    lv_anim_set_values(&a, 488, -8);
    lv_anim_set_duration(&a, mms[i]);
    lv_anim_set_delay(&a, i * 900);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_repeat_delay(&a, 500 + i * 400);
    lv_anim_start(&a);
    lv_anim_set_exec_cb(&a, anim_opa_cb);   // independent shimmer on top
    lv_anim_set_values(&a, 25, 100);        // ₿ is bigger than a dot: stay faint
    lv_anim_set_duration(&a, 2600 + i * 500);
    lv_anim_set_reverse_duration(&a, 2600 + i * 500);
    lv_anim_set_delay(&a, 0);
    lv_anim_set_repeat_delay(&a, 0);
    lv_anim_start(&a);
  }
}

static void motes_stop(void) {
  for (int i = 0; i < N_MOTES; i++) {
    if (!s_mote[i]) return;
    lv_anim_delete(s_mote[i], NULL);
    lv_obj_set_style_opa(s_mote[i], 0, 0);
    lv_obj_set_y(s_mote[i], 500);           // parked below the panel
  }
}

static void wallet_start(void) {           // unlocked via login -> reveal the wallet home
  if (s_wallet_on) return;
  s_wallet_on = true;
  {  // the home chip shows the fingerprint of the wallet that was just unlocked
    uint8_t fp[4];
    wallet_ui_last_fp(fp);
    snprintf(s_fp_hex, sizeof(s_fp_hex), "%02X%02X%02X%02X", fp[0], fp[1], fp[2], fp[3]);
    if (s_fp_chip) {
      lv_label_set_text(s_fp_chip, s_fp_hex);
      fp_chip_place();
      lv_obj_add_flag(s_fp_chip, LV_OBJ_FLAG_HIDDEN);  // revealed when the flight lands
      lv_obj_add_flag(s_fp_cap, LV_OBJ_FLAG_HIDDEN);
      fp_fly_start();
    }
  }
  saver_hide();
  lv_obj_add_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_over_panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_wallet, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_wallet);
  motes_start();                           // ambient idle drift
  wallet_home_refresh();                   // show/hide the TESTNET badge for this session
  s_wallet_act_t = lv_tick_get();          // fresh idle clock for this session
}

static void wallet_lock(void) {            // back to the game cover (tap the KISS logo)
  if (!s_wallet_on) return;
#ifndef SIMULATOR
  if (camera_spike_is_on()) {              // never leave the camera running behind the game
    camera_spike_toggle(s_wallet, s_i2c_bus);
    if (s_cam_lbl) lv_label_set_text(s_cam_lbl, camera_spike_status());
  }
#endif
  s_wallet_on = false;
  wallet_session_close();                  // locked: no key material stays in RAM
  motes_stop();
  lv_obj_add_flag(s_wallet, LV_OBJ_FLAG_HIDDEN);
  s_state = ST_MENU;
  lv_obj_clear_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN);
  s_gest_swallow = true;                   // ignore the rest of this tap so we land on the menu
  menu_intro();
}

// wallet_settings.c: seed already wiped + session closed; just drop to the game.
void wallet_wiped_lock(void) { wallet_lock(); }

// ---- idle auto-lock: an unlocked signer must not sit open forever ----
#define WALLET_AUTOLOCK_MS 120000    // 2 min without a touch -> lock to the game

static void game_tick(lv_timer_t *t) {
  (void)t;
  int tx = 0, ty = 0;
  bool pressed = read_touch(&tx, &ty);

  if (wallet_ui_active() || wallet_setup_active()) {   // login/wizard own the touch
    s_prev_press = pressed;          // (LVGL indev); the game must not also see it
    return;                          // (and are exempt from auto-lock: writing the
  }                                  //  backup words down takes minutes, untouched)

  if (s_wallet_on) {                              // in the wallet: tap the KISS logo to lock
    if (pressed) s_wallet_act_t = lv_tick_get();  // any touch anywhere resets the clock
    else if (lv_tick_elaps(s_wallet_act_t) > WALLET_AUTOLOCK_MS) {
      if (wallet_scan_active())     wallet_scan_close();      // camera off first
      if (wallet_sign_active())     wallet_sign_close();      // drops any loaded PSBT
      if (wallet_recv_active())     wallet_recv_close();
      if (wallet_settings_active()) wallet_settings_close();
      wallet_lock();                              // session key leaves RAM
      s_prev_press = pressed;
      return;
    }
    if (wallet_recv_active() || wallet_sign_active() || wallet_scan_active() ||
        wallet_settings_active()) {
      s_prev_press = pressed;            // wallet sub-screens own the touch (LVGL buttons)
      return;
    }
#ifndef SIMULATOR
    // camera spike (step 2): the Sign tile opens the live view (Sign = scan a QR
    // later, so the camera belongs here). While live: top-left corner CLOSES the
    // camera (wallet-lock is suspended so a stray gesture can't dump you to the
    // game), top-right corner cycles the orientation finder, vertical drag on
    // either screen edge zooms, and taps elsewhere do nothing.
    static int s_zoom_anchor; static bool s_zoom_drag;
    if (camera_spike_check_died()) {                          // stream errored out:
      lv_obj_invalidate(lv_screen_active());                  // repaint the wallet UI
      if (s_cam_lbl) lv_label_set_text(s_cam_lbl, camera_spike_status());
    }
    bool cam_on = camera_spike_is_on();
    // hot-plug SD indicator: while idle on the home screen, poll for a card and
    // briefly flash "SD card ready" in the bottom-center slot on INSERT (a short
    // toast, not an always-on label). Cheap when a card is mounted; a mount
    // attempt (only when none is present) can briefly block, but the home art is
    // static so a hitch never shows.
    static int s_sd_tick; static bool s_sd_present; static int s_sd_toast;
    if (!cam_on && ++s_sd_tick >= 90) {              // poll ~1.5s at TICK_MS
      s_sd_tick = 0;
      bool present = platform_sd_probe() != 0;
      if (present && !s_sd_present && s_cam_lbl) {   // just inserted: show the toast
        lv_label_set_text(s_cam_lbl, "SD card ready");
        lv_obj_set_style_text_color(s_cam_lbl, lv_color_hex(0x35D07F), 0);
        s_sd_toast = 3;                              // ~3 polls (~4.5s) then fade
      } else if (s_sd_toast > 0 && --s_sd_toast == 0 && s_cam_lbl) {
        lv_label_set_text(s_cam_lbl, "");
        lv_obj_set_style_text_color(s_cam_lbl, lv_color_hex(0x7A869C), 0);  // reset: no green leak
      }
      s_sd_present = present;
    }
    if (!cam_on && pressed && !s_prev_press && tx < 200 && ty < 110) {
      wallet_lock();
    } else if (cam_on) {
      bool zoom_zone = (tx >= 680 || tx <= 120) && ty > 120;
      if (pressed && (s_zoom_drag || zoom_zone)) {            // edge drag = zoom
        if (!s_prev_press) { s_zoom_anchor = ty; s_zoom_drag = true; }
        else if (s_zoom_drag) {
          while (s_zoom_anchor - ty >= 60) { camera_spike_zoom(+1); s_zoom_anchor -= 60; }
          while (ty - s_zoom_anchor >= 60) { camera_spike_zoom(-1); s_zoom_anchor += 60; }
        }
      } else if (pressed && !s_prev_press && ty < 110) {
        if (tx < 200) {                                       // top-left: close camera
          camera_spike_toggle(s_wallet, s_i2c_bus);
          if (s_cam_lbl) lv_label_set_text(s_cam_lbl, "");    // don't leave dev status on home
        } else if (tx >= 600) {                               // top-right: orientation
          camera_spike_cycle_orientation();
        }
      }
    } else if (pressed && !s_prev_press &&
               tx >= 40 && tx <= 220 && ty >= 140 && ty <= 340) {  // Sign tile
      s_tile_pend = 1;
    } else if (pressed && !s_prev_press &&
               tx >= 230 && tx <= 390 && ty >= 140 && ty <= 340) { // Receive tile
      s_tile_pend = 2;
    } else if (pressed && !s_prev_press &&
               tx >= 410 && tx <= 570 && ty >= 140 && ty <= 340) { // Wallet tile: export
      s_tile_pend = 3;
    } else if (pressed && !s_prev_press &&
               tx >= 590 && tx <= 750 && ty >= 140 && ty <= 340) { // Settings tile
      s_tile_pend = 4;
    } else if (!pressed && s_prev_press && s_tile_pend) {          // finger lifted: open
      int t = s_tile_pend;
      s_tile_pend = 0;
      if (t == 1) wallet_sign_open(lv_screen_active());
      else if (t == 2) wallet_recv_open(lv_screen_active());
      else if (t == 3) wallet_export_open(lv_screen_active());
      else wallet_settings_open(lv_screen_active());
    }
    if (!pressed) s_zoom_drag = false;
#else
    if (pressed && !s_prev_press && tx < 200 && ty < 110) wallet_lock();
    else if (pressed && !s_prev_press && tx >= 40 && tx <= 220 && ty >= 140 && ty <= 340)
      s_tile_pend = 1;
    else if (pressed && !s_prev_press && tx >= 230 && tx <= 390 && ty >= 140 && ty <= 340)
      s_tile_pend = 2;
    else if (pressed && !s_prev_press && tx >= 410 && tx <= 570 && ty >= 140 && ty <= 340)
      s_tile_pend = 3;
    else if (pressed && !s_prev_press && tx >= 590 && tx <= 750 && ty >= 140 && ty <= 340)
      s_tile_pend = 4;
    else if (!pressed && s_prev_press && s_tile_pend) {
      int t = s_tile_pend;
      s_tile_pend = 0;
      if (t == 1) wallet_sign_open(lv_screen_active());
      else if (t == 2) wallet_recv_open(lv_screen_active());
      else if (t == 3) wallet_export_open(lv_screen_active());
      else wallet_settings_open(lv_screen_active());
    }
#endif
    s_prev_press = pressed;
    return;
  }

  if (s_state != ST_PLAY) {
    if (pressed) {
      if (s_saver_on) { saver_hide(); s_gest_swallow = true; s_gn = 0; s_strokes = 0; }  // wake saver
      else if (!s_gest_swallow) {
        if (!s_prev_press) {                            // a new stroke begins
          if (s_gn > 0) {                               // drop a stale prior attempt if this stroke
            int mx = -9999;                             // starts well LEFT of how far right we'd
            for (int i = 0; i < s_gn; i++)              // reached: KISS is drawn L->R, so only a
              if (s_gpt[i].x > mx) mx = s_gpt[i].x;     // RESTART (a fresh K) begins far to the left.
            if (tx < mx - 160) { s_gn = 0; s_strokes = 0; }  // -> no points bleeding between tries
          }
          s_stroke_n0 = s_gn; s_strokes++;
        }
        if (s_gn < GEST_MAX) { s_gpt[s_gn].x = tx; s_gpt[s_gn].y = ty;
                               s_gid[s_gn] = (uint8_t)s_strokes; s_gn++; }
      }
      s_gest_idle = 0; s_idle_ms = 0;
    } else {
      if (s_prev_press) {                                    // a touch just lifted
        if (s_gest_swallow) { s_gest_swallow = false; s_gn = 0; s_strokes = 0; }
        else {
          int x0 = 9999, x1 = -9999, y0 = 9999, y1 = -9999;  // bbox of THIS stroke
          for (int i = s_stroke_n0; i < s_gn; i++) {
            if (s_gpt[i].x < x0) x0 = s_gpt[i].x;
            if (s_gpt[i].x > x1) x1 = s_gpt[i].x;
            if (s_gpt[i].y < y0) y0 = s_gpt[i].y;
            if (s_gpt[i].y > y1) y1 = s_gpt[i].y;
          }
          bool tap = (s_stroke_n0 == 0 && x1 - x0 < 22 && y1 - y0 < 22);
          if (tap && s_state == ST_OVER) {
            // baked buttons (coords from gameover_mock.py, padded): PLAY AGAIN
            // restarts; the MENU pill -- and any stray tap -- returns to the menu
            if (x1 >= 220 && x1 <= 580 && y1 >= 356 && y1 <= 458) start_game();
            else go_menu();
            s_gn = 0; s_strokes = 0;
          } else if (tap) {
            start_game(); s_gn = 0; s_strokes = 0;            // menu: a tap -> play
          } else if (detect_KISS(s_gpt, s_gn, s_strokes)) {
            // "KISS" -> passphrase login; a device with no seed yet gets the
            // setup wizard first, then the type-twice login
            if (wallet_seed_exists()) wallet_login_open(wallet_start);
            else wallet_setup_open(lv_screen_active(), setup_done_login);
            s_gn = 0; s_strokes = 0;
          }                                                  // else: keep, await more strokes (3s clears)
        }
        s_gest_idle = 0;
      } else if (s_gn > 0) {                                 // mid-draw, finger up
        s_gest_idle += TICK_MS;
        if (s_gest_idle >= 3000) { s_gn = 0; s_strokes = 0; } // gave up -> clear (never starts game)
      } else {
        s_idle_ms += TICK_MS;
        if (s_idle_ms >= IDLE_MS) saver_show();              // idle -> attract mode
      }
    }
    s_prev_press = pressed;
    return;
  }
  s_idle_ms = 0;
  s_prev_press = pressed;
  if (!pressed) s_swipe_n = 0;   // finger lifted -> combo chain ends

  update_blade(tx, ty, pressed);
  check_slices();

  for (int i = 0; i < MAX_ENT; i++) {
    ent_t *e = &s_ent[i];
    if (!e->active) continue;
    e->vy += (e->kind == K_FRUIT) ? GRAVITY : GRAVITY * 1.7f;  // debris falls faster -> clears the play area sooner
    e->x += e->vx;
    e->y += e->vy;
    place(e);
    bool gone = e->y > SCREEN_H + e->size + 8;
    // debris (halves/juice) is also culled off the sides/top so it never lingers in front of new fruit
    if (e->kind != K_FRUIT)
      gone = gone || e->x < -e->size - 8 || e->x > SCREEN_W + e->size + 8 || e->y < -e->size - 40;
    if (gone) {
      if (e->kind == K_FRUIT && !e->bomb && e->y > SCREEN_H) lose_life();  // a real fruit fell past the bottom
      if (e->obj) lv_obj_delete(e->obj);
      e->obj = NULL;
      e->active = false;
    }
  }
}

static void spawn_tick(lv_timer_t *t) {
  if (s_state != ST_PLAY) return;
  s_wave_xn = 0;                                           // fresh set of separated lanes for this wave
  float p = diff_progress();                               // 0 -> 1 smoothly over ~3.5 min

  lv_timer_set_period(t, (int)(820 - 200 * p));            // waves tighten gradually 820 -> 620ms

  // Always one fruit to slice; the chance of a 2nd ramps in smoothly (0 -> ~28%) -> never a sudden swarm.
  spawn_fruit_idx(pick_fruit());
  if ((int)rnd(100) < (int)(p * 28))
    spawn_fruit_idx(pick_fruit());

  // Bombs are THE escalating difficulty lever, ramping smoothly 6% -> 34%.
  if ((int)rnd(100) < (int)(6 + 28 * p))
    spawn_fruit_idx(BOMB_IDX);
}

void build_game(void) {  // non-static: the simulator harness calls this too
  wallet_settings_load();              // persisted network choice (NVS on device)
  lv_obj_t *scr = lv_screen_active();
  // solid black: fruit pops hardest on it, and no gradient to re-render each frame
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  // fruit/popups animate past the edges; stop LVGL auto-scrolling the screen to chase them
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

  // ---- HUD (hidden until play) ----
  s_score_lbl = lv_label_create(scr);
  lv_label_set_text(s_score_lbl, "0");
  lv_obj_set_style_text_color(s_score_lbl, lv_color_hex(0xF6D157), LV_PART_MAIN);  // gold
  lv_obj_set_style_text_font(s_score_lbl, &lv_font_montserrat_40, LV_PART_MAIN);
  lv_obj_align(s_score_lbl, LV_ALIGN_TOP_LEFT, 22, 40);  // below top overscan, level with hearts
  lv_obj_add_flag(s_score_lbl, LV_OBJ_FLAG_HIDDEN);

  for (int i = 0; i < 3; i++) {
    s_hearts[i] = lv_image_create(scr);
    lv_image_set_src(s_hearts[i], &img_heart);
    lv_obj_align(s_hearts[i], LV_ALIGN_TOP_RIGHT, -64 - (2 - i) * 44, 50);  // padded sprite + below overscan band
    lv_obj_add_flag(s_hearts[i], LV_OBJ_FLAG_HIDDEN);
  }

  // blade = soft blue glow underneath + bright white core on top (Fruit-Ninja style)
  s_blade_glow = lv_line_create(scr);
  lv_obj_set_style_line_color(s_blade_glow, lv_color_hex(0x8CC8FF), LV_PART_MAIN);
  lv_obj_set_style_line_width(s_blade_glow, 12, LV_PART_MAIN);  // soft Fruit-Ninja glow (now cheap: tight blade bbox)
  lv_obj_set_style_line_rounded(s_blade_glow, true, LV_PART_MAIN);
  lv_obj_set_style_line_opa(s_blade_glow, 90, LV_PART_MAIN);
  lv_obj_add_flag(s_blade_glow, LV_OBJ_FLAG_HIDDEN);

  s_blade = lv_line_create(scr);
  lv_obj_set_style_line_color(s_blade, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_line_width(s_blade, 5, LV_PART_MAIN);
  lv_obj_set_style_line_rounded(s_blade, true, LV_PART_MAIN);
  lv_obj_add_flag(s_blade, LV_OBJ_FLAG_HIDDEN);

  // ---- main menu: baked backdrop (with logo shadows) + live logo letters on top ----
  s_menu_panel = lv_image_create(scr);
  lv_image_set_src(s_menu_panel, &img_menu);
  lv_obj_set_pos(s_menu_panel, 0, 0);
  for (int i = 0; i < LOGO_LT_N; i++) {
    s_logo_lt[i] = lv_image_create(s_menu_panel);
    lv_image_set_src(s_logo_lt[i], &img_logo_lt[i]);
    lv_obj_set_pos(s_logo_lt[i], logo_lt_x[i], logo_lt_y[i]);
  }
  for (int i = 0; i < MENU_FRUIT_N; i++) {
    s_menu_fruit[i] = lv_image_create(s_menu_panel);
    lv_image_set_src(s_menu_fruit[i], &img_menu_fruit[i]);
    lv_obj_set_pos(s_menu_fruit[i], menu_fruit_x[i], menu_fruit_y[i]);
  }
  menu_intro();  // boot lands on the menu: play the drop-in right away

  // ---- game over: baked artwork image + overlaid dynamic numbers ----
  s_over_panel = lv_image_create(scr);
  lv_image_set_src(s_over_panel, &img_gameover);
  lv_obj_set_pos(s_over_panel, 0, 0);
  lv_obj_add_flag(s_over_panel, LV_OBJ_FLAG_HIDDEN);

  s_over_lbl = lv_label_create(s_over_panel);  // big score number (on the card)
  lv_label_set_text(s_over_lbl, "0");
  lv_obj_set_style_text_color(s_over_lbl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(s_over_lbl, &lv_font_montserrat_48, 0);
  lv_obj_align(s_over_lbl, LV_ALIGN_TOP_MID, 0, 240);

  s_best_lbl = lv_label_create(s_over_panel);  // BEST n
  lv_label_set_text(s_best_lbl, "BEST  0");
  lv_obj_set_style_text_color(s_best_lbl, lv_color_hex(0xECC878), 0);
  lv_obj_set_style_text_font(s_best_lbl, &lv_font_montserrat_28, 0);
  lv_obj_align(s_best_lbl, LV_ALIGN_TOP_MID, 0, 316);

  s_newbest = lv_image_create(s_over_panel);  // NEW BEST! ribbon (shown when beaten)
  lv_image_set_src(s_newbest, &img_newbest);
  lv_obj_align(s_newbest, LV_ALIGN_TOP_MID, 0, 128);
  lv_obj_add_flag(s_newbest, LV_OBJ_FLAG_HIDDEN);


  // ---- hidden KISS wallet menu (baked) — revealed only by the "K" unlock gesture ----
  s_wallet = lv_image_create(scr);
  lv_image_set_src(s_wallet, &img_wallet);
  lv_obj_set_pos(s_wallet, 0, 0);
  lv_obj_add_flag(s_wallet, LV_OBJ_FLAG_HIDDEN);

  // Fingerprint chip (top-right) — the baked art leaves this area BLANK (dynamic
  // content); live labels own it. Coords from assets/generators/wallet_mock.py.
  // Step-1 proof: shows the boot-selftest fingerprint of the dev seed.
  s_fp_chip = lv_label_create(s_wallet);
  lv_label_set_text(s_fp_chip, s_fp_hex);
  lv_obj_set_style_text_color(s_fp_chip, lv_color_hex(0xE8EEF7), 0);  // Mono theme accent
  lv_obj_set_style_text_font(s_fp_chip, &lv_font_montserrat_28, 0);   // fills the chip frame
  lv_obj_set_style_text_letter_space(s_fp_chip, 2, 0);

  s_fp_cap = lv_label_create(s_wallet);
  lv_label_set_text(s_fp_cap, "fingerprint");
  lv_obj_set_style_text_color(s_fp_cap, lv_color_hex(0x7A869C), 0);   // muted, like the mock
  lv_obj_set_style_text_font(s_fp_cap, &lv_font_montserrat_14, 0);
  fp_chip_place();

  s_cam_lbl = lv_label_create(s_wallet);         // bottom-center status/error slot: blank
  lv_label_set_text(s_cam_lbl, "");              // until something (camera/error) fills it.
  lv_obj_set_style_text_color(s_cam_lbl, lv_color_hex(0x7A869C), 0);  // baked art leaves this
  lv_obj_set_style_text_font(s_cam_lbl, &lv_font_montserrat_14, 0);   // bottom gap free
  lv_obj_align(s_cam_lbl, LV_ALIGN_BOTTOM_MID, 0, -14);

  // TESTNET badge — top-center, between the baked "KISS" logo (left) and the
  // fingerprint chip (right). Amber pill, shown ONLY on testnet so mainnet stays
  // clean; kept in sync by wallet_home_refresh() (unlock + return from Settings).
  s_net_lbl = lv_label_create(s_wallet);
  lv_label_set_text(s_net_lbl, "TESTNET");
  lv_obj_set_style_text_color(s_net_lbl, lv_color_hex(0xF2B84B), 0);
  lv_obj_set_style_text_font(s_net_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(s_net_lbl, 3, 0);
  lv_obj_set_style_bg_color(s_net_lbl, lv_color_hex(0x2A2113), 0);    // dark amber, reads on grid
  lv_obj_set_style_bg_opa(s_net_lbl, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(s_net_lbl, lv_color_hex(0xF2B84B), 0);
  lv_obj_set_style_border_width(s_net_lbl, 1, 0);
  lv_obj_set_style_radius(s_net_lbl, 14, 0);
  lv_obj_set_style_pad_hor(s_net_lbl, 16, 0);
  lv_obj_set_style_pad_ver(s_net_lbl, 6, 0);
  lv_obj_align(s_net_lbl, LV_ALIGN_TOP_MID, 0, 48);
  lv_obj_add_flag(s_net_lbl, LV_OBJ_FLAG_HIDDEN);

  // Build identity, bottom-left — the baked art used to carry a permanent
  // CAUTION pill here (a status light that never changed = dead chrome); now
  // this corner tells the truth instead, same line as the Settings footer.
  wallet_build_id_make(s_wallet, 48, 424);

  // idle motes: small ₿ glyphs (started/stopped with the session)
  for (int i = 0; i < N_MOTES; i++) {
    s_mote[i] = lv_image_create(s_wallet);
    lv_image_set_src(s_mote[i], &img_mote_btc);
    lv_obj_set_style_opa(s_mote[i], 0, 0);
    lv_obj_set_pos(s_mote[i], 0, 500);
  }

  // ---- idle screensaver (attract mode): baked sunset backdrop + pulsing prompt ----
  // Floating fruit are created on activation; this just sets up the backdrop + "tap to play".
  s_saver = lv_image_create(scr);
  lv_image_set_src(s_saver, &img_saver);
  lv_obj_set_pos(s_saver, 0, 0);
  lv_obj_clear_flag(s_saver, LV_OBJ_FLAG_SCROLLABLE);  // floating fruit go off-edge; don't let it scroll
  lv_obj_set_scrollbar_mode(s_saver, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_flag(s_saver, LV_OBJ_FLAG_HIDDEN);

  s_saver_hint = lv_label_create(scr);   // direct child of the screen (shown/hidden with the saver)
  lv_label_set_text(s_saver_hint, "tap to play");
  lv_obj_set_style_text_color(s_saver_hint, lv_color_hex(0xFFF2CD), 0);
  lv_obj_set_style_text_font(s_saver_hint, &lv_font_montserrat_28, 0);
  lv_obj_set_style_bg_color(s_saver_hint, lv_color_hex(0x10131C), 0);
  lv_obj_set_style_bg_opa(s_saver_hint, 110, 0);            // subtle dark pill so it reads on any backdrop
  lv_obj_set_style_pad_hor(s_saver_hint, 24, 0);
  lv_obj_set_style_pad_ver(s_saver_hint, 11, 0);
  lv_obj_set_style_radius(s_saver_hint, 20, 0);
  lv_obj_align(s_saver_hint, LV_ALIGN_BOTTOM_MID, 0, -64);
  lv_obj_add_flag(s_saver_hint, LV_OBJ_FLAG_HIDDEN);
  lv_anim_t hp;
  lv_anim_init(&hp);
  lv_anim_set_var(&hp, s_saver_hint);
  lv_anim_set_exec_cb(&hp, anim_opa_cb);
  lv_anim_set_values(&hp, 150, 255);   // floor kept high so the prompt always stays readable
  lv_anim_set_duration(&hp, 950);
  lv_anim_set_reverse_duration(&hp, 950);
  lv_anim_set_repeat_count(&hp, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&hp);

  lv_timer_create(game_tick, TICK_MS, NULL);
  s_spawn_timer = lv_timer_create(spawn_tick, 800, NULL);
}

#ifndef SIMULATOR
// device touch: read the GT911 controller
bool platform_read_touch(int *x, int *y) {
  if (!s_touch) return false;
  esp_lcd_touch_read_data(s_touch);
  uint16_t tx[1], ty[1];
  uint8_t cnt = 0;
  bool pressed = esp_lcd_touch_get_coordinates(s_touch, tx, ty, NULL, &cnt, 1);
  if (pressed && cnt > 0) {
    // raw GT911 is portrait (x:0..479, y:0..799); map to the logical 800x480 landscape.
    // Must match the 90deg mapping in rot_flush. Flip if it feels mirrored.
    *x = ty[0];
    *y = (LCD_H_RES - 1) - tx[0];
    return true;
  }
  return false;
}

void app_main(void) {
  log_board_info();
#ifndef KISS_RELEASE
  {  // step 1 of the wallet build order: prove the crypto stack (libwally)
    uint8_t fp[4];
    int rc = wallet_selftest(fp);
    ESP_LOGI(TAG, "wallet crypto selftest: %s (stage %d) fingerprint %02X%02X%02X%02X",
             rc == 0 ? "PASS" : "FAIL", rc, fp[0], fp[1], fp[2], fp[3]);
    if (rc == 0)
      snprintf(s_fp_hex, sizeof(s_fp_hex), "%02X%02X%02X%02X",
               fp[0], fp[1], fp[2], fp[3]);
    else
      snprintf(s_fp_hex, sizeof(s_fp_hex), "FAIL %d", rc);
  }
#else
  wallet_selftest(NULL);   // release: just wally_init; the chip stays blank
#endif
  display_start();
  backlight_on();
  touch_start();
  build_game();
  ESP_LOGI(TAG, "fruit game running (landscape, manual rotated flush)");
  while (1) {           // single-threaded LVGL loop (we own the display + flush)
    uint32_t next = lv_timer_handler();
    if (next > 20) next = 20;
    vTaskDelay(pdMS_TO_TICKS(next < 4 ? 4 : next));
  }
}
#endif  // !SIMULATOR
