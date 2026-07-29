#include "osd_text.h"

#include <stdlib.h>
#include <string.h>

// LVGL keeps its UTF-8 walk in a private header. Reaching for it beats
// hand-rolling a decoder next to a renderer that has to agree with LVGL glyph
// for glyph, and the coupling is loud: an LVGL bump that moves this file is a
// compile error, not a quiet wrong answer.
#include "src/misc/lv_text_private.h"

#ifndef SIMULATOR
#include "esp_heap_caps.h"
#endif

// A strip lives for as long as a camera screen is open and is never touched
// per frame, so it belongs in PSRAM next to the other camera buffers rather
// than in the internal heap the display path is competing for.
static void *osd_alloc(size_t n)
{
#ifdef SIMULATOR
    return calloc(1, n);
#else
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) memset(p, 0, n);
    return p;
#endif
}

// blit_a4() reads a continuous nibble stream, high nibble first, w nibbles to
// a row with no padding at the end of one. Rows run straight into each other,
// so a pixel is at index y*w + x and nothing is byte aligned but the buffer
// itself.
//
// Glyphs overlap. Kerning pulls a pair like KA close enough that the K's leg
// and the A's foot share their last row of pixels, and a glyph with a negative
// ofs_x reaches back under its predecessor outright. LVGL composites there,
// because it draws each glyph onto what is already on the canvas, so this has
// to composite too: taking the larger of the two coverages leaves the join a
// shade light, which is a real difference on screen and not only in a gate.
static inline void put_a4(uint8_t *buf, int w, int x, int y, uint8_t a)
{
    size_t i = (size_t)y * (size_t)w + (size_t)x;
    uint8_t *b = &buf[i >> 1];
    uint8_t old = (i & 1) ? (uint8_t)(*b & 0x0F) : (uint8_t)(*b >> 4);
    uint8_t out = (uint8_t)(old + ((15 - old) * a + 7) / 15);   // src over dst
    if (i & 1) *b = (uint8_t)((*b & 0xF0) | out);
    else       *b = (uint8_t)((*b & 0x0F) | (out << 4));
}

// True when this glyph carries pixels we can read. lv_font_get_glyph_dsc
// answers true even when NO font in the fallback chain had the character: it
// hands back resolved_font NULL with the format forced to A1, so testing the
// return value alone would dereference nothing and read one bit as four.
static bool glyph_is_drawable(const lv_font_glyph_dsc_t *g)
{
    return g->resolved_font != NULL && g->box_w > 0 && g->box_h > 0
        && g->format > LV_FONT_GLYPH_FORMAT_NONE
        && g->format < LV_FONT_GLYPH_FORMAT_IMAGE;
}

// Draw one glyph into the strip. LVGL hands glyphs over as A8: the public
// accessor clears req_raw_bitmap on purpose, and the raw one needs a
// static_bitmap flag lv_font_conv does not emit, so the 4 bit data sitting in
// flash is not reachable as a pointer. Shifting A8 down to A4 costs one
// operation per pixel and happens once per screen rather than once per frame,
// and it means this works whatever bpp the font was built at.
static void draw_glyph(uint8_t *buf, int w, int h, int pen_x, int baseline,
                       lv_font_glyph_dsc_t *g)
{
    lv_draw_buf_t *db = lv_draw_buf_create(g->box_w, g->box_h,
                                           LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    if (!db) return;

    // The return value is the draw buffer itself, NOT its pixels, so it is
    // only good as a success flag. Reading it as a bitmap gets you the struct
    // header rendered as text, which looks enough like anti-aliasing to pass a
    // glance.
    if (lv_font_get_glyph_bitmap(g, db)) {
        const uint8_t *px = db->data;
        const uint32_t stride = db->header.stride;
        const int x0 = pen_x + g->ofs_x;
        const int y0 = baseline - g->box_h - g->ofs_y;
        for (int gy = 0; gy < g->box_h; gy++) {
            int y = y0 + gy;
            if (y < 0 || y >= h) continue;
            for (int gx = 0; gx < g->box_w; gx++) {
                int x = x0 + gx;
                if (x < 0 || x >= w) continue;
                uint8_t a8 = px[(size_t)gy * stride + (size_t)gx];
                if (a8) put_a4(buf, w, x, y, (uint8_t)(a8 >> 4));
            }
        }
    }
    lv_draw_buf_destroy(db);
}

bool osd_text_strip(const char *utf8, const lv_font_t *f, int max_w,
                    scan_osd_strip_t *out)
{
    if (!out) return false;
    out->w = 0;
    out->h = 0;
    out->a4 = NULL;
    if (!utf8 || !*utf8 || !f) return false;

    const int h = lv_font_get_line_height(f);
    const int baseline = h - f->base_line;
    if (h <= 0) return false;

    // Pass one measures, so the buffer is allocated once at the size the text
    // actually needs. A glyph that would cross max_w ends the line whole.
    int w = 0;
    {
        uint32_t ofs = 0, letter = 0, next = 0;
        for (;;) {
            lv_text_encoded_letter_next_2(utf8, &letter, &next, &ofs);
            if (letter == 0) break;
            lv_font_glyph_dsc_t g;
            if (!lv_font_get_glyph_dsc(f, &g, letter, next)) continue;
            int adv = g.adv_w;
            lv_font_glyph_release_draw_data(&g);
            if (max_w > 0 && w + adv > max_w) break;
            w += adv;
        }
    }
    if (w <= 0) return false;

    uint8_t *buf = osd_alloc(((size_t)w * (size_t)h + 1) / 2);
    if (!buf) return false;

    // Pass two draws, walking the string again rather than caching every
    // descriptor: the strings here are one short line and the walk is cheap
    // next to holding a descriptor array alive across an allocation.
    {
        int pen = 0;
        uint32_t ofs = 0, letter = 0, next = 0;
        for (;;) {
            lv_text_encoded_letter_next_2(utf8, &letter, &next, &ofs);
            if (letter == 0) break;
            lv_font_glyph_dsc_t g;
            if (!lv_font_get_glyph_dsc(f, &g, letter, next)) continue;
            int adv = g.adv_w;
            if (pen + adv > w) { lv_font_glyph_release_draw_data(&g); break; }
            // Spaces and unresolved characters advance the pen and draw
            // nothing, which is what a missing glyph should do here. The
            // label path would put a placeholder box in the sentence.
            if (glyph_is_drawable(&g)) draw_glyph(buf, w, h, pen, baseline, &g);
            lv_font_glyph_release_draw_data(&g);
            pen += adv;
        }
    }

    out->w = w;
    out->h = h;
    out->a4 = buf;
    return true;
}

void osd_text_free(scan_osd_strip_t *s)
{
    if (!s || !s->a4) return;
    free((void *)s->a4);
    s->a4 = NULL;
    s->w = 0;
    s->h = 0;
}
