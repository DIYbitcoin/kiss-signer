// The cover word, recognised by shape.
//
// This lived as two file statics in main.c, which is the reason a real bug
// survived to a device test: main.c links into no test binary, so nothing on
// the host could ever draw a shape at the recogniser and ask what it said.
// "KIS" -- three letters -- opened the decoy, and every gate was green.
//
// Integer geometry only, no LVGL, for exactly the reason wallet_gword.h states
// for itself: the desktop runner has to be able to hammer it with the shapes
// this panel actually sees. See sim/test_kissword.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Points accepted in one call. Matches GEST_MAX in main.c, which is the buffer
// the collector fills.
#define KW_MAX_PTS 384

// Is this drawing the word KISS?
//
// xs/ys are the accumulated points of the whole draw, sid[i] the stroke each
// point belongs to (any monotone numbering; only changes matter), n the point
// count, strokes the number of pen lifts.
//
// Lenient on the letter shapes and strict on what tells a WORD from a smudge:
// this opens the DECOY, so a fumbled shape costs the owner nothing, while a
// tap or one flat swipe must never fire it.
bool kw_is_kiss(const int *xs, const int *ys, const uint8_t *sid,
                int n, int strokes);
