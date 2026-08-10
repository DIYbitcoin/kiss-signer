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
// The colour the scan reticle turns when it locks on, as raw 5/6/5 components.
// Components and not an lv_color_t because the caller is camera_spike.c, which
// blends straight into the framebuffer and owns no LVGL object on that screen.
// The accent, except in MONO -- see the body for why that one keeps the green.
void       wt_lock_565(int *r5, int *g6, int *b5);

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
// Re-fit the title into `w` px on ONE line, stepping 34 -> 28 -> 23. wt_screen
// already does this at 704, the full width between the page margins. Call it
// again, narrower, on any screen that puts something else on the title's row:
// a title has no width of its own, so a long translation simply keeps going
// and runs straight through whatever is up there.
void wt_title_fit(lv_obj_t *scr, int w);
// The title label of a wt_screen, or NULL if `scr` is not one. Use this rather
// than reaching for a child index: screen_card() is deliberately wt_screen's
// FIRST child, so lv_obj_get_child(scr, 0) is the decorative frame, and setting
// a text colour on it silently does nothing. Three screens recoloured the card
// for months believing they were recolouring the title. Index 1 is not the fix
// either, because it is only the title on screens that have no subtitle.
lv_obj_t *wt_screen_title(lv_obj_t *scr);
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
// moved depending on which screen you were escaping from.
//
// This said "one number, one corner, no exceptions" for a while and it was not
// true, which is worse than a rule nobody follows. The real rule turns on WHAT
// ELSE IS IN THE BAR:
//
//   the bar holds nothing but the exit  -> it takes this corner. Settings,
//             Wallet, Details, the file chooser, the pairing screens. There is
//             no consequential control for the corner to keep away from the
//             reflex tap, so consistency wins and the exit sits where the eye
//             already looks for it.
//   the bar holds the screen's real action too -> the exit goes LEFTMOST and
//             the far right is reserved for the action, because THAT is the
//             safety property: nothing that spends money should sit under the
//             thumb's resting corner. Sign's verify row and Receive's four-pill
//             row are the cases, and redraws 01, 02, 03 and 05 all draw them
//             this way.
//
// THE RULE IS ABOUT ESCAPING A SCREEN, NOT ABOUT THE WORD "BACK". STR_C_BACK
// does two unrelated jobs in this app and only one of them belongs here:
//
//   escape  - leaves for the level above (close_cb, files_back_cb, sp_back_cb,
//             the sign details page returning to verify). If a screen's escape
//             is called DONE instead, DONE takes the corner: the corner belongs
//             to the exit, whatever it is labelled.
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
// The caution grid's dust attack badge: a drop, for the smallest amount of a
// thing there is. Already in SYMS, so it costs no font rebuild.
#define WT_ICON_DUST    "\xEF\x81\x83"   // U+F043 tint
// "never shown", "not stored", "nobody can see this" -- the claim several
// explainers make in words. Already in SYMS.
#define WT_ICON_HIDDEN  "\xEF\x81\xB0"   // U+F070 eye-slash
// A coordinator running on a phone, for the pairing steps. Already in SYMS.
#define WT_ICON_PHONE   "\xEF\x82\x95"   // U+F095 phone

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

