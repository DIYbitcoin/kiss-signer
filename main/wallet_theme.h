// Shared wallet UI kit: the one house style every wallet screen builds from
// (screen frame, pills, labels, QR card, text grouping) plus the switchable
// accent the home art's theme dots promised. Compiled in both device and sim
// builds; holds no LVGL state beyond the accent id.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"

// fixed palette (identical to what every screen used before the kit)
#define WT_BG   lv_color_hex(0x070A10)
#define WT_INK  lv_color_hex(0xE8EEF7)
#define WT_MUT  lv_color_hex(0x7A869C)
#define WT_KEY  lv_color_hex(0x10141D)
#define WT_OK   lv_color_hex(0x35D07F)   // status semantics: never themed
#define WT_WARN lv_color_hex(0xF2B84B)
#define WT_STOP lv_color_hex(0xFF4D5E)
#define WT_CARD lv_color_hex(0xF2F5FA)   // QR cards: scanners want dark-on-light

// accent themes = the dots on the baked home art. MONO keeps the shipped look.
enum { WT_ACC_MONO = 0, WT_ACC_GREEN, WT_ACC_PINK, WT_ACC_ORANGE, WT_ACC_N };
void       wt_accent_set(int id);        // clamps to a valid id; caller persists
int        wt_accent_get(void);
lv_color_t wt_accent(void);              // MONO -> WT_INK
lv_color_t wt_primary(void);             // selected/action accent: MONO -> WT_INK
lv_color_t wt_accent_bg(void);           // dark tinted fill for accent controls
lv_color_t wt_accent_pressed(void);      // slightly brighter pressed fill
const char *wt_accent_name(void);        // "MONO"/"GREEN"/"CYPHERPINK"/"ORANGE"

// wallet text fonts: the generated i18n fonts (kiss_fonts.h), which carry
// every script the translations use plus the LV_SYMBOL icons. ALL wallet
// screens take their 14/28pt fonts from here; the game keeps the built-in
// Montserrat (ASCII HUD only). 40/48pt stay built-in (digits/symbols only).
const lv_font_t *wt_font14(void);
const lv_font_t *wt_font14_for_lang(int lang);  // native-name rows in the language picker
const lv_font_t *wt_font23(void);   // wallet-home tile titles (baked-art size)
const lv_font_t *wt_font28(void);

// screen frame: 800x480 bg + title (accent) + muted subtitle. Returns the screen.
lv_obj_t *wt_screen(lv_obj_t *parent, const char *title, const char *sub);

// pills (buttons). wt_pill = the standard 52px height.
lv_obj_t *wt_pillh(lv_obj_t *scr, const char *txt, int x, int y, int w, int h,
                   lv_event_cb_t cb, void *ud);
lv_obj_t *wt_pill(lv_obj_t *scr, const char *txt, int x, int y, int w,
                  lv_event_cb_t cb, void *ud);
void      wt_pill_primary(lv_obj_t *pill);   // accent border = the suggested action
void      wt_pill_select(lv_obj_t *pill, bool on);  // chooser pills: filled when active

lv_obj_t *wt_lbl(lv_obj_t *scr, const char *txt, int x, int y,
                 const lv_font_t *f, lv_color_t col);
lv_obj_t *wt_wrap(lv_obj_t *scr, int x, int y, int w);      // muted wrapping body text
lv_obj_t *wt_section(lv_obj_t *scr, const char *txt, int x, int y);  // column caption

// white QR card; *qr receives the lv_qrcode (NULL if creation failed)
lv_obj_t *wt_qr_card(lv_obj_t *scr, lv_obj_t **qr, int x, int y, int card_px, int qr_px);

// Explainer-card entrance: fade the dim backdrop in, then stagger the card's
// direct children (title, body, OK) rising up and fading in with an ease-out
// settle. Opacity + translate only (never scale — it hangs LVGL). Call once
// after building an overlay card's children. Every "?" card uses this.
void wt_card_intro(lv_obj_t *card);

// Mini "equation" diagrams for the "?" cards, built from the app's own chip
// vocabulary (so they read on-brand, never like cheap clip-art). A row is a
// centered flex strip; add chips and operator glyphs to it left to right.
lv_obj_t *wt_diagram_row(lv_obj_t *parent, int y);            // centered strip at y
lv_obj_t *wt_chip(lv_obj_t *row, const char *txt, bool accent); // rounded token
lv_obj_t *wt_diagram_op(lv_obj_t *row, const char *txt);     // "+", arrow, etc.
// the deniability equation: WORDS + PASSPHRASE -> FINGERPRINT (accent result).
void wt_diagram_fp(lv_obj_t *parent, int y);
// the airgap: ONLINE APP <- QR -> KISS OFFLINE (accent = the signer).
void wt_diagram_pair(lv_obj_t *parent, int y);

// grouped address with the compare-ends bright: first/last 4 chars ink, middle
// muted. People check the ends against the other screen — light those up.
lv_obj_t *wt_addr_spans(lv_obj_t *par, const char *grouped, int w, const lv_font_t *f);

// text helpers shared by receive/sign/info
void wt_group4(const char *in, char *out, size_t out_len);     // addr in blocks of 4
void wt_fmt_sats(uint64_t v, char *out, size_t out_len);       // 1234567 -> 1 234 567
void wt_fmt_btc(uint64_t sats, char *out, size_t out_len);     // 61000 -> 0.00061000
