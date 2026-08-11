// The cover word, recognised by shape.
//
// This lived as two file statics in main.c, which is the reason a real bug
// survived to a device test: main.c links into no test binary, so nothing on
// the host could ever draw a shape at the recogniser and ask what it said.
// "KIS" -- three letters -- opened the decoy, and every gate was green.
//
// Integer geometry only, no LVGL, for exactly the reason kiss_gword.h states
// for itself: the desktop runner has to be able to hammer it with the shapes
// this panel actually sees. See sim/test_coverword.c.
//
// Nothing in this path is called kiss_ or KISS_, deliberately. kiss_ is the
// product namespace -- it is on 61 modules, the fonts, the NVS namespaces and
// the build macros -- so a reviewer who greps for it to find the decoy opener
// gets 1300 hits and no signal. cw_ and cover_ name this feature and nothing
// else, which makes `grep -rn cover main/` the one search that finds every
// line of the way in. The letters K-I-S-S the owner actually draws are still
// KISS; they live in the comments and in the shapes, not in the symbols.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Points accepted in one call. Matches GEST_MAX in main.c, which is the buffer
// the collector fills.
#define CW_MAX_PTS 384

// Does this drawing match the cover word? The built-in one is the letters
// K-I-S-S; an owner who enrols their own goes through kiss_gword.c instead,
// and main.c asks that one first.
//
// xs/ys are the accumulated points of the whole draw, sid[i] the stroke each
// point belongs to (any monotone numbering; only changes matter), n the point
// count, strokes the number of pen lifts.
//
// Lenient on the letter shapes and strict on what tells a WORD from a smudge:
// this opens the DECOY, so a fumbled shape costs the owner nothing, while a
// tap or one flat swipe must never fire it.
bool cw_match(const int *xs, const int *ys, const uint8_t *sid,
                int n, int strokes);
