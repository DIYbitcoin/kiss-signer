// See kiss_gword.h. Three steps, all integer: resample the draw to a fixed
// number of points, move it to its own centre, scale it to a fixed size. Two
// draws of the same word then land on top of each other and the distance
// between their points is small.
//
// This is the well-trodden shape of a template recogniser and deliberately not
// more than that. What it leaves OUT is as considered as what it keeps:
//
//   * no rotation normalising. A word is written upright. Rotating to a
//     canonical angle would let the same word drawn upside down match, which
//     is a stranger holding the device the wrong way up.
//   * no aspect squashing. Scaling x and y independently to a square is what
//     $1 does, and it throws away the one property every written word has:
//     that it is wider than it is tall. Both axes take the SAME scale here.
//   * strokes are joined end to start rather than matched stroke by stroke.
//     The pen-up jump becomes part of the picture, which is consistent between
//     enrolment and unlock and makes the ORDER the letters were written in part
//     of the word. Writing the same letters in another order is another word.
#include "kiss_gword.h"

#include <string.h>

// A word is a written thing, not a flick. Same floor detect_cover_word uses for the
// draw it recognises, for the same reason: below this it is a gesture at the
// panel, and the owner did not mean anything by it.
#define GW_MIN_SPAN 120
#define GW_MIN_PTS  8

static int isqrt_i(long v)
{
    if (v <= 0) return 0;
    long x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return (int)x;
}

static long dist_l(int ax, int ay, int bx, int by)
{
    long dx = bx - ax, dy = by - ay;
    return isqrt_i(dx * dx + dy * dy);
}

int gw_make(const int *xs, const int *ys, const uint8_t *sid, int n,
            gw_template_t *out)
{
    if (!xs || !ys || !out || n < GW_MIN_PTS)
        return -1;

    memset(out, 0, sizeof *out);

    int x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
    long total = 0;
    for (int i = 1; i < n; i++) {
        if (xs[i] < x0) x0 = xs[i];
        if (xs[i] > x1) x1 = xs[i];
        if (ys[i] < y0) y0 = ys[i];
        if (ys[i] > y1) y1 = ys[i];
        total += dist_l(xs[i - 1], ys[i - 1], xs[i], ys[i]);
    }
    const int w = x1 - x0, h = y1 - y0;
    const int span = w > h ? w : h;
    if (span < GW_MIN_SPAN || total <= 0)
        return -1;

    // Count pen lifts. No sid means the whole draw is one stroke, which is what
    // a caller with a single stroke can honestly say.
    int strokes = 1;
    if (sid)
        for (int i = 1; i < n; i++)
            if (sid[i] != sid[i - 1]) strokes++;

    // Resample to GW_PTS points spaced equally along the path. Walking the
    // polyline rather than picking every nth sample is what makes this immune
    // to how FAST the word was written: a slow start leaves more raw samples in
    // the same ink, and taking every nth would weight it.
    int rx[GW_PTS], ry[GW_PTS];
    const long step = total / (GW_PTS - 1);
    rx[0] = xs[0]; ry[0] = ys[0];
    int got = 1;
    long acc = 0;
    for (int i = 1; i < n && got < GW_PTS - 1; i++) {
        long seg = dist_l(xs[i - 1], ys[i - 1], xs[i], ys[i]);
        if (seg <= 0) continue;
        long from = 0;
        while (step > 0 && acc + (seg - from) >= step && got < GW_PTS - 1) {
            long need = step - acc;
            from += need;
            rx[got] = (int)(xs[i - 1] + (xs[i] - xs[i - 1]) * from / seg);
            ry[got] = (int)(ys[i - 1] + (ys[i] - ys[i - 1]) * from / seg);
            got++;
            acc = 0;
        }
        acc += seg - from;
    }
    while (got < GW_PTS) { rx[got] = xs[n - 1]; ry[got] = ys[n - 1]; got++; }

    // Centre on the resampled points' own centroid, then scale both axes by the
    // SAME factor so the word keeps its proportions. 100 puts the long side at
    // roughly +-100, which is the range int8 holds with room for a hand that
    // wanders past the box it drew last time.
    long cx = 0, cy = 0;
    for (int i = 0; i < GW_PTS; i++) { cx += rx[i]; cy += ry[i]; }
    cx /= GW_PTS; cy /= GW_PTS;

    for (int i = 0; i < GW_PTS; i++) {
        long nx = (rx[i] - cx) * 100 / (span / 2 + 1);
        long ny = (ry[i] - cy) * 100 / (span / 2 + 1);
        if (nx >  127) nx =  127;
        if (nx < -127) nx = -127;
        if (ny >  127) ny =  127;
        if (ny < -127) ny = -127;
        out->x[i] = (int8_t)nx;
        out->y[i] = (int8_t)ny;
    }
    out->strokes = (uint8_t)(strokes > 255 ? 255 : strokes);
    out->set = 1;
    return 0;
}

