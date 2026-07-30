// Shared wallet UI kit: the one house style every wallet screen builds from
// (screen frame, pills, labels, QR card, text grouping) plus the switchable
// accent the home art's theme dots promised. Compiled in both device and sim
// builds; global state is limited to the accent id, while QR zoom state is
// owned and freed by each card.
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

// Surfaces. Everything above is ink or status; these are the things ink sits
// ON. They are near enough to WT_BG to read as the same darkness and far
// enough to separate a panel from the page behind it, which is the only job
// they have. Never use one as a text colour except WT_DIM, which is ink.
#define WT_BAR   lv_color_hex(0x0B0E14)  // the action bar's floor
#define WT_HAIR  lv_color_hex(0x1E2531)  // 1px rule where a surface meets the page
#define WT_PANEL lv_color_hex(0x0A0E15)  // explainer cards, any raised block
#define WT_DIV   lv_color_hex(0x1A2130)  // divider between rows inside one panel
#define WT_DIM   lv_color_hex(0x4C5666)  // ink for something present but inert
#define WT_EDGE  lv_color_hex(0x2A3346)  // border of a control that is not a pill

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
// Page titles + primary buttons. Latin/Cyrillic get a real 34px face;
// CJK locales get 28 (no CJK face exists at 34, and CJK glyphs already
// read larger at a given pixel size). Never returns a Latin-only font to
// a CJK locale.
const lv_font_t *wt_font34(void);

// Fixed pitch. Never hand one a translated string: they have no CJK variant
// and no fallback, so a localised glyph draws LVGL's placeholder box.
const lv_font_t *wt_font_mono14(void);
const lv_font_t *wt_font_mono23(void);
const lv_font_t *wt_font_mono28(void);
const lv_font_t *wt_font_num48(void);   // the Sign hero, digits only
// Largest body font that fits `txt` into w x max_h, measured for the ACTIVE
// locale's font. Explainers should read at arm's length (and on a 3.5" port),
// so short copy gets the big font; a long translation degrades to the small one
// instead of overflowing its card. Shorten the copy to get the big size.
const lv_font_t *wt_body_font(const char *txt, int w, int max_h);

// screen frame: 800x480 bg + title (accent) + muted subtitle. Returns the screen.
lv_obj_t *wt_screen(lv_obj_t *parent, const char *title, const char *sub);
// Draw the corner-lock affordance: a small × in WT_MUT at (16, 22), font23.
// The 88x88 tap region this hints at is enforced in main.c's touch handler,
// which routes through the same auto-lock teardown as the idle timer. This is
// discoverability only; do NOT wire a click handler here, because the corner
// gesture is a global router that lives beside the wallet sub-screens rather
// than inside them. Call from any wallet sub-screen that owns its own touches.
void wt_lock_mark(lv_obj_t *scr);
// Re-fit the title into `w` px on ONE line, stepping 34 -> 28 -> 23. wt_screen
// already does this at 704, the full width between the page margins. Call it
// again, narrower, on any screen that puts something else on the title's row:
// a title has no width of its own, so a long translation simply keeps going
// and runs straight through whatever is up there.
void wt_title_fit(lv_obj_t *scr, int w);
// The same for the subtitle, which wt_screen gives the full 704px lane and one
// line of height. Narrow it on any screen that parks something inside that
// lane: the subtitle's BOX is 704 wide whatever the translation does, so it
// overlaps a chip sitting at x=652 even when the words stop at 400.
void wt_sub_fit(lv_obj_t *scr, int w);

// The action row: where a screen's buttons live, and the line content may not
// cross. These name the geometry the screens already used as bare numbers; the
// values are unchanged, so nothing moves. They exist so sim/overlapcheck.c has
// one number to test against instead of grepping for 404, and so the row can be
// moved once rather than in ninety places.
//
// Two heights, and the tall one is not an accident. A pill label auto-fits
// 23 -> 14, and at 66 it can take a SECOND LINE at 23 instead of dropping a
// rung: "HOLD TO SIGN" has no one-line size above 14 in French, Italian or
// Swedish, and the button that moves money should not be the smallest type on
// screen. Use TALL for any row whose label may wrap, standard everywhere else.
// A row shares one height across all its pills or they stop lining up.
#define WT_ACTION_Y       404   // standard row: 404..456, 24px above the edge
#define WT_ACTION_H        52   // == wt_pill's height
#define WT_ACTION_Y_TALL  398   // wrapping row: 398..464
#define WT_ACTION_H_TALL   66
// Nothing above the row may extend past this. It is WT_ACTION_Y_TALL exactly,
// not a rounder number with a gutter invented on top: the tall row is the
// highest anything in the action band reaches, so crossing it is the failure.
#define WT_CONTENT_BOTTOM WT_ACTION_Y_TALL

