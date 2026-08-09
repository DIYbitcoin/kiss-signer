// See wallet_duress.h. Two halves, both deliberately dependency-light:
//
//   * the stroke classifier -- pure integer geometry, no LVGL, so the desktop
//     test runner can hammer it with the shapes this panel actually sees
//   * the configuration -- NVS namespace "kiss" on device (the same namespace
//     as the other preferences), one RAM int on host builds
//
// The picker screens live in wallet_duress_ui.c so that this file stays
// linkable into /tmp/kisstest without dragging LVGL in behind it.
#include "wallet_duress.h"

#include <string.h>

#include "i18n_keys.h"

// ---- classifier ----------------------------------------------------------
//
// Everything is measured against the bounding box of the KISS that was just
// drawn, never against screen coordinates: people draw the word big or small,
// high or low, and a modifier is only meaningful relative to the word it
// modifies.
//
// The bands below do not tile the box. A flat stroke that is neither clearly
// high, clearly central nor clearly low returns WDG_NONE instead of being
// rounded to the nearest one. That is the whole safety argument of this file:
// refusing an ambiguous stroke costs a redraw, while guessing at one can put a
// passphrase keyboard on screen in front of somebody holding the device.

static int isqrt_i(long v)
{
    if (v <= 0) return 0;
    long x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return (int)x;
}

static int dist_i(int ax, int ay, int bx, int by)
{
    long dx = bx - ax, dy = by - ay;
    return isqrt_i(dx * dx + dy * dy);
}

int wallet_duress_classify(const int *xs, const int *ys, int n,
                           int bx0, int by0, int bx1, int by1)
{
    if (!xs || !ys || n < 3)
        return WDG_NONE;

    const int W = bx1 - bx0, H = by1 - by0;
    if (W < 80 || H < 40)        // no usable reference frame; refuse
        return WDG_NONE;

    int sx0 = xs[0], sx1 = xs[0], sy0 = ys[0], sy1 = ys[0];
    long path = 0;
    for (int i = 1; i < n; i++) {
        if (xs[i] < sx0) sx0 = xs[i];
        if (xs[i] > sx1) sx1 = xs[i];
        if (ys[i] < sy0) sy0 = ys[i];
        if (ys[i] > sy1) sy1 = ys[i];
        path += dist_i(xs[i - 1], ys[i - 1], xs[i], ys[i]);
    }
    const int sw = sx1 - sx0, sh = sy1 - sy0;
    const int cy = (sy0 + sy1) / 2;
    const int span = sw > sh ? sw : sh;
    const int ends = dist_i(xs[0], ys[0], xs[n - 1], ys[n - 1]);

    // A loop AROUND the word: covers most of the box in both axes and finishes
    // near where it began. Checked first because a circle is wide and tall
    // enough to look like several other things if you only measure its box.
    //
    // The closing test is deliberately generous. It was ends*4 <= span, which
    // meant the bigger the loop the more precisely it had to close -- a board
    // reported having to draw the circle SMALL to get it to register, which is
    // the opposite of what a person does when circling a word. Hand-drawn loops
    // close worst when they are big and quick. Half the span, and a floor of
    // 60px so a small circle is not held to a few pixels either.
    int close = span / 2;
    if (close < 60) close = 60;
    if (sw * 10 >= W * 6 && sh * 10 >= H * 6 && ends <= close)
        return WDG_CIRCLE;

    // Everything below is a single deliberate line, so the ink drawn must stay
    // close to the straight run between its ends. This is what stops the
    // three-sided remains of an abandoned circle from reading as a slash.
    const int straight = path * 100 <= 145L * ends;

    // Flat and at least half the width of the word -> underline / strike /
    // overline, told apart by where it sits. Note the gaps between the bands.
    if (straight && sw * 2 >= W && sh * 10 <= H * 3) {
        if (cy * 4 <= by0 * 4 + H)                      // top quarter, or above
            return WDG_OVERLINE;
        if (cy * 10 >= by0 * 10 + H * 3 &&              // middle 40%
            cy * 10 <= by0 * 10 + H * 7)
            return WDG_STRIKE;
        if (cy * 4 >= by0 * 4 + H * 3)                  // bottom quarter, or below
            return WDG_UNDERLINE;
        return WDG_NONE;                                // in a gap: do not guess
    }

    // One diagonal across the whole word, either direction.
    if (straight && sw * 2 >= W && sh * 2 >= H)
        return WDG_SLASH;

    // A tick: down-and-right to a vertex, then up-and-right well past it. The
    // vertex has to be interior -- a single stroke that only descends (the K's
    // lower arm) or only climbs (its upper arm) puts the lowest point at an end.
    if (sw * 5 >= W && sh * 5 >= H) {
        int vi = 0;
        for (int i = 1; i < n; i++)
            if (ys[i] > ys[vi]) vi = i;
        if (vi > 0 && vi < n - 1 &&
            xs[vi] > xs[0] && xs[n - 1] > xs[vi] &&
            (ys[vi] - ys[n - 1]) * 100 >= 35 * sh &&
            (ys[vi] - ys[0]) * 100 >= 20 * sh)
            return WDG_CHECK;
    }

    return WDG_NONE;
}