int gw_distance(const gw_template_t *a, const gw_template_t *b)
{
    if (!a || !b || !a->set || !b->set)
        return INT16_MAX;
    // Pen lifts are matched EXACTLY. They are the one feature of a draw that
    // survives a shaky hand unchanged -- detect_cover_word leans on the same fact --
    // so two draws with different lift counts are different words however
    // similar their outlines, and saying so here is cheaper and stricter than
    // letting the point distance argue about it.
    if (a->strokes != b->strokes)
        return INT16_MAX;
    long sum = 0;
    for (int i = 0; i < GW_PTS; i++)
        sum += dist_l(a->x[i], a->y[i], b->x[i], b->y[i]);
    return (int)(sum / GW_PTS);
}

bool gw_matches(const gw_template_t *a, const gw_template_t *b)
{
    return gw_distance(a, b) <= GW_MATCH_MAX;
}

// ---- storage --------------------------------------------------------------

#ifdef ESP_PLATFORM
#include "nvs.h"

// Alongside the duress gesture in the same namespace, and deliberately NOT in
// kiss_seed.c's KEEP_KEYS: erasing the wallet must take every way into it,
// or the next owner of a wiped device inherits a door to a seed that is gone.
#define K_GWORD "gwtpl"

bool gw_stored_get(gw_template_t *out)
{
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READONLY, &h) != ESP_OK)
        return false;
    gw_template_t t;
    size_t len = sizeof t;
    bool ok = nvs_get_blob(h, K_GWORD, &t, &len) == ESP_OK &&
              len == sizeof t && t.set;
    nvs_close(h);
    if (ok && out) *out = t;
    return ok;
}

int gw_stored_set(const gw_template_t *t)
{
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READWRITE, &h) != ESP_OK)
        return -1;
    esp_err_t e;
    if (!t)                       e = nvs_erase_key(h, K_GWORD);
    else if (!t->set)             e = ESP_ERR_INVALID_ARG;
    else                          e = nvs_set_blob(h, K_GWORD, t, sizeof *t);
    // Clearing a key that was never written is the state the caller asked for,
    // not a failure.
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    int rc = (e == ESP_OK && nvs_commit(h) == ESP_OK) ? 0 : -1;
    nvs_close(h);
    return rc;
}

#else   // host (sim + desktop tests)

static gw_template_t s_stored;

// One-shot write failure, host only. There is no way to make real NVS refuse a
// write from a test, and a persistence failure the UI cannot be shown reaching
// is a screen no gate has ever rendered -- which is how "saved" came to be
// printed over a write that never happened.
static bool s_fail_next_set;

void gw_test_fail_next_set(void) { s_fail_next_set = true; }

bool gw_stored_get(gw_template_t *out)
{
    if (!s_stored.set) return false;
    if (out) *out = s_stored;
    return true;
}

int gw_stored_set(const gw_template_t *t)
{
    if (s_fail_next_set) { s_fail_next_set = false; return -1; }
    if (!t)        { memset(&s_stored, 0, sizeof s_stored); return 0; }
    if (!t->set)   return -1;
    s_stored = *t;
    return 0;
}

#endif

bool gw_stored_any(void) { return gw_stored_get(NULL); }
