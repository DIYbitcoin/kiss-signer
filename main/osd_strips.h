// The camera overlay's captions, composed once per camera screen.
//
// osd_text.c renders one line into one strip. This is what decides WHICH lines,
// at WHAT size, and holds them for as long as the camera is up. It exists as
// its own file so osd_text.c stays a pure renderer with a pixel-exact gate
// (sim/osdcheck.c) and no knowledge of i18n keys or overlay states.
//
// Everything is composed on the UI task before the stream task is created and
// freed after it is joined, so nothing allocates while frames are flowing.
#pragma once

#include <stdbool.h>

#include "lvgl.h"
#include "osd_text.h"

// Overlay states. These were the generated header's enum; the states outlived
// the art. Order is not persisted anywhere, so it is free to change.
enum { OSD_SEARCH, OSD_SEEN, OSD_STUCK, OSD_CUTOFF, OSD_READ,
       OSD_ENT_LOW, OSD_ENT_OK, OSD_CLOSE, SCAN_OSD_N };

// The lane a caption gets: 800px of landscape width, less the overscan insets
// the panel's own bezel eats. Inherited from the generator, which fitted its
// TrueType size down until the text cleared it.
#define OSD_MAX_W 640

// Muting, which the baked art used to carry in its own alpha and a composed
// strip cannot: a composed strip is full coverage, because that is what the
// gate compares against LVGL. Passed to blit_a4 instead. Same numbers the
// generator drew with, so nothing on screen moves brightness.
#define OSD_DIM_FULL  255
#define OSD_DIM_SUB   145
#define OSD_DIM_CLOSE 190

// Compose every caption, digit and the word "of" for the ACTIVE language.
// Idempotent. Returns false only if nothing could be allocated at all; a
// partial result is kept and its accessors answer NULL, because a camera
// screen with no caption is still a working camera screen.
bool osd_strips_open(void);

// Free everything. Safe to call when nothing is open.
void osd_strips_close(void);

// NULL when not composed, so every caller must check. osd_sub() is NULL for
// the states that never had a second line.
const scan_osd_strip_t *osd_title(int state);
const scan_osd_strip_t *osd_sub(int state);
const scan_osd_strip_t *osd_digit(int d);       // 0..9
const scan_osd_strip_t *osd_dot(void);          // a real '.', not a square
const scan_osd_strip_t *osd_of(void);           // localized, for "12 of 34"

// The fitting ladder, exposed so sim/osdcheck.c can gate the same choice this
// makes rather than a guess at it. Titles walk 34 -> 28 -> 23, subtitles
// 23 -> 14, both measured unwrapped against OSD_MAX_W in the active locale.
const lv_font_t *osd_title_font(const char *txt);
const lv_font_t *osd_sub_font(const char *txt);

// The i18n key each state draws, so the gate can walk the real strings instead
// of a copy of the list that drifts. -1 means the state has no second line.
int osd_title_key(int state);
int osd_sub_key(int state);