// ---- free marks -----------------------------------------------------------
//
// Same geometry, one thing removed: there is no word to measure against, so
// nothing here can ask "is this below it" or "is this half its width". Scale
// comes from an absolute floor instead. 80px on an 800x480 panel is a tenth of
// the screen -- big enough that a wobble, a tap or the tail of a swipe cannot
// reach it, small enough that a mark drawn casually still does.
//
// The tests are ordered exactly as the framed ones are, and for the same
// reason: a loop is wide and tall enough to look like several other things if
// you only measure its box, so it is asked first.
#define WDF_MIN_SPAN 80

int wallet_duress_classify_free(const int *xs, const int *ys, int n)
{
    if (!xs || !ys || n < 3)
        return WDF_NONE;

    int sx0 = xs[0], sx1 = xs[0], sy0 = ys[0], sy1 = ys[0];
    long path = 0;
    for (int i = 1; i < n; i++) {
        if (xs[i] < sx0) sx0 = xs[i];
        if (xs[i] > sx1) sx1 = xs[i];
        if (ys[i] < sy0) sy0 = ys[i];
        if (ys[i] > sy1) sy1 = ys[i];
        path += dist_i(xs[i - 1], ys[i - 1], xs[i], ys[i]);
    }
    const int sw = sx1 - sx0, sh = sy1 - sy0;
    const int span = sw > sh ? sw : sh;      // the mark's own size...
    const int lo   = sw < sh ? sw : sh;      // ...and how square it is
    const int ends = dist_i(xs[0], ys[0], xs[n - 1], ys[n - 1]);

    if (span < WDF_MIN_SPAN)
        return WDF_NONE;                     // a wobble, not a mark

    // A loop: substantial on BOTH axes and finishing near where it began. The
    // closing rule is the generous one the framed circle already had to learn
    // -- hand-drawn loops close worst when they are big and quick, and asking
    // for precision is how a board ends up drawing tiny circles to be
    // understood at all.
    int close = span / 2;
    if (close < 60) close = 60;
    if (lo * 10 >= span * 6 && ends <= close)
        return WDF_CIRCLE;

    // Everything below is one deliberate line, so the ink has to stay close to
    // the straight run between its ends. This is what stops the three-sided
    // remains of an abandoned loop from reading as a diagonal.
    const bool straight = path * 100 <= 145L * ends;

    // Flat: the ink stays inside 30% of its own width. A VERTICAL stroke is
    // deliberately not a fifth shape. It is the same gesture at 90 degrees, and
    // a hand that draws one quickly draws the other by accident; two shapes
    // that differ only by the angle of the wrist is how a sequence stops being
    // reproducible on a cold morning.
    if (straight && sh * 10 <= sw * 3)
        return WDF_LINE;

    // A diagonal spends itself on both axes.
    if (straight && lo * 2 >= span)
        return WDF_SLASH;

    // A tick: down-and-right to a vertex, then up-and-right well past it. The
    // vertex must be INTERIOR, which is the whole difference between a tick and
    // one descending stroke that happened to curve.
    if (lo * 5 >= span) {
        int vi = 0;
        for (int i = 1; i < n; i++)
            if (ys[i] > ys[vi]) vi = i;
        if (vi > 0 && vi < n - 1 &&
            xs[vi] > xs[0] && xs[n - 1] > xs[vi] &&
            (ys[vi] - ys[n - 1]) * 100 >= 35 * sh &&
            (ys[vi] - ys[0]) * 100 >= 20 * sh)
            return WDF_CHECK;
    }

    return WDF_NONE;
}

int wallet_duress_free_label_key(int mark)
{
    switch (mark) {
    // A free LINE reuses the strike label rather than the underline one: with
    // no word under it, "line through" is the name that does not promise a
    // position the mark no longer has.
    case WDF_LINE:   return STR_GD_STRIKE;
    case WDF_SLASH:  return STR_GD_SLASH;
    case WDF_CIRCLE: return STR_GD_CIRCLE;
    case WDF_CHECK:  return STR_GD_CHECK;
    default:         return -1;
    }
}

int wallet_duress_label_key(int gesture)
{
    switch (gesture) {
    case WDG_UNDERLINE: return STR_GD_UNDERLINE;
    case WDG_OVERLINE:  return STR_GD_OVERLINE;
    case WDG_STRIKE:    return STR_GD_STRIKE;
    case WDG_SLASH:     return STR_GD_SLASH;
    case WDG_CIRCLE:    return STR_GD_CIRCLE;
    case WDG_CHECK:     return STR_GD_CHECK;
    default:            return -1;
    }
}

// ---- unlock routing ------------------------------------------------------
// See wallet_duress.h for why this ignores the configured stroke, and why it
// lives here rather than beside the gesture plumbing in main.c.
//
// Outside the ESP_PLATFORM split below on purpose: there is one rule, and the
// device and the host must not be able to drift apart on it.
int wallet_duress_route(bool word_ok, int stroke)
{
    if (!word_ok)
        return WDR_NONE;
    return (stroke > WDG_NONE && stroke < WDG_N) ? WDR_REAL : WDR_DECOY;
}

