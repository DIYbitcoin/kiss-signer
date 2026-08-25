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
#include "kiss_defrow.h"   // the definition rows' lane arithmetic

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
const lv_font_t *wt_font_mono18(void);
// The scale's GUARDED faces: the mono rung when every glyph of `s` fits the
// mono set, one sans rung DOWN otherwise -- never up into the hairline. For
// pages composing their own chrome content; the kit uses the same guards
// internally.
const lv_font_t *wt_chrome18(const char *s);
const lv_font_t *wt_chrome21(const char *s);
const lv_font_t *wt_chrome23(const char *s);
const lv_font_t *wt_chrome28(const char *s);
const lv_font_t *wt_font_mono21(void);
const lv_font_t *wt_font_mono23(void);
const lv_font_t *wt_font_mono28(void);
const lv_font_t *wt_font_mono34(void);
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

// THE BOTTOM RIGHT CORNER IS ALWAYS THE WAY OUT. Every screen, whether or not
// its bar holds anything else. The exit ends at 752; the screen's action starts
// at 48. There is no question to ask about a given screen, which is the point.
//
//   the bar holds nothing but the exit -> WT_BACK_X.
//   the bar holds the screen's action too -> the exit stays at WT_EXIT_X, which
//             is the SAME corner, and the action takes WT_ACT_X on the left.
//
// It used to be the reverse -- the corner did the screen's JOB, so the exit was
// pushed left the moment a screen gained an action. Two things were wrong with
// it. The way out moved depending on what else happened to be on the bar, so
// the one control every screen has was the one control with no fixed home. And
// it put the app's heaviest controls in the easiest place on the panel to hit
// without looking: HOLD TO SIGN, INSTALL, ERASE, REMOVE ALL, SHOW WORDS and
// TAP TO OPEN were all in that corner, and the invariant "a tap in the corner
// may never be irreversible" had to be defended one screen at a time, by making
// each of them a hold or putting a confirm in front of it.
//
// Reversed, that invariant holds by construction: the corner contains the exit,
// and an exit is the one control that undoes nothing. The holds and confirms
// stay -- they were right on their own merits -- but they are no longer what
// keeps the corner safe.
//
// This also settles the disagreement the old note complained about: ERASE THE
// WORDS took the corner while REMOVE SIGNED sat at 48 insisting the corner was
// exactly where it must not go. REMOVE SIGNED was right. Both are at 48 now.
//
// THE RULE IS ABOUT ESCAPING A SCREEN, NOT ABOUT THE WORD "BACK". STR_C_BACK
// does two unrelated jobs in this app and only one of them belongs here:
//
//   escape  - leaves for the level above (close_cb, files_back_cb, sp_back_cb,
//             the sign details page returning to verify). If a screen's escape
//             is called DONE instead, DONE is what moves: the two positions
//             belong to the exit and the action, whatever they are labelled.
//   paging  - steps within the screen you are already on, and always has a
//             NEXT beside it (the recovery words pages, the pairing QR page).
//             That pair stays adjacent on the LEFT, because splitting BACK and
//             NEXT across the full width to satisfy a corner rule would break
//             the one thing a paged sequence needs, which is that its two
//             halves look like one control.
//
// One more exemption, and it is the sharp edge of the reversal. Where the exit
// itself DESTROYS work on a single unconfirmed tap, it does not get the corner:
// the dice screen's CANCEL throws away a hand-rolled roll set, and the cards
// CANCEL throws away typed words. Putting those in the reflex corner is the
// exact harm the reversal exists to remove, so they keep their old left slot and
// the corner on those two screens stays empty. Give one of them a confirm and it
// can move like everything else.
#define WT_BACK_X          612   // the exit's left edge, for the standard 140px
                                 // pill: 612+140 = 752, the lane edge. It was
                                 // 610 while the corner held the ACTION and the
                                 // 2px sat on whichever pill happened to be
                                 // there; now one pill is in that corner on
                                 // every screen and the gap would be the most
                                 // looked at 2px on the device.
#define WT_EXIT_X          612   // the exit's left edge when the bar ALSO holds
                                 // the screen's action. Same corner: the way out
                                 // does not move when a screen gains an action.
                                 // Kept as a separate name so a grep finds both
                                 // halves of the rule, not just one.
#define WT_ACT_X            48   // and the screen's action takes the left.

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
// The KEYS/RECEIVE redesign's own marks, added to SYMS for it. The long arrows
// are NOT LV_SYMBOL_LEFT/RIGHT: those are chevrons, which read as a list
// disclosure, and the drawing uses a full arrow to say "this takes you
// somewhere". All five live in the FontAwesome face merged into the Montserrat
// ones -- IoskeleyMono carries 0x20-0x7E and three punctuation marks and none
// of this touches it.
#define WT_ICON_ARR_L  "\xEF\x81\xA0"   // U+F060 arrow-left
#define WT_ICON_ARR_R  "\xEF\x81\xA1"   // U+F061 arrow-right
// The screen system's three marks (tools/fonts/gen_fonts.sh names them too).
#define WT_ICON_WHAT   "\xEF\x84\xA8"   // U+F128 question: the [ ? ] tab's mark
#define WT_ICON_CAMERA "\xEF\x80\xB0"   // U+F030 camera: the SCANNING trail
#define WT_ICON_SIGN   "\xEF\x95\xB3"   // U+F573 file-signature: the SIGN trail
#define WT_ICON_ERASE  "\xEF\x8B\xAD"   // U+F2ED trash-alt: the gate's mark
#define WT_ICON_EXPAND "\xEF\x81\xA5"   // U+F065 expand
#define WT_ICON_LIST   "\xEF\x80\xBA"   // U+F03A list
#define WT_ICON_LINK   "\xEF\x83\x81"   // U+F0C1 link
// The caution grid's dust attack badge: a drop, for the smallest amount of a
// thing there is. Already in SYMS, so it costs no font rebuild.
#define WT_ICON_DUST    "\xEF\x81\x83"   // U+F043 tint
// "never shown", "not stored", "nobody can see this" -- the claim several
// explainers make in words. Already in SYMS.
#define WT_ICON_HIDDEN  "\xEF\x81\xB0"   // U+F070 eye-slash
// A coordinator running on a phone, for the pairing steps. Already in SYMS.
#define WT_ICON_PHONE   "\xEF\x82\x95"   // U+F095 phone
// The SETTINGS SECURITY tab. Deliberately NOT WT_ICON_SECRET: that glyph means
// silent payments everywhere else in the app, and a mark cannot say two
// things. Added to SYMS in the same edit that named it here -- an icon missing
// from the font hard-hangs the renderer rather than drawing a tofu box.
#define WT_ICON_SHIELD  "\xEF\x8F\xAD"   // U+F3ED shield-halved

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
// the ones no gate has ever been able to question. See kiss_theme.c.
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
// Muted wrapping body text, at the largest size that fits max_h. The text is a
// parameter because the size depends on it: this used to hand back an empty
// font14 label for the caller to fill.
lv_obj_t *wt_wrap(lv_obj_t *scr, const char *txt, int x, int y, int w, int max_h);
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