// BACK is always the bottom RIGHT pill, on every screen that has one. A thumb
// arrives at that corner at an angle and lands short, which is why Settings put
// it there first and gave it 10px of ext click area; the rest of the app then
// hand typed 48 on fourteen screens and 330 on two more, so the escape hatch
// moved depending on which screen you were escaping from. One number, one
// corner, no exceptions.
//
// The corollary is worth stating because it is a safety property and not a
// tidiness one: the right corner is where the least consequential button on
// each screen now lives. Anything that spends money sits further left, away
// from the reflex tap and out from under the help chips that hang above the
// row's right end.
//
// THE RULE IS ABOUT ESCAPING A SCREEN, NOT ABOUT THE WORD "BACK". STR_C_BACK
// does two unrelated jobs in this app and only one of them belongs here:
//
//   escape  - leaves for the level above (close_cb, files_back_cb, sp_back_cb,
//             the sign details page returning to verify). Right corner. If a
//             screen's escape is called DONE instead, DONE takes the corner:
//             the corner belongs to the exit, whatever it is labelled.
//   paging  - steps within the screen you are already on, and always has a
//             NEXT beside it (the recovery words pages, the pairing QR page).
//             That pair stays adjacent on the LEFT, because splitting BACK and
//             NEXT across the full width to satisfy a corner rule would break
//             the one thing a paged sequence needs, which is that its two
//             halves look like one control.
#define WT_BACK_X          610   // BACK's left edge, for the standard 140px pill

// The action bar is the floor the row stands on: full width, WT_BAR fill, one
// WT_HAIR line along its top. It is not a call you make. wt_pillh builds it the
// first time a pill lands at or below WT_CONTENT_BOTTOM on a wt_screen, so a
// screen cannot acquire an action row and forget the bar, and a screen with no
// action row never grows one.
//
// It exists because a button floating over text is read as a rendering fault,
// while text meeting a bar is read as text continuing underneath. That is a
// last line of defence and not a fix: sim/overlapcheck.c still fails anything
// that crosses WT_CONTENT_BOTTOM, because content hidden behind the bar is
// content the owner cannot read.

// Pill icons. FontAwesome PUA codepoints baked into every generated Latin size
// by the SYMS list in tools/fonts/gen_fonts.sh — keep the two in lockstep, an
// icon that is not in the font hard-hangs the renderer rather than drawing a
// tofu box. These three have no LV_SYMBOL_* macro; the sd card does, so it is
// spelled with LVGL's own name.
#define WT_ICON_QR     "\xEF\x80\xA9"   // U+F029 qrcode
#define WT_ICON_KEY    "\xEF\x82\x84"   // U+F084 key
#define WT_ICON_SECRET "\xEF\x88\x9B"   // U+F21B user-secret (the incognito hat)
#define WT_ICON_SD     LV_SYMBOL_SD_CARD
// The RBF explainer's two states. LOCK has no LV_SYMBOL macro; REFRESH does,
// and LVGL already ships its codepoint, so it is spelled with LVGL's name.
#define WT_ICON_LOCK    "\xEF\x80\xA3"   // U+F023 lock
#define WT_ICON_REPLACE LV_SYMBOL_REFRESH

// Compose "<icon>  <label>" into out. The icon rides INSIDE the pill's label
// rather than sitting beside it as a second object, so wt_pill_fit keeps
// measuring the whole thing and a long translation still degrades honestly.
// Use this everywhere, including when measuring: the fit report and the screen
// must size the identical string or the ratchet is checking the wrong text.
// Buffers are WT_ICON_TEXT_MAX: the longest label today is Russian "СВЯЗАТЬ
// КООРДИНАТОР" at 78 bytes composed, and Cyrillic costs two bytes a letter, so
// the margin is smaller than the character count suggests.
#define WT_ICON_TEXT_MAX 128
void wt_icon_text(char *out, size_t out_len, const char *icon, const char *txt);

// Give any clickable object the pill's press answer: it sinks 2px while held
// and a ring travels out of its edge as it fades on release. wt_pillh already
// does this; call it directly for tappable things that are not pills, such as
// the rows of the address list. Never scales anything — see the comment on the
// implementation for why that matters.
void wt_tap_feedback(lv_obj_t *obj);
// One visual language for anonymous "?" affordances: a 30px circle with a
// 54px effective hit target. `color` carries warning semantics when needed;
// size, border and press feedback remain identical everywhere.
lv_obj_t *wt_help_chip(lv_obj_t *parent, int x, int y, lv_color_t color,
                       lv_event_cb_t cb, void *ud);