// ---- decoration ----
// Stamp an object that carries NO information: ambient motion, texture, a
// speck of atmosphere. The layout rules exist so content stays readable and
// stays out of the action bar, and a thing nobody reads has neither property
// to lose -- so the screen-walk gate skips these where the rule would
// otherwise fire on the wrong object. Use it sparingly and never on anything
// the owner is meant to look at: silencing a check is the whole cost.
void wt_mark_decor(lv_obj_t *o);
bool wt_is_decor(const lv_obj_t *o);
#ifdef SIMULATOR
// Screen coverage. The walk calls wt_sim_capture() from every save(), and asks
// wt_sim_uncaptured() at the end which screens it BUILT and never captured --
// the ones no gate has ever been able to question. See wallet_theme.c.
void wt_sim_capture(void);
int  wt_sim_uncaptured(int *out, int max);
int  wt_sim_built(int *out, int max);
const char *wt_sim_title_key(int id);
#endif
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
// RECOVERY WORDS + PASSPHRASE -> YOUR KEYS is what wt_diagram_fp says; this one
// says YOUR KEYS -> <code>, for the card that has to explain the code itself.
void wt_diagram_fpid(lv_obj_t *parent, const char *code);
// the backup check's claim: RECOVERY WORDS -> THIS WALLET (accent = the match).
void wt_diagram_verify(lv_obj_t *parent);
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
// wt_addr_short's fold as a plain string, for the places that take text rather
// than an object. Mono is not optional on the result: the fold only helps if the
// characters either side of the ellipsis are readable one at a time.
void wt_addr_fold(const char *addr, char *out, size_t len);
// A status badge: `col` border, 5 percent `col` fill, radius 100, label at
// font14 in `col` with 1px tracking. Sizes itself to its text. This is what a
// state reads as in the design review, and it is not a pill: no press states,
// no click flag, nothing to tap. Use wt_state_chip_set to change the text and
// colour later, which re-measures the box for the new string and locale.
lv_obj_t *wt_state_chip(lv_obj_t *par, const char *txt, lv_color_t col);
void      wt_state_chip_set(lv_obj_t *chip, const char *txt, lv_color_t col);

// ---- the review's settings row list ----
// A section eyebrow above a group of rows: font14, WT_MUT, tracked. Redraw 05
// groups Settings under THIS WALLET / YOUR BACKUP / NO UNDO instead of leaving
// eleven controls in one undifferentiated grid.
// Objects that wear the accent and must be repainted when it changes. A user
// flag rather than a list, because eyebrows and chevrons are built by shared
// helpers in six files and any list of them is a list that goes stale.
#define WT_FLAG_ACCENT LV_OBJ_FLAG_USER_1
// Repaint every WT_FLAG_ACCENT object under scr. Call after wt_accent_set.
void wt_accent_restyle(lv_obj_t *scr);

lv_obj_t *wt_row_head(lv_obj_t *scr, const char *txt, int x, int y, int w);

// One row of that list: `label` in WT_INK at font23, `sub` under it in WT_MUT at
// font14, an optional `val` right aligned in `vcol`, and a chevron at the right
// edge when `cb` is given. A hairline along the bottom, a pressed fill, no
// border: a row is a line in a list you pick from, not a button, and eleven
// stacked lozenges read as eleven competing controls. Height is WT_ROW_H.
//
// Pass sub or val as NULL to omit them. Returns the row so a caller can recolour
// its parts for a destructive group.
// 64, which is what a CARD needs and no more: one line of font23 from y=7 owns
// 7..35, the font14 sub under it owns 38..56, and 8px of bottom padding closes
// the box. It was 68 while these were flat list lines, to keep the value's box
// from sharing a band with the label's; the value is centred rather than sitting
// on the sub-line now, and the two are separated horizontally in any case, so
// the extra four pixels only bought air inside the card. Redraw 05 draws 56 for
// its smaller type; 64 is the same proportion at the type this device has.
#define WT_ROW_H 64
// Severity of a row CARD, applied after wt_row builds it. Redraw 05 tints the
// whole box rather than one note inside it, so a group reads before its words
// do: green for a state already satisfied, amber for a warning about the
// wallet, red for the pair that cannot be undone. PLAIN is WT_PANEL + WT_HAIR.
enum { WT_SEV_PLAIN = 0, WT_SEV_OK, WT_SEV_WARN, WT_SEV_STOP };
void wt_row_sev(lv_obj_t *row, int sev);
// Recolour a built row's sub-line and nothing else. For an option that is fine
// to pick but whose explanation is a warning -- FLASH storage on a chip with no
// encryption is the case this exists for.
void wt_row_sub_color(lv_obj_t *row, lv_color_t c);
// The row's BOX with nothing in it: WT_PANEL fill, WT_HAIR hairline, radius 10,
// not clickable, not scrollable. Position children relative to the card. Every
// screen that shows a block of content wears one, because the fill is what makes
// small type read on glass. wt_row_sev() tints it like any row.
lv_obj_t *wt_card(lv_obj_t *scr, int x, int y, int w, int h);
// A wt_card with a WT_EDGE edge and four bracket corners just OUTSIDE it: the
// camera preview on the scan and entropy screens. The brackets are outside on
// purpose, because on the device the camera paints over the rect itself and
// anything drawn inside is hidden from the first frame on.
lv_obj_t *wt_viewfinder(lv_obj_t *scr, int x, int y, int w, int h);
// The destructive row LABEL. Lighter than WT_STOP so it stays readable as text
// on a WT_STOP-tinted card: the drawing uses rgb(255,140,151) for "Replace this
// wallet" and "Erase this wallet", against the rgb(255,77,94) of the border
// around them. Full WT_STOP on the tint is the one combination that vibrates.
#define WT_STOP_INK lv_color_hex(0xFF8C97)
lv_obj_t *wt_row(lv_obj_t *scr, const char *label, const char *sub,
                 const char *val, lv_color_t vcol, int x, int y, int w,
                 lv_event_cb_t cb, void *ud);