// ---- the fit helpers, when they give up -----------------------------------
//
// wt_pill_fit and wt_note_fit pick the biggest font that FITS, which makes them
// silent: hand either one a long string in a small box and it lands on font14
// and says nothing, so the string never looks like a bug in the source. That
// has now come off the bench three separate times -- loudest as "WHY IS THE
// TEXT SO SMALL, LITERALLY, I KEEP ASKING" -- and every time the fix was to cut
// words or to stop throwing the layout budget away, never to accept the size.
//
// So they tell somebody. A sink, host only, because the device has nowhere to
// put it and the point is to fail a GATE before a screen reaches glass.
// overlapcheck installs one and reports what lands here as a finding against
// the stop that was being built.
#ifndef ESP_PLATFORM
typedef void (*wt_fit_sink_t)(const char *kind, const char *txt,
                              int w, int h);
void wt_fit_set_sink(wt_fit_sink_t fn);

// The same idea for a sub-line that has been ELLIPSISED. A sub-line is pinned
// to one line with LV_LABEL_LONG_DOT, so copy too long for its lane does not
// overflow -- it silently loses its second half, and nothing on the screen or
// in the source says so.
//
// It has to be measured HERE, as the label is built. LVGL rewrites the label's
// own text to insert the dots, so by the time a gate walks the tree the
// original string is gone and what is left measures exactly one lane wide.
typedef void (*wt_cut_sink_t)(const char *txt, int want, int lane);
void wt_cut_set_sink(wt_cut_sink_t fn);
#endif
lv_obj_t *wt_section(lv_obj_t *scr, const char *txt, int x, int y);  // column caption

// White QR card; *qr receives the lv_qrcode (NULL if creation failed). Every
// card is tappable and has an external "+" cue; tapping opens a crisp,
// re-encoded full-screen view rather than scaling the original bitmap.
lv_obj_t *wt_qr_card(lv_obj_t *scr, lv_obj_t **qr, int x, int y, int card_px, int qr_px);
// Open the zoom overlay from somewhere that is not the card: RECEIVE's
// "TAP TO ENLARGE" line is a second way into the same overlay, and the state
// the opener needs already hangs off the QR.
void wt_qr_zoom(lv_obj_t *qr);
// Hide/show a wt_qr_card by its qr when the payload could not be derived.
void wt_qr_refusal(lv_obj_t *qr, bool locked);
// Update a QR created by wt_qr_card. This caches the exact payload for zoom and
// keeps animated QRs moving while enlarged. Use instead of lv_qrcode_update().
lv_result_t wt_qr_update(lv_obj_t *qr, const void *data, uint32_t data_len);
// Take a secret OUT of a QR before its screen goes. wt_qr_update already keeps
// the cached payload in a wiped-on-free block, but the drawn code is the same
// secret in another form: the module bitmap lives in an lv_draw_buf that LVGL
// frees without scrubbing, and a QR is machine readable by construction. This
// zeroes the cache and clears the bitmap, on the QR and on its zoom if one is
// open. For payloads that are actually secret -- the silent-payment scan key.
void wt_qr_scrub(lv_obj_t *qr);

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
lv_obj_t *wt_chip(lv_obj_t *row, const char *txt, bool accent); // underlined term
lv_obj_t *wt_diagram_op(lv_obj_t *row, const char *txt);     // "+", arrow, etc.
// the deniability equation: WORDS + PASSPHRASE -> FINGERPRINT (accent result).
void wt_diagram_fp(lv_obj_t *parent);
// RECOVERY WORDS + PASSPHRASE -> YOUR KEYS is what wt_diagram_fp says; this one
// says YOUR KEYS -> <code>, for the card that has to explain the code itself.
void wt_diagram_fpid(lv_obj_t *parent, const char *code);
// the backup check's claim: RECOVERY WORDS -> THIS WALLET (accent = the match).
void wt_diagram_verify(lv_obj_t *parent);
// the airgap in words, stacked: ONLINE APP, down/up QR, KISS OFFLINE (accent =
// the signer). Column shaped, so it fits the PAIR card's 344px aside.
void wt_diagram_pair(lv_obj_t *parent);
// the airgap DRAWN: a phone showing a QR, this signer's camera framing it,
// a dashed break between. Fixed size; returns the figure for the caller to
// place. The SIGN page's hero.
lv_obj_t *wt_diagram_airgap(lv_obj_t *parent);

// ---- the sign screen's bundle graph ----
// Coins on the left, a junction, where the money goes on the right, and every
// strand as thick as its share of the value. It exists because the old verify
// screen stated the same facts as text panels and left the reader to assemble
// the SHAPE of the transaction in their head: which coin is big, how much of
// the send the fee really is, whether anything comes back.
//
// Roles, not colours, because the caller must not be able to paint a strand
// something the theme did not sanction. IN is WT_MUT until its signature
// exists, SEND is WT_INK, FEE is WT_DIM, CHANGE is the accent -- and CHANGE is
// the only accent text in the graph, so "arriving" and "leaving" stay two
// families under all four accents including MONO, where the accent is ink.
//
// The AMOUNTS follow one law and the caller states it in words on the screen:
// WT_INK leaves your control (a recipient's amount and the fee alike), the
// accent comes back to you (change), WT_MUT is a coin being spent until its
// signature exists. Colour is never the only cue for any of them -- each row
// carries the glossary's own mark for what it is, so the fee is told from the
// send by a pair of scissors and not by a shade of white.
// WT_STRAND_LINKED is an input on a spend wide enough to raise the coins
// linked caution. It is the same strand in WT_WARN, and it is the caution
// DRAWN: the convergence on the junction is what the warning is describing, so
// the screen can point at it instead of asking the reader to picture it.
// Appended, never inserted -- these are stored in the widget by value.
enum { WT_STRAND_IN = 0, WT_STRAND_SEND, WT_STRAND_FEE, WT_STRAND_CHANGE,
       WT_STRAND_LINKED };

// Strands the graph can hold in total. Five is what the elision leaves on the
// input side at any coin count (first two, the group, last two). The output
// side is NOT elided at any count -- each output is a place your money goes,
// and one folded into a group would be a destination visible nowhere -- so it
// needs room for every output a PSBT may carry plus the note row a spend with
// no change adds. 24 covers that with slack and is still a fixed bound: a
// caller may not size this block from a field an attacker writes.
#define WT_BUNDLE_MAX 24

