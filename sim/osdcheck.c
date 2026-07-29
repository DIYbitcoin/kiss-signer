// The overlay text gate: does a composed strip say the same thing an LVGL
// label would have said?
//
// main/osd_text.c exists because the scan and entropy screens draw straight
// into the camera framebuffer and cannot use a label. That makes it the one
// text renderer in this firmware that nobody can look at on a desktop, and
// also the only one written here rather than borrowed. So this gate renders
// every overlay string twice, once through osd_text_strip and once through
// LVGL's own label draw, and compares the two pixel for pixel.
//
// Both paths start from the same glyph data in the same font, so agreement is
// not a happy coincidence: it is the only acceptable result, and any
// disagreement is a defect in the composer. Placement, the baseline formula,
// the nibble packing and the fallback walk are all covered by the comparison
// rather than by anyone's reading of them.
//
// Asked once per locale, because CJK arrives through a fallback font and Latin
// does not, so an English-only run tests the easy half.
//
// Exit code is 1 on any mismatch. Unlike the overlap gate there is no escape
// hatch, because there is no legacy layout here to be honest about. This one
// went green the day the composer was correct.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

#include "i18n.h"
#include "i18n_keys.h"
#include "osd_text.h"
#include "wallet_theme.h"

#define CANVAS_W 1000
#define CANVAS_H 96

static int g_fail;
static int g_checked;
static long g_px;

// The reference is drawn by the real display pipeline rather than onto a
// canvas: lv_canvas_finish_layer spins waiting on a draw dispatcher that a
// headless harness never runs, and the point here is to compare against what
// LVGL actually puts on a screen anyway.
static uint8_t g_fb[CANVAS_H][CANVAS_W][4];   // captured ARGB8888, BGRA in memory

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            const uint8_t *s = px + (((size_t)(y - area->y1) *
                                      (area->x2 - area->x1 + 1)) +
                                     (x - area->x1)) * 4;
            if (x >= 0 && x < CANVAS_W && y >= 0 && y < CANVAS_H)
                memcpy(g_fb[y][x], s, 4);
        }
    }
    lv_display_flush_ready(disp);
}

// A composed strip is alpha; a rendered label is colour. Comparing them means
// drawing the label pure white on pure black, where the red channel of the
// result IS the coverage LVGL computed. Both sides then reduce to 0..15.
static uint8_t label_alpha(int x, int y)
{
    return (uint8_t)(g_fb[y][x][2] >> 4);     // ARGB8888 is BGRA in memory
}

static uint8_t strip_alpha(const scan_osd_strip_t *s, int x, int y)
{
    size_t i = (size_t)y * (size_t)s->w + (size_t)x;
    return (i & 1) ? (uint8_t)(s->a4[i >> 1] & 0x0F)
                   : (uint8_t)(s->a4[i >> 1] >> 4);
}