// The same row with the sub-line and the value in fonts you choose (NULL keeps
// wt_row's font14 and font23). It exists for the WALLET screen: a fingerprint,
// a derivation path and an address are read CHARACTER BY CHARACTER against
// another screen, and the proportional face is the one that hides the
// difference between the glyphs you are checking. Everything else about the row
// is identical, including the one-line pinning and the ellipsis.
lv_obj_t *wt_row_f(lv_obj_t *scr, const char *label, const char *sub,
                   const lv_font_t *sf, const char *val, const lv_font_t *vf,
                   lv_color_t vcol, int x, int y, int w,
                   lv_event_cb_t cb, void *ud);
// The full row: a leading ICON badge, and a SELECTED state that swaps the
// chevron for a tick in the accent and puts the accent on the border. Both of
// the calls above land here with icon NULL and sel false, so this is the only
// place a row's geometry lives.
//
// The icon shifts every text on the row from x=14 to x=52, which is 38px off the
// label's lane. That is why SETTINGS and WALLET pass NULL: their cards are 365
// wide and already carry a label and a value on one line, and FINGERPRINT beside
// EC5A4595 has no 38px to give. Rows on the full 704 lane have it to spare, and
// those are the ones a mark actually helps -- a file, a card, a QR and a saved
// file are four things whose icons are recognised before their words are read.
//
// `sel` is for CHOOSERS, where the rows are options rather than destinations:
// STORAGE, ADDRESS TYPE, the word count. Only ever set it on one row of a group;
// nothing enforces that, because the caller is the only thing that knows which.
//
// Only codepoints in tools/fonts/gen_fonts.sh's SYMS resolve. One that is not
// draws a blank box the width of half a line, and it draws it identically in the
// simulator, so a wrong pick survives every gate and is caught on glass.
// `h` is 0 for the standard WT_ROW_H row, whose sub-line is pinned to ONE line
// and ellipsised. Pass a TALLER height and the sub becomes a paragraph instead:
// it wraps, and with sf NULL it takes the largest size that fits the box left
// under the label. That is what a chooser needs and a settings list does not --
// "saved here unencrypted, your passphrase guards your real wallet and is never
// saved here" is the reason somebody picks a different mode, and the half of it
// that an ellipsis eats is the half that matters.
lv_obj_t *wt_row_x(lv_obj_t *scr, const char *icon, const char *label,
                   const char *sub, const lv_font_t *sf,
                   const char *val, const lv_font_t *vf, lv_color_t vcol,
                   bool sel, int x, int y, int w, int h,
                   lv_event_cb_t cb, void *ud);