typedef struct {
    uint64_t    sats;
    const char *label;      // the words beside the amount; NULL for a bare input
    uint8_t     role;       // WT_STRAND_*
    bool        signed_ok;  // repaint this strand in wt_accent(): its signature landed
    bool        is_group;   // the elided middle: dashed, and holds group_n coins
    uint16_t    group_n;
    // A row that reserves its place and its words but has no strand and no
    // amount, because there is no output. "no change, this empties all 20" is
    // the case: the row has to exist, or a spend that keeps nothing back is
    // drawn as a spend with one fewer destination and the reader is left to
    // notice an absence. Drawing a strand to it would be worse still -- a line
    // to a place the money does not go.
    bool        note_only;
    // The destination this output pays, drawn under its amount with the
    // compared runs lit. Set it when the graph is the ONLY place an address
    // can appear -- more than one recipient, where a single line under the
    // graph could name only the first and would leave every other destination
    // readable nowhere. With one recipient the screen puts it below the graph
    // at mono23 instead, which is the frame and the more legible of the two.
    const char *addr;
    // This wallet has signed to `addr` before (kiss_payee.h). Draws a small
    // repeat mark beside the row's words -- recognition only, so a first
    // payment carries nothing and the ordinary case stays unmarked.
    bool        known;
} wt_strand_t;

// A strand's stroke, in px, linear on the largest strand in the transaction.
//
// The floor is not cosmetic. An 800 sat fee against a 4.2M send computes to
// zero, and a fee that vanishes is the one number on this screen that must not:
// 2px is the thinnest stroke that still reads as a line on this panel.
int wt_strand_px(uint64_t sats, uint64_t max_sats);

// Build the graph into (x, y, w, h) -- the box the drawing's path data is
// expressed in, 1:1, so a page coordinate is the box origin plus a path
// coordinate. Returns the container, which owns every strand, every label and
// the point arrays LVGL refuses to copy (see the note on lv_line below).
//
// Callers draw the two captions themselves: they sit ABOVE this box and belong
// to the screen, not to the graph.
//
// `max_sats` is passed in rather than taken from these arrays, so a graph
// showing part of a scrolling output column still scales against the whole
// transaction. Renormalising per screenful would make a strand's thickness mean
// something different after a scroll, which is the one thing it may never do.
lv_obj_t *wt_bundle(lv_obj_t *scr, int x, int y, int w, int h,
                    const wt_strand_t *in,  size_t n_in,
                    const wt_strand_t *out, size_t n_out,
                    uint64_t max_sats);

// The graph's output column, which scrolls when its rows do not fit. Returned
// so the sign screen can keep asking it the question it has always asked a
// panel of destinations: is anything below the fold, and has it been read.
// The graph owns the strands; the caller owns what the answer means.
lv_obj_t *wt_bundle_outputs(lv_obj_t *bundle);

// What the graph is doing.
//
//   LIVE     the transaction as verified, waiting for a decision.
//   HOLDING  a finger is down: the outputs stand down exactly as they do below,
//            because where the money goes was settled on the screen behind this
//            one and holding the button is not a decision about it. The inputs
//            are left at their resting mute ON PURPOSE -- wt_bundle_hold draws
//            the accent OVER them, and a strand already at WT_INK gives it
//            nothing to be drawn over. Brightening here made the fill invisible.
//   SIGNING  the key is working: inputs go WT_INK at full strength, outputs go
//            WT_EDGE. The screen stops being about where the money goes and
//            starts being about the coins being signed, and the dimmed output
//            side is what says the destinations are settled.
//   SIGNED   every input strand and its amount in the accent, together.
//
// Together, and not one at a time. `kiss_psbt_sign` is a single libwally call
// that signs every input inside it with no hook to count from, so a per coin
// sequence here would be a timer inventing steps -- and the whole claim of this
// screen is that a strand in the accent means a signature exists. It changes
// when that becomes true of all of them, which is the moment the call returns.
enum { WT_BUNDLE_LIVE = 0, WT_BUNDLE_HOLDING, WT_BUNDLE_SIGNING,
       WT_BUNDLE_SIGNED };
void wt_bundle_state(lv_obj_t *bundle, int state);

// SIGNED, arrived at over `ms` instead of between two frames.
//
// It is the same end state and the same claim: every input strand and its
// amount in the accent, together, because one libwally call signed all of them.
// What changes is that the colour crosses from WT_INK to the accent over time
// rather than in one repaint, so the moment the whole screen has been building
// toward is something the eye can follow. Nothing here is a progress bar and
// nothing is per input: the work is already done when this starts, all strands
// move on the same value, and they all finish together.
//
// Calls wt_bundle_state(WT_BUNDLE_SIGNED) itself at the end, which is what sets
// the accent FLAGS a later theme change repaints from -- the interpolated
// colours in between are not flagged and are not meant to survive one.
void wt_bundle_signed_reveal(lv_obj_t *bundle, uint32_t ms);

// The hold, drawn on the graph. 0 is at rest, 255 is every input strand landed
// at the junction and no signature yet. Call it on each tick of the hold with
// the same fraction the ring is given.
//
// It is that fraction and nothing else: not a per coin position, not an
// estimate of how long signing will take, and not a timer that keeps running
// after the hold completes. All the strands fill at one rate and arrive
// together, because one call is going to sign all of them.
//
// It moves strands, not numbers. A breakdown of several coins stays WT_MUT
// until WT_BUNDLE_SIGNED, which is the moment a signature exists: the strand is
// the commitment, the label is the signature. A single input row is the input
// TOTAL rather than one coin of several -- it rests at WT_INK and at mono23,
// because it is half the arithmetic the reader is here to do -- and for it the
// signature is the crossing to the accent, which is the step that says a
// signature exists either way.
void wt_bundle_hold(lv_obj_t *bundle, uint8_t progress);

