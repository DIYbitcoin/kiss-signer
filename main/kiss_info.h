// The WALLET tile: facts about the logged-in wallet (with plain-words "?"
// education) and PAIR COORDINATOR exports.
#pragma once
#include <stdbool.h>
#include "lvgl.h"

bool kiss_info_active(void);            // true while any of its screens is up
void kiss_info_open(lv_obj_t *parent);
// Shared fingerprint explainer, opened from the home chip, the WALLET row and
// the reveal screen's "?".
//
// `fingerprint` (eight hex characters, or NULL) puts the code in the title AND
// is what the card's picture is drawn from, so the two can never disagree.
//
// `exit_hint` appends the "tap the KISS logo to go back to the game" line. That
// gesture only exists on HOME, so only the home chip passes true. It used to be
// inferred from `fingerprint != NULL`, which was the same thing right up until
// the other two callers started passing a code as well.
lv_obj_t *kiss_info_fp_card_open(lv_obj_t *parent, const char *fingerprint,
                                   bool exit_hint);
// The plain "?" card, title over body over OK, for screens outside this file.
// There is one implementation of that card and it lives here, so Receive
// borrows it rather than growing a second one that drifts: the receive screen
// already had a hand rolled overlay for the silent payment chip and two is one
// too many.
// `icon` is a WT_ICON_* / LV_SYMBOL_* badge for the title row, or NULL. Only
// codepoints in tools/fonts/gen_fonts.sh's SYMS resolve; anything else draws a
// blank box the width of half a line.
lv_obj_t *kiss_info_help_card_open(lv_obj_t *parent, const char *title,
                                     const char *body, const char *icon);
// Settings owns the RECOVERY WORDS entry. The sensitive reveal/verify screens
// stay here so there is only one implementation of that flow; done_cb returns
// to Settings when the user leaves it.
// The backup page it opens carries the ENCRYPTED BACKUP row too: consent, a
// password typed twice, then the keys as a KEF envelope — a locked QR and
// optionally a .kef file on the card.
void kiss_info_open_words(lv_obj_t *parent, void (*done_cb)(void));
// RECEIVE's SILENT tab opens the same scan key consent gate and reveal that
// live on KEYS' COORDINATOR WALLET tab; done_cb returns to RECEIVE when the
// owner leaves. One flow, two doors — never a second consent to keep in sync.
void kiss_info_open_scan_key(lv_obj_t *parent, void (*done_cb)(void));
void kiss_info_close(void);             // idle auto-lock: drop whichever is up