// pills (buttons). wt_pill = the standard 52px height.
lv_obj_t *wt_pillh(lv_obj_t *scr, const char *txt, int x, int y, int w, int h,
                   lv_event_cb_t cb, void *ud);
// wt_pillh with an icon before the label (see wt_icon_text).
lv_obj_t *wt_pill_icon(lv_obj_t *scr, const char *icon, const char *txt,
                       int x, int y, int w, int h, lv_event_cb_t cb, void *ud);
lv_obj_t *wt_pill(lv_obj_t *scr, const char *txt, int x, int y, int w,
                  lv_event_cb_t cb, void *ud);
// Accent border = the suggested action. ALSO promotes the label to the top
// rung: pill labels auto-fit 23 -> 14 by default, and 28 -> 23 -> 14 once
// marked primary, so the button is never smaller than the note beside it.
void      wt_pill_primary(lv_obj_t *pill);
// Just the label promotion, no colour change: the sign pill is deliberately
// green rather than accent, but it is still the screen's main action.
void      wt_pill_label_max(lv_obj_t *pill);
// Add the second line to a pill ("DESKTOP" over "Sparrow") and re-fit the main
// label to the room left above it. Replaces three hand-tuned copies.
void      wt_pill_two_line(lv_obj_t *pill, const char *sub);
void      wt_pill_two_line_val(lv_obj_t *pill, const char *sub);
// Pills sharing a row share a label size (smallest wins). Without it one long
// word drops a single pill a rung and the row looks broken.
void      wt_pill_row(lv_obj_t **pills, int n);
// How a pill label renders in the ACTIVE locale: the font, its tracking, and
// whether it takes a second line. Closing the tracking, then wrapping, both
// come BEFORE dropping a font size, so a wide-enough or tall-enough pill keeps
// its rung. Exposed so the fit report measures buttons the way the kit draws
// them; sim/fitcheck.c fails the build when a key action lands on font14.
typedef struct {
    const lv_font_t *font;
    int  space;     // letter tracking to apply with it
    bool wrap;      // needs LV_LABEL_LONG_WRAP at (w - 28)
} wt_pill_fit_t;
wt_pill_fit_t wt_pill_fit(const char *txt, int w, int h, bool primary);
// One rung for a whole GROUP of pills: the smallest any member needs, so a
// row of buttons reads as a set instead of one shouting neighbour. Pass every
// label that shares a row/column, then wt_pill_apply_fit each pill.
wt_pill_fit_t wt_pill_group_fit(const char *const *txts, int n, int w, int h,
                                bool primary);
void wt_pill_apply_fit(lv_obj_t *pill, wt_pill_fit_t f, int w);
void      wt_pill_select(lv_obj_t *pill, bool on);  // chooser pills: filled when active

lv_obj_t *wt_lbl(lv_obj_t *scr, const char *txt, int x, int y,
                 const lv_font_t *f, lv_color_t col);
lv_obj_t *wt_wrap(lv_obj_t *scr, int x, int y, int w);      // muted wrapping body text
// Same, but auto-sized to a known gap: prefer this. wt_wrap is fixed-small and
// only right where the caller genuinely has no vertical room to give.
lv_obj_t *wt_wraph(lv_obj_t *scr, const char *txt, int x, int y, int w, int h);
// Re-fit one of those when its text is replaced later (chooser captions).
void      wt_wrap_fit(lv_obj_t *l, const char *txt, int w, int h);
// Same auto-fit, capped at font23: for a note that belongs to a control. At 28
// a short note renders larger than the button it describes, which reads as the
// note being the important thing. Explainer cards use wt_wraph; notes use this.
lv_obj_t *wt_note(lv_obj_t *scr, const char *txt, int x, int y, int w, int h);
void      wt_note_fit(lv_obj_t *l, const char *txt, int w, int h);
lv_obj_t *wt_section(lv_obj_t *scr, const char *txt, int x, int y);  // column caption

// White QR card; *qr receives the lv_qrcode (NULL if creation failed). Every
// card is tappable and has an external "+" cue; tapping opens a crisp,
// re-encoded full-screen view rather than scaling the original bitmap.
lv_obj_t *wt_qr_card(lv_obj_t *scr, lv_obj_t **qr, int x, int y, int card_px, int qr_px);
// Update a QR created by wt_qr_card. This caches the exact payload for zoom and
// keeps animated QRs moving while enlarged. Use instead of lv_qrcode_update().
lv_result_t wt_qr_update(lv_obj_t *qr, const void *data, uint32_t data_len);