// Grouped address with only the LAST 8 characters lit, everything before them
// muted. Not the first: every Native SegWit address begins bc1q (or tb1q), so
// highlighting the front invited people to compare a constant and feel checked.
// The tail carries real entropy and the bech32 checksum, so a swapped address
// always differs there.
lv_obj_t *wt_addr_spans(lv_obj_t *par, const char *grouped, int w, const lv_font_t *f);
// Same, but the compared tail is lifted one rung (14 -> 23) so it can carry
// legibility on its own. For screens that draw the compared run NOWHERE else;
// where a blocked mono23 copy sits beneath, use wt_addr_spans. No-op above 14.
lv_obj_t *wt_addr_spans_lift(lv_obj_t *par, const char *grouped, int w,
                             const lv_font_t *f);

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
// The accent is not always TEXT. A flag that only ever meant "repaint the text
// colour" silently did nothing on the two objects that carry the accent without
// any text in them -- a strand, which paints with LV_STYLE_LINE_COLOR, and a
// pill's rim, which paints with LV_STYLE_BORDER_COLOR. Both went stale the
// moment the accent changed with the screen up, and neither could be seen to,
// because a flagged object with an unhandled property fails silently by
// construction.
//
// So the flag says WHICH channel:
//   WT_FLAG_ACCENT         the object's own ink -- text, or line, or arc.
//   WT_FLAG_ACCENT_BORDER  its rim.
//   WT_FLAG_ACCENT_BG      its fill, from wt_accent_bg(), and the pressed fill
//                          with it, so a control does not answer a press in
//                          last theme's colour.
//   WT_FLAG_ACCENT_FILL    its fill at full strength, from wt_accent(). BG is a
//                          tint behind text and this is the object itself being
//                          the mark -- the junction dot, where a stale colour
//                          would read as a seam in the drawing rather than as a
//                          control in the wrong theme.
// They compose: the hold pill wears BORDER and BG together.
#define WT_FLAG_ACCENT_BORDER LV_OBJ_FLAG_USER_2
#define WT_FLAG_ACCENT_BG     LV_OBJ_FLAG_USER_3
#define WT_FLAG_ACCENT_FILL   LV_OBJ_FLAG_USER_4
//   WT_FLAG_ACCENT_SCROLL  its SCROLLBAR. A part rather than a channel, and it
//                          needs its own flag because accent_walk repaints
//                          LV_PART_MAIN only -- a scrollbar painted with the
//                          accent and no flag is correct once and stale for
//                          every theme after.
#define WT_FLAG_ACCENT_SCROLL LV_OBJ_FLAG_USER_1
// Repaint every flagged object under scr. Call after wt_accent_set.
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
// Recolour a built row's icon badge to the accent, theme-change safe. For a
// mark that names an identity elsewhere and has to keep one colour everywhere.
void wt_row_icon_accent(lv_obj_t *row);
// The opposite, for a row that has already spent its colour on a severity
// tint. Every badge is accent by default now, and an accent mark on a green or
// amber card reads as a second verdict.
void wt_row_icon_mute(lv_obj_t *row);
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

// ---- SETTINGS: the section tabs ----------------------------------------
// The accordion laid on its side. Five groups, one on screen at a time, so a
// group can hold four rows on the full page lane instead of nine rows fighting
// for one 800x480 page in two 365px columns.
//
// The highlight is its OWN object, under the buttons, and it SLIDES between
// them. It has to be its own object to move at all: styling the selected
// button meant a tab change could only be a rebuild, and a rebuild cannot
// carry which way along the strip you went. Nothing else about a button
// depends on selection -- its ink and its mark come from `stop` -- so the
// buttons themselves are identical whether or not they are the one you are on.
//
// `icon` must be a codepoint in tools/fonts/gen_fonts.sh's SYMS. One that is
// not draws a blank box half a line wide, identically in the simulator, so a
// wrong pick survives every gate and is caught on glass.
#define WT_TAB_H     46
#define WT_TAB_W    144
#define WT_TAB_PITCH 152   // 144 + 8 of gap
typedef struct {
    const char *icon;
    const char *label;
    bool        dot;    // a 7px WT_WARN mark: something in this group wants reading
    bool        stop;   // the destructive group, in WT_STOP_INK on a WT_STOP tint
} wt_tab_t;
// Builds `n` tabs left to right from (x, y); the one at `sel` wears the
// highlight. `cb` is called with the tab's index as its user data.
//
// Returns the HIGHLIGHT, which is the handle wt_tabs_select needs and the only
// part of the strip that ever moves. It is not the first button: nothing on
// the page has ever wanted that, and a caller holding a button could not slide
// anything.
lv_obj_t *wt_tabs(lv_obj_t *scr, const wt_tab_t *tabs, int n, int sel,
                  int x, int y, lv_event_cb_t cb);
// Slide the highlight from tab `from` to tab `to`, over WT_TAB_MS. `stop` is
// the destructive group's tint, which is a different fill and a different
// border, so it cross-fades over the same span rather than snapping at either
// end. Safe to call while a previous slide is still running: it takes the
// highlight from wherever it currently IS.
#define WT_TAB_MS 200
void wt_tabs_select(lv_obj_t *hl, int from, int to, bool stop);

// The one line under a group of wide rows, at font23 in the page's margin.
// `rows` is how many the group drew, so the line lands under the last one.
void wt_group_note(lv_obj_t *pane, int rows, const char *txt);

// The scrollbar on a scrolling list: accent, half opacity, and flagged so it
// survives a theme change. The caller still picks LV_SCROLLBAR_MODE_ON or OFF.
void wt_list_scrollbar(lv_obj_t *list);

// ---- the group that MOVES ----------------------------------------------
// A tabbed page holds ONE group at a time, and a tab change slides the old one
// out while the new one arrives. That needs two panes alive at once, a latch
// saying the arriving one has not settled, and the strip's highlight -- five
// pieces of state that lived as statics in kiss_settings.c until a second page
// wanted the same chrome. They are a context now, owned by the page's module.
//
// Nothing captures a pointer to a pane. The two animation callbacks that reach
// past their own object take THIS, through the animation's user_data: by the
// time either fires the pane it meant may already have been deleted, by a
// second tab tap or by the screen closing over it, and the context is the only
// thing that reliably outlives both.
typedef struct {
    lv_obj_t *scr;        // the page the groups are built on
    lv_obj_t *pane;       // the group on screen
    lv_obj_t *pane_out;   // the group leaving, alive for its own 200ms
    lv_obj_t *tabs;       // the strip's handle: wt_tabs' highlight, or the
                          // whole strip from wt_tabs_flex
    int       tab;        // which group is open. Survives a page rebuild.
    bool      entering;   // the arriving group has not settled yet
    // How this page's strip moves its marker. NULL means wt_tabs_select, which
    // is every page that came before the bracket strip; the tabbed chrome
    // pages set wt_tabs_flex_select. A hook rather than a kind enum because
    // the two take the same four arguments and wt_pane_go calls one.
    void (*select)(lv_obj_t *tabs, int from, int to, bool stop);
} wt_pane_t;

