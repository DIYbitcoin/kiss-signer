#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifndef SIMULATOR  // ESP-only hardware bring-up; the desktop simulator provides its own platform
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
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#include "lvgl.h"
#include "sprites.h"
#include "kiss_art.h"
#include "menu_img.h"
#include "menu_logo.h"
#include "gameover_img.h"
#include "game_bg.h"
#include "kiss_img.h"
#include "tile_lbls.h"   // TILE_LBL_Y (strips replaced by live i18n labels)
#include "i18n.h"
#include "kiss_ui.h"
#include "kiss_usage.h"   // has a coordinator ever spoken: the home's next step
#include "kiss_recv.h"
#include "kiss_sign.h"
#include "kiss_scan.h"
#include "kiss_settings.h"
#include "kiss_fw_ui.h"
#include "kiss_fw.h"   // kiss_fw_mark_valid: release the previous slot
#include "kiss_info.h"
#include "kiss_setup.h"
#include "kiss_seed.h"
#include "kiss_crypto.h"
#include "kiss_theme.h"
#include "kiss_panel.h"
#include "kiss_duress.h"
#include "kiss_gword.h"
#include "kiss_coverword.h"
#include "kiss_duress_ui.h"
#include "kiss_word_ui.h"
#include "kiss_rngaudit.h"
// platform_sd.c is compiled in BOTH builds (host dir vs SDMMC), and the home
// SD-storage badge probes it outside any device-only block, so its header is
// platform-agnostic here. kiss_crypto/camera_spike stay device-only: they
// pull in ESP headers the sim cannot compile.
#include "platform_sd.h"
#ifndef SIMULATOR
#include "kiss_crypto.h"
#include "camera_spike.h"
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
  int points;
  int8_t lift;     // added to launch speed. Negative = heavy, low arc.
  int8_t spin;     // max |deg/frame|
} def_t;

// A watermelon is a heavy slow three points and a cherry is a fast tumbling
// one. Sizes are untouched: make_sprite takes the pre-sized fast path on
// purpose, so real size variety means re-baking art. Character comes out of
// launch speed, spin rate and point value instead, which is free. The bomb
// gets a NEGATIVE lift deliberately -- a slower arc is a readable arc, which
// is the fix for bombs at high spawn rates being noise rather than threat.
static const def_t DEFS[] = {
    {&img_watermelon, &img_watermelon_half, &img_watermelon_halfr, 104, 104, false, false, 0xF0364C, 3, -3, 3},
    {&img_apple,      &img_apple_half,      &img_apple_halfr,       94,  94, false, false, 0xF3E8C6, 1,  0, 5},
    {&img_orange,     &img_orange_half,     &img_orange_halfr,      94,  94, false, false, 0xFF9E1B, 1,  0, 5},
    {&img_pineapple,  &img_pineapple_half,  &img_pineapple_halfr,  112, 112, false, false, 0xFFD23A, 2, -2, 4},
    {&img_strawberry, NULL, NULL, 100, 0, false, true,  0xFF466E, 1,  1, 7},
    {&img_cherries,   NULL, NULL,  90, 0, false, true,  0xE01F2A, 1,  2, 8},
    {&img_grapes,     NULL, NULL,  94, 0, false, true,  0x9C4DCC, 1,  1, 6},
    {&img_bomb,       NULL, NULL,  92, 0, true,  false, 0,        0, -2, 2},
};
#define NUM_DEFS (sizeof(DEFS) / sizeof(DEFS[0]))
#define BOMB_IDX (NUM_DEFS - 1)