static void compare_one(const char *txt, const lv_font_t *f, const char *what,
                        const char *lang)
{
    scan_osd_strip_t strip = {0};
    if (!osd_text_strip(txt, f, 0, &strip)) {
        printf("FAIL [%s] %s: composer produced nothing for \"%s\"\n",
               lang, what, txt);
        g_fail++;
        return;
    }
    if (strip.w > CANVAS_W || strip.h > CANVAS_H) {
        printf("FAIL [%s] %s: strip %dx%d exceeds the %dx%d canvas\n",
               lang, what, strip.w, strip.h, CANVAS_W, CANVAS_H);
        osd_text_free(&strip);
        g_fail++;
        return;
    }

    // The reference: an ordinary label, white on black, drawn at the origin by
    // the same pipeline every other screen in this firmware goes through.
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, f, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(lbl, 0, 0);

    memset(g_fb, 0, sizeof g_fb);
    lv_screen_load(scr);
    lv_obj_invalidate(scr);
    lv_refr_now(NULL);

    // A one step difference on a 16 level ramp is two paths rounding the same
    // coverage differently. Two steps is a different pixel.
    int bad = 0, worst = 0, bx = -1, by = -1, bm = 0, bt = 0;
    for (int y = 0; y < strip.h; y++) {
        for (int x = 0; x < strip.w; x++) {
            int mine = strip_alpha(&strip, x, y);
            int theirs = label_alpha(x, y);
            int diff = mine > theirs ? mine - theirs : theirs - mine;
            if (diff > worst) worst = diff;
            if (diff > 1) {
                if (!bad) { bx = x; by = y; bm = mine; bt = theirs; }
                bad++;
            }
        }
    }
    // OSD_DUMP=1 prints the first failing pair as hex coverage, mine on the
    // left and LVGL's on the right, then stops. Reading the two side by side
    // is what turned "252 of 252 strings differ" into a one line fix twice
    // over, so it is worth keeping rather than rewriting from scratch next
    // time. Left column garbage where the right column is legible letters
    // means the glyph source is wrong; a clean shift means placement is.
    if (bad && getenv("OSD_DUMP")) {
        printf("[%s] %s \"%s\"  mine | lvgl\n", lang, what, txt);
        for (int y = 0; y < strip.h; y++) {
            for (int x = 0; x < strip.w && x < 60; x++)
                putchar("0123456789ABCDEF"[strip_alpha(&strip, x, y)]);
            printf("   ");
            for (int x = 0; x < strip.w && x < 60; x++)
                putchar("0123456789ABCDEF"[label_alpha(x, y)]);
            putchar('\n');
        }
        exit(2);
    }
    if (bad) {
        // The bounding box of the ink on each side. A gate that only says
        // "different" sends the reader back to printf; a shifted box names the
        // fault (baseline, padding, pen origin) in one line.
        int mx0 = strip.w, my0 = strip.h, mx1 = -1, my1 = -1;
        int tx0 = strip.w, ty0 = strip.h, tx1 = -1, ty1 = -1;
        for (int y = 0; y < strip.h; y++) {
            for (int x = 0; x < strip.w; x++) {
                if (strip_alpha(&strip, x, y)) {
                    if (x < mx0) mx0 = x; if (x > mx1) mx1 = x;
                    if (y < my0) my0 = y; if (y > my1) my1 = y;
                }
                if (label_alpha(x, y)) {
                    if (x < tx0) tx0 = x; if (x > tx1) tx1 = x;
                    if (y < ty0) ty0 = y; if (y > ty1) ty1 = y;
                }
            }
        }
        printf("FAIL [%s] %s: %d of %d px differ (worst %d of 15) \"%s\"\n"
               "        ink mine x%d..%d y%d..%d | lvgl x%d..%d y%d..%d\n"
               "        first at (%d,%d): mine %d, lvgl %d\n",
               lang, what, bad, strip.w * strip.h, worst, txt,
               mx0, mx1, my0, my1, tx0, tx1, ty0, ty1, bx, by, bm, bt);
        g_fail++;
    }
    g_checked++;
    g_px += (long)strip.w * strip.h;

    osd_text_free(&strip);
    // The screen cannot be deleted while it is the active one, so park on a
    // fresh blank first and let the next comparison build its own.
    lv_obj_t *blank = lv_obj_create(NULL);
    lv_screen_load(blank);
    lv_obj_delete(scr);
}

static int osdcheck_run(void)
{
    // The strings the overlay draws, at every size the overlay uses them at.
    static const int keys[] = {
        STR_C_OSD_SEARCH_S, STR_S_POINT_CAM, STR_N_T, STR_N_S,
    };

    for (int lang = 0; lang < I18N_LANG_N; lang++) {
        i18n_set_lang(lang);
        const i18n_lang_t *info = i18n_lang_info(lang);
        const char *code = info ? info->code : "??";
        const lv_font_t *fonts[3] = { wt_font14(), wt_font23(), wt_font28() };
        const char *names[3] = { "font14", "font23", "font28" };
        for (unsigned k = 0; k < sizeof keys / sizeof keys[0]; k++) {
            const char *txt = tr(keys[k]);
            if (!txt || !*txt) continue;
            for (unsigned fi = 0; fi < 3; fi++)
                compare_one(txt, fonts[fi], names[fi], code);
        }
    }

    printf("\noverlay text gate: %d strings, %ld px, %d failures\n",
           g_checked, g_px, g_fail);
    return g_fail ? 1 : 0;
}

// The display is exactly the comparison window, and ARGB8888 so the captured
// red channel is the coverage LVGL computed rather than a 5 bit approximation
// of it. Nothing is ever shown; flush_cb is the only reader.
static uint8_t s_dispbuf[CANVAS_W * CANVAS_H * 4];

int main(void)
{
    lv_init();
    lv_display_t *d = lv_display_create(CANVAS_W, CANVAS_H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_buffers(d, s_dispbuf, NULL, sizeof s_dispbuf,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, flush_cb);
    return osdcheck_run();
}