// A transparent, unclipped, untappable 800x480 layer to build a group into.
// Does NOT assign p->pane -- the caller does, because a tab change wants the
// old one held in p->pane_out first.
lv_obj_t *wt_pane_new(wt_pane_t *p);
// Mark a child as scenery rather than a row: it fades on its own schedule and
// is never dealt a row's slide. The stop group's wash is the only one.
void wt_pane_scenery(lv_obj_t *child);
// Watch the strip so the context forgets it when the screen takes it.
void wt_pane_tabs_watch(wt_pane_t *p);
// The whole tab change: `stop` is the destructive group's tint and its rise,
// `build` is the page's own switch over p->tab. Safe to call mid-flight.
void wt_pane_go(wt_pane_t *p, int tab, bool stop, void (*build)(void));
// Point at the cautions in a group already on the glass, without moving it.
// The attention chip's second tap wants this and not a whole entry: the group
// is not arriving, it is being indicated.
void wt_pane_point(const wt_pane_t *p);
// Everything moving, stopped, and both lanes accounted for. Every route off
// the page calls this before dropping the screen.
void wt_pane_stop(wt_pane_t *p);
// The two halves of wt_pane_go, for a page that needs them apart.
void wt_pane_enter(wt_pane_t *p, int dir, bool rise);
void wt_pane_exit(wt_pane_t *p, int dir);

// ---- swipe: the page as a horizontal deck -------------------------------
// A tabbed page is a deck of panes; a paged list inside a tab extends the
// deck with its pages. wt_swipe_step reads a finished horizontal stroke
// (+1 left, -1 right, 0 neither) and swallows the rest of the press so a
// swipe never clicks the row it started on; wt_swipe_watch makes the page
// screen the gesture's terminus; wt_page_flip rebuilds a pane and slides it
// in from the side the flip came from (same-tab, unlike wt_pane_go);
// wt_pager_line is the deck's foot -- count/hint left, page dots right, dots
// only up to WT_PAGER_DOTS_MAX pages (beyond that the count line carries the
// position alone, full width).
#define WT_PAGER_DOTS_MAX 8
int wt_swipe_step(lv_event_t *e);
void wt_swipe_watch(lv_obj_t *scr, lv_event_cb_t cb);
void wt_page_flip(wt_pane_t *ctx, void (*build)(void), int dir);
lv_obj_t *wt_pager_line(lv_obj_t *p, const char *txt, bool warn, int page,
                        int npages);

// ---- KEYS / RECEIVE: the borderless idioms ------------------------------
// Three shapes that exist so those two screens can drop the card entirely: a
// row is a line with a rule under it, a tab is marked with brackets, and a
// button is an arrow with no box. Everything here is additive -- wt_row_x,
// wt_tabs and wt_pill are untouched, because the rest of the device still
// reads in that language and a half-converted device reads in neither.
//
// Three sizes the handoff asks for are not on this device's ladder, which is
// 14/23/28/34 and mono 14/23/28. Substituted once, here, rather than per call
// site: font16 brackets -> wt_font_mono14 (a bracket is a MARK, and 23 would
// stand a 30px glyph in a 30px tab), font20 arrows -> wt_font23, font18
// explainer -> wt_note's own 23-or-14 ladder, which reports through FIT when
// it gives up instead of silently shrinking.

// The line row. No fill, no border and no radius at rest: the row is invisible
// until you touch it, and the pressed state below is therefore the ENTIRE
// affordance of the redesign rather than a flourish on top of one.
//
// `val` may be NULL for a row that draws its own value -- an address needs a
// spangroup with its last eight lit, which no signature short of passing the
// spans could express. Build it at (WT_LINE_PAD, wt_line_val_y()) and it lands
// exactly where a plain value would have.
//
// A row with no `cb` gets no arrow, no radius and no pressed style. It is not
// a dimmed control, it is not a control: KEYS' NETWORK line is the only one.
// The value's tag, so wt_line_row_stage can find it among the caption, the
// sub and the arrow -- and so a caller building its own value (an address
// needs a spangroup) can opt into the same beat.
#define WT_LINE_VAL_TAG ((void *)0x57A6E)
#define WT_LINE_PAD  14   // left inset for the caption and the value
#define WT_LINE_CAP_Y 6   // caption's top inside the row
// The value's top inside the row: under the caption with a 4px gap. A function
// rather than a constant because the mono14 line height is what it is measured
// from, and that moves with the face.
int wt_line_val_y(void);
// The pressed rail on its own, for a row that is not a caption-over-value line
// -- RECEIVE's derivation path is one line with its value on the right, and it
// has to answer a press exactly the way the lines above it do or the page has
// two kinds of touchable.
void wt_line_press(lv_obj_t *row);
lv_obj_t *wt_line_row(lv_obj_t *par, int x, int y, int w, int h,
                      const char *cap, const char *val, const lv_font_t *vf,
                      lv_color_t vcol, const char *sub, const lv_font_t *sf,
                      lv_event_cb_t cb, void *ud);
// The 1px WT_DIV rule that belongs to a line row, as its OWN object at y + h,
// so the entry animation can draw it with transform_scale_x without touching
// the row's box -- and so overlapcheck, which reads real positions, never sees
// it move. Returns it because the animation needs the handle.
lv_obj_t *wt_line_rule(lv_obj_t *par, int x, int y, int w);
// The rule DRAWS itself in, left to right, behind the line that just rose.
// Motion 3 and 15 of the handoff, which ask for transform_scale_x with a left
// pivot -- and that is the one thing not to use. A transform puts LVGL on the
// layer path: four transformed 704x1 rules on one screen took free heap from
// 80KB to 24KB with the largest free block at 3.5KB, and the language picker
// two stops later then spun for ever inside a failed allocation. Animating the
// WIDTH is the same picture and allocates nothing.
void wt_line_rule_draw(lv_obj_t *rule, int delay_ms, int ms);
// A line's VALUE arrives after the line does: motion 4 and 5, opacity up and
// 7px of travel, on the beat the row came in on. `k` is the line's index
// within its group, 0-based.
void wt_line_row_stage(lv_obj_t *row, int k);

// The 19px round "?" that marks a row whose whole box opens an explainer.
// wt_help_chip is the same idiom at 30px, which is the size of a chip you aim
// at; this one is a SIGN on a target you cannot miss, so it is smaller and
// takes no taps of its own -- the row under it does.
lv_obj_t *wt_help_mark(lv_obj_t *par, int x, int y);

// The blinking block after a page title. It is the one thing on either screen
// that repeats for ever, and it does NOT need deleting by hand on the way out:
// LVGL's object destructor calls lv_anim_delete(obj, NULL) on every object it
// frees, so the animation goes with the screen. Written down because the
// handoff asks for the delete, and a delete that duplicates the framework is
// dead code that reads like a safety net.
lv_obj_t *wt_title_cursor(lv_obj_t *scr);

// The bracketed tab row's shared geometry (wt_tabs_flex draws it now; the
// fixed-pitch wt_brackets strip it replaced is gone).
#define WT_BR_H      30   // a tab
#define WT_BR_STRIP_H 36  // the tabs, the gap, and the rule at the bottom of it