// The two-column list geometry Settings is drawn on, lifted out of it so the
// WALLET screen cannot drift from the screen it is meant to match. First
// eyebrow at WT_LIST_TOP, first card WT_LIST_HEAD below it, then WT_LIST_PITCH
// per row: four rows land at 379, above the 398 floor.
#define WT_LIST_L_X    25
#define WT_LIST_R_X   412
#define WT_LIST_W     365
#define WT_LIST_TOP    72
#define WT_LIST_HEAD   23
#define WT_LIST_PITCH  71    // WT_ROW_H plus 7 of gap
#define WT_LIST_Y(i)  (WT_LIST_TOP + WT_LIST_HEAD + (i) * WT_LIST_PITCH)

// The chooser lane, which every screen offering a short list of options draws
// on and none of them may drift from: the storage modes, the address types, the
// two ways into SIGN, the setup wizard's create-or-restore and its word count.
// 716 wide from x=36 keeps the page's 752 right margin.
//
// These used to be CARDS with a rounded pill centred inside and a paragraph
// floating beside it -- a button that said "press me" about a thing that is
// really either a destination or a setting, with its explanation orbiting it.
// They are wt_row_x lists now, which is the same idiom SETTINGS and WALLET are
// built from: the option is the row, its explanation is the row's sub-line, and
// the row says with a chevron or a tick which of the two kinds it is.
//
// The grid does NOT move: 96 is the content line every screen builds against,
// the pitch of 102 lands the third row's bottom edge on 396, and a 96px row is
// what gives the note three lines at font23 -- which is what Turkish,
// Portuguese and Russian actually need for a storage mode. Only the object
// changed. A row of this height wraps its sub-line instead of pinning it,
// which is the whole reason wt_row_x takes a height at all.
#define WT_CHOICE_X      42   // (800 - WT_CHOICE_W) / 2: rows centered, equal margins
#define WT_CHOICE_W     716
#define WT_CHOICE_H      96
#define WT_CHOICE_Y(i)  (96 + (i) * 102)

// A value in a box: small muted caption, then the value large and monospaced
// inside a bordered WT_PANEL card. The review draws every figure worth reading
// off the glass this way -- an ID code, a fingerprint, an amount -- because a
// bare label above bare text reads as a form field, while a framed value reads
// as the thing the screen is about. `big` picks font_mono28 over font_mono23 for
// the values a holder compares character by character. Returns the card; its
// height is whatever the content needed, so measure it before placing anything
// underneath.
lv_obj_t *wt_value_card(lv_obj_t *scr, const char *cap, const char *val,
                        int x, int y, int w, bool big);
void      wt_value_card_set(lv_obj_t *card, const char *val);  // value only, no rebuild

// A note with a coloured rule down its left edge: optional heading in WT_INK,
// body in WT_MUT, a 3px bar in `col`. The review's "why it matters" and "how
// you'll use it" pattern. Two of these side by side turn a centred paragraph
// nobody reads into two claims somebody can, which is the whole reason it
// exists. Pass head as NULL for body only. Pass f as NULL to take the largest
// size that fits `max_h`, so a short claim reads big and a long translation
// shrinks rather than overflowing; pass a font to make several blocks share one
// size. Returns the block so the caller can measure it.
// One shared size for a PAIR of blocks: the smaller of the two rungs, since
// the taller half decides whether either fits. Never pick by strlen.
const lv_font_t *wt_body_font2(const char *a, const char *b, int w, int max_h);
// Same, for a pair of why-blocks WITH headings. Measures the headings rather
// than charging the caller a constant for the worst case they might reach.
const lv_font_t *wt_body_font2_head(const char *h1, const char *b1,
                                    const char *h2, const char *b2,
                                    int w, int max_h);

lv_obj_t *wt_why_block(lv_obj_t *scr, const char *head, const char *body,
                       int x, int y, int w, int max_h, const lv_font_t *f,
                       lv_color_t col);