// Explainer-card entrance: fade the dim backdrop in, then stagger the card's
// direct children (title, body, OK) rising up and fading in with an ease-out
// settle. Opacity + translate only (never scale — it hangs LVGL). Call once
// after building an overlay card's children. Every "?" card uses this.
void wt_card_intro(lv_obj_t *card);

// Mini "equation" diagrams for the "?" cards, built from the app's own chip
// vocabulary (so they read on-brand, never like cheap clip-art). A row is a
// centered flex strip; add chips and operator glyphs to it left to right.
// A centered flex strip. No y: it takes its place from the parent's own layout,
// because the block above it is wt_body_font-sized and a caller cannot know how
// tall that came out in the locale being rendered. Parents of a diagram are
// flex columns.
lv_obj_t *wt_diagram_row(lv_obj_t *parent);
lv_obj_t *wt_chip(lv_obj_t *row, const char *txt, bool accent); // rounded token
lv_obj_t *wt_diagram_op(lv_obj_t *row, const char *txt);     // "+", arrow, etc.
// the deniability equation: WORDS + PASSPHRASE -> FINGERPRINT (accent result).
void wt_diagram_fp(lv_obj_t *parent);
// the airgap: ONLINE APP <- QR -> KISS OFFLINE (accent = the signer).
void wt_diagram_pair(lv_obj_t *parent);

// Grouped address with only the LAST 8 characters lit, everything before them
// muted. Not the first: every Native SegWit address begins bc1q (or tb1q), so
// highlighting the front invited people to compare a constant and feel checked.
// The tail carries real entropy and the bech32 checksum, so a swapped address
// always differs there.
lv_obj_t *wt_addr_spans(lv_obj_t *par, const char *grouped, int w, const lv_font_t *f);

// One-line address for a list row: prefix and middle muted, with the four
// characters AFTER the prefix and the final four lit. Those eight are the ones
// worth comparing — every native segwit address begins bc1q, so lighting the
// front alone invites comparing a constant, and lighting only the tail makes
// people read backwards. Takes the RAW address (not grouped); it does its own
// spacing, because the tail is chunked from the right so the lit four always
// lands on its own block.
lv_obj_t *wt_addr_short(lv_obj_t *par, const char *addr, const lv_font_t *f);
// The design review's address treatment, as two objects. `head` is everything
// except the final 8 characters, grouped in fours, WT_MUT, wrapped inside `w`.
// `tail` is those final 8 as "xxxx xxxx" in WT_INK with a 2px underline, on one
// line that can never wrap, which is the whole reason it is a separate object:
// these 8 are what the owner compares against a coordinator, so a line break
// through them is the one layout failure that changes what a person checks.
// Either out pointer may be NULL. Neither object is positioned; the caller
// places both.
void wt_addr_head_tail(lv_obj_t *par, const char *addr, int w,
                       const lv_font_t *headf, const lv_font_t *tailf,
                       lv_obj_t **head, lv_obj_t **tail);

// A status badge: `col` border, 5 percent `col` fill, radius 100, label at
// font14 in `col` with 1px tracking. Sizes itself to its text. This is what a
// state reads as in the design review, and it is not a pill: no press states,
// no click flag, nothing to tap. Use wt_state_chip_set to change the text and
// colour later, which re-measures the box for the new string and locale.
lv_obj_t *wt_state_chip(lv_obj_t *par, const char *txt, lv_color_t col);
void      wt_state_chip_set(lv_obj_t *chip, const char *txt, lv_color_t col);

// Hold-to-confirm pill: the action fires only after the finger has been held
// down for ms, and a fill sweeps across the pill while it does. Letting go
// early cancels and resets. Use this for anything a stray double tap must not
// be able to trigger — the sign screen's hold-to-sign is the same idea, and
// erasing a wallet is the other one.
lv_obj_t *wt_hold_pill(lv_obj_t *scr, const char *txt, int x, int y, int w, int h,
                       int ms, void (*done)(void *), void *ud);

// text helpers shared by receive/sign/info
void wt_group4(const char *in, char *out, size_t out_len);     // addr in blocks of 4
void wt_fmt_sats(uint64_t v, char *out, size_t out_len);       // 1234567 -> 1 234 567
void wt_fmt_btc(uint64_t sats, char *out, size_t out_len);     // 61000 -> 0.00061000