// The arrow action. The action bar's control with no box at all: a label, and
// an arrow pointing WHERE THE TAP TAKES YOU -- leading the label when it
// leaves this screen, trailing it when it opens or advances.
//
// `primary` puts the accent on the label as well as the arrow. There is no
// filled primary on these screens; a fill would be a box, which is the thing
// being removed.
//
// Positions are the pill bar's: WT_ACT_X for the screen's own action, and
// WT_BACK_X for the exit. Pass `right` to right-align inside x..x+w instead of
// left-aligning at x, which is what keeps BACK's arrow against the margin
// when a translation changes the label's width.
// Re-label one, for a control whose word CHANGES -- the silent payment
// screen's fold toggle says FULL ADDRESS or SHORT ADDRESS depending on which
// way the tap goes. It is not lv_label_set_text on a child: the control's box
// IS its two labels, so the width and the arrow's position both have to move
// with the word, and the arrow is child 0 on a forward action, which is what
// made a naive setter re-label the arrow and draw the word twice.
void wt_arrow_action_set_text(lv_obj_t *ctrl, const char *txt);
lv_obj_t *wt_arrow_action(lv_obj_t *scr, const char *txt, bool back,
                          bool primary, int x, int y, int w, bool right,
                          lv_event_cb_t cb, void *ud);

// ---- the SCREEN SYSTEM: chrome contract, [ ? ] tab, definition rows -----
// design_handoff_system/ Parts 1-3. Five fixed parts of chrome that no
// content shape may move, and the two idioms every page in that pass shares.
// Additive, like the KEYS/RECEIVE block above: wt_screen and everything on it
// are untouched, and screens move onto this one at a time.
//
// The lane arithmetic lives in kiss_defrow.h (pure integers, proven by
// kisstest); the geometry here is the rest of the contract.
#define WT_CHROME_TITLE_Y  14   // title top; mono28 ls3, WT_INK, at x=48
#define WT_CHROME_STRIP_Y  70   // the 30px row that carries tabs OR the trail
#define WT_CHROME_RULE_Y   99   // the static header hairline
#define WT_LANE_X          48   // the content lane -- the ONE part shapes use
#define WT_LANE_Y         114
#define WT_LANE_W         704   // its height is WT_DEF_LANE (kiss_defrow.h):
                                // 114 + 284 = 398 = WT_CONTENT_BOTTOM exactly
// The contract: bezel (wt_screen's card), title + blinking cursor, the strip
// row, the hairline, the lane, and the action band -- which is built HERE,
// unconditionally, because under the contract even a page with no control
// keeps the band (it carries the standing statement or the first-run hint).
// The title is WT_INK under this contract, not the accent: the cursor is the
// accent's one appearance in the header, and it is what makes the title read
// as a prompt rather than a decoration.
lv_obj_t *wt_chrome(lv_obj_t *parent, const char *title);

// The contract's TITLE treatment alone -- INK mono28 at (48,14) with the
// blinking cursor -- for a screen whose content cannot take the full
// contract: the sign chain's hero, facts strip and graph fill 64..390 and
// are device-tested, so the hairline at 99 and the 114 lane would cut
// straight through what the owner is approving. The header identity still
// lands; the middle does not move by a pixel.
void wt_chrome_head(lv_obj_t *scr);

// THE tab strip (frame 7a, grown into the only one): content-sized bracketed
// labels at chrome23 SPREAD across the 620 lane the [ ? ] divider leaves.
// Each tab's MARK is drawn when the whole row has room for the icons --
// the five-up settings strip has none and stays words-only, the two and
// three tab pages get theirs. Unselected tabs keep TRANSPARENT brackets, so
// the space is reserved and nothing shifts as selection moves; the
// destructive tab's label is the only WT_STOP text in any header. `cb` is
// called with the tab's index as its user data.
lv_obj_t *wt_tabs_flex(lv_obj_t *scr, const wt_tab_t *tabs, int n, int sel,
                       lv_event_cb_t cb);
void wt_tabs_flex_select(lv_obj_t *strip, int from, int to, bool stop);

// The trail: where the owner is, said as how they got there. An icon in the
// accent, then "PARENT / CHILD" at chrome23 ls2 WT_DIM, on the strip row.
// Replaces the subtitle idiom on every page that was opened FROM somewhere;
// a page with sibling views puts wt_tabs_flex on that row instead, never both.
// `stop` paints the icon full WT_STOP: the erase gate's one deliberate
// exception, so the red is in the breadcrumb before it is in the sentence.
lv_obj_t *wt_trail(lv_obj_t *scr, const char *icon, const char *path,
                   bool stop);

// The standing statement: the action band's left lane on a page that has no
// action of its own. An 8px dot then one mono18 ls2 line, both in `col`
// (WT_DIM for a background fact, WT_WARN on the word grid), saying something
// permanently true about the page. It never changes while the page is open.
// `pulse` breathes the dot -- the grid's caution earns it, nothing else does.
lv_obj_t *wt_standing(lv_obj_t *scr, const char *txt, lv_color_t col,
                      bool pulse);

// The [ ? ] explainer tab (Part 2). Not a content tab: it teaches instead of
// switching, so it is held off the real tabs by a 1px divider at x=668 and
// pinned by its RIGHT edge to x=752 -- the mark's rendered width varies with
// the glyph metrics, so a computed left edge drifts. All three parts wear the
// accent in every state; unlike a content tab its brackets never dim, because
// it is always available.
//
// `hint`: until [ ? ] has been opened once on this device, the mark breathes
// and the band's left lane carries this lowercase line -- but only a page
// whose left lane is empty may say it, so a page with its own action passes
// NULL and keeps the breathing mark alone. The kit stays string-free: the
// caller translates. Both stop for good on the first open, wherever it
// happens: the tab flips wt_help_seen itself, then calls `cb` to let the
// page swap its lane for the explainer.
lv_obj_t *wt_help_tab(lv_obj_t *scr, const char *hint,
                      lv_event_cb_t cb, void *ud);
// Whether [ ? ] has ever been opened. RAM here, one NVS byte in settings:
// kiss_settings_load restores it at boot via _set, and the hook (registered
// once, at boot) is how the first open reaches the store without the theme
// ever including nvs.h -- the same split the accent id already uses.
bool wt_help_seen(void);
void wt_help_seen_set(bool seen);
void wt_help_seen_hook(void (*persist)(void));

// What the tab shows: the content lane, replaced -- not a card, not an
// overlay. One headline sentence at mono28, one paragraph at mono18 (two
// lines max at 690), then 2-4 labelled facts on a 200px caption lane that
// never wraps and never widens: a caption that would wrap gets shorter copy.
// Nothing on it is interactive; BACK is how the owner leaves, same as ever.
typedef struct {
    const char *cap;   // upper case, mono18 ls2, the accent
    const char *val;   // mono18, WT_MUT
} wt_fact_t;
void wt_explain(lv_obj_t *scr, const char *headline, const char *para,
                const wt_fact_t *facts, int n);