// ---- configuration -------------------------------------------------------

static int valid_gesture(int g)
{
    return g == WDG_NONE || (g > WDG_NONE && g < WDG_N);   // WDG_NONE = off
}

// ---- the word ------------------------------------------------------------
//
// Validation and matching sit ABOVE the storage split, like wallet_duress_route
// does and for the same reason: there is one rule about what a word is, and the
// device and the host must not be able to drift apart on it.

static int valid_word(const uint8_t *marks, int n)
{
    if (n < 0 || n > WDW_MAX)
        return 0;
    for (int i = 0; i < n; i++)
        if (marks[i] <= WDF_NONE || marks[i] >= WDF_N)
            return 0;
    return 1;
}

int wallet_duress_word_len(void) { return wallet_duress_word_get(NULL); }

int wallet_duress_word_match(const uint8_t *marks, int n)
{
    uint8_t word[WDW_MAX];
    int wn = wallet_duress_word_get(word);
    // No word set is not a match against an empty prefix. It means KISS is
    // still the word and this function has no opinion; answering 0 sends the
    // caller to detect_KISS, which is where that device belongs.
    if (wn <= 0 || !marks || n < wn)
        return 0;
    for (int i = 0; i < wn; i++)
        if (marks[i] != word[i])
            return 0;
    return wn;
}

#ifdef ESP_PLATFORM
#include "nvs.h"

// Deliberately NOT in wallet_seed.c's KEEP_KEYS: erasing the wallet must take
// the unlock configuration with it, or the next owner of a wiped device inherits
// a gesture layout for a seed that no longer exists.
#define K_REAL  "greal"

static int read_u8(const char *key)
{
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READONLY, &h) != ESP_OK)
        return WDG_NONE;
    uint8_t v = WDG_NONE;
    if (nvs_get_u8(h, key, &v) != ESP_OK)
        v = WDG_NONE;
    nvs_close(h);
    return v < WDG_N ? v : WDG_NONE;
}

int wallet_duress_real(void) { return read_u8(K_REAL); }

int wallet_duress_set(int gesture)
{
    if (!valid_gesture(gesture))
        return -1;
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READWRITE, &h) != ESP_OK)
        return -1;
    int rc = nvs_set_u8(h, K_REAL, (uint8_t)gesture) == ESP_OK &&
             nvs_commit(h) == ESP_OK ? 0 : -1;
    nvs_close(h);
    return rc;
}

// The word: one blob, because the marks are only meaningful in order and a
// half-written word is worse than none. K_WORD absent = KISS.
#define K_WORD  "gword"

int wallet_duress_word_get(uint8_t out[WDW_MAX])
{
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READONLY, &h) != ESP_OK)
        return 0;
    uint8_t buf[WDW_MAX];
    size_t len = sizeof buf;
    int n = 0;
    if (nvs_get_blob(h, K_WORD, buf, &len) == ESP_OK && valid_word(buf, (int)len)) {
        n = (int)len;
        if (out) memcpy(out, buf, len);
    }
    nvs_close(h);
    return n;
}

int wallet_duress_word_set(const uint8_t *marks, int n)
{
    if (!valid_word(marks, n) || (n > 0 && !marks))
        return -1;
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READWRITE, &h) != ESP_OK)
        return -1;
    esp_err_t e = n == 0 ? nvs_erase_key(h, K_WORD)
                         : nvs_set_blob(h, K_WORD, marks, (size_t)n);
    // Erasing a key that was never written is not a failure: it is the state
    // the caller asked for.
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    int rc = (e == ESP_OK && nvs_commit(h) == ESP_OK) ? 0 : -1;
    nvs_close(h);
    return rc;
}

// The word goes with the gesture, and for the same reason: a wiped device must
// not answer to the previous owner's way in.
void wallet_duress_forget(void)
{
    (void)wallet_duress_set(WDG_NONE);
    (void)wallet_duress_word_set(NULL, 0);
}

#else   // host (sim + desktop tests)

static int s_real = WDG_NONE;

int wallet_duress_real(void) { return s_real; }

int wallet_duress_set(int gesture)
{
    if (!valid_gesture(gesture))
        return -1;
    s_real = gesture;
    return 0;
}

static uint8_t s_word[WDW_MAX];
static int     s_word_n;

int wallet_duress_word_get(uint8_t out[WDW_MAX])
{
    if (out && s_word_n > 0) memcpy(out, s_word, (size_t)s_word_n);
    return s_word_n;
}

int wallet_duress_word_set(const uint8_t *marks, int n)
{
    if (!valid_word(marks, n) || (n > 0 && !marks))
        return -1;
    if (n > 0) memcpy(s_word, marks, (size_t)n);
    s_word_n = n;
    return 0;
}

void wallet_duress_forget(void)
{
    s_real = WDG_NONE;
    s_word_n = 0;
}

#endif
