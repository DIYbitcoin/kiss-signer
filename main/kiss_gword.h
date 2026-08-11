// A word the owner writes, matched by SHAPE.
//
// The device does not read letters and this does not teach it to. It learns
// what one particular draw looks like, normalised, and later asks whether a new
// draw looks like the same thing. So the owner can write whatever they can
// write twice: a word, initials, digits, a squiggle. What is stored is a
// picture of the movement, not the letters in it.
//
// That is the honest limit, and it is worth being clear about which way it
// cuts. Nothing here can tell an owner they wrote "BITCOIN" -- it can only tell
// them this draw is or is not the draw it was taught. In exchange it costs no
// handwriting classifier, no per-letter training and no alphabet, and it works
// the same in every language on a device that speaks 21 of them.
//
// It is NOT a key. The passphrase is the key. This is a door, and a wrong draw
// opens nothing and says nothing, so the threshold below is set to be kind to
// a real hand rather than tight against an imagined attacker: a false reject
// costs the owner their device, a false accept costs them nothing that the
// passphrase was not already holding.
//
// Integer geometry only, no LVGL, so the desktop runner can hammer it with the
// shapes this panel actually sees. Same reasoning as kiss_duress.c.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Points a draw is resampled to. 32 is enough to keep the shape of a four
// letter word and small enough that the whole template is 66 bytes of NVS.
#define GW_PTS 32

// Normalised so two draws of the same word compare regardless of where on the
// panel they were made or how big. Coordinates run about -100..100.
typedef struct {
    int8_t  x[GW_PTS];
    int8_t  y[GW_PTS];
    uint8_t strokes;      // pen lifts, matched exactly: see gw_distance
    uint8_t set;          // 0 = nothing stored here
} gw_template_t;

// The largest distance still counted as the same word. Exposed so enrolment,
// unlock and the tests cannot drift apart on it.
#define GW_MATCH_MAX 26

// The shape of a raw draw, for the same reason and after they drifted anyway.
// GW_MATCH_MAX was exposed here and the two numbers that decide what a draw
// even IS were left one in each collector: enrolment took 512 points and folded
// every stroke past its twelfth into the twelfth, unlock took 384 and kept
// every boundary. A word between 385 and 512 points, or of more than twelve
// strokes, could be written twice, confirmed, saved -- and then never open the
// device, because `strokes` is matched exactly (see gw_template_t) and the
// points beyond 384 were never sampled. The owner's only symptom is a signer
// that stopped answering to them.
//
// 384 is unlock's budget and therefore the real one: it is what the panel can
// hand gw_make, and nothing that cannot be reproduced there is worth storing
// here. It covers the word AND the mark that follows it, so a word using all of
// it leaves nothing for the modifier -- which is the honest reason to keep a
// word short rather than a limit to raise.
#define GW_MAX_PTS     384
#define GW_MAX_STROKES 12

// Build a template from a raw draw. `sid` is the stroke id per point, as the
// game's collector already keeps. Returns 0, or -1 when the draw is too small
// or too short to be anybody's word.
int gw_make(const int *xs, const int *ys, const uint8_t *sid, int n,
            gw_template_t *out);

// How far apart two templates are. 0 is identical, higher is worse, and
// INT16_MAX means they are not comparable at all (different stroke counts, or
// either one unset).
int gw_distance(const gw_template_t *a, const gw_template_t *b);

// The same question with the threshold already applied.
bool gw_matches(const gw_template_t *a, const gw_template_t *b);

// ---- storage --------------------------------------------------------------
// NVS on device, RAM on host. Absent means KISS still stands.

// Copy the stored word out. Returns true when there is one.
bool gw_stored_get(gw_template_t *out);

// True when a word is stored, without copying it.
bool gw_stored_any(void);

// Persist a word, or pass NULL to clear it and put KISS back. Returns 0 only
// once the write is committed.
//
// Callers must READ IT BACK rather than trust the 0: see store_word in
// kiss_word_ui.c. A device whose flash refuses the write keeps opening on the
// old word, and that is invisible until the day the new one is needed.
int gw_stored_set(const gw_template_t *t);

#ifndef ESP_PLATFORM
// Host only: make the NEXT gw_stored_set fail, so the screen that says nothing
// was saved can actually be rendered by the walk and measured in 21 locales.
void gw_test_fail_next_set(void);
#endif