// The in-place definition (Part 3). Tap a row and its explanation opens where
// the row already is; the others collapse to 34px ghosts to make the room.
// One list owns all of its rows because the height sums must agree to the
// pixel and the open/close animation moves every row in the same tick.
typedef struct {
    const char *cap;     // upper case caption, 168px lane, fixed
    const char *val;     // the value: never yields, never wraps
    const char *val_tail;// the value's LIT run: non-NULL renders the value as
                         // the device's address idiom -- a grey head and this
                         // tail as the final span of a flagged spangroup, so
                         // accent_walk repaints it like every other tail
    const char *sub;     // lower-case fragment; the element that YIELDS
    const char *plain;   // the definition: a plain sentence, 2 lines max
    const char *term;    // the real term, shown as "CALLED: <term>" -- never
                         // alone and never first
    bool        lamp;    // lead the value with an 8px state lamp
    lv_color_t  lamp_col;
    bool        lamp_pulse;
    // `closed_h` overrides this row's share of the closed lane (0 = LANE/n;
    // the overrides must still sum to the lane). The fingerprint hero that
    // once used it is gone -- the home page already headlines the same code.
    int         closed_h;
} wt_def_t;
// Builds the rows across the whole content lane, closed. Entry runs the
// KEYS/RECEIVE stagger (rise, fade, rule draws itself in). Returns the list
// handle; rows open and close themselves on tap.
lv_obj_t *wt_def_list(lv_obj_t *scr, const wt_def_t *defs, int n);
// Open row `idx` (-1 closes everything), animating every row's height in the
// same tick -- the walk uses it to photograph settled open states.
void wt_def_list_open(lv_obj_t *list, int idx);
// Told after every open/close with the new open index (-1 for none), for a
// page that keeps something outside the list in step with it.
void wt_def_list_on_change(lv_obj_t *list, void (*cb)(int open_idx, void *ud),
                           void *ud);

// ---- shape 4: the gate (frame 7d) ---------------------------------------
// One danger sentence under the mark, one paragraph, then the two lines that
// answer the only question an owner actually has at a gate: what survives
// this, and what does not. The kit stays string-free -- the exact captions
// (C_SURVIVES / C_NOT_SURVIVES) come in with the values.
//
// `stop` is Part 6's whole rule: WT_STOP is the irreversible and the erase
// gate is the only gate that gets it; every caution an owner can still walk
// back from is WT_WARN. Text on stop takes WT_STOP_INK so the sentence stays
// legible; the mark takes the full colour.
typedef struct {
    const char *mark;       // the gate's glyph; NULL takes WT_ICON_ERASE
    const char *sentence;   // one line, mono28, the danger ink
    const char *para;       // two lines max at 690, mono18, WT_MUT
    const char *warn;       // optional one-liner under the para, WT_WARN
    const char *surv_cap, *surv;   // WHAT SURVIVES, and its answer
    const char *goes_cap, *goes;   // WHAT DOES NOT, and its answer
    bool        stop;
} wt_gate_t;
void wt_gate(lv_obj_t *scr, const wt_gate_t *g);

// wt_hold_rule in a stated colour instead of the accent: the gate's hold
// fills its track in the danger colour, and a danger never restyles with the
// theme, so these carry no accent flags. `ink` is the label and the arrow,
// `fill` the track's fill -- WT_STOP_INK over WT_STOP on the erase gate,
// WT_WARN over WT_WARN everywhere retryable.
lv_obj_t *wt_hold_rule_c(lv_obj_t *scr, const char *txt, const char *held,
                         int x, int y, int w, int ms,
                         lv_color_t ink, lv_color_t fill,
                         void (*done)(void *), void *ud);

// ---- shape 6: the outcome (frame 7f) ------------------------------------
// A lamp, a headline that names the NEXT MOVE rather than the fact, one
// paragraph, and the facts an owner will re-read. Never a full-page tick:
// the result is information and the next action is the point. A failure uses
// this exact geometry with `ok` false -- the lamp turns WT_WARN and the
// headline says what to try -- so nothing jumps when a result turns out
// badly. Facts are optional; the rule above them only draws when they exist.
typedef struct {
    const char *headline;   // one line, mono28, WT_INK
    const char *para;       // mono18, WT_MUT
    const char *f1c, *f1v;  // 168 caption lane; NULL to omit
    const char *f2c, *f2v;
    bool        ok;
} wt_outcome_t;
void wt_outcome(lv_obj_t *scr, const wt_outcome_t *o);

// ---- shape 3: the grid you read aloud (frame 7c) -------------------------
// Twelve to a sheet, three columns of four, columns filled top to bottom so
// the numbers run 1-4 / 5-8 / 9-12. The number sits dim in a 30px lane so
// the WORD carries -- this is the screen an owner copies onto paper one line
// at a time and reads back across a desk. `first` is the sheet's first word
// index (0-based), `n` how many of `words` to draw, 12 at most.
//
// The words come as pointers because every caller already has them split;
// they are drawn and forgotten, never copied into the kit.
void wt_word_grid(lv_obj_t *scr, const char *const *words, int n, int first);

// The sheet dots, in the trail line at x=524 -- the band under a word grid
// is saying something more important. Current sheet in the accent.
void wt_sheet_dots(lv_obj_t *scr, int n, int cur);

// ---- SETTINGS: the full-lane row ---------------------------------------
// A sibling of wt_row_x, not a mode flag on it: the two have different internal
// geometry and sharing one function would mean a branch in every measurement.
//
// wt_row_x stacks the label over its sub because it was drawn for a 365px
// column. On the full 752px lane the label, its sub and the control fit on ONE
// line, which is what lets the page be read straight down the value column.
//
//   label   x=18, capped at 250px, one line, ellipsised
//   sub     x=268, one line, height pinned to the font's line height
//   control the chip, the chevron, or nothing, right aligned
//
// The 250px cap is not cosmetic. sim/overlapcheck.c compares BOXES, and an
// uncapped label box spans the whole row and therefore contains the value's
// box, which reports a collision against every value on the page.
// The mark on the right is a PROMISE about what the tap does, and the page
// keeps exactly two:
//
//   LV_SYMBOL_LOOP   the tap resolves HERE, now. The value advances to the
//                    next one in its set and the sub line under the label
//                    changes to match. Nothing opens, nothing is confirmed.
//   LV_SYMBOL_RIGHT  the tap LEAVES. A screen opens.
//
// Every one of these was a dropdown first. A dropdown is a third thing -- it
// neither resolves nor leaves, it hovers -- and on a set of two or three it is
// three taps and an overlay to do what one tap does. It also has to fit its
// options into a floating box, which is how the storage list shipped reading
// "SD C...". The sets here are two, three and four long. They cycle.
enum { WT_WIDE_CYCLE = 0,  // a value chip that ADVANCES where it stands
       WT_WIDE_CHIP,       // a value chip that opens a screen
       WT_WIDE_OPEN,       // an optional value and a right chevron: opens a screen
       WT_WIDE_INERT };    // present, stated, and dead. See the AMNESIC case.