// A whole explainer body as ruled blocks, filling the room from `y` down to
// WT_CONTENT_BOTTOM. Splits the string on its blank lines and picks whichever
// of full width or two balanced columns reads best at the largest font that
// fits, so a screen gets the reveal screen's arrangement without hand placing
// anything. `sev` colours the first block (accent, or a status colour when the
// screen is a warning); the second is always WT_MUT.
//
// Costs nothing to translate: it splits copy that already exists.
// `two_col`: true on a full screen, where 2+ paragraphs should become two
// columns rather than one wide block; false on the explainer overlay, whose
// full width arrangement is tuned and approved.
void wt_why_body(lv_obj_t *par, const char *body, int y, lv_color_t sev,
                 bool two_col);

// ---- the explainer card, behind every "?" on the device ----
// Title top left like any other page, an optional icon badge on the title's row,
// an optional value card for the thing the card is about, an optional diagram,
// and the body below.
//
// The body layout is CHOSEN, not fixed. The paragraphs are split on blank lines
// and then measured at font28, 23 and 14 in turn, in two arrangements: the whole
// body across the full 704 lane, and the paragraphs dealt into two 344 columns at
// the boundary that makes the columns most nearly equal. The first arrangement
// that fits wins, so the type is the largest the page can actually hold instead
// of the largest a 330px column could. This is measurement only and costs no
// translation: the strings were already written as paragraphs in all 21 locales.
//
// `grid` overrides all of that for the one body that is a LIST rather than prose:
// see WT_GRID_ICONS.
//
// `aside` draws a diagram into (x, y, w) and returns the height it used, so the
// theme needs no dependency on the screens that own those diagrams.
//
// Tapping anywhere closes, as it always has.

// Body modes.
//   WT_BODY_PROSE  paragraphs, laid out as described above.
//   WT_GRID_ICONS  one `term: definition` per LINE, drawn as a grid of icon
//                  badges. `icons` supplies one glyph per line, in order, and
//                  must hold at least as many as the body has lines. The colon
//                  split is what makes this free: the glossary is written that
//                  way in every locale, so an icon grid needs no new string.
enum { WT_BODY_PROSE = 0, WT_GRID_ICONS };
typedef struct {
    const char *title;
    const char *sub;      // NULL to omit
    const char *icon;     // WT_ICON_* / LV_SYMBOL_*, NULL to omit
    const char *cap;      // caption over the value
    const char *val;      // NULL to omit the value card
    const char *body;
    const char *ok_txt;   // the dismiss pill's label, already translated
    int sev;              // WT_SEV_*: colours the title and the first rule
    int mode;             // WT_BODY_PROSE / WT_GRID_ICONS
    const char *const *icons;   // WT_GRID_ICONS only, one per body line
    int (*aside)(lv_obj_t *par, int x, int y, int w);
} wt_explain_t;
lv_obj_t *wt_explain_open(lv_obj_t *parent, const wt_explain_t *e);

// Split a `term: definition` line at its first colon. Writes the term into
// `head` and returns a pointer into `line` at the definition, or NULL when the
// line carries no colon at all (then the whole line is the term).
//
// Accepts the ASCII colon AND the full width one, U+FF1A: the Chinese glossary
// uses the wide form and a miss would put a whole sentence in the term slot.
// Trailing space before the colon is trimmed, which is what French needs.
const char *wt_split_colon(const char *line, char *head, size_t head_len);

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

// The backlight, so a screen that cannot be drawn is dark rather than torn.
// Implemented in main.c on both sides: real PWM on device, a no-op on the
// desktop. See the comment there for why a flash write and this panel cannot
// both have the cache.
void kiss_backlight_set(int on);

// Brightness 0..100 as a progress indicator, for the one window where nothing
// can be drawn. Blacks both framebuffers first or the light reveals tearing.
void kiss_backlight_level(int pct);
void kiss_panel_black(void);
