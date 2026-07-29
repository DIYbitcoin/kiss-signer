// Compose camera-overlay text at runtime, from the fonts already in flash.
//
// The scan and entropy screens draw straight into the camera framebuffer and
// bypass LVGL entirely, so their text cannot be an lv_label. It used to be
// baked into a generated main/scan_osd.c as 4-bit alpha strips: 1,797,528
// bytes of flash, and a generator that rendered Japanese, Korean and Chinese
// out of /System/Library/Fonts, so the overlay could only be regenerated on a
// Mac and its CJK did not match the CJK on every other screen.
//
// A strip built here costs no flash at all. A new overlay string costs what
// any other string in this firmware costs: its UTF-8 in the locale files.
//
// See osd_strips.h for the cache that turns these into the overlay's actual
// captions; this file is only the renderer, and sim/osdcheck.c gates it
// against LVGL's own label draw pixel for pixel.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

// What blit_a4() in camera_spike.c consumes: a landscape strip of 4-bit alpha,
// row-major, high nibble first, w nibbles to a row with no padding at the end
// of one. This was the generated header's type; it outlived the generator.
typedef struct {
    int w, h;               // landscape strip dims (w along landscape-x)
    const uint8_t *a4;
} scan_osd_strip_t;

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