#define WT_WIDE_X      25
#define WT_WIDE_W     752
#define WT_WIDE_H      60
#define WT_WIDE_PITCH  66   // 60 + 6 of gap
// Four rows land at 126, 192, 258 and 324; the last bottom edge is 384, clear
// of WT_CONTENT_BOTTOM at 398.
#define WT_WIDE_Y(i)  (126 + (i) * WT_WIDE_PITCH)
// The line under the last row of a group. ONE line, never two: a translation
// that does not fit is copy to shorten, not a paragraph to wrap.
#define WT_WIDE_EXPL_Y(rows) (WT_WIDE_Y((rows) - 1) + WT_WIDE_H + 12)
typedef struct {
    const char *label;
    const char *sub;
    lv_color_t  sub_col;   // zero for WT_MUT; set it when the sub is a status
    int         kind;      // WT_WIDE_*
    const char *val;       // NULL to omit
    const lv_font_t *vf;   // NULL for wt_font23()
    lv_color_t  vcol;      // zero for WT_INK
    bool        swatch;    // an accent dot before the value, inside the chip
    int         sev;       // WT_SEV_*, tinting the card as wt_row_sev does
    lv_event_cb_t cb;
    void       *ud;
} wt_wide_t;
lv_obj_t *wt_row_wide(lv_obj_t *scr, int y, const wt_wide_t *r);
// The row's CONTROL: the chip a value sits in, or the value itself on a row
// that only opens a screen, or NULL where a row states nothing. It exists so
// the page can land the setting a beat after the label it belongs to without
// knowing which of the four kinds drew it.
lv_obj_t *wt_row_wide_ctrl(lv_obj_t *row);
// The "?" after a wide row's label, on the round-mark idiom wt_help_chip
// defines. Shrinks the label's box to its text first, so the chip lands after
// the words instead of inside the label's 250px box.
lv_obj_t *wt_row_wide_help(lv_obj_t *row, lv_event_cb_t cb, void *ud);

// ---- overlays: the popover and the help card ---------------------------
// A full screen scrim with a floating box on it. The scrim is black at
// LV_OPA_70, which is load bearing beyond the look: sim/overlapcheck.c only
// treats content as buried under a backdrop at LV_OPA_50 or more, so a lighter
// scrim would report every row underneath as a collision.
//
// A tap anywhere on the scrim runs `close_cb`; a tap on the box itself does
// not fall through to it. Returns the BOX and writes the scrim to *scrim,
// which is the handle to delete -- deleting the box alone leaves the scrim
// swallowing every tap on the page.
lv_obj_t *wt_overlay_box(lv_obj_t *scr, lv_obj_t **scrim, int x, int y,
                         int w, int h, int radius, lv_event_cb_t close_cb);

// ---- the attention chip ------------------------------------------------
// Bottom left of the action bar, opposite the exit. It exists so a caution
// inside a COLLAPSED tab is still visible from every other tab -- the one
// thing section tabs cost, bought back. Sizes itself to its label. Absent at
// zero: there is no "all good" chip.
lv_obj_t *wt_alert_chip(lv_obj_t *scr, const char *txt,
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
// the values a holder compares character by character. Caption and value are
// both CENTRED in the card, so the same figure sits in the same place on every
// screen that frames one. Returns the card; its height is whatever the content
// needed, so measure it before placing anything underneath.
lv_obj_t *wt_value_card(lv_obj_t *scr, const char *cap, const char *val,
                        int x, int y, int w, bool big);

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
// The heading rung a why-block body implies: font14 only when the body is.
const lv_font_t *wt_why_head_font(const lv_font_t *body);
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
//                  badges. `icons` supplies glyphs in line order and
//                  `icons_count` says how many are safe to read. A body may have
//                  more lines; those entries render without a badge. The colon
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
    const char *const *icons;   // WT_GRID_ICONS only, in body-line order
    size_t icons_count;         // number of readable entries in icons
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

// The same hold, drawn as a label over a rule instead of a fill inside a pill.
// A SIBLING of wt_hold_pill and not a flag on it: every other hold on the
// device -- the storage move, erasing the recovery words -- keeps its pill,
// and a shared function would mean a branch in every measurement.
//
// The label sits at (x, y) with an arrow after it, and a `w` wide, 2px track
// runs 8px under it with the progress filling left to right. `held` replaces
// the label while the finger is down, so the screen answers the press with a
// word as well as a bar.
//
// Letting go returns the fill to 0 over 180ms rather than clearing it: a fill
// that VANISHES on release reads as an action that completed. A fill that runs
// back reads as one that did not.
//
// This belongs on WT_ACTION_Y, not WT_ACTION_Y_TALL. The tall row exists to
// give a fat pill room, and the label plus its 2px rule fits the standard 52.
lv_obj_t *wt_hold_rule(lv_obj_t *scr, const char *txt, const char *held,
                       int x, int y, int w, int ms,
                       void (*done)(void *), void *ud);

// text helpers shared by receive/sign/info
void wt_group4(const char *in, char *out, size_t out_len);     // addr in blocks of 4
// Display unit. A preference about rendering only: amounts are satoshis
// everywhere in this firmware, and nothing below the screen ever sees this.
#define WT_DENOM_SATS 0
#define WT_DENOM_BTC  1
int  wt_denom(void);
void wt_denom_set(int d);
const char *wt_denom_unit(void);       // "sats" / "BTC"
const char *wt_denom_unit_alt(void);   // the other one
// Make a label carrying an amount switch the unit when tapped. The screen
// says what a switch costs it (usually a repaint) through wt_denom_on_tap.
void wt_denom_bind(lv_obj_t *o);
void wt_denom_on_tap(void (*fn)(void));
void wt_fmt_bytes(uint64_t bytes, char *out, size_t out_len);  // 1.2 MB / 29.7 GB
void wt_fmt_amount(uint64_t sats, char *out, size_t out_len);      // in the chosen unit
void wt_fmt_amount_alt(uint64_t sats, char *out, size_t out_len);  // ...and in the other
void wt_fmt_sats(uint64_t v, char *out, size_t out_len);       // 1234567 -> 1 234 567
void wt_fmt_btc(uint64_t sats, char *out, size_t out_len);     // 61000 -> 0.00061000

// The backlight and the framebuffers moved to kiss_panel.h. They are main.c's,
// not the kit's -- they touch the DSI panel handle, which the kit never sees.
