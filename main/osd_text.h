// Compose camera-overlay text at runtime, from the fonts already in flash.
//
// The scan and entropy screens draw straight into the camera framebuffer and
// bypass LVGL entirely, so their text cannot be an lv_label. Until now that
// meant baking every string into the generated main/scan_osd.c as a 4-bit
// alpha strip: 199 strips, 1.74 MB, and a generator that renders Japanese,
// Korean and Chinese out of /System/Library/Fonts, so the overlay can only be
// regenerated on a Mac and its CJK does not match the CJK on every other
// screen.
//
// A strip built here costs no flash at all. A new overlay string costs what
// any other string in this firmware costs: its UTF-8 in the locale files.
#pragma once

#include <stdbool.h>

#include "lvgl.h"
#include "scan_osd.h"   // scan_osd_strip_t: {w, h, a4}, what blit_a4 consumes

// Render one line of UTF-8 into a freshly allocated strip, ready to hand to
// blit_a4() exactly like a baked one. Returns false and leaves *out zeroed if
// the text is empty or the allocation fails.
//
// max_w clips at a whole glyph boundary rather than mid stroke; pass 0 for no
// limit. The caller gets the final width back in out->w and can check it: a
// clipped overlay line is a caption that lies, so it is worth looking at
// rather than assuming.
//
// One line per strip. Wrapping is the caller's business, because the overlay
// lays its rows out on a grid the camera image has to fit around.
bool osd_text_strip(const char *utf8, const lv_font_t *f, int max_w,
                    scan_osd_strip_t *out);

// Release a strip from osd_text_strip. Safe on a zeroed or already freed one.
void osd_text_free(scan_osd_strip_t *s);