typedef enum { K_NONE, K_FRUIT, K_HALF, K_JUICE } kind_t;
typedef struct {
  bool active;
  kind_t kind;
  bool bomb;
  bool gold;           // the rare frenzy fruit
  int defi;
  float x, y, vx, vy;
  float rot, av;       // degrees, and degrees per frame
  int size;
  int16_t rot_q;       // last angle actually pushed to LVGL
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
static bool s_menu_idle_drift;     // ...forever, unless menu_idle_drift_stop ends it
static lv_obj_t *s_over_panel, *s_over_lbl, *s_best_lbl, *s_newbest;
static int s_score, s_best, s_lives = 3;
// Combo is a TIME window, not a touch. Resetting only on finger-lift meant
// a fast player holding one long stroke accumulated a combo across four
// seconds of unrelated fruit, and a player who lifted between two fruit
// they clearly cut together got nothing.
#define COMBO_MS 350
static int s_combo_n;
static uint32_t s_combo_t;
static float s_combo_x, s_combo_y;
static const int COMBO_BONUS[] = {0, 0, 1, 3, 6, 10, 15, 21};
#define COMBO_MAX ((int)(sizeof COMBO_BONUS / sizeof COMBO_BONUS[0]) - 1)

// The rare gold fruit buys a few seconds of dense, bomb-free fruit. Largest
// raise to the game's ceiling for the least code, and it needs no new art.
#define FRENZY_MS 3200
static uint32_t s_frenzy_ms;
// Waves of deliberate quiet still owed after a big throw. Declared up here
// rather than beside the spawner that uses it, because start_game clears it
// and start_game is a thousand lines above.
static int s_wave_rest;
static int s_life_milestone; // highest 50-pt mark a bonus life was granted for
enum { ST_MENU, ST_PLAY, ST_OVER };
static int s_state = ST_MENU;
static bool s_prev_press;
// Home tiles arm on press but OPEN on release: opening under a
// still-pressed finger lets the release "click through" onto whatever LVGL
// button the new screen put beneath it (the sim caught this on the Sign
// chooser — the finger sat exactly on a chooser pill).
static int s_tile_pend;   // 0 none, 1 Sign, 2 Receive, 3 Keys/export
static bool s_fp_pend;    // fingerprint chip pressed; opens the card on release (so
                          // the live LVGL indev binds this press to home, not the
                          // full-screen overlay we are about to create)

static lv_timer_t *s_spawn_timer;  // handle so start_game can reset the difficulty ramp

// ---- hidden KISS Signer: revealed by drawing a "K" on the game menu (cover -> signer) ----
static lv_obj_t *s_home;         // baked KISS Signer menu (visual shell only, for now)
#define N_MOTES 5
static lv_obj_t *s_mote[N_MOTES];  // ambient idle life: dim dots drifting up
static lv_obj_t *s_tile_ttl[4];            // live tile labels (settle in on unlock)
static lv_obj_t *s_next_lbl;               // the one step this signer has not taken
// One number, two placements: built here and re-aligned after every text
// change, because the label is content sized and a translation of a different
// width would otherwise stay centred on the old one.
#define HOME_NEXT_Y 346
// tile title string ids, in tile order (sign, receive, keys, settings).
// STR_H_TILE_WALLET is a legacy KEY NAME whose value has been "Keys" for a
// while; renaming the key would touch all 21 locale files for nothing.
static const int TILE_TTL_STR[4] = {STR_H_TILE_SIGN, STR_H_TILE_RECV,
                                    STR_H_TILE_WALLET, STR_H_TILE_SETTINGS};
static bool s_home_on;
// dev-seed fingerprint for the top-right chip; filled from the boot selftest on
// device (sim build has no libwally, keeps the placeholder)
static char s_fp_hex[12] = "--------";
static lv_obj_t *s_fp_chip;              // home fingerprint chip (updated at unlock)
static lv_obj_t *s_card_frame[4];        // live accent chrome over the baked skeleton:
static lv_obj_t *s_corner[4];            // the theme recolors these instantly, no re-bake
static lv_obj_t *s_underline, *s_chip_frame;
static lv_obj_t *s_theme_dot, *s_theme_lbl, *s_theme_cap;
// The test-network mark: a breathing amber dot beside a themed word,
// the same shape the theme readout in the opposite corner already wears.
static lv_obj_t *s_net_dot;
static lv_obj_t *s_fp_cap;               // "fingerprint" caption under the chip frame
static lv_obj_t *s_fp_card;              // tap the chip -> what-this-number-means card
static lv_obj_t *s_fp_fly;               // transient: the code flying from the reveal card
static lv_obj_t *s_cam_lbl;              // bottom-center status/error slot
static lv_obj_t *s_sd_badge;             // home: SD-storage indicator (SD mode only)
static bool s_sd_badge_live;             // SD mode + card in: game_tick breathes it
static lv_obj_t *s_net_lbl;              // top-center test-network badge (hidden on mainnet)
static lv_obj_t *s_home_build_id;
static uint32_t s_home_act_t;          // idle auto-lock: last touch while unlocked
static lv_obj_t *s_lock_warn;            // "locking soon" toast, up for the last 30s
static uint32_t s_secret_act_t;          // secret-idle deadline: last touch on a
static uint32_t s_secret_rows;           // deadline row, and WHICH rows those were
#ifndef SIMULATOR
static i2c_master_bus_handle_t s_i2c_bus;  // shared touch bus; camera SCCB probes it too
#endif
// Sized by INK, not by time (the sampler below decimates to 10px moves). KISS
// itself is ~100 points; a circle drawn right around it is ~125 more. At 256
// that pair overflowed, and an overflow drops the TRAILING points -- which is
// exactly the modifier stroke the unlock now depends on, so it failed silently
// for the people who draw big.
// Was a 384 written here while enrolment wrote 512 of its own. Same number,
// one place: kiss_gword.h owns it now, beside the threshold that was already
// shared for exactly this reason.
#define GEST_MAX GW_MAX_PTS        // accumulated points across the strokes of the unlock draw
static lv_point_t s_gpt[GEST_MAX];
static uint8_t s_gid[GEST_MAX];     // stroke id per point (for same-stroke gap filling)
static int s_mx[GEST_MAX], s_my[GEST_MAX];  // scratch: the final stroke, for kiss_duress
static uint8_t s_msid[GEST_MAX];    // ...and its stroke ids, for kiss_gword
static int s_gn;
static int s_strokes;              // number of strokes in the current draw (KISS is many)
static int s_stroke_n0;            // index where the current stroke began (tap vs draw test)
static uint32_t s_gest_idle;       // ms since the last gesture activity (abandon timeout)
static bool s_gest_swallow;        // ignore the touch that just woke the screensaver
// The decoy opens with NO login screen in between, and detect_cover_word fires as
// soon as the word is recognizable -- which can be before the finger has
// finished the last S. Without this the remaining ink lands on the freshly
// revealed home and taps whatever tile is under it. The passphrase path never
// had the problem because its keyboard owns the touch.
static bool s_home_swallow;      // ignore the rest of the gesture that opened the signer
// KISS matched, but a stroke is configured, so the word alone is not yet an
// answer: hold briefly in case a modifier is on its way. Without this the word
// fires on the LIFT OF THE LAST S and the decoy opens before the owner can draw
// anything after it -- which made the configured stroke literally unreachable.
//
// BOTH doors wait this long, and that is the point. When only the decoy waited,
// the delay was a tell: the real signer appeared the instant the modifier
// stroke ended, the decoy appeared a beat later, so anyone who had once watched
// the owner unlock properly could read which door was taken without seeing the
// hand that drew it. On the one screen whose entire job is to be unreadable
// under duress, that is the wrong thing to leak. Equal waits leak nothing.
//
// 500, not 900. The window only has to cover finger-lift to finger-DOWN, since
// a touch restarts the count and the stroke is then classified normally. It
// never had to cover drawing the modifier itself, which is what made 900 too
// generous. Do not cut it much further: too short and the configured stroke
// starts being missed, which is the bug this whole mechanism exists to fix.
// How long the bare word gets to collect a modifier stroke, and therefore the
// whole cost of a successful unlock: everything else that used to be in the
// way is now either hidden behind it or gone. It cannot be zero -- a window of
// nothing is a passphrase door nobody can reach -- and it is not a security
// margin either, so 150ms is the shortest that still catches a mark drawn on
// purpose. Raise it if the mark starts being missed; nothing else depends on
// the number.
#define COVER_OPEN_DELAY_MS 150
static bool s_cover_pending;

// The owner's stroke landed. Points are already cleared, so this is not
// awaiting anything: it is holding the real signer back to the same beat the
// decoy opens on. Absolute deadline rather than the idle counter, because a
// stray touch must not be able to postpone a door the owner already opened.
static bool s_real_pending;
static uint32_t s_real_at;
static uint32_t s_cover_at;        // ...and the spare's, on the same clock
// Deriving the spare's keys INSIDE the 500ms window instead of after it.
//
// The window is not dead time that can be deleted: it is how long a modifier
// stroke has to begin, and it is what makes both doors open on the same beat.
// It was, though, 500ms of nothing followed by 485ms of PBKDF2, so the panel
// sat dead for a second and a half after the stroke ended.
//
// The derivation cannot just move to the front of the window on this task.
// game_tick is what polls the touch controller, so 485ms of blocking here is
// 485ms of a modifier stroke going unsampled -- a mark started 100ms after the
// lift would arrive as its own last fragment, classify as something else, and
// silently open the wrong door. It runs on CPU1 instead, and LVGL keeps CPU0.
//
// One writer: the task owns the prepared session and only while s_prep is
// PREP_RUN. The UI reads s_prep, and touches key material only at PREP_DONE,
// by which point the task has already exited.
enum { PREP_IDLE, PREP_RUN, PREP_DONE };
static volatile int s_prep;
static volatile int s_prep_rc;
static bool s_prep_drop;           // asked for while it ran; done when it lands
static bool s_open_pending;        // home is up, its keys are still landing
#ifdef SIMULATOR
#define SIM_PREP_TICKS 26          // ~440ms at TICK_MS: what the device takes
static int s_sim_prep_ticks;
#endif
static uint32_t s_home_swallow_t;  // last tick that gesture was still touching

// ---- idle attract-mode screensaver ----
#define IDLE_MS 300000             // show the screensaver after 5min with no touch (menu/game-over).
                                   // Cosmetic only, and it runs on the LOCKED cover, so it never
                                   // races the security timeout. That one is autolock_ms(): the
                                   // same 5 min on mainnet, an hour on a test network. Never assume
                                   // either is the longer of the pair.
#define SAVER_N 6
static lv_obj_t *s_saver;          // full-screen backdrop (img_saver)
static lv_obj_t *s_saver_fruit[SAVER_N];
static lv_obj_t *s_saver_hint;     // pulsing "tap to play" prompt
static bool s_saver_on;
static uint32_t s_idle_ms;
static void saver_hide(void);      // defined below; start_game (above it) needs it

// portable xorshift RNG (was esp_random) so the game compiles for device + simulator
//
// Was a fixed literal, which made every boot the same game down to the
// bomb timing. Seeded from the first touch instead: the tick it lands on
// and where it lands. Cosmetic entropy for a cosmetic generator -- nothing
// reached from here is security relevant (the game, the screensaver, and
// the fingerprint chip's scramble flourish at :1348), and the backup quiz
// uses its own generator elsewhere. Do not promote this to anything.
static uint32_t s_rng = 0x9e3779b9u;
static bool s_rng_seeded;
static void rng_seed(int x, int y) {
  if (s_rng_seeded) return;
  s_rng_seeded = true;
  uint32_t s = (uint32_t)lv_tick_get() * 2654435761u;
  s ^= ((uint32_t)x << 16) ^ (uint32_t)y;
  s ^= s << 13; s ^= s >> 17;
  if (s == 0) s = 0x9e3779b9u;   // xorshift is dead at zero
  s_rng = s;
}
static uint32_t rnd(uint32_t n) {
  s_rng ^= s_rng << 13;
  s_rng ^= s_rng >> 17;
  s_rng ^= s_rng << 5;
  return n ? s_rng % n : 0;
}
static int rnd_range(int a, int b) { return a + (int)rnd(b - a + 1); }

#ifndef SIMULATOR
// The board pairs the radio-less ESP32-P4 with an ESP32-C6 WiFi/Bluetooth
// coprocessor (SDIO, reset line on GPIO54 per Guition's EV-board-derived
// BSP). KISS never uses it: hold its reset low from the first code we run
// so whatever firmware shipped on the C6 never executes, and latch the pad
// so the level survives soft resets. Logged at W because release builds
// strip INFO. Settings/home read the pad back via radio_is_held().
#define C6_RESET_GPIO GPIO_NUM_54
static void radio_hold_in_reset(void) {
  gpio_set_level(C6_RESET_GPIO, 0);   // level first: no high glitch on config
  gpio_config_t io = {.pin_bit_mask = 1ULL << C6_RESET_GPIO,
                      .mode = GPIO_MODE_INPUT_OUTPUT};
  ESP_ERROR_CHECK(gpio_config(&io));
  gpio_set_level(C6_RESET_GPIO, 0);
  gpio_hold_en(C6_RESET_GPIO);
  ESP_LOGW(TAG, "C6 radio held in reset (GPIO54 low)");
}
bool radio_is_held(void) { return gpio_get_level(C6_RESET_GPIO) == 0; }

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

static void touch_start(void) {
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
#else   // SIMULATOR
// No panel to starve and no flash to write, so the desktop keeps the symbol
// and does nothing with it. kiss_fw_ui.c then has ONE shape on both sides,
// and the screen walk still reaches the WRITING stop with the same code path
// the device runs.
void kiss_backlight_set(int on) { (void)on; }
void kiss_backlight_level(int pct) { (void)pct; }
void kiss_panel_black(void) { }
#endif  // !SIMULATOR

// ---------------- entities ----------------
static lv_obj_t *make_sprite(const lv_image_dsc_t *dsc) {
  lv_obj_t *o = lv_image_create(lv_screen_active());
  lv_image_set_src(o, dsc);  // pre-sized sprite -> no runtime scaling (fast path)
  // The default pivot is the top-left corner, so a rotating sprite ORBITS
  // rather than spins. art_unpack_all can leave a descriptor NULL on failure
  // and that stays non-fatal, hence the guard.
  if (dsc) lv_image_set_pivot(o, dsc->header.w / 2, dsc->header.h / 2);
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
//
// THE COVER NEVER WEARS THE SIGNER'S FACE. IoskeleyMono is the signer's whole
// visual identity now, so a game surface set in it links the "game" to the
// device it is covering for -- the owner called it a giveaway from the bench.
// Every label the game or its screensaver shows stays on stock Montserrat,
// here and at the four sites below (score, game over, best, tap to play).
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

// Rotation is the expensive path on this build (LV_DRAW_SW_ASM_NONE: scalar
// C rotate, and a ~41% bigger invalidation box than the upright blit). Two
// things keep it affordable. Angle is quantized to 15 degrees, so a fruit
// spinning at 5 deg/frame only forces a re-render every third frame instead
// of every frame. And av is zero for droplets, so the spray -- by far the
// most numerous entity -- never takes the path at all.
// Tumbling WHOLE fruit is off, and this is the measurement that turned it off.
// A non-zero rotation puts an image on LVGL's transform path for every redraw,
// not just for the redraws where the angle changed, so SPIN_STEP cannot buy it
// back. On the board, with the 16ms physics tick instrumented:
//
//   spin on   27.8 ticks/s, 38.2ms mean gap, 69ms worst, 23 late ticks/s
//   spin off  58.0 ticks/s, 16.5ms mean gap, 24ms worst,  0 late ticks/s
//
// Half the physics rate, and it compounds: a fruit takes twice as long in wall
// clock to finish its arc, so twice as many are alive, so it gets slower again.
// The bench reported it as lag three times before this was measured properly.
// The earlier flush-throughput number said rotation was free -- it was measuring
// bandwidth, which was saturated either way, and could not see the frames.
//
// Halves came off it too, on the next measurement. Keeping the cut direction
// cost 3 ticks/s on average and, worse, 9 off the FLOOR: 42.0 mean and 24 worst
// second with them flat against 39.0 and 15 with them angled, under a finger
// that never stops cutting. A seam that always runs vertical is a smaller loss
// than a blade that stalls for a tenth of a second.
#ifndef KISS_GAME_SPIN
#define KISS_GAME_SPIN 0
#endif
#define SPIN_STEP 15

static void place(ent_t *e) {
  if (!e->obj) return;
  lv_obj_set_pos(e->obj, (int)e->x - e->size / 2, (int)e->y - e->size / 2);
  if (!KISS_GAME_SPIN) return;   // nothing is on LVGL's transform path
  if (e->av == 0.0f) return;
  int16_t q = (int16_t)(((int)e->rot / SPIN_STEP) * SPIN_STEP);
  if (q == e->rot_q) return;               // same step: nothing to redraw
  e->rot_q = q;
  lv_image_set_rotation(e->obj, (int32_t)q * 10);   // LVGL takes 0.1 deg
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

// Both halves of "it gets harder" that a player actually SEES: how fast a
// fruit is thrown, and how hard it is pulled back down. The first fruit of a
// run floats up in a slow readable arc at 15..18 px a tick against 0.35
// gravity; by the end it is 20..27 against the full 0.5, which is where the
// game has always been. Only the top of the launch range used to move, so a
// fresh run opened at full speed and the ramp was invisible -- the owner read
// that off the bench as "shouldn't it start kinda slow".
//
// Apex is deliberately roughly constant across the ramp. What changes is the
// TIME the fruit spends in the air, which is the part that reads as difficulty.
static int launch_speed(float p) {
  return rnd_range(15 + (int)(p * 5), 18 + (int)(p * 9));
}
static float fruit_gravity(float p) { return GRAVITY * (0.70f + 0.30f * p); }

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

// Split out so a wave pattern can place its own throw instead of taking the
// one random lane spawn_fruit_idx picks.
// The renderer's budget, measured on the board: the spawn period was swept
// from 900ms down to 130ms while the 16ms physics tick was logged against the
// live fruit count.
//
//   1..5 fruit   57 ticks/s   17ms mean      full speed
//   6..7         46           21ms
//   8            37           27ms
//   12           28           35ms
//   18           19           54ms
//   28 (pool)    10           100ms          unplayable
//
// That sweep had nobody CUTTING, which was the flaw in it. Repeated with a
// synthetic finger sweeping the play area, so halves and juice are on screen
// where they belong, the same curve turns out to be about TOTAL entities and
// not fruit:
//
//   1..4 entities   43..51 ticks/s
//   5..7            29..32
//   8..10           23..31
//
// A slice adds two halves and a juice drop to whatever fruit are already up,
// so a cap of five fruit is a screen of eight or nine things and half the tick
// rate. Four fruit, and one juice drop instead of two on a fruit that already
// throws two halves, keeps the usual case inside the fast band.
//
// Sprites are alpha blended in scalar C on this build (LV_DRAW_SW_ASM_NONE),
// so every entity costs real CPU per frame and there is no tuning of waves or
// speeds that gets it back. The cap goes here, where every fruit is born.
#define MAX_LIVE_FRUIT 4

static int live_fruit(void) {
  int n = 0;
  for (int i = 0; i < MAX_ENT; i++)
    if (s_ent[i].active && s_ent[i].kind == K_FRUIT) n++;
  return n;
}

static void spawn_fruit_at(int idx, bool gold, float x, float vx, float vy) {
  // The bomb is the threat, so it always gets its slot. Fruit are what flood.
  if (!DEFS[idx].bomb && live_fruit() >= MAX_LIVE_FRUIT) return;
  ent_t *e = alloc_ent();
  if (!e) return;
  const def_t *d = &DEFS[idx];
  e->active = true;
  e->kind = K_FRUIT;
  e->bomb = d->bomb;
  e->gold = gold;
  e->defi = idx;
  e->size = d->size;
  e->x = x;
  e->y = SCREEN_H + d->size;
  e->vx = vx;
  e->vy = vy;
  e->rot = (float)rnd(360);
  e->av = (float)rnd_range(-d->spin, d->spin);
  if (e->av == 0.0f) e->av = 1.0f;
  e->rot_q = INT16_MIN;
  e->obj = make_sprite(d->whole);
  if (gold) {
    lv_obj_set_style_image_recolor(e->obj, lv_color_hex(0xFFD23A), 0);
    lv_obj_set_style_image_recolor_opa(e->obj, 150, 0);
  }
  place(e);
}

static void spawn_fruit_idx(int idx) {
  float p = diff_progress();  // arcs get a little faster/wider as the game goes on (gentle, smooth)
  const def_t *d = &DEFS[idx];
  float vx = (rnd_range(0, 100 + (int)(p * 50)) - (50 + (int)(p * 25))) / 26.0f;  // tighter spread -> fruit stay on screen
  float vy = -(float)(launch_speed(p) + d->lift);
  spawn_fruit_at(idx, false, pick_spawn_x(), vx, vy);
}

static void spawn_half(const lv_image_dsc_t *dsc, int size, float x, float y,
                       float vx, float vy, float rot, float av) {
  ent_t *e = alloc_ent();
  if (!e) return;
  e->active = true;
  e->kind = K_HALF;
  e->bomb = false;
  e->gold = false;
  e->size = size;
  e->x = x;
  e->y = y;
  e->vx = vx;
  e->vy = vy;
  e->rot = rot;
  e->av = av;
  e->rot_q = INT16_MIN;                    // force the first apply
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
    e->gold = false;
    e->rot = 0.0f;
    e->av = 0.0f;                          // droplets never take the rotate path
    e->rot_q = 0;
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
  s_frenzy_ms = 0;
  bool newbest = s_score > s_best;
  if (newbest) {
    // Written only on a new best, which is rare enough to add no meaningful
    // wear to the partition that also holds the KEEP seed.
    s_best = s_score;
    kiss_game_best_store((uint16_t)(s_score > 65535 ? 65535 : s_score));
  }
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
  s_combo_n = 0;      // an open window at game over would pay out on the next
  s_frenzy_ms = 0;
  s_wave_rest = 0;
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
  s_menu_idle_drift = true;
}

// Stop the idle drift once the menu is not the screen being looked at.
//
// These two animations are LV_ANIM_REPEAT_INFINITE and nothing ever ended them,
// so the four fruit went on floating under the home, the wizard and every
// sub-screen for the whole session. A moving object under an opaque screen is
// not free: LVGL invalidates the rect it vacated, recomposites it with whatever
// is on top, and rot_flush pushes the result to the panel.
//
// On the seed entropy screen that is not merely wasteful, it is the bug. Fruit 0
// and 2 sit at x 150 and 172, inside the preview column at x 48..348, so ~15
// times a second a 93x92 patch of the LAYOUT was being stamped over the live
// camera picture -- the black box crawling across the video. Measured with
// KISS_FLUSH_STATS: 16, 18 and 10 preview hits per second while streaming, and
// the hit rects were these fruit's exact drift ranges.
//
// It also cost every other screen in the product a repaint it never used: the
// idle flush rate sat at 28-40/s on signer screens with nothing animating.
//
// Only the stop lives here. Every path back to the menu already runs
// menu_intro(), which deletes any surviving drift and starts it again, so this
// stays a one-sided change.
static void menu_idle_drift_stop(void) {
  if (!s_menu_idle_drift) return;
  for (int i = 0; i < MENU_FRUIT_N; i++)
    if (s_menu_fruit[i]) lv_anim_delete(s_menu_fruit[i], NULL);
  s_menu_idle_drift = false;
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

static void slice(ent_t *e, float bdx, float bdy) {
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
  const def_t *dd = &DEFS[e->defi];
  if (lv_tick_elaps(s_combo_t) > COMBO_MS) s_combo_n = 0;   // window lapsed
  s_combo_n++;
  s_combo_t = lv_tick_get();
  s_combo_x = e->x; s_combo_y = e->y;
  int pts = e->gold ? 5 : dd->points;
  s_score += pts;
  { char b[8]; snprintf(b, sizeof b, "+%d", pts);
    score_popup((int)e->x - 6, (int)e->y - 24, b,
                e->gold ? 0xFFD23A : 0xFFFFFF); }
  lv_label_set_text_fmt(s_score_lbl, "%d", s_score);
  if (s_score / 50 > s_life_milestone) {       // every 50 pts: earn a life back (handles combo jumps)
    s_life_milestone = s_score / 50;
    if (s_lives < 3) {
      s_lives++;
      update_hearts();
      life_gain_fx(s_hearts[s_lives - 1]);     // subtle heart pop, not a screen flash
    }
  }
  if (e->gold) {
    s_frenzy_ms = FRENZY_MS;
    screen_flash(0xFFD23A);
    score_popup(SCREEN_W/2 - 110, SCREEN_H/2 - 40, "FRENZY!", 0xFFD23A);
  }
  const def_t *d = &DEFS[e->defi];
  juice_splat(e->x, e->y, d->juice);  // big juicy splash on every slice
  if (d->burst) {
    spawn_juice(e->x, e->y, d->juice, 3);  // cherries/grapes: burst, no halves
  } else {
    // The cut face lines up with the stroke and the halves separate across
    // it, carrying the parent's momentum: a fruit cut at the top of its arc
    // now drops apart instead of relaunching itself sideways.
    // Blade angle, less the quarter turn the art already carries: hl and hr
    // are the LEFT and RIGHT of an upright fruit, so an unrotated pair is
    // itself a vertical cut. Rotating by the raw blade angle -- which is what
    // this was -- puts the seam across the stroke instead of along it, and a
    // vertical swipe came apart with a horizontal seam.
    float ang = atan2f(bdy, bdx) * 57.29578f - 90.0f;
    float nx = -bdy, ny = bdx;                  // unit normal to the cut
    const float SEP = 7.0f;
    spawn_half(d->hl, d->hsize, e->x - nx*6, e->y - ny*6,
               e->vx - nx*SEP, e->vy*0.6f - ny*SEP,
               ang, -(float)rnd_range(2, 5));
    spawn_half(d->hr, d->hsize, e->x + nx*6, e->y + ny*6,
               e->vx + nx*SEP, e->vy*0.6f + ny*SEP,
               ang,  (float)rnd_range(2, 5));
    spawn_juice(e->x, e->y, d->juice, 1);   // it already threw two halves
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

static bool seg_cross(float ax, float ay, float bx, float by,
                      float cx, float cy, float dx, float dy) {
  float d1 = (bx-ax)*(cy-ay) - (by-ay)*(cx-ax);
  float d2 = (bx-ax)*(dy-ay) - (by-ay)*(dx-ax);
  float d3 = (dx-cx)*(ay-cy) - (dy-cy)*(ax-cx);
  float d4 = (dx-cx)*(by-cy) - (dy-cy)*(bx-cx);
  return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}

// Distance between two segments. Crossing is the case that matters and the
// endpoint minimum gets it badly wrong (it can report half a fruit width
// for two segments that plainly intersect), so it is tested separately.
static float seg_seg_dist(float ax, float ay, float bx, float by,
                          float cx, float cy, float dx, float dy) {
  if (seg_cross(ax, ay, bx, by, cx, cy, dx, dy)) return 0.0f;
  float m = seg_dist(ax, ay, bx, by, cx, cy), t;
  t = seg_dist(ax, ay, bx, by, dx, dy); if (t < m) m = t;
  t = seg_dist(cx, cy, dx, dy, ax, ay); if (t < m) m = t;
  t = seg_dist(cx, cy, dx, dy, bx, by); if (t < m) m = t;
  return m;
}

// Below this much ink over the live trail the blade is not moving and must
// not cut. Without it a resting finger is an armed blade and fruit die on
// it -- which is most of why slicing felt like nothing. Measured over the
// WHOLE trail, not one segment: one segment is a single frame and rounds
// to noise at 60Hz.
#define SLICE_MIN_TRAVEL 22.0f

// Takes `pressed` because the blade outlives the finger: on release
// update_blade drains ONE trail point per frame, so for the next two or
// three frames the survivors are still a real, full-length edge and the
// travel gate cannot suppress them -- that ink is genuine. Measured at one
// cut per four hundred strokes on the walk's own stroke, in this code and
// in the code before it. A blade with no finger on it is the same defect as
// a blade under a still one.
static void check_slices(bool pressed) {
  if (!pressed) return;
  int n = s_trail_count;
  if (n < 2) return;

  float travel = 0;
  for (int i = 1; i < n; i++) {
    float dx = (float)(s_trail[i].x - s_trail[i-1].x);
    float dy = (float)(s_trail[i].y - s_trail[i-1].y);
    travel += sqrtf(dx*dx + dy*dy);
  }
  if (travel < SLICE_MIN_TRAVEL) return;

  // Cut angle: the newest segment, normalized. Everything downstream --
  // which way the halves separate, which way the cut face points -- comes
  // from this, and it is the thing the old code threw away.
  float bdx = (float)(s_trail[n-1].x - s_trail[n-2].x);
  float bdy = (float)(s_trail[n-1].y - s_trail[n-2].y);
  float bl = sqrtf(bdx*bdx + bdy*bdy);
  if (bl < 0.001f) { bdx = 1.0f; bdy = 0.0f; } else { bdx /= bl; bdy /= bl; }

  for (int i = 0; i < MAX_ENT; i++) {
    ent_t *e = &s_ent[i];
    if (!e->active || e->kind != K_FRUIT) continue;
    float r = e->size / 2.0f + 4.0f;
    // Sweep BOTH bodies. Trail entries are consecutive frames (update_blade
    // drops from the front, so the survivors stay adjacent), so trail index
    // k was sampled n-1-k frames ago and the fruit was that many frames of
    // its own velocity back up its arc.
    bool hit = false;
    for (int s = 1; s < n && !hit; s++) {
      float a0 = (float)(n - s), a1 = (float)(n - 1 - s);
      float fx0 = e->x - e->vx * a0, fy0 = e->y - e->vy * a0;
      float fx1 = e->x - e->vx * a1, fy1 = e->y - e->vy * a1;
      if (seg_seg_dist((float)s_trail[s-1].x, (float)s_trail[s-1].y,
                       (float)s_trail[s].x,   (float)s_trail[s].y,
                       fx0, fy0, fx1, fy1) < r) hit = true;
    }
    if (hit) slice(e, bdx, bdy);
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
  {
    lv_anim_t hp;
    lv_anim_init(&hp);
    lv_anim_set_var(&hp, s_saver_hint);
    lv_anim_set_exec_cb(&hp, anim_opa_cb);
    lv_anim_set_values(&hp, 150, 255);  // floor kept high so the prompt stays readable
    lv_anim_set_duration(&hp, 950);
    lv_anim_set_reverse_duration(&hp, 950);
    lv_anim_set_repeat_count(&hp, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&hp);
  }
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
  lv_anim_delete(s_saver_hint, NULL);   // the pulse goes with the screen it is on
  lv_obj_add_flag(s_saver, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_saver_hint, LV_OBJ_FLAG_HIDDEN);
  // restore the panel for whatever state we returned to
  if (s_state == ST_MENU) { lv_obj_clear_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN); menu_intro(); }
  else if (s_state == ST_OVER) lv_obj_clear_flag(s_over_panel, LV_OBJ_FLAG_HIDDEN);
}

// The word recogniser moved to main/kiss_coverword.c. It went there because
// it could not be tested here: main.c links into no test binary, so "KIS"
// opened the decoy for months with every gate green. See sim/test_coverword.c.
//
// This adapter is the whole cost of the move -- the collector stores
// lv_point_t and the module takes plain int arrays, for the same reason
// kiss_gword.c does: no LVGL type crosses into code the desktop runner has
// to build.
static bool detect_cover_word(const lv_point_t *p, int n, int strokes) {
  if (n > GEST_MAX) n = GEST_MAX;
  static int kx[GEST_MAX], ky[GEST_MAX];
  for (int i = 0; i < n; i++) { kx[i] = p[i].x; ky[i] = p[i].y; }
  return cw_match(kx, ky, s_gid, n, strokes);
}

// center the code + caption on the baked chip frame (566..760 x 39..86 in kiss_mock.py)
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
  // Held at pure noise while the keys are still being derived. The animation
  // was always a decrypt that had nothing to decrypt -- the fingerprint was
  // known before it started. Now it is honest, and it is what pays for opening
  // the home before the session exists.
  int resolved = s_open_pending ? 0 : s_fp_scr_step - 4;
  if (s_open_pending) s_fp_scr_step = 4;      // ...and does not run out of steps
  if (resolved < 0) resolved = 0;
  if (resolved > 8) resolved = 8;
  char buf[9];
  for (int i = 0; i < 8; i++) buf[i] = (i < resolved) ? s_fp_hex[i] : HEXD[rnd(16)];
  buf[8] = 0;
  lv_label_set_text(s_fp_fly, buf);
  lv_obj_set_style_text_color(s_fp_fly,       // electric flicker while unresolved
      (s_fp_scr_step & 1) ? wt_accent() : WT_INK, 0);
  s_fp_scr_step++;
  if (resolved >= 8) {                        // locked: settle to ink and glide away
    lv_timer_delete(s_fp_scr_tmr);
    s_fp_scr_tmr = NULL;
    lv_obj_set_style_text_color(s_fp_fly, wt_accent(), 0);
    fp_fly_glide();
  }
}
static void fp_fly_start(void) {
  // A second unlock inside the ~1.25s flight would overwrite the pointer and
  // strand the first flier on the glass forever. Nothing deleted it before.
  if (s_fp_fly) { lv_obj_delete(s_fp_fly); s_fp_fly = NULL; }
  if (s_fp_scr_tmr) { lv_timer_delete(s_fp_scr_tmr); s_fp_scr_tmr = NULL; }
  // The home screen, NOT lv_layer_top(). The top layer draws above every
  // screen there is, so a flight still in the air when anything opened over
  // the home -- a tile, the fingerprint card, the setup wizard, the lock --
  // kept painting the fingerprint of those keys across it. The walk caught it
  // as an 800px "12A4BB6B" lying over NEW SEED WORDS, one screen after a wipe
  // that had just erased the wallet it names; kiss_lock is the worse one,
  // because it calls kiss_ui_forget_fp to stop remembering WHICH keys those
  // were and the flier went on drawing them over the game.
  //
  // As a child of s_home it is covered by whatever covers the home and hidden
  // with it, by construction -- no call site to add, and none for a future
  // screen to forget. Everything else on this screen (the chip, the tile
  // titles, the motes) is already a child of it, and it is created last so it
  // still draws over them.
  s_fp_fly = lv_label_create(s_home);
  lv_label_set_text(s_fp_fly, s_fp_hex);      // real code first: size the label off it
  lv_obj_set_style_text_color(s_fp_fly, wt_accent(), 0);
  lv_obj_set_style_text_font(s_fp_fly, wt_font_num48(), 0);
  lv_obj_set_style_text_letter_space(s_fp_fly, 4, 0);
  lv_obj_update_layout(s_fp_fly);
  // start where the reveal card showed the code (box center 400,163 in kiss_ui.c)
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

static void kiss_start(void);
static void setup_done_login(void) {       // wizard stored the seed: first login
  // Which passphrase flow depends on whose passphrase it is.
  //
  // NEW words: the passphrase is being invented, nobody has ever seen it, and a
  // typo is unrecoverable -- so it is typed twice.
  //
  // RESTORED words: the owner is re-entering one they already have, and typing
  // it twice checks nothing. The same slip made twice matches itself and opens
  // a different, valid, empty set of keys. The fingerprint screen that follows
  // the only step that can tell those apart, so it carries the check alone.
  if (kiss_setup_restoring())
    kiss_login_open_restore(kiss_start);
  else
    kiss_login_open_setup(kiss_start);
}

// A seed already owned by the user is ready, either loaded into an AMNESIC
// session or verified on the configured SD card. This is an ordinary unlock,
// not the type-twice ritual used for newly created or restored keys.
static void stored_seed_ready(void) {
  kiss_login_open(kiss_start);
}

// The wizard on demand, not just at first boot: SETTINGS reaches it straight
// after a wipe, so an owner who erased in order to type their paper back in
// is not sent round by the menu. Completing it overwrites the stored seed;
// cancelling leaves it untouched.
void kiss_begin_setup(void) {
  kiss_setup_open(lv_screen_active(), setup_done_login);
}

// Sync the home TESTNET badge to the current network. Called on unlock and by
// Settings when it closes, so flipping the network updates the home immediately.
static void kiss_home_restyle(void) {
  if (!s_home) return;
  // THE WALK FIRST, then the exceptions. Everything under the home wearing
  // WT_FLAG_ACCENT is repainted by its flag, which is the mechanism the rest
  // of the device uses and the one accent_walk's own comment argues for:
  // "keeping a static list of them in every screen that has some is how they
  // get missed".
  //
  // The home was the last screen still keeping that list, and it had already
  // missed one. The TESTNET badge is built with the flag and was not in the
  // list, so changing the theme from Settings left the word on the home
  // screen in the PREVIOUS accent -- reported from the bench as picking
  // orange and finding TESTNET still pink. Nothing about the badge was
  // wrong; nothing was calling it.
  wt_accent_restyle(s_home);
  lv_color_t ac = wt_accent();
  if (s_fp_chip) lv_obj_set_style_text_color(s_fp_chip, ac, 0);
  kiss_build_id_restyle(s_home_build_id);
  if (s_fp_fly)  lv_obj_set_style_text_color(s_fp_fly, ac, 0);
  if (s_cam_lbl) {
    const char *msg = lv_label_get_text(s_cam_lbl);
    lv_obj_set_style_text_color(s_cam_lbl, (msg && *msg) ? ac : WT_MUT, 0);
  }
  // labels keep their fixed white/grey (the CARD wears the theme, not the
  // text); re-set the TEXT though: a language switch lands here via
  // kiss_home_refresh(), and the home is built once per boot
  for (int i = 0; i < 4; i++)
    if (s_tile_ttl[i]) {
      // These objects survive a Settings language change. Refresh the font as
      // well as the text so regional CJK glyph forms switch immediately.
      lv_obj_set_style_text_font(s_tile_ttl[i], wt_font23(), 0);
      lv_label_set_text(s_tile_ttl[i], tr(TILE_TTL_STR[i]));
    }
  if (s_theme_cap) {
    lv_obj_set_style_text_font(s_theme_cap, wt_font14(), 0);
    lv_label_set_text(s_theme_cap, tr(STR_H_THEME));
  }
  if (s_fp_cap) {
    lv_obj_set_style_text_font(s_fp_cap, wt_font14(), 0);
    lv_label_set_text(s_fp_cap, tr(STR_H_FINGERPRINT));
    fp_chip_place();
  }
  for (int i = 0; i < 4; i++) {
    if (s_card_frame[i]) {
      // All four, one weight. The two-tier version painted KEYS and SETTINGS
      // in WT_EDGE and left their glow uncoloured, which is what made two of the
      // four tiles look unfinished rather than secondary.
      lv_obj_set_style_border_color(s_card_frame[i], ac, 0);
      lv_obj_set_style_bg_color(s_card_frame[i], ac, 0);
      lv_obj_set_style_shadow_color(s_card_frame[i], ac, 0);
    }
    if (s_corner[i]) lv_obj_set_style_border_color(s_corner[i], ac, 0);
  }
  if (s_underline)  lv_obj_set_style_bg_color(s_underline, ac, 0);
  if (s_chip_frame) lv_obj_set_style_border_color(s_chip_frame, ac, 0);
  if (s_theme_dot)  lv_obj_set_style_bg_color(s_theme_dot, ac, 0);
  if (s_theme_lbl) {
    lv_label_set_text(s_theme_lbl, wt_accent_name());
    lv_obj_set_style_text_color(s_theme_lbl, lv_color_hex(0xE8EEF7), 0);
    lv_obj_update_layout(s_theme_lbl);           // right-align: long names must not
    int tx = 760 - lv_obj_get_width(s_theme_lbl);  // leave the safe area (overscan!)
    lv_obj_set_pos(s_theme_lbl, tx, 428);
    if (s_theme_dot) lv_obj_set_pos(s_theme_dot, tx - 26, 430);
    if (s_theme_cap) {
      lv_obj_update_layout(s_theme_cap);
      lv_obj_set_pos(s_theme_cap, 760 - lv_obj_get_width(s_theme_cap), 406);
    }
  }
  for (int i = 0; i < N_MOTES; i++)
    if (s_mote[i]) lv_obj_set_style_bg_color(s_mote[i], ac, 0);
}

// The SD-storage badge: shown only when these keys live on the card, accent
// while the card is in (game_tick breathes it), an amber cross while it is
// out. Marks, not words: the pair needs no locale and the colour carries the
// state. Driven both here (immediate, on unlock and return from Settings) and
// by the game_tick hot-plug poll (catches a card pulled or pushed while idle).
static void sd_badge_sync(bool present) {
  if (!s_sd_badge) return;
  s_sd_badge_live = false;
  if (kiss_seed_mode() == WSEED_MODE_SD) {
    lv_obj_clear_flag(s_sd_badge, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_sd_badge,
                      present ? LV_SYMBOL_SD_CARD "  " LV_SYMBOL_OK
                              : LV_SYMBOL_SD_CARD "  " LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(s_sd_badge, present ? wt_accent() : WT_WARN, 0);
    if (present) s_sd_badge_live = true;             // breathe in game_tick
    else         lv_obj_set_style_opa(s_sd_badge, LV_OPA_COVER, 0);  // warning stays solid
  } else {
    lv_obj_add_flag(s_sd_badge, LV_OBJ_FLAG_HIDDEN);
  }
}

// The step, or nothing. Read on every refresh rather than cached: the paper
// can be checked and a coordinator can speak inside one unlocked session, and
// both of those land here through the refresh the screens that change them
// already call.
static void next_step_sync(void) {
  if (!s_next_lbl) return;
  uint8_t fp[4];
  kiss_ui_last_fp(fp);
  const bool have_keys = (fp[0] | fp[1] | fp[2] | fp[3]) != 0;
  int chigh; uint32_t cheight;
  const char *step = NULL;
  if (have_keys) {
    if (!kiss_ui_backup_checked())
      step = tr(STR_H_NEXT_BACKUP);
    else if (!kiss_usage_chain_known(fp, kiss_testnet() ? 1 : 0, kiss_script(),
                                     &chigh, &cheight))
      step = tr(STR_H_NEXT_PAIR);
  }
  if (!step) { lv_obj_add_flag(s_next_lbl, LV_OBJ_FLAG_HIDDEN); return; }
  // The font too, not just the text: a language change reaches the home
  // through this call and CJK wants its own face, the same reason the tile
  // titles re-set theirs.
  lv_obj_set_style_text_font(s_next_lbl, wt_font23(), 0);
  char buf[128];
  snprintf(buf, sizeof buf, "%s  %s", LV_SYMBOL_RIGHT, step);
  lv_label_set_text(s_next_lbl, buf);
  lv_obj_clear_flag(s_next_lbl, LV_OBJ_FLAG_HIDDEN);
  // Re-align after the text: the label is content sized, so a translation of a
  // different width would otherwise stay centred on the old one.
  lv_obj_align(s_next_lbl, LV_ALIGN_TOP_MID, 0, HOME_NEXT_Y);
}

void kiss_home_refresh(void) {
  kiss_home_restyle();
  next_step_sync();
  sd_badge_sync(platform_sd_probe() != 0);
  if (!s_net_lbl) return;
  if (kiss_testnet()) {
    lv_label_set_text(s_net_lbl, kiss_net_name());   // TESTNET or SIGNET
    lv_obj_clear_flag(s_net_lbl, LV_OBJ_FLAG_HIDDEN);
    // CENTRED AS A PAIR, measured after the word is set: SIGNET and TESTNET are
    // different widths and so is every locale, so the dot's x comes off the
    // rendered label rather than a number written here.
    lv_obj_update_layout(s_net_lbl);
    const int lw = lv_obj_get_width(s_net_lbl), gap = 10;
    const int x0 = (800 - (8 + gap + lw)) / 2;
    if (s_net_dot) {
      lv_obj_set_pos(s_net_dot, x0, 56);
      lv_obj_clear_flag(s_net_dot, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_pos(s_net_lbl, x0 + 8 + gap, 52);
  } else {
    lv_obj_add_flag(s_net_lbl, LV_OBJ_FLAG_HIDDEN);
    if (s_net_dot) lv_obj_add_flag(s_net_dot, LV_OBJ_FLAG_HIDDEN);
  }
}

#ifdef SIMULATOR
// sim-only: drive the bottom-center status slot so the SD-insert indicator can
// be previewed (the real poll is device-only — no SDMMC in the sim).
void sim_home_status(const char *msg) {
  if (!s_cam_lbl) return;
  lv_label_set_text(s_cam_lbl, msg);
  lv_obj_set_style_text_color(s_cam_lbl, wt_accent(), 0);
}
#endif

// Ambient life while the home idles: dim ink dots rising slowly through the
// grid. HARD RULE (found the hard way): every animated position, including
// travel endpoints and parking spots, must stay INSIDE the parent's bounds.
// A child moving outside the parent makes LVGL invalidate the WHOLE parent
// every tick -> the entire baked home re-renders + re-blits = panel-wide
// twitching. In-bounds, only the dots' tiny dirty rects redraw (sim-proven:
// 24,774 changed px/frame out-of-bounds vs 133 in-bounds).
// the tile labels settle in: a short staggered drop + fade, left to right,
// then they hold still (translate/opa only). Runs on every unlock.
static void tiles_settle(void) {
  for (int i = 0; i < 4; i++) {
    // One readable title per card. The old 14px subtitles repeated what the
    // icon/title already said, so removing them also lets the title sit at the
    // visual centre of the label area.
    lv_obj_t *label = s_tile_ttl[i];
    const int base = TILE_LBL_Y + 22;
    if (!label) return;
    lv_anim_delete(label, NULL);        // re-unlock mid-settle: start clean
    lv_obj_set_style_opa(label, 0, 0);
    lv_obj_set_y(label, base - 12);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, label);
    lv_anim_set_delay(&a, 120 + i * 70);
    lv_anim_set_duration(&a, 260);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, fly_y_cb);
    lv_anim_set_values(&a, base - 12, base);
    lv_anim_start(&a);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_start(&a);
  }
}

static void motes_start(void) {
  static const int mx[N_MOTES]  = {150, 260, 430, 590, 735};
  static const int mms[N_MOTES] = {9000, 12400, 7600, 10800, 14200};
  for (int i = 0; i < N_MOTES; i++) {
    if (!s_mote[i]) return;
    lv_anim_delete(s_mote[i], NULL);
    lv_obj_set_pos(s_mote[i], mx[i], 474);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_mote[i]);
    lv_anim_set_exec_cb(&a, fly_y_cb);
    lv_anim_set_values(&a, 474, 2);
    lv_anim_set_duration(&a, mms[i]);
    lv_anim_set_delay(&a, i * 900);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_repeat_delay(&a, 500 + i * 400);
    lv_anim_start(&a);
    lv_anim_set_exec_cb(&a, anim_opa_cb);   // independent shimmer on top
    lv_anim_set_values(&a, 30, 120);
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
    lv_obj_set_y(s_mote[i], 474);           // parked in-bounds (invisible: opa 0)
  }
}

// The four bytes the chip carries, read from wherever the last unlock left
// them. Split out because the provisional open calls it twice: once with
// nothing to say, and again when the derivation lands.
static void home_fp_publish(void) {
  uint8_t fp[4];
  kiss_ui_last_fp(fp);
  snprintf(s_fp_hex, sizeof(s_fp_hex), "%02X%02X%02X%02X", fp[0], fp[1], fp[2], fp[3]);
  if (!s_fp_chip) return;
  lv_label_set_text(s_fp_chip, s_fp_hex);
  fp_chip_place();
}

static void kiss_start(void) {           // unlocked via login -> reveal the home
  if (s_home_on) return;
  // The LVGL pointer indev is created lazily, and until this release the ONLY
  // things that created it were the login screen and the setup wizard -- every
  // way into the signer went through one of them. The decoy does not: it opens
  // the session and lands here directly, so it arrived with no indev at all.
  //
  // The game never noticed, because game_tick reads the touch controller itself
  // (read_touch) and does not go through LVGL. But every SUB-screen behind it is
  // ordinary LVGL buttons, so Settings, Receive, Sign and the scan screen's
  // CLOSE all drew perfectly and ignored every touch -- a screen that looks
  // alive and is deaf, with the UI task still running and nothing in the log.
  //
  // It belongs here rather than in kiss_open_decoy: this is the one point
  // every way in passes through, so no future entry path can miss it again.
  kiss_ui_ensure_indev();
  s_home_on = true;
  {  // the home chip shows the fingerprint of the keys that were just unlocked
    home_fp_publish();          // ...or of nothing yet, if they are still landing
    if (s_fp_chip) {
      lv_obj_add_flag(s_fp_chip, LV_OBJ_FLAG_HIDDEN);  // revealed when the flight lands
      lv_obj_add_flag(s_fp_cap, LV_OBJ_FLAG_HIDDEN);
      kiss_home_restyle();
      fp_fly_start();
    }
  }
  saver_hide();
  menu_idle_drift_stop();          // nothing under the home may keep moving
  lv_obj_add_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_over_panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_home, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_home);
  tiles_settle();                          // labels drop in, staggered
  motes_start();                           // ambient idle drift
  kiss_home_refresh();                   // show/hide the TESTNET badge for this session
  s_home_act_t = lv_tick_get();          // fresh idle clock for this session
}

static void kiss_lock(void) {            // back to the game cover (tap the KISS logo)
  if (!s_home_on) return;
#ifndef SIMULATOR
  if (camera_spike_is_on()) {              // never leave the camera running behind the game
    camera_spike_toggle(s_home, s_i2c_bus);
    if (s_cam_lbl) lv_label_set_text(s_cam_lbl, camera_spike_status());
  }
#endif
  s_home_on = false;
  s_home_swallow = false;
  s_real_pending = false;
  if (s_fp_card) { lv_obj_delete(s_fp_card); s_fp_card = NULL; }
  kiss_session_close();                  // locked: no key material stays in RAM
  kiss_ui_forget_fp();                   // and no memory of WHICH keys they were
  motes_stop();
  lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN);
  s_state = ST_MENU;
  lv_obj_clear_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN);
  s_gest_swallow = true;                   // ignore the rest of this tap so we land on the menu
  cw_quick_reset();                        // a tap from the last session pairs with nothing
  menu_intro();
}

// kiss_settings.c: seed already wiped + session closed; just drop to the game.
void kiss_wiped_lock(void) { kiss_lock(); }

// The decoy signer opens STRAIGHT from the game: empty BIP39 passphrase, no
// keyboard, nothing on screen suggesting there is another way in. It is a real
// signer -- own fingerprint, pairs, signs -- which is the point: the story an
// attacker is shown has to survive them using it.
// The gesture just opened a screen, and the finger that drew it is still down.
//
// LVGL resolves the pressed object during the press and delivers the click on
// the release, so a screen that appears mid-stroke inherits that touch: the last
// letter of KISS ends inside whatever now sits under the fingertip, and the lift
// clicks it. On a wiped device that meant the gesture picked CREATE or RESTORE
// by itself and the setup chooser was never seen. It was luck that kept it
// harmless before -- the 340px pills there stopped at x=388 and the stroke ends
// at 462. Rows are 716 wide and run the width of the page.
//
// The mechanism is already in this file for the decoy, which has the same
// problem for the same reason and is the only exit that ever set the flag.
// game_tick does the arming; this just says when.
static void gesture_swallow(void) {
  // Arm LVGL HERE, not only from game_tick's poll. A stroke is classified on the
  // lift, so this runs during the very release that would deliver the stray
  // click, and there is no ordering guarantee between game_tick and the input
  // device's own timer -- wait for the next tick and the click may already be
  // gone. lv_indev_wait_release clears the resolved object as well as arming the
  // flag, so a release still to be processed has nothing left to click.
  for (lv_indev_t *d = lv_indev_get_next(NULL); d; d = lv_indev_get_next(d))
    lv_indev_wait_release(d);
  s_home_swallow = true;
  s_home_swallow_t = lv_tick_get();
}

static void kiss_open_decoy(void) {
  // Still deriving. Open anyway: the home does not need the keys to be drawn,
  // only to be USED, and the fingerprint chip spends its first moments as a
  // scramble that now genuinely has nothing to resolve to yet. session_land
  // finishes the job when the other core is done, and the home refuses every
  // touch until it has. Without this the door waited on a 530ms derivation and
  // the window's length stopped mattering at all.
  if (s_prep == PREP_RUN) {
    s_open_pending = true;
    gesture_swallow();
    kiss_start();
    return;
  }
  // Already derived, or never started. This is a memcpy and a re-blind; the
  // inline derive is for the ways in that have no window at all -- the two tap
  // shortcut, and a device whose prepare task would not start.
  int rc;
  if (s_prep == PREP_DONE) {
    rc = s_prep_rc ? s_prep_rc : kiss_session_activate_prepared();
    s_prep = PREP_IDLE;
    s_prep_drop = false;
  } else {
    rc = kiss_session_open(NULL);
  }
  if (rc != 0) {                        // no seed, or derivation failed
    kiss_login_open(kiss_start);      // fall back to the ordinary way in
    return;
  }
  // Set it either way. A failed derivation used to leave whatever the last
  // session put here, which on a decoy is the real keys' fingerprint. The
  // open above is what can fail; by here there is a session, and asking IT
  // costs a hash instead of a second PBKDF2 over the whole seed.
  uint8_t fp[4] = {0};
  (void)kiss_session_fingerprint(fp);
  kiss_ui_set_last_fp(fp);
  gesture_swallow();                      // the finger may still be mid-word
  kiss_start();
}

// The other core finished while the home was already on the glass. Publish the
// session, let the scramble resolve to something true, and hand the tiles back.
static void session_land(void) {
  s_open_pending = false;
  int rc = s_prep_rc ? s_prep_rc : kiss_session_activate_prepared();
  s_prep = PREP_IDLE;
  s_prep_drop = false;
  if (rc != 0) {
    // The seed went bad between open_door's check and here -- corrupt storage,
    // not a missing one. Take the home back down rather than leave a signer on
    // screen with no keys behind it, and ask the ordinary way in.
    //
    // By hand rather than kiss_lock, which closes a SESSION and there is none to
    // close: its kiss_session_close reaches kiss_seed_forget, and on an AMNESIC
    // device that clears the only copy of the seed there is. The teardown for a
    // derivation that failed would have destroyed the words a retry needs.
    s_home_on = false;
    s_home_swallow = false;
    kiss_ui_forget_fp();              // do not go on naming keys nothing opened
    motes_stop();
    lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN);
    s_state = ST_MENU;
    lv_obj_clear_flag(s_menu_panel, LV_OBJ_FLAG_HIDDEN);
    kiss_login_open(kiss_start);      // the same hand-off stored_seed_ready makes
    return;
  }
  uint8_t fp[4] = {0};
  (void)kiss_session_fingerprint(fp);
  kiss_ui_set_last_fp(fp);
  home_fp_publish();
}

#ifdef SIMULATOR
// The simulator's way past the wizard: open a known signer.
//
// "signer" and not "wallet" -- a wallet is what a coordinator watches, and
// nothing here hands anyone a coordinator view. The identity a passphrase
// selects is a signer in this product's own words: the duress family names the
// spare signer and the real signer, and this is the same shape, opened with a
// known seed instead of a typed passphrase.
//
// Every interesting screen -- sign, receive, the keys page, settings -- sits
// behind first boot, and first boot is fifty dice rolls and a quiz. That is the
// right shape for a device somebody owns and the wrong shape for a link
// somebody was sent, who will close the tab long before they reach the part
// worth seeing.
//
// This is not a shortcut through the crypto: it stores a real mnemonic and
// opens a real session, so the fingerprint on the home screen and every address
// behind it are derived the way the device derives them. It is a shortcut past
// the COLLECTION of the words, nothing else. kiss_start is static, which is why
// this lives here rather than in the frontend -- same reason sim_capture_word
// does, and it takes the same sequence kiss_open_decoy just above uses, which
// is the one path in this file that reaches the home without a login screen.
void sim_open_signer(const char *mnemonic, const char *passphrase)
{
    if (!mnemonic) return;
    const char *pass = (passphrase && *passphrase) ? passphrase : NULL;
    kiss_seed_set_mode(WSEED_MODE_KEEP);
    if (kiss_seed_store(mnemonic) != WSEED_OK) return;
    if (kiss_session_open(pass) != 0) return;
    uint8_t fp[4] = {0};
    (void)kiss_session_fingerprint(fp);
    kiss_ui_set_last_fp(fp);
    gesture_swallow();
    kiss_start();
}
#endif

// Which signer the draw that just finished opens: 1 = the real one (ask for the
// passphrase), 0 = the decoy (open it now), -1 = not a KISS at all.
//
// The modifier has to be classified SEPARATELY from the word, because it
// changes the word's shape. An underline is wide and low, and it merges the
// letters' x-clusters into one blob -- detect_cover_word needs >=3 and would reject
// the very draw the owner meant. So the word is matched against everything
// BEFORE the final stroke, and the final stroke goes to the classifier alone.
//
// This function recognises shapes; it does not decide anything. The decision is
// kiss_duress_route, which lives in kiss_duress.c because nothing here is
// linked into a test binary.
//
// It consults kiss_duress_real() again, and the leak the audit found is closed
// at its source instead of by ignoring the setting. That leak was on the BARE
// WORD -- a configured device answered it with the decoy, an unconfigured one
// with a passphrase keyboard, so one gesture separated them. The bare word now
// answers with the decoy in every configuration, which is what leaves the
// stroke free to decide. Which stroke was drawn matters here for the first
// time, so pass the classifier's real answer and never a stand-in.
// The last answer unlock_kind gave, for the scripted walk to assert on. The
// walk cannot reliably assert on WHICH SCREEN follows -- that depends on
// storage mode, card presence and where in the walk it stands -- but the
// routing decision is the property the audit findings were about, and it is
// exactly one value.
#ifdef SIMULATOR
int g_last_unlock_kind = -2;
#endif

// Match the draw against the owner's own written word.
//
// Returns how many STROKES the word consumed, or 0 if the draw does not begin
// with it. The word's own stroke count is what says where it ends, which is the
// one measurement that survives a shaky hand: the template stores it, and
// gw_distance refuses to compare across a different one anyway.
//
// Prefix, not equality, for the same reason KISS is: whatever strokes are left
// over are the mark that picks the door.
static int written_word_match(const gw_template_t *stored)
{
    if (!stored || !stored->set || s_strokes < stored->strokes)
        return 0;
    // Points belonging to the first `stored->strokes` strokes. s_gid is 1 based
    // (the collector increments s_strokes before stamping), so the word is
    // every point whose id is <= the stored count.
    int n = 0;
    for (int i = 0; i < s_gn; i++)
        if (s_gid[i] <= stored->strokes) { s_mx[n] = s_gpt[i].x; s_my[n] = s_gpt[i].y; s_msid[n] = s_gid[i]; n++; }
    gw_template_t t;
    if (gw_make(s_mx, s_my, s_msid, n, &t) != 0)
        return 0;
    return gw_matches(stored, &t) ? stored->strokes : 0;
}

// A word used to be read as a run of classified free marks, one per stroke;
// free_marks lived here and did that. 1b24d57 replaced it with shape matching
// (written_word_match above), which asks whether the draw LOOKS like the one it
// was taught rather than trying to name each stroke, and nothing has called
// free_marks since. It stayed as a -Wunused-function warning on every device
// build. kiss_duress_classify_free, the classifier it used, is still the
// modifier reader and still under test in sim/test_duress.c.

#ifdef SIMULATOR
// Build a template from whatever the collector is holding, so the walk can
// teach the device a word through the same ink the game reads rather than by
// hand-filling the struct. Exists only here: nothing on the device needs it,
// and the enrolment screen has its own capture.
int sim_capture_word(gw_template_t *out)
{
    int n = 0;
    for (int i = 0; i < s_gn; i++) {
        s_mx[n] = s_gpt[i].x; s_my[n] = s_gpt[i].y; s_msid[n] = s_gid[i]; n++;
    }
    return gw_make(s_mx, s_my, s_msid, n, out);
}
#endif

static int unlock_kind(void) {
  // An owner who set their own word replaces KISS OUTRIGHT. detect_cover_word is not
  // consulted below, and that is the feature: a locked device shows Fruit
  // Island, the branding only exists past the unlock, and a device that does
  // not answer to the word has the honest cover story of not being a signer.
  //
  // Same two doors, same rule. The word alone opens the spare; the word plus
  // one more mark asks for the passphrase. kiss_duress_route still decides,
  // through route_marked, so the property the audit closed holds here too --
  // the door is picked by whether there was an extra mark, never by which one.
  gw_template_t stored;
  if (gw_stored_get(&stored)) {
    int used = written_word_match(&stored);
    int n = used ? s_strokes : 0;
    // used == 0 covers both a miss and an unreadable stroke, and answers the
    // same way a wrong draw always has: nothing happens and nothing is said.
    // n - used > 1 is trailing junk after a correct word, which is not a way
    // in either -- a door is the word and at most one mark, never a paragraph.
    int k = WDR_NONE;
    if (used > 0 && n - used <= 1)
      k = kiss_duress_route_marked(true, n - used == 1);
#ifdef SIMULATOR
    if (k != WDR_NONE) g_last_unlock_kind = k;
#endif
    return k;
  }
  if (s_strokes >= 5 && s_stroke_n0 >= 12 && s_gn > s_stroke_n0) {
    int bx0 = s_gpt[0].x, bx1 = bx0, by0 = s_gpt[0].y, by1 = by0;
    for (int i = 1; i < s_stroke_n0; i++) {          // bbox of the WORD only
      if (s_gpt[i].x < bx0) bx0 = s_gpt[i].x;
      if (s_gpt[i].x > bx1) bx1 = s_gpt[i].x;
      if (s_gpt[i].y < by0) by0 = s_gpt[i].y;
      if (s_gpt[i].y > by1) by1 = s_gpt[i].y;
    }
    if (detect_cover_word(s_gpt, s_stroke_n0, s_strokes - 1)) {
      int n = 0;
      for (int i = s_stroke_n0; i < s_gn; i++) {
        s_mx[n] = s_gpt[i].x; s_my[n] = s_gpt[i].y; n++;
      }
      int stroke = kiss_duress_classify(s_mx, s_my, n, bx0, by0, bx1, by1);
      if (stroke != WDG_NONE) {
        int k = kiss_duress_route(true, stroke);
#ifdef SIMULATOR
        g_last_unlock_kind = k;
#endif
        return k;
      }
      // an unrecognized final scribble is not a modifier: fall through to the
      // plain-word test below, which lands on the decoy. A scribble must never
      // be the thing that surfaces a passphrase prompt.
    }
  }
  int k = kiss_duress_route(detect_cover_word(s_gpt, s_gn, s_strokes), WDG_NONE);
#ifdef SIMULATOR
  if (k != WDR_NONE) g_last_unlock_kind = k;   // a non-word is not an answer
#endif
  return k;
}

// Where a finished way in actually lands.
//
// Every gesture that opens something comes through here, which is the whole
// reason it exists: the two tap shortcut must not reinvent the checks the draw
// already makes. No seed on here means one of two things -- an AMNESIC device
// (nothing is ever stored, so every power-on loads the seed first, then the
// normal one-passphrase login), or a fresh device that needs the whole setup
// wizard. Neither has a decoy to open, whichever gesture asked.
//
// `immediate` decides whether a decoy opens now or after the settling wait.
// COVER_OPEN_DELAY_MS exists so the bare word can still collect a modifier
// stroke, and so both doors open on the same beat and cannot be told apart by
// timing. A shortcut with one door has neither problem: nothing can follow a
// tap, and there is no second door to compare it to.
#ifndef SIMULATOR
static void prep_task(void *arg) {
  (void)arg;
  s_prep_rc = kiss_session_prepare(NULL);
  s_prep = PREP_DONE;
  vTaskDelete(NULL);
}
#endif

// Let go of a prepared session that is not going to be opened. While the task
// still runs there is nothing safe to free, so the ask is recorded and the
// sweep at the top of game_tick lands it the moment the task is done.
static void prep_drop(void) {
  if (s_prep == PREP_RUN) { s_prep_drop = true; return; }
  if (s_prep == PREP_DONE) kiss_session_discard_prepared();
  s_prep = PREP_IDLE;
  s_prep_drop = false;
}

// The word landed: start the clock, and start deriving what it opens. Nine
// places used to assign s_cover_pending by hand and every one of them would
// now have had a key to free as well, so they go through these two instead.
static void cover_pending_set(void) {
  s_cover_pending = true;
  s_cover_at = lv_tick_get();
  if (s_prep != PREP_IDLE) return;         // already deriving the same thing
  s_prep_drop = false;
  s_prep = PREP_RUN;
#ifdef SIMULATOR
  // The stub costs nothing, and that is the problem: derived instantly, the
  // provisional home never exists here, and it is the state an owner spends
  // ~440ms looking at on every single unlock. Hold it for a comparable number
  // of ticks so the walk opens it, photographs it, and the 21-locale gate has
  // an opinion about it. The device's own timing comes from a real PBKDF2.
  s_prep_rc = kiss_session_prepare(NULL);
  s_sim_prep_ticks = SIM_PREP_TICKS;
#else
  // CPU1: the main task is pinned to CPU0 (CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0),
  // so this is real overlap rather than time sliced against the display. Its
  // priority is read off the caller rather than named -- the caller IS the
  // display loop, and CONFIG_ESP_MAIN_TASK_PRIORITY does not exist on IDF 6.
  if (xTaskCreatePinnedToCore(prep_task, "kissprep", 8192, NULL,
                              uxTaskPriorityGet(NULL), NULL, 1) != pdPASS)
    s_prep = PREP_IDLE;                    // no task: the open derives inline
#endif
}

static void cover_pending_clear(void) {
  s_cover_pending = false;
  prep_drop();
}

static void open_door(int kind, bool immediate) {
  int seed_mode = kiss_seed_mode();
  if (seed_mode == WSEED_MODE_SD) {
    // An SD seed with its card removed/corrupt is still a configured seed.
    // Check it BEFORE generic seed existence; never mistake removable storage
    // for a factory-fresh device and silently offer to create over it.
    int sd_rc = kiss_setup_sd_status();
    if (sd_rc != WSEED_OK) {
      kiss_setup_open_sd_missing(lv_screen_active(), sd_rc, stored_seed_ready);
      gesture_swallow();
      cover_pending_clear();
      s_gn = 0; s_strokes = 0;
      return;                        // the retry screen owns the hand-off now
    }
  }
  if (!kiss_seed_exists()) {
    if (seed_mode == WSEED_MODE_AMNESIC)
      kiss_setup_open_load(lv_screen_active(), stored_seed_ready);
    else kiss_setup_open(lv_screen_active(), setup_done_login);
    gesture_swallow();               // the finger is still on the panel
    cover_pending_clear();
    s_gn = 0; s_strokes = 0;
    return;
  }
  if (kind == WDR_REAL) {            // a modifier stroke: ask for the passphrase
    cover_pending_clear();           // the spare's keys are not the ones wanted
    s_real_pending = true;           // same beat as the decoy: see COVER_OPEN_DELAY_MS
    s_real_at = lv_tick_get();
    gesture_swallow();
    s_gn = 0; s_strokes = 0;
    return;
  }
  if (immediate) {
    cover_pending_clear();
    kiss_open_decoy();
    s_gn = 0; s_strokes = 0;
    return;
  }
  // Bare KISS on a signer that HAS a stroke configured. Do not open anything
  // yet -- the modifier may still be coming. game_tick's idle branch opens the
  // decoy once the panel has been quiet for COVER_OPEN_DELAY_MS, and the points
  // are kept meanwhile so the next stroke can still be classified against the
  // word. The derivation the open needs starts here too, on its own core, so
  // the wait is spent rather than added to.
  cover_pending_set();
}

// ---- idle auto-lock: an unlocked signer must not sit open forever ----
// 5 min, matching the cosmetic screensaver (IDLE_MS) and Sparrow's default.
// It was 2 min since the first commit, which is the timeout people actually
// hit: reading a warning screen or checking an address against a phone takes
// longer than that, and being thrown back to the game mid-read reads as a bug.
// This is the SECURITY timeout, not the screensaver -- it drops the session key.
#define KISS_AUTOLOCK_MS 300000

// An hour where the coins are not real, for the same reason the corner opens on
// two taps there: five minutes is a timeout a testing afternoon hits over and
// over, and every one of them costs a re-entry to reach keys that guard
// nothing. It is the only knob in this file that trades security for
// convenience, so it is spelled out: on MAINNET nothing here changes.
#define KISS_AUTOLOCK_TEST_MS 3600000

static uint32_t autolock_ms(void) {
  return kiss_testnet() ? KISS_AUTOLOCK_TEST_MS : KISS_AUTOLOCK_MS;
}

#ifdef SIMULATOR
// The walk runs the real clock down rather than mocking it, so it has to know
// which deadline it is waiting for -- and asserting the two differ is what
// stops a future edit quietly giving mainnet the hour.
uint32_t sim_autolock_ms(void) { return autolock_ms(); }
#endif

// The four home tiles answer a press with NOTHING drawn.
//
// This is the third and last version of that decision, so the reasoning is
// worth keeping. It began as a glass pane that sat under the finger for as
// long as the finger was down, which on a tile you hold for half a second
// reads as the screen having got stuck. That was replaced by one 190ms accent
// flash, on the theory that a press has to be acknowledged somehow. It does
// not. The acknowledgement IS the screen changing, and on a target this large
// there is no ambiguity about what you hit: a flash between the touch and the
// new screen is one more thing happening in a place the eye is already leaving.
//
// s_tile_pend stays and has nothing to do with any of this. It is the latch
// that makes a tile open on RELEASE rather than on touch, so a finger that
// lands on the wrong tile can slide off it and let go without opening it.
// Home and KEYS use the same fingerprint explainer. The delete event clears
// this input gate whether the card closes by its OK action or by tapping outside.
static void fp_card_deleted_cb(lv_event_t *e) {
  (void)e;
  s_fp_card = NULL;
}

static void fp_card_open(void) {
  if (s_fp_card) return;
  s_fp_card = kiss_info_fp_card_open(s_home, s_fp_hex, true);
  if (s_fp_card)
    lv_obj_add_event_cb(s_fp_card, fp_card_deleted_cb, LV_EVENT_DELETE, NULL);
}

// ---- the screen registry ---------------------------------------------------
//
// One row per screen that can be over the home, because keeping the same
// facts in three hand-maintained lists is what has now failed twice. The
// A dead touch panel is invisible: the screen draws perfectly and ignores
// every finger, and nothing on it says so. Worn by the game cover in the
// game's own voice -- a hardware complaint reveals nothing about what else
// this box is -- with the one instruction that always applies here: replug
// (this board never boots off a USB reset, so replug IS the restart). On an
// update's first boot the withheld mark_valid turns that same replug into
// the rollback that brings back the firmware whose touch worked.
// Returns the label so the walk can photograph it and take it back down.
// A chip, not a line of text: this sits on the game's artwork, where bare
// amber words are unreadable and unframed (kit rule 1). wt_state_chip is the
// same framed mark Settings and the words page wear for a status fact, and it
// self sizes, so 21 locales need no geometry here.
lv_obj_t *kiss_touch_dead_banner(lv_obj_t *parent)
{
  lv_obj_t *chip = wt_state_chip(parent,
                                 tr_sym(LV_SYMBOL_WARNING, STR_G_TOUCH_DEAD),
                                 WT_WARN);
  // Opaque, unlike everywhere else the chip is used: those sit on a screen
  // background, this sits on the game's artwork, and the kit's translucent
  // fill let a palm tree through the middle of the sentence.
  lv_obj_set_style_bg_color(chip, lv_color_hex(0x0B0D12), 0);
  lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
  // font23, overriding the chip default. A state chip is a MARK and font14 is
  // right for one -- but this chip is the only thing on the screen when the
  // touch panel is dead, and it is the instruction that gets the owner out of
  // that state. The chip self sizes, so nothing else moves.
  lv_obj_set_style_text_font(chip, wt_font23(), 0);
  lv_obj_update_layout(chip);
  // Bottom edge, not the top: the top is where the game's own title art is,
  // and the fault does not get to cover the cover story.
  lv_obj_set_pos(chip, (SCREEN_W - lv_obj_get_width(chip)) / 2,
                 SCREEN_H - lv_obj_get_height(chip) - 12);
  lv_obj_move_foreground(chip);
  return chip;
}

// firmware screen was in NEITHER the auto-lock list nor the touch owner list,
// which is how the idle lock left it lit with RECOVERY WORDS two taps away.
// kiss_word_ui was missing from the touch owner list before it, so the game's
// sampler read the same strokes the writing canvas did and opened tiles
// underneath a screen that still looked correct.
//
// Neither was hard to fix and neither was ever going to be noticed: adding a
// screen meant editing code that does not mention the screen. A row here is the
// whole registration.
//
// ORDER IS PART OF THE DATA. Scan closes first so the camera stops before
// anything else runs, and firmware closes before settings because its close
// deliberately does not hand control back the way its BACK does.
static const struct {
  bool (*active)(void);
  void (*close)(void);       // NULL: nothing to tear down, only to notice
  bool owns_touch;           // LVGL buttons; the game must not read the same finger
  bool holds_lock_off;       // exempt from the idle auto-lock, on purpose
  // "Exempt" must not mean "forever" when a SECRET is idling on the glass.
  // secret_idle_ms is that screen's own deadline; 0 is exempt and contributes
  // NOTHING to the countdown -- never an instant expiry, or the RECOVER row
  // below strands the last copy of a seed, which is the bug this registry
  // exists to prevent. idle_expire wipes the secret or drops the screen; it
  // never fires a done callback and never touches the seed staging.
  // idle_needs_session: the same wizard runs inside setup (nothing to lock,
  // the staged seed lives behind it) and from Settings (session key live);
  // the deadline only counts in the second case.
  uint32_t secret_idle_ms;
  bool idle_needs_session;
  // idle_keeps_session: expiry wipes this row's secret and stops there. The
  // ordinary expiry locks an open session afterwards, which is right for a
  // typed passphrase and fatal for RECOVER -- the lock would close the one
  // screen naming a staged seed that may exist nowhere else. A row that
  // holds the last copy sets this, and its expiry is a wipe, not a teardown.
  bool idle_keeps_session;
  void (*idle_expire)(void);
} SCREENS[] = {
  // Wizards and login. They own the touch AND hold the clock off: writing
  // twelve words onto paper takes minutes of a screen nobody is touching.
  // The login is the exception that proves it: a typed passphrase is not a
  // thing to read slowly, so it alone gets a short deadline at every stage.
  // The deadline is suppressed while the RECOVER screen is up: its retry
  // keeps the passphrase in s_pass, and a wipe would make TRY AGAIN open an
  // empty-passphrase keys beneath the stale fingerprint.
  { kiss_ui_login_deadline_active, NULL,         true,  true,  120000, false, false, kiss_ui_idle_wipe },
  { kiss_setup_active,     NULL,                  true,  true,  0,      false, false, NULL },
  { kiss_duress_ui_active, NULL,                  true,  true,  300000, true,  false, kiss_duress_ui_lock_close },
  { kiss_word_ui_active,   NULL,                  true,  true,  300000, true,  false, kiss_word_ui_lock_close },
  // The commit-failed RECOVER screen, and the words screen it opens. Same
  // two trues for the same reason, plus one of its own: the staged seed it
  // names may be the last copy anywhere, so it registers no close -- the
  // lock must neither fire under it nor take it away.
  //
  // Its deadline is the passphrase alone. The words behind SHOW WORDS have
  // no deadline and must not gain one; the typed passphrase the retry keeps
  // is a different thing, and leaving it in RAM for as long as the screen
  // stands was the one place on the device where a secret idled forever.
  // Five minutes is what the other read-slowly screens already use. On
  // expiry the wipe touches nothing else, and TRY AGAIN asks for the
  // passphrase again instead of opening keys nobody chose.
  { kiss_ui_recover_active, NULL,                 true,  true,  300000, false, true,  kiss_ui_idle_wipe },
  // Home sub-screens. They own the touch and the lock takes them away.
  { kiss_scan_active,      kiss_scan_close,     true,  false, 0, false, false, NULL },
  { kiss_sign_active,      kiss_sign_close,     true,  false, 0, false, false, NULL },
  { kiss_recv_active,      kiss_recv_close,     true,  false, 0, false, false, NULL },
  { kiss_info_active,      kiss_info_close,     true,  false, 0, false, false, NULL },
  { kiss_fw_ui_active,     kiss_fw_ui_close,    true,  false, 0, false, false, NULL },
  { kiss_rngaudit_active,  kiss_rngaudit_close, true,  false, 0, false, false, NULL },
  { kiss_settings_active,  kiss_settings_close, true,  false, 0, false, false, NULL },
};
#define N_SCREENS (sizeof SCREENS / sizeof SCREENS[0])

static void game_tick(lv_timer_t *t) {
  (void)t;
  int tx = 0, ty = 0;
  bool pressed = read_touch(&tx, &ty);
  if (pressed) rng_seed(tx, ty);   // consumes no randomness; ahead of every branch

  // A prepared session nobody is going to open, freed the moment its task is
  // done writing it. Above every early return below: the ask can outlive the
  // menu (the login is already up by then), and the key must not outlive it.
#ifdef SIMULATOR
  if (s_sim_prep_ticks && --s_sim_prep_ticks == 0) s_prep = PREP_DONE;
#endif
  if (s_prep_drop && s_prep != PREP_RUN) prep_drop();
  // ...and the opposite: a home already up, waiting for the same task.
  if (s_open_pending && s_prep == PREP_DONE) session_land();

  // Swallow the rest of the touch that opened a screen. Above every early
  // return below, because the screens this protects -- the setup wizard, the
  // login -- are exactly the ones that make game_tick bail out immediately.
  //
  // WHILE PRESSED ONLY, and re-armed every tick. lv_indev_wait_release clears
  // its own flag on the next press cycle, so arming it once skips a single
  // frame and LVGL resolves on the frame after; that is why the one-shot
  // attempts at screen-creation time did nothing. Arming it after the finger is
  // up is the opposite mistake -- the flag would then be waiting to eat the
  // owner's next real tap.
  //
  // Cleared the instant the finger is up, with no quiet period. The MULTI-STROKE
  // case -- the decoy opens on a lift and the next stroke of the same word lands
  // milliseconds later -- is already handled by the 400ms in the s_home_on
  // branch below, which is the only situation it can happen in. Holding the flag
  // here for a grace period as well would swallow a real tap: the screens this
  // protects are ones the owner starts using immediately, and the setup chooser
  // lost its first tap to exactly that.
  if (s_home_swallow) {
    if (pressed) {
      for (lv_indev_t *d = lv_indev_get_next(NULL); d; d = lv_indev_get_next(d))
        lv_indev_wait_release(d);
      s_home_swallow_t = lv_tick_get();
    } else if (!s_home_on) {
      s_home_swallow = false;      // the home's own branch owns its timing
    }
  }

  // kiss_word_ui_active was MISSING here, and it is the one screen on the
  // device where the owner drags a finger across the panel on purpose. Without
  // it the game's sampler read the same strokes the writing canvas was reading,
  // and its own recogniser then opened whatever was under them -- the walk
  // caught it opening Receive and the Sign chooser UNDERNEATH a write screen
  // that still looked correct. Nothing had ever drawn on that screen: it had no
  // walk stop, which is the only reason a bug this loud survived.
  bool lock_held_off = false;
  for (size_t i = 0; i < N_SCREENS; i++)
    if (SCREENS[i].holds_lock_off && SCREENS[i].active()) { lock_held_off = true; break; }
  if (lock_held_off) {                                        // login/wizard own the touch
    // The menu is buried; its fruit must stop drifting. This is the hook and not
    // the menu panel's hidden flag because the wizard opens OVER the menu with
    // the panel still visible, which is how the drift reached the camera preview
    // in the first place. Idempotent, so running it every tick costs one bool.
    menu_idle_drift_stop();
    // VERIFY BACKUP runs the setup module DURING a session; keep the idle clock
    // fresh so finishing a long word-entry doesn't insta-lock on return.
    if (s_home_on && pressed) s_home_act_t = lv_tick_get();

    // The secret-idle deadline. Held-off rows are exempt from the auto-lock on
    // purpose, but a typed passphrase or a half-enrolled unlock word is a
    // secret sitting on powered glass, and its row carries its own deadline.
    // Rows at 0 contribute nothing (exempt means exempt -- a min() over the
    // zeros would expire the RECOVER screen instantly, stranding the last copy
    // of a seed). The clock resets on any touch and whenever the set of
    // deadline rows changes, so a screen never inherits the previous screen's
    // spent minutes. Expiry runs each due row's idle_expire -- wipe or drop,
    // never a done callback -- and then locks only a session actually open,
    // through the same close loop the auto-lock uses.
    uint32_t deadline = 0, rows = 0;
    for (size_t i = 0; i < N_SCREENS; i++) {
      if (!SCREENS[i].secret_idle_ms || !SCREENS[i].active()) continue;
      if (SCREENS[i].idle_needs_session && !s_home_on) continue;
      rows |= 1u << i;
      if (!deadline || SCREENS[i].secret_idle_ms < deadline)
        deadline = SCREENS[i].secret_idle_ms;
    }
    if (rows != s_secret_rows || pressed) {
      s_secret_rows = rows;
      s_secret_act_t = lv_tick_get();
    } else if (deadline && lv_tick_elaps(s_secret_act_t) > deadline) {
      bool keep_session = false;
      for (size_t i = 0; i < N_SCREENS; i++)
        if ((rows & (1u << i)) &&
            lv_tick_elaps(s_secret_act_t) > SCREENS[i].secret_idle_ms) {
          SCREENS[i].idle_expire();
          // One expired row is enough to stop the lock: it is up because it
          // holds the last copy of something, and locking would take the
          // screen naming it away. Its secret is already gone.
          keep_session |= SCREENS[i].idle_keeps_session;
        }
      if (s_home_on && !keep_session) {
        for (size_t i = 0; i < N_SCREENS; i++)
          if (SCREENS[i].close && SCREENS[i].active())
            SCREENS[i].close();
        kiss_lock();
      }
      s_secret_act_t = lv_tick_get();
    }

    s_prev_press = pressed;          // (LVGL indev); the game must not also see it
    return;                          // (and are exempt from auto-lock: writing the
  }                                  //  backup words down takes minutes, untouched)

  // The owner's door, held to the decoy's timing. Everything is swallowed until
  // it opens: the points are gone, so a tap landing in here would otherwise
  // reach the menu's "tap to play" branch and start a game under the login.
  if (s_real_pending) {
    // Neither door waits on the spare's derivation now -- this one never wanted
    // it, and the other opens over the top of it. Both are the window and
    // nothing else, which is what made the window worth shortening.
    if (lv_tick_elaps(s_real_at) >= COVER_OPEN_DELAY_MS) {
      s_real_pending = false;
      s_gest_idle = 0;
      kiss_login_open(kiss_start);
    }
    s_prev_press = pressed;
    return;
  }

  if (s_home_on) {                              // on the home: tap the KISS logo to lock
    // Drawn, but not yet a signer. Every tile behind this reads a session that
    // does not exist for another few hundred milliseconds. s_home_swallow would
    // cover most of it by accident; this covers it on purpose.
    if (s_open_pending) {
      s_home_act_t = lv_tick_get();
      s_prev_press = pressed;
      return;
    }
    // Waiting for a plain finger-lift is not enough: the decoy opens ON a lift
    // (the end of one stroke), so the flag would clear before the NEXT stroke
    // of the same word arrived -- and that stroke is the one that lands on a
    // tile. Swallow until the panel has actually been quiet.
    if (s_home_swallow) {
      if (pressed) s_home_swallow_t = lv_tick_get();
      else if (lv_tick_elaps(s_home_swallow_t) > 400) s_home_swallow = false;
      s_home_act_t = lv_tick_get();
      s_prev_press = pressed;
      return;
    }
    if (pressed) {
      s_home_act_t = lv_tick_get();  // any touch anywhere resets the clock
      if (s_lock_warn) { lv_obj_delete(s_lock_warn); s_lock_warn = NULL; }
    }
    else if (lv_tick_elaps(s_home_act_t) > autolock_ms()) {
      if (s_lock_warn) { lv_obj_delete(s_lock_warn); s_lock_warn = NULL; }
      // Everything the registry says the lock owns, in the order it lists them:
      // the camera stops first, and firmware goes before settings because its
      // close deliberately does not hand control back the way its BACK does. A
      // screen is torn down here by having a row, which is the point of the row.
      for (size_t i = 0; i < N_SCREENS; i++)
        if (SCREENS[i].close && SCREENS[i].active())
          SCREENS[i].close();
      kiss_lock();                              // session key leaves RAM
      s_prev_press = pressed;
      return;
    }
    else if (lv_tick_elaps(s_home_act_t) > autolock_ms() - 30000 &&
             !s_lock_warn) {
      // The lock announces itself. Without this, five quiet minutes ended in
      // the screen simply becoming the game -- correct, and indistinguishable
      // from a crash for someone mid-read on an explainer, which is the one
      // way to be idle while USING the device. Thirty seconds of notice turns
      // the surprise into a choice: any touch anywhere keeps the session (the
      // same touch that always reset the clock), and ignoring it locks as
      // before. On the top layer, so it floats over whatever screen is up and
      // needs no screen's cooperation; deleted on the touch that dismisses it,
      // on the lock it precedes, and by kiss_lock's own layer sweep.
      // A DIM, not just a toast -- the idiom every phone taught: a darkened
      // screen means "about to sleep" before any word is read, in every
      // locale. The scrim is also the safety half of the design. The raw
      // handler above dismisses on any press, but that same press flows on
      // into LVGL and lands on whatever control is under the finger -- so
      // "tap to stay open" was an invitation to press a live row, or start a
      // hold, blind. CLICKABLE on the scrim swallows the press before any
      // control beneath sees it: the waking tap wakes, and does nothing else.
      // (The dismiss itself never depended on LVGL -- the handler reads the
      // panel directly -- which is why swallowing costs nothing.)
      s_lock_warn = lv_obj_create(lv_layer_top());
      lv_obj_remove_style_all(s_lock_warn);
      lv_obj_set_size(s_lock_warn, LV_PCT(100), LV_PCT(100));
      lv_obj_set_style_bg_color(s_lock_warn, lv_color_black(), 0);
      lv_obj_set_style_bg_opa(s_lock_warn, LV_OPA_60, 0);
      lv_obj_add_flag(s_lock_warn, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_remove_flag(s_lock_warn, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_t *card = lv_obj_create(s_lock_warn);
      lv_obj_remove_style_all(card);
      lv_obj_set_size(card, 420, 56);
      lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 8);
      lv_obj_set_style_radius(card, 10, 0);
      lv_obj_set_style_bg_color(card, WT_PANEL, 0);
      lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(card, 2, 0);
      lv_obj_set_style_border_color(card, WT_WARN, 0);
      lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_t *l = wt_lbl(card, tr(STR_C_LOCK_SOON), 0, 0,
                           wt_font23(), WT_WARN);
      lv_obj_center(l);
    }
    // The scan screen gets one escape that does NOT go through LVGL. Its own
    // CLOSE is an LVGL control, and while the camera streams it is painted over
    // by a direct-to-panel video path; on a real board that button did nothing
    // and the only way out was pulling the power. This handler reads the same
    // touch the unlock gesture reads, so it is on a path known to work here.
    // Same top-left corner CANCEL once occupied, so nothing new has to be learned.
    if (kiss_scan_active() && pressed && !s_prev_press && tx < 200 && ty < 110) {
      kiss_scan_cancel();
      s_prev_press = pressed;
      return;
    }
    // NO corner lock on home sub-screens, and no × drawn in that corner.
    // SWEEP-01 edit 4 put one on all four of them, reasoning that H_EXIT_HINT
    // promises the gesture works "any time". The owner's answer, having lived
    // with it: every one of those screens has a BACK, BACK is the way out people
    // reach for, and a second unlabelled exit that skips past the level above and
    // lands in the GAME is a way to lose your place by brushing the glass.
    //
    // The two corners that survive both earn it. Above: the scan screen, where
    // the CANCEL control is LVGL and the live camera paints over LVGL, so on a real
    // board that control can be dead and this is the only escape. Below: the home,
    // which has no BACK to reach for.
    // Same table, the other question it answers. The firmware screen was absent
    // here as well as from the lock, so the game's own sampler read the finger
    // holding INSTALL and its recogniser opened whatever sat under the stroke.
    // s_fp_card is not a screen module -- it is a card built inline on the home
    // -- so it keeps its own test.
    bool owned = s_fp_card;
    for (size_t i = 0; !owned && i < N_SCREENS; i++)
      if (SCREENS[i].owns_touch && SCREENS[i].active()) owned = true;
    if (owned) {
      s_prev_press = pressed;            // home sub-screens own the touch (LVGL buttons)
      return;
    }
#ifndef SIMULATOR
    // camera spike (step 2): the Sign tile opens the live view (Sign = scan a QR
    // later, so the camera belongs here). While live: top-left corner CLOSES the
    // camera (the idle lock is suspended so a stray gesture can't dump you to the
    // game), top-right corner cycles the orientation finder, vertical drag on
    // either screen edge zooms, and taps elsewhere do nothing.
    static int s_zoom_anchor; static bool s_zoom_drag;
    if (camera_spike_check_died()) {                          // stream errored out:
      lv_obj_invalidate(lv_screen_active());                  // repaint the home UI
      if (s_cam_lbl) lv_label_set_text(s_cam_lbl, camera_spike_status());
    }
    bool cam_on = camera_spike_is_on();
    // hot-plug SD indicator: while idle on the home screen, poll for a card and
    // briefly flash "SD card ready" in the bottom-center slot on INSERT (a short
    // toast, not an always-on label). Cheap when a card is mounted; a mount
    // attempt (only when none is present) can briefly block, but the home art is
    // static so a hitch never shows.
    static int s_sd_tick; static bool s_sd_present; static int s_sd_toast;
    static bool s_sd_badge_live;                     // SD mode + card in: breathe it
    if (!cam_on && ++s_sd_tick >= 90) {              // poll ~1.5s at TICK_MS
      s_sd_tick = 0;
      bool present = platform_sd_probe() != 0;
      if (present && !s_sd_present && s_cam_lbl) {   // just inserted: show the toast
        lv_label_set_text(s_cam_lbl, tr(STR_S_SD_READY));
        lv_obj_set_style_text_color(s_cam_lbl, wt_accent(), 0);
        s_sd_toast = 3;                              // ~3 polls (~4.5s) then fade
      } else if (s_sd_toast > 0 && --s_sd_toast == 0 && s_cam_lbl) {
        lv_label_set_text(s_cam_lbl, "");
        lv_obj_set_style_text_color(s_cam_lbl, WT_MUT, 0);  // reset: no green leak
      }
      s_sd_present = present;
      sd_badge_sync(present);                        // persistent SD-storage badge
    }
    // Gentle opacity breathe on the SD badge while the card is present, so it
    // reads as a live link to the card rather than a static label. Opacity only
    // (never transform_scale), so it forces no draw layer.
    if (s_sd_badge_live) {
      static uint32_t sd_anim;
      uint32_t ph = ++sd_anim % 120;                 // ~2s loop at TICK_MS
      uint32_t tri = ph < 60 ? ph : 120 - ph;        // 0..60..0
      lv_obj_set_style_opa(s_sd_badge, (lv_opa_t)(180 + tri * 75 / 60), 0);
    }
    if (!cam_on && pressed && !s_prev_press && tx < 88 && ty < 88) {
      kiss_lock();
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
          camera_spike_toggle(s_home, s_i2c_bus);
          if (s_cam_lbl) lv_label_set_text(s_cam_lbl, "");    // don't leave dev status on home
        } else if (tx >= 600) {                               // top-right: orientation
          camera_spike_cycle_orientation();
        }
      }
    } else if (pressed && !s_prev_press && tx >= 566 && ty < 110) {
      s_fp_pend = true;                  // fingerprint chip: open the card on release
    } else if (pressed && !s_prev_press &&
               tx >= 40 && tx <= 220 && ty >= 140 && ty <= 340) {  // Sign tile
      s_tile_pend = 1;
    } else if (pressed && !s_prev_press &&
               tx >= 230 && tx <= 390 && ty >= 140 && ty <= 340) { // Receive tile
      s_tile_pend = 2;
    } else if (pressed && !s_prev_press &&
               tx >= 410 && tx <= 570 && ty >= 140 && ty <= 340) { // Keys tile: export
      s_tile_pend = 3;
    } else if (pressed && !s_prev_press &&
               tx >= 590 && tx <= 750 && ty >= 140 && ty <= 340) { // Settings tile
      s_tile_pend = 4;
    } else if (!pressed && s_prev_press && s_fp_pend) {           // finger lifted: card
      s_fp_pend = false;
      fp_card_open();
    } else if (!pressed && s_prev_press && s_tile_pend) {          // finger lifted: open
      int t = s_tile_pend;
      s_tile_pend = 0;
      if (t == 1) kiss_sign_open(lv_screen_active());
      else if (t == 2) kiss_recv_open(lv_screen_active());
      else if (t == 3) kiss_info_open(lv_screen_active());
      else kiss_settings_open(lv_screen_active());
    }
    if (!pressed) s_zoom_drag = false;
#else
    if (pressed && !s_prev_press && tx < 88 && ty < 88) kiss_lock();
    else if (pressed && !s_prev_press && tx >= 566 && ty < 110)
      s_fp_pend = true;                  // fingerprint chip: open the card on release
    else if (pressed && !s_prev_press && tx >= 40 && tx <= 220 && ty >= 140 && ty <= 340)
      s_tile_pend = 1;
    else if (pressed && !s_prev_press && tx >= 230 && tx <= 390 && ty >= 140 && ty <= 340)
      s_tile_pend = 2;
    else if (pressed && !s_prev_press && tx >= 410 && tx <= 570 && ty >= 140 && ty <= 340)
      s_tile_pend = 3;
    else if (pressed && !s_prev_press && tx >= 590 && tx <= 750 && ty >= 140 && ty <= 340)
      s_tile_pend = 4;
    else if (!pressed && s_prev_press && s_fp_pend) {
      s_fp_pend = false;
      fp_card_open();
    } else if (!pressed && s_prev_press && s_tile_pend) {
      int t = s_tile_pend;
      s_tile_pend = 0;
      if (t == 1) kiss_sign_open(lv_screen_active());
      else if (t == 2) kiss_recv_open(lv_screen_active());
      else if (t == 3) kiss_info_open(lv_screen_active());
      else kiss_settings_open(lv_screen_active());
    }
#endif
    s_prev_press = pressed;
    return;
  }

  if (s_state != ST_PLAY) {
    if (pressed) {
      if (s_saver_on) { saver_hide(); s_gest_swallow = true; s_gn = 0; s_strokes = 0; cover_pending_clear(); }  // wake saver
      else if (!s_gest_swallow) {
        if (!s_prev_press) {                            // a new stroke begins
          // ...but NOT while the word is already matched and waiting for a
          // modifier. An underline, a strike, a circle -- every one of them
          // begins at the LEFT edge of the word it modifies, which is exactly
          // what this heuristic reads as "starting a fresh K". It threw the
          // whole draw away and the configured stroke could never land.
          //
          // And NOT when the owner has a word of their own. This heuristic is
          // built on KISS being written left to right; free marks are drawn
          // wherever there is room, so a second mark to the left of the first
          // is ordinary rather than a restart. It threw away every custom word
          // whose marks were not in left-to-right order, which is most of them.
          // What still catches a genuinely abandoned attempt on those devices
          // is the same thing that always did: the idle clear.
          if (s_gn > 0 && !s_cover_pending &&
              !gw_stored_any()) {                      // drop a stale prior attempt if this stroke
            int mx = -9999;                             // starts well LEFT of how far right we'd
            for (int i = 0; i < s_gn; i++)              // reached: KISS is drawn L->R, so only a
              if (s_gpt[i].x > mx) mx = s_gpt[i].x;     // RESTART (a fresh K) begins far to the left.
            if (tx < mx - 160) { s_gn = 0; s_strokes = 0; }  // -> no points bleeding between tries
          }
          s_stroke_n0 = s_gn; s_strokes++;
        }
        // Decimate: store a sample only when it actually MOVED (>=10px) from the
        // last stored point. Samples arrive every tick (~60/s), so without this a
        // slow, careful draw fills the buffer in ~2.5s and the trailing letters
        // are silently dropped -- the classic "I drew KISS perfectly and nothing
        // happened" failure. Decimation bounds points by ink length, not time.
        if (s_gn < GEST_MAX &&
            (s_gn == s_stroke_n0 ||
             LV_ABS(tx - s_gpt[s_gn - 1].x) >= 10 ||
             LV_ABS(ty - s_gpt[s_gn - 1].y) >= 10)) {
          s_gpt[s_gn].x = tx; s_gpt[s_gn].y = ty;
          s_gid[s_gn] = (uint8_t)s_strokes; s_gn++;
        }
      }
      s_gest_idle = 0; s_idle_ms = 0;
    } else {
      if (s_prev_press) {                                    // a touch just lifted
        if (s_gest_swallow) { s_gest_swallow = false; s_gn = 0; s_strokes = 0;
                                cover_pending_clear(); s_real_pending = false; }
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
            // restarts; the MENU button -- and any stray tap -- returns to the menu
            if (x1 >= 220 && x1 <= 580 && y1 >= 356 && y1 <= 458) start_game();
            else go_menu();
            s_gn = 0; s_strokes = 0;
          } else if (tap) {
            // The two tap way in, and only where the coins are not real. The
            // corner has to stop starting the game as well as answer the pair:
            // a first tap that launched Fruit Island would leave ST_MENU before
            // the second one arrived, and the collector is skipped in ST_PLAY.
            if (kiss_testnet() && s_state == ST_MENU && cw_quick_zone(x1, y1)) {
              if (cw_quick_tap(x1, y1, lv_tick_get())) open_door(WDR_DECOY, true);
              s_gn = 0; s_strokes = 0;
            } else {
              start_game(); s_gn = 0; s_strokes = 0;          // menu: a tap -> play
            }
          } else {
            int kind = unlock_kind();
            if (kind >= 0) open_door(kind, false);
          }                                                  // else: keep, await more strokes (3s clears)
        }
        s_gest_idle = 0;
      } else if (s_gn > 0) {                                 // mid-draw, finger up
        s_gest_idle += TICK_MS;
        // lv_tick, not the tick COUNT s_gest_idle keeps. The two doors are
        // meant to open on the same beat and the passphrase one has always
        // used lv_tick_elaps, so a stalled UI task already pushed this one
        // later than that one -- 540ms against 500ms with nothing else
        // running. It also has to survive the derivation now overlapping it.
        if (s_cover_pending && lv_tick_elaps(s_cover_at) >= COVER_OPEN_DELAY_MS) {
          s_cover_pending = false;                            // no modifier came: the spare
          kiss_open_decoy();                                  // ...whose keys are ready
          s_gn = 0; s_strokes = 0; s_gest_idle = 0;
        } else if (s_gest_idle >= 3000) {
          s_gn = 0; s_strokes = 0; cover_pending_clear();     // gave up -> clear (never starts game)
        }
      } else {
        s_idle_ms += TICK_MS;
        if (s_idle_ms >= IDLE_MS) saver_show();              // idle -> attract mode
      }
    }
    s_prev_press = pressed;
    return;
  }
  s_idle_ms = 0;
  // The banner lands when the combo ENDS, so a four-fruit swipe reads as one
  // "4 FRUIT +6" instead of four racing +1s.
  if (s_combo_n >= 2 && lv_tick_elaps(s_combo_t) > COMBO_MS) {
    int nn = s_combo_n > COMBO_MAX ? COMBO_MAX : s_combo_n;
    int bonus = COMBO_BONUS[nn];
    s_score += bonus;
    lv_label_set_text_fmt(s_score_lbl, "%d", s_score);
    char b[28];
    snprintf(b, sizeof b, "%d FRUIT  +%d", s_combo_n, bonus);
    score_popup((int)s_combo_x - 60, (int)s_combo_y - 30, b, 0xFFD23A);
    s_combo_n = 0;
  }
  if (s_frenzy_ms > 0) {
    s_frenzy_ms = (s_frenzy_ms > TICK_MS) ? s_frenzy_ms - TICK_MS : 0;
  }
  s_prev_press = pressed;

  update_blade(tx, ty, pressed);
  check_slices(pressed);

  float fg = fruit_gravity(diff_progress());   // one read, not one per entity
  for (int i = 0; i < MAX_ENT; i++) {
    ent_t *e = &s_ent[i];
    if (!e->active) continue;
    e->vy += (e->kind == K_FRUIT) ? fg : GRAVITY * 1.7f;  // debris falls faster -> clears the play area sooner
    if (e->av != 0.0f) {
      e->rot += e->av;
      if (e->rot >= 360.0f) e->rot -= 360.0f;
      else if (e->rot < 0.0f) e->rot += 360.0f;
    }
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

// A fixed period throwing one or two random fruit is a drizzle. The real game
// AUTHORS its throws: mostly small waves, an occasional big one, and quiet
// AFTER the big one so there is release as well as tension.
enum { WV_SINGLE, WV_ARC, WV_FOUNTAIN, WV_PINCER, WV_BIG };

static int pick_wave(float p) {
  int r = (int)rnd(100);
  if (r < 34) return WV_SINGLE;
  if (r < 56) return WV_ARC;
  if (r < 74) return WV_FOUNTAIN;
  if (r < 90) return WV_PINCER;
  return (p > 0.25f) ? WV_BIG : WV_SINGLE;   // big waves only once warmed up
}

static void spawn_tick(lv_timer_t *t) {
  if (s_state != ST_PLAY) return;
  float p = diff_progress();                   // 0 -> 1 smoothly over ~3.5 min

  if (s_frenzy_ms > 0) {                       // dense, no bombs, no patterns
    lv_timer_set_period(t, live_fruit() >= MAX_LIVE_FRUIT ? 260 : 130);
    s_wave_xn = 0;
    spawn_fruit_idx(pick_fruit());
    return;
  }
  if (s_wave_rest > 0) {                       // the breather after a big one
    s_wave_rest--;
    lv_timer_set_period(t, 520);
    return;
  }

  s_wave_xn = 0;                               // fresh set of separated lanes
  int w = pick_wave(p);

  // Hold the line at MAX_LIVE_FRUIT. A pattern that will not fit is thrown as
  // a single rather than half drawn, and with no room at all the wave is
  // skipped and tried again shortly. A thin second beats a laggy one.
  static const int WAVE_N[] = {1, 3, 3, 2, 5};
  int room = MAX_LIVE_FRUIT - live_fruit();
  if (room <= 0) { lv_timer_set_period(t, 260); return; }
  if (WAVE_N[w] > room) w = WV_SINGLE;
  float vy0 = -(float)launch_speed(p);
  int n;                                       // fruit this wave throws

  switch (w) {
    case WV_ARC: {                             // thrown left to right
      for (int i = 0; i < 3; i++)
        spawn_fruit_at(pick_fruit(), false,
                       140.0f + i * ((SCREEN_W - 280.0f) / 2.0f),
                       (i - 1) * 1.4f, vy0 - i * 0.8f);
      n = 3;
    } break;
    case WV_FOUNTAIN: {                        // one point, fanning out
      float x = rnd_range(180, SCREEN_W - 180);
      for (int i = 0; i < 3; i++)
        spawn_fruit_at(pick_fruit(), false, x, (i - 1) * 3.2f, vy0 - i * 1.2f);
      n = 3;
    } break;
    case WV_PINCER: {                          // opposite sides, crossing
      spawn_fruit_at(pick_fruit(), false, 110, 2.6f, vy0);
      spawn_fruit_at(pick_fruit(), false, SCREEN_W - 110, -2.6f, vy0 - 0.6f);
      n = 2;
    } break;
    case WV_BIG: {
      for (int i = 0; i < 5; i++)
        spawn_fruit_at(pick_fruit(), false,
                       90.0f + i * ((SCREEN_W - 180.0f) / 4.0f),
                       (i - 2) * 1.1f, vy0 - (i % 2) * 1.6f);
      n = 5;
      s_wave_rest = 1;
    } break;
    default:
      spawn_fruit_idx(pick_fruit());
      n = 1;
      break;
  }

  // A wave that throws n fruit waits n times as long, on the SAME 820 -> 620ms
  // curve the game used before it had patterns at all.
  //
  // Shipped per wave, the patterns were not a new shape of throw, they were a
  // density rise nobody asked for: 1.9 fruit/s at the start against 1.2 before
  // (+57%), and 2.8 against 1.7 by mid game (+67%). Two things go wrong at
  // once. It is unreadable -- three fruit arrive as a clump and the bomb that
  // matters arrives inside it -- and it is past what the panel will push,
  // because a rotating sprite invalidates its rotated bounding box and those
  // merge: on the bench every flush was a full 800x48 band, 58 of them a
  // second, where the same scene without rotation flushed 240 sprite-sized
  // rects. Same pixels per second either way. Far fewer frames.
  int period = (int)((820 - 200 * p) * n);
  if (w == WV_BIG) period -= 520;              // s_wave_rest already pays that

  // Bombs per second, not per wave, and on the old 6 -> 34% ramp. A three
  // fruit wave that waits three times as long rolls a third as often, so the
  // per-wave form quietly halved the one thing making this game hard.
  if (w != WV_BIG && (int)rnd(100) < (int)((6 + 28 * p) * n))
    spawn_fruit_idx(BOMB_IDX);                 // never on a big wave: five
                                               // fruit and a bomb is not a
                                               // harder wave, it is an
                                               // unreadable one

  // Gold, rare, and never in the same wave as a big throw. NOT scaled by n
  // the way the bomb is: cutting one buys 3.2s of frenzy throw, so its rate is
  // per wave on purpose. Scaling it tripled how often the densest thing in the
  // game fired, and that is what "at 100 there is fruit everywhere" was.
  if (s_score > 20 && w != WV_BIG && (int)rnd(100) < 4)
    spawn_fruit_at(3, true, pick_spawn_x(), 0.0f, vy0 - 1.0f);

  lv_timer_set_period(t, period);
}

static void storage_locked_screen(lv_obj_t *root,
                                  kiss_settings_load_status_t status) {
  static const char *BODY =
      "Storage could not be opened. Your keys and any SD card device key "
      "may still be intact.\n\n"
      "Do not erase storage or set up again. Power off, then reinstall the "
      "same or a newer compatible firmware. Restore from your paper backup "
      "only if recovery is required.";
  char cause[112];
  snprintf(cause, sizeof cause, "CAUSE: %s  (code 0x%X)",
           kiss_settings_load_status_name(status),
           (unsigned)kiss_settings_load_error_code());

  // Nothing behind this page is built: no game touch timer, setup wizard,
  // home, or erase action exists on a failed-storage boot.
  lv_obj_clean(root);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);
  lv_obj_t *page = wt_screen(root, "STORAGE LOCKED",
                             "NON-DESTRUCTIVE SAFE MODE");
  lv_obj_set_style_text_color(wt_screen_title(page), WT_STOP, 0);
  lv_obj_t *body = wt_wraph(page, BODY, 48, 116, 704, 236);
  lv_obj_set_style_text_color(body, WT_INK, 0);
  lv_obj_t *code = wt_lbl(page, cause, 48, 398, wt_font14(), WT_WARN);
  lv_obj_set_width(code, 704);
  lv_label_set_long_mode(code, LV_LABEL_LONG_WRAP);
}

// Set by build_game, read by app_main's rollback gate below. A boot that lands
// on the safe-mode screen has built nothing behind it at all, and confirming
// the slot there would make a storage-broken image permanent -- which is the
// failure a reboot into the previous firmware undoes, since the thing that
// broke storage arrived with the update. Not guarded out for the sim: the same
// assignment runs there, and the harness that drives the safe-mode screen is
// what proves it is reached.
static bool s_storage_blocked;

void build_game(void) {  // non-static: the simulator harness calls this too
  // The baked art lives in flash as RLE and its descriptors start empty, so
  // this has to run before the first lv_image_set_src below (kiss_art.h says
  // why the art is compressed at all). It is free where it stands: nothing is
  // painted until the lv_timer_handler loop in app_main, so this only delays
  // first paint on a screen that is still black.
  //
  // A failure count is logged inside and deliberately not fatal -- an image
  // that could not be unpacked stays NULL and simply does not draw, which
  // costs the decoy its looks and costs the keys nothing.
  art_unpack_all();

  kiss_settings_load_status_t settings_status = kiss_settings_load();
  s_storage_blocked = settings_status != WSETTINGS_LOAD_OK;
  lv_obj_t *scr = lv_screen_active();
  if (settings_status != WSETTINGS_LOAD_OK) {
#ifndef SIMULATOR
    ESP_LOGE(TAG, "storage blocked: %s (0x%x)",
             kiss_settings_load_status_name(settings_status),
             (unsigned)kiss_settings_load_error_code());
#endif
    storage_locked_screen(scr, settings_status);
    return;
  }
  s_best = (int)kiss_game_best_load();   // opportunistic; never part of the gate

  // Match the signer's near-black blue and faint 46px grid. The tiny RGB565 tile
  // keeps flash use low; full-screen menu/saver/home artwork covers it outside PLAY.
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x00080F), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_image_src(scr, &img_game_bg, LV_PART_MAIN);
  lv_obj_set_style_bg_image_tiled(scr, true, LV_PART_MAIN);
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


  // ---- hidden KISS Signer menu (baked) — revealed only by the "K" unlock gesture ----
  s_home = lv_image_create(scr);
  // img_wallet keeps its name: it is generated into kiss_img.c beside a baked
  // RLE blob, so renaming it means regenerating three thousand lines of asset
  // for a symbol no owner ever sees.
  lv_image_set_src(s_home, &img_wallet);
  lv_obj_set_pos(s_home, 0, 0);
  // THE home-twitch bug: motes travel outside the 480px bounds (park at y=500,
  // exit above the top), which silently makes this container scrollable — LVGL
  // then shifts the ENTIRE page to chase the overflowing children. Same trap
  // the screensaver already guards against ("floating fruit go off-edge").
  lv_obj_clear_flag(s_home, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(s_home, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN);

  // Live accent chrome — the baked art carries only a DIM skeleton of these
  // (kiss_mock.py live_frames): card frames + wash, corner brackets, title
  // underline, chip frame, theme tag. kiss_home_restyle() paints them in the
  // active accent, so switching themes recolors the home with zero re-bake.
  //
  // ONE weight for all four tiles. This was a two-tier treatment per HANDOFF-05:
  // SIGN and RECEIVE, the two daily verbs, took a 2px accent border plus the
  // 18px glow, while KEYS and SETTINGS took 1px of WT_EDGE and no glow.
  //
  // On glass that did not read as a hierarchy, it read as a rendering fault --
  // two lit tiles beside two dim ones, on a row of four identically sized boxes
  // doing the same kind of job. A hierarchy needs something to separate the
  // tiers; four tiles in one strip, same size, same spacing, same icon language,
  // gives the eye nothing to attribute the difference to, so it attributes it to
  // a bug. The order they sit in already says which two are the daily verbs.
  //
  // The strips inside the frames are RGB565A8 images and are unaffected: dimming
  // those would mean regenerating the bake, which is the pipeline this whole
  // live-chrome approach exists to avoid.
  for (int i = 0; i < 4; i++) {
    lv_obj_t *c = lv_obj_create(s_home);
    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, 50 + i * 180, 150);
    lv_obj_set_size(c, 161, 183);
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_bg_opa(c, 26, 0);              // glass wash; icons stay readable
    lv_obj_set_style_shadow_width(c, 18, 0);        // the baked art's neon glow, live
    lv_obj_set_style_shadow_opa(c, 70, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    s_card_frame[i] = c;
  }
  static const struct { int x, y; lv_border_side_t side; } CORN[4] = {
    {24, 24, LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_TOP},
    {744, 24, LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_TOP},
    {24, 424, LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_BOTTOM},
    {744, 424, LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM},
  };
  for (int i = 0; i < 4; i++) {
    lv_obj_t *c = lv_obj_create(s_home);
    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, CORN[i].x, CORN[i].y);
    lv_obj_set_size(c, 32, 32);
    lv_obj_set_style_border_width(c, 3, 0);
    lv_obj_set_style_border_side(c, CORN[i].side, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    s_corner[i] = c;
  }
  s_underline = lv_obj_create(s_home);
  lv_obj_remove_style_all(s_underline);
  lv_obj_set_pos(s_underline, 46, 94);
  lv_obj_set_size(s_underline, 143, 3);
  lv_obj_set_style_bg_opa(s_underline, LV_OPA_COVER, 0);
  s_chip_frame = lv_obj_create(s_home);
  lv_obj_remove_style_all(s_chip_frame);
  lv_obj_set_pos(s_chip_frame, 566, 40);
  lv_obj_set_size(s_chip_frame, 195, 47);
  lv_obj_set_style_radius(s_chip_frame, 10, 0);
  lv_obj_set_style_border_width(s_chip_frame, 2, 0);
  lv_obj_remove_flag(s_chip_frame, LV_OBJ_FLAG_CLICKABLE);
  // live theme tag, bottom-right (replaces the baked dot that always lied MONO)
  s_theme_dot = lv_obj_create(s_home);
  lv_obj_remove_style_all(s_theme_dot);
  lv_obj_set_pos(s_theme_dot, 676, 426);
  lv_obj_set_size(s_theme_dot, 16, 16);
  lv_obj_set_style_radius(s_theme_dot, 8, 0);
  lv_obj_set_style_bg_opa(s_theme_dot, LV_OPA_COVER, 0);
  s_theme_cap = lv_label_create(s_home);
  lv_label_set_text(s_theme_cap, tr(STR_H_THEME));
  lv_obj_set_style_text_color(s_theme_cap, lv_color_hex(0x7A869C), 0);
  lv_obj_set_style_text_font(s_theme_cap, wt_font14(), 0);
  lv_obj_set_pos(s_theme_cap, 704, 408);
  s_theme_lbl = lv_label_create(s_home);
  lv_obj_set_style_text_font(s_theme_lbl, wt_font14(), 0);
  lv_obj_set_style_text_letter_space(s_theme_lbl, 1, 0);
  lv_obj_set_pos(s_theme_lbl, 704, 428);

  // Fingerprint chip (top-right) — the baked art leaves this area BLANK (dynamic
  // content); live labels own it. Coords from assets/generators/kiss_mock.py.
  // Step-1 proof: shows the boot-selftest fingerprint of the dev seed.
  s_fp_chip = lv_label_create(s_home);
  lv_label_set_text(s_fp_chip, s_fp_hex);
  lv_obj_set_style_text_color(s_fp_chip, wt_accent(), 0);
  lv_obj_set_style_text_font(s_fp_chip, wt_font_mono28(), 0);   // fills the chip frame
  lv_obj_set_style_text_letter_space(s_fp_chip, 2, 0);

  s_fp_cap = lv_label_create(s_home);
  lv_label_set_text(s_fp_cap, tr(STR_H_FINGERPRINT));
  lv_obj_set_style_text_color(s_fp_cap, lv_color_hex(0x7A869C), 0);   // muted, like the mock
  lv_obj_set_style_text_font(s_fp_cap, wt_font14(), 0);
  fp_chip_place();

  s_cam_lbl = lv_label_create(s_home);         // bottom-center status/error slot: blank
  lv_label_set_text(s_cam_lbl, "");              // until something (camera/error) fills it.
  lv_obj_set_style_text_color(s_cam_lbl, lv_color_hex(0x7A869C), 0);  // baked art leaves this
  lv_obj_set_style_text_font(s_cam_lbl, wt_font14(), 0);   // bottom gap free
  lv_obj_align(s_cam_lbl, LV_ALIGN_BOTTOM_MID, 0, -14);

  // Persistent storage badge, on the SAME LINE as the build identity and to the
  // right of it. It appears ONLY when these keys live on the SD card, so the
  // home says at a glance that a card is required: accent while the card is in
  // (and breathing, so it reads as live), amber "no card" while it is out.
  // game_tick drives its state and the breathe; hidden for FLASH/AMNESIC, where
  // there is nothing to insert.
  //
  // It sat at (48, 398), stacked directly above the version line, which put two
  // unrelated facts in one corner and left the whole middle of the bottom edge
  // empty. It then sat at a hardcoded 410, on a comment that said the build
  // line "ends around x=380". That is true of a dev build. A RELEASE build
  // prints the commit rather than "dev (local)", which is wider, and this
  // badge came down on the last letter of "encryption: OFF" -- reported off a
  // real board, invisible to every gate, because the sim never builds RELEASE.
  //
  // So it asks. The position is set below, once the build id exists to measure
  // -- the theme cluster does not start until ~710, so a badge of two glyphs
  // has room wherever the version leaves it.
  s_sd_badge = lv_label_create(s_home);
  lv_label_set_text(s_sd_badge, "");
  lv_obj_set_style_text_font(s_sd_badge, wt_font14(), 0);
  lv_obj_add_flag(s_sd_badge, LV_OBJ_FLAG_HIDDEN);

  // The test network, top centre between the baked "KISS" logo and the
  // fingerprint. A DOT AND A WORD, not a pill: the lozenge was the only
  // rounded box on a screen whose other status -- "theme (dot) MONO", bottom
  // right -- is already exactly this shape, and the fingerprint beside it
  // wears square brackets. One screen, three shapes for three facts, and the
  // odd one out was the one that mattered most.
  //
  // The DOT keeps the amber and BREATHES; the word takes the theme. Same rule
  // as every status on the device now: the mark carries the caution, the words
  // carry the accent. Shown only off mainnet, so an ordinary signer's home is
  // clean, and kept in sync by kiss_home_refresh().
  s_net_dot = lv_obj_create(s_home);
  lv_obj_remove_style_all(s_net_dot);
  lv_obj_set_size(s_net_dot, 8, 8);
  lv_obj_set_style_radius(s_net_dot, 4, 0);
  lv_obj_set_style_bg_color(s_net_dot, WT_WARN, 0);
  lv_obj_set_style_bg_opa(s_net_dot, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_color(s_net_dot, WT_WARN, 0);
  lv_obj_set_style_shadow_width(s_net_dot, 10, 0);
  lv_obj_set_style_shadow_opa(s_net_dot, 140, 0);
  lv_obj_remove_flag(s_net_dot, LV_OBJ_FLAG_CLICKABLE);
  wt_dot_breathe(s_net_dot, 8, 3, false);
  lv_obj_add_flag(s_net_dot, LV_OBJ_FLAG_HIDDEN);

  s_net_lbl = lv_label_create(s_home);
  lv_label_set_text(s_net_lbl, kiss_net_name());   // rewritten per refresh
  lv_obj_set_style_text_color(s_net_lbl, wt_accent(), 0);
  lv_obj_add_flag(s_net_lbl, WT_FLAG_ACCENT);
  lv_obj_set_style_text_font(s_net_lbl, wt_font14(), 0);
  lv_obj_set_style_text_letter_space(s_net_lbl, 3, 0);
  lv_obj_add_flag(s_net_lbl, LV_OBJ_FLAG_HIDDEN);

  // Build identity, bottom-left — the baked art used to carry a permanent
  // CAUTION pill here (a status light that never changed = dead chrome); now
  // this corner tells the truth instead, same line as the Settings footer.
  // ONE line here. Settings stacks because it has to share its bottom edge with
  // a row of buttons; this edge is empty, so the signature runs along it and
  // stays the quiet thing it is meant to be.
  s_home_build_id = kiss_build_id_make(s_home, 48, 424, false, false);
  // Now the row has a measured width, the badge can stand clear of it.
  lv_obj_set_pos(s_sd_badge, kiss_build_id_right() + 28, 424);

  // NOTHING here says how to reach the other signer.
  //
  // The line that used to sit at y=372 read "a stroke after the word asks for
  // a passphrase". The argument for it was sound as far as it went: routing is
  // uniform, so a word alone opens these keys on every device, and an owner
  // who configured a passphrase and then forgot the stroke would land here and
  // conclude the device lost their coins. The line was shown in every session
  // precisely so its presence could not single anybody out.
  //
  // It is still off. Whoever is holding the device is reading this screen, and
  // a permanent caption naming the second door tells them the exact next thing
  // to demand. "Every device says it" answers the question of which OWNER is
  // hiding something; it does not answer why the screen should teach the
  // question at all. The way in belongs in docs/walkthrough.md and in the
  // wizard that configures it, not standing under the tiles of a signer
  // somebody may have been made to open.

  // ONE next step, in the band under the tiles.
  //
  // The order that matters is written down in docs/walkthrough.md -- check the
  // paper, pair a coordinator, verify an address on the device, then move a
  // little money -- and the device said none of it. Four equal tiles is a menu,
  // and a menu tells a newcomer what they CAN do without ever saying which of
  // it comes first. The step this line names is the one that catches a
  // computer showing an address that is not yours, and it is worth nothing
  // once the money is already sent.
  //
  // Two facts, both already stored and both already read elsewhere on this
  // device: kiss_ui_backup_checked() is the question SETTINGS asks about the
  // paper, and kiss_usage_chain_known() is the only honest signal this signer
  // has for "a coordinator has spoken", which kiss_info.c already treats as
  // paired-ness. So this adds no state; it reads what two screens read.
  //
  // MUTED, and no all-good version. Accent here would be the GREEN that is
  // byte identical to WT_OK on one theme, which would dress a suggestion up as
  // something the device has checked -- exactly what task 6 of the UX
  // acceptance walks the flows looking for. And a badge that is always on
  // screen is a badge nobody reads, which is why the settings attention chip
  // has no "all clear" state either: when both steps are done this is hidden
  // and the band goes back to being empty.
  //
  // NOTHING HERE NAMES THE SECOND DOOR. The line the band used to carry did,
  // and the reasoning that removed it is a few paragraphs up and still holds.
  // Pairing and paper are not that: every signer of this kind wants both, and
  // saying so singles nobody out.
  s_next_lbl = lv_label_create(s_home);
  lv_label_set_text(s_next_lbl, "");
  lv_obj_set_style_text_font(s_next_lbl, wt_font23(), 0);
  lv_obj_set_style_text_color(s_next_lbl, lv_color_hex(0x7A869C), 0);
  lv_obj_set_style_text_align(s_next_lbl, LV_TEXT_ALIGN_CENTER, 0);
  // The lane, and WRAP rather than DOT. A one-line label pinned with LONG_DOT
  // is the CUT fault: it loses its second half and rewrites its own text to
  // say so, which a walk of the finished tree cannot see. Wrapping instead
  // means a translation too long for one line pushes past WT_CONTENT_BOTTOM,
  // where the screen walk reports it as what it is -- copy that needs cutting
  // at the sweep, not a sentence quietly missing its end.
  lv_obj_set_width(s_next_lbl, 704);
  lv_label_set_long_mode(s_next_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_add_flag(s_next_lbl, LV_OBJ_FLAG_HIDDEN);
  // 346, measured rather than estimated. The tiles end at 333 and the theme
  // cluster starts at 408, so the band is real -- but the 23px rung's LINE BOX
  // is 49px on the Latin face, not the ~31 the glyph height suggests, and 352
  // put the bottom of it 4px past WT_CONTENT_BOTTOM. The screen walk said so;
  // no estimate in this file's history has ever been right about a line box.
  lv_obj_align(s_next_lbl, LV_ALIGN_TOP_MID, 0, HOME_NEXT_Y);

  // Tile labels, live + translated. The 23px title carries the whole action;
  // the former 14px subtitle duplicated it and was unreadable at arm's length.
  for (int i = 0; i < 4; i++) {
    s_tile_ttl[i] = lv_label_create(s_home);
    lv_label_set_text(s_tile_ttl[i], tr(TILE_TTL_STR[i]));
    lv_obj_set_style_text_font(s_tile_ttl[i], wt_font23(), 0);
    lv_obj_set_style_text_color(s_tile_ttl[i], lv_color_hex(0xE8EEF7), 0);
    lv_obj_set_style_text_align(s_tile_ttl[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_tile_ttl[i], 160);
    lv_obj_set_pos(s_tile_ttl[i], 50 + i * 180, TILE_LBL_Y + 22);
  }

  kiss_home_restyle();

  // idle motes: small dim dots (started/stopped with the session)
  for (int i = 0; i < N_MOTES; i++) {
    s_mote[i] = lv_obj_create(s_home);
    lv_obj_remove_style_all(s_mote[i]);
    lv_obj_clear_flag(s_mote[i], LV_OBJ_FLAG_CLICKABLE);   // must never eat a tap
    lv_obj_set_size(s_mote[i], 4, 4);
    lv_obj_set_style_radius(s_mote[i], 2, 0);
    lv_obj_set_style_bg_color(s_mote[i], wt_accent(), 0);
    lv_obj_set_style_bg_opa(s_mote[i], LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_mote[i], 0, 0);
    lv_obj_set_pos(s_mote[i], 0, 474);
    // A mote rises the full height of the screen, so at some tick it is always
    // sitting below WT_CONTENT_BOTTOM -- which is content crossing the line as
    // far as the screen-walk gate can tell, and is nothing at all as far as a
    // reader can tell. Whether it fired came down to how many ticks the walk
    // had spent before it saved the home frame, so the gate reported a real
    // rule against the one object on the screen the rule was never about.
    wt_mark_decor(s_mote[i]);
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
  lv_obj_set_style_bg_opa(s_saver_hint, 110, 0);            // subtle dark backing so it reads on any backdrop
  lv_obj_set_style_pad_hor(s_saver_hint, 24, 0);
  lv_obj_set_style_pad_ver(s_saver_hint, 11, 0);
  lv_obj_set_style_radius(s_saver_hint, 20, 0);
  lv_obj_align(s_saver_hint, LV_ALIGN_BOTTOM_MID, 0, -64);
  lv_obj_add_flag(s_saver_hint, LV_OBJ_FLAG_HIDDEN);
  // The pulse starts with the saver and dies with it (saver_hint_pulse /
  // saver_hide). It used to start HERE, once, at REPEAT_INFINITE -- so it ran
  // for the life of the device, setting an opacity on a hidden label 60 times a
  // second and invalidating it every time, behind the signer's screens and
  // behind the game. Nothing showed it, and nothing stopped it.

  lv_timer_create(game_tick, TICK_MS, NULL);
  s_spawn_timer = lv_timer_create(spawn_tick, 800, NULL);
}

#ifndef SIMULATOR
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
    *x = pt[0].y;
    *y = (LCD_H_RES - 1) - pt[0].x;
    return true;
  }
  return false;
}

void app_main(void) {
  radio_hold_in_reset();   // before anything else: smallest window for the C6
  log_board_info();
#ifndef KISS_RELEASE
  {  // step 1 of the signer build order: prove the crypto stack (libwally)
    uint8_t fp[4];
    int rc = kiss_selftest(fp);
    ESP_LOGI(TAG, "crypto selftest: %s (stage %d) fingerprint %02X%02X%02X%02X",
             rc == 0 ? "PASS" : "FAIL", rc, fp[0], fp[1], fp[2], fp[3]);
    if (rc == 0)
      snprintf(s_fp_hex, sizeof(s_fp_hex), "%02X%02X%02X%02X",
               fp[0], fp[1], fp[2], fp[3]);
    else
      snprintf(s_fp_hex, sizeof(s_fp_hex), "FAIL %d", rc);
  }
#else
  kiss_selftest(NULL);   // release: just wally_init; the chip stays blank
#endif
  // Signing determinism, on the chip that will actually sign. The host test
  // suite proves this against a 64-bit secp256k1 field backend; a riscv32
  // device compiles field_10x26 instead, so these bytes have never been
  // checked here. Two signatures, ~ms, every boot including release.
  // A failure here is not only logged: kiss_psbt_sign refuses to sign at
  // all, so a unit whose curve code has drifted cannot produce a signature
  // rather than producing a quietly wrong one.
  //
  // Hoisted out of its block because the rollback decision below needs it. A
  // signer that cannot sign is exactly the image rollback exists to undo, and
  // confirming it anyway made the refusal permanent instead of temporary.
  int src = kiss_sign_selftest();
  ESP_LOGI(TAG, "signing selftest: %s (stage %d)", src == 0 ? "PASS" : "FAIL", src);
  display_start();
  backlight_on();
  touch_start();
  // After the display, because switching the entropy source on reconfigures
  // ADC1 and the analog i2c clock, and the panel's LDO comes up through the
  // same analog block. Nothing needs randomness before a screen exists, so
  // this costs nothing and keeps boot order boring.
  kiss_trng_start();
  build_game();
  ESP_LOGI(TAG, "fruit game running (landscape, manual rotated flush)");
  // The one symptom of the GT911 not coming up is a device that ignores you;
  // say it instead. s_menu_panel exists as of build_game.
  if (!s_touch) kiss_touch_dead_banner(s_menu_panel);

  // Release the slot that was running before an SD update, now that this
  // firmware has proved the parts a bad image would take out: the crypto
  // selftest above, the display, the touch panel and a built screen. Anything
  // that reboots before this line -- a crash, the watchdog, a hand on the
  // power -- hands the device back to the firmware that was working.
  //
  // Here rather than at unlock, on purpose. Waiting for the owner to type a
  // passphrase would silently revert a good update if they set the device down
  // first, and keys that unlock are not the bar: a device that boots and
  // draws is.
  //
  // "Draws" is this line, and it used to be below the confirmation rather than
  // above it. build_game builds an object tree; the pixels reach the panel from
  // rot_flush, inside the handler loop under this comment. So the slot was
  // released having proved everything EXCEPT the one path no gate on this
  // project can reach -- the manual rotated flush, which the simulator does not
  // compile and never runs. An image that panics on its first flush rebooted
  // with rollback already cancelled, straight back into itself.
  lv_refr_now(NULL);
  // And the selftest is a gate now, not a log line. Both of these decide
  // whether the PREVIOUS firmware gets to come back, which is the only thing
  // that can save a unit whose new image cannot draw or cannot sign.
  // Touch is the third gate, beside drawing and signing. touch_start returns
  // silently on a dead GT911 or a dead I2C bus, the screen keeps looking
  // perfect, and a slot confirmed in that state is a signer nobody can ever
  // drive -- with the rollback that would have undone it already cancelled.
  // Init success only, deliberately NOT a touch event: waiting for a finger
  // re-creates the walk-away revert the comment above rules out.
  // Storage is the fourth gate, beside drawing, signing and touch, and it was
  // missing. build_game returns early on a failed kiss_settings_load and paints
  // the safe-mode screen -- no setup, no home, nothing that can reach the keys
  // -- and then this line confirmed the slot anyway, because the condition
  // only asked about signing and touch. So an image that cannot open NVS made
  // itself permanent on the one boot a reboot would have undone it. Nothing
  // about that is theoretical: a partition table or an encryption state that
  // moved is exactly what an update changes, and exactly what the previous
  // firmware still works with.
  if (kiss_fw_confirm_ok(src == 0, s_touch != NULL, !s_storage_blocked)) {
    kiss_fw_mark_valid();
  } else if (src != 0) {
    ESP_LOGE(TAG, "signing selftest failed (stage %d): leaving this slot on "
                  "trial so a reboot returns the firmware that worked", src);
  } else if (s_touch == NULL) {
    ESP_LOGE(TAG, "touch never came up: leaving this slot on trial so a "
                  "reboot returns the firmware that worked");
  } else {
    ESP_LOGE(TAG, "storage would not open: leaving this slot on trial "
                  "so a reboot returns the firmware that worked");
  }

  while (1) {           // single-threaded LVGL loop (we own the display + flush)
    uint32_t next = lv_timer_handler();
    if (next > 20) next = 20;
    vTaskDelay(pdMS_TO_TICKS(next < 4 ? 4 : next));
  }
}
#endif  // !SIMULATOR
