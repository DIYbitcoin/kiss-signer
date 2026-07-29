#include "osd_strips.h"

#include <string.h>

#include "i18n.h"
#include "i18n_keys.h"
#include "wallet_theme.h"

// OSD_CLOSE is a corner hint, not a caption, so it takes the subtitle ladder.
// Everything else is a title with an optional second line under it.
static const struct { int t, s; } s_keys[SCAN_OSD_N] = {
    [OSD_SEARCH]  = { STR_C_OSD_SEARCH_T,  STR_C_OSD_SEARCH_S },
    [OSD_SEEN]    = { STR_C_OSD_SEEN_T,    -1 },
    [OSD_STUCK]   = { STR_C_OSD_STUCK_T,   STR_C_OSD_STUCK_S },
    [OSD_CUTOFF]  = { STR_C_OSD_CUTOFF_T,  STR_C_OSD_CUTOFF_S },
    [OSD_READ]    = { STR_C_OSD_READ_T,    -1 },
    [OSD_ENT_LOW] = { STR_C_OSD_ENT_LOW_T, STR_C_OSD_ENT_LOW_S },
    [OSD_ENT_OK]  = { STR_C_OSD_ENT_OK_T,  STR_C_OSD_ENT_OK_S },
    [OSD_CLOSE]   = { STR_C_OSD_CLOSE,     -1 },
};

int osd_title_key(int state)
{
    return (state < 0 || state >= SCAN_OSD_N) ? -1 : s_keys[state].t;
}

int osd_sub_key(int state)
{
    return (state < 0 || state >= SCAN_OSD_N) ? -1 : s_keys[state].s;
}

static bool fits(const char *txt, const lv_font_t *f)
{
    lv_point_t sz;
    // EXPAND, so max_width is ignored and the answer is the line's real width.
    // Without it a long translation wraps and measures narrow, which is the
    // one answer that would let a caption overflow the lane it is being
    // checked against.
    lv_text_get_size(&sz, txt, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
    return sz.x <= OSD_MAX_W;
}

const lv_font_t *osd_title_font(const char *txt)
{
    if (!txt || !*txt) return wt_font34();
    if (fits(txt, wt_font34())) return wt_font34();
    if (fits(txt, wt_font28())) return wt_font28();
    return wt_font23();
}

const lv_font_t *osd_sub_font(const char *txt)
{
    if (!txt || !*txt) return wt_font23();
    return fits(txt, wt_font23()) ? wt_font23() : wt_font14();
}

static scan_osd_strip_t s_title[SCAN_OSD_N];
static scan_osd_strip_t s_sub[SCAN_OSD_N];
static scan_osd_strip_t s_digit[10];
static scan_osd_strip_t s_dot;
static scan_osd_strip_t s_of;
static bool s_open;

static const scan_osd_strip_t *ready(const scan_osd_strip_t *s)
{
    return s->a4 ? s : NULL;
}

const scan_osd_strip_t *osd_title(int state)
{
    if (!s_open || state < 0 || state >= SCAN_OSD_N) return NULL;
    return ready(&s_title[state]);
}

const scan_osd_strip_t *osd_sub(int state)
{
    if (!s_open || state < 0 || state >= SCAN_OSD_N) return NULL;
    return ready(&s_sub[state]);
}

const scan_osd_strip_t *osd_digit(int d)
{
    if (!s_open || d < 0 || d > 9) return NULL;
    return ready(&s_digit[d]);
}

const scan_osd_strip_t *osd_dot(void) { return s_open ? ready(&s_dot) : NULL; }
const scan_osd_strip_t *osd_of(void)  { return s_open ? ready(&s_of)  : NULL; }

bool osd_strips_open(void)
{
    if (s_open) return true;

    for (int i = 0; i < SCAN_OSD_N; i++) {
        const char *t = tr(s_keys[i].t);
        // OSD_CLOSE aside, a caption's own size is decided by its own length,
        // per locale, so a long German title steps down without dragging the
        // short ones with it.
        const lv_font_t *tf = i == OSD_CLOSE ? osd_sub_font(t)
                                             : osd_title_font(t);
        osd_text_strip(t, tf, OSD_MAX_W, &s_title[i]);
        if (s_keys[i].s >= 0) {
            const char *sub = tr(s_keys[i].s);
            osd_text_strip(sub, osd_sub_font(sub), OSD_MAX_W, &s_sub[i]);
        }
    }

    // The live-count atlas. Composed as separate glyphs rather than one
    // "Reading 12 of 34" string because the counts change while frames are
    // flowing, and recomposing there would put an allocation on the stream
    // task once per part for nothing.
    const lv_font_t *gf = wt_font34();
    for (int d = 0; d < 10; d++) {
        const char s[2] = { (char)('0' + d), 0 };
        osd_text_strip(s, gf, 0, &s_digit[d]);
    }
    osd_text_strip(".", gf, 0, &s_dot);
    osd_text_strip(tr(STR_C_OSD_OF), gf, OSD_MAX_W, &s_of);

    s_open = true;
    // Anything at all is enough to call this open: the accessors answer NULL
    // per strip, and the draws skip what is missing. Only a total failure is
    // worth reporting, and only because a caller may want to log it.
    for (int i = 0; i < SCAN_OSD_N; i++)
        if (s_title[i].a4) return true;
    return false;
}

void osd_strips_close(void)
{
    for (int i = 0; i < SCAN_OSD_N; i++) {
        osd_text_free(&s_title[i]);
        osd_text_free(&s_sub[i]);
    }
    for (int d = 0; d < 10; d++) osd_text_free(&s_digit[d]);
    osd_text_free(&s_dot);
    osd_text_free(&s_of);
    s_open = false;
}
