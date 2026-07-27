// See wallet_duress.h. Two halves, both deliberately dependency-light:
//
//   * the stroke classifier -- pure integer geometry, no LVGL, so the desktop
//     test runner can hammer it with the shapes this panel actually sees
//   * the configuration -- NVS namespace "kiss" on device (the same namespace
//     as the other preferences), a RAM pair on host builds
//
// The picker screens live in wallet_duress_ui.c so that this file stays
// linkable into /tmp/kisstest without dragging LVGL in behind it.
#include "wallet_duress.h"

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
    // where it began. Checked first because a circle is wide and tall enough to
    // look like several other things if you only measure its bounding box.
    if (sw * 10 >= W * 6 && sh * 10 >= H * 6 && ends * 4 <= span)
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

// ---- configuration -------------------------------------------------------

static int valid_pair(int real, int decoy)
{
    if (real == WDG_NONE && decoy == WDG_NONE)
        return 1;                                  // "off" is a valid state
    if (real <= WDG_NONE || real >= WDG_N)
        return 0;
    if (decoy <= WDG_NONE || decoy >= WDG_N)
        return 0;                                  // half-configured = lockout
    return real != decoy;
}

#ifdef ESP_PLATFORM
#include "nvs.h"

// Deliberately NOT in wallet_seed.c's KEEP_KEYS: erasing the wallet must take
// the unlock configuration with it, or the next owner of a wiped device inherits
// a gesture layout for a seed that no longer exists.
#define K_REAL  "greal"
#define K_DECOY "gdecoy"

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

int wallet_duress_real(void)  { return read_u8(K_REAL); }
int wallet_duress_decoy(void) { return read_u8(K_DECOY); }

int wallet_duress_set(int real, int decoy)
{
    if (!valid_pair(real, decoy))
        return -1;
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READWRITE, &h) != ESP_OK)
        return -1;
    int rc = nvs_set_u8(h, K_REAL, (uint8_t)real) == ESP_OK &&
             nvs_set_u8(h, K_DECOY, (uint8_t)decoy) == ESP_OK &&
             nvs_commit(h) == ESP_OK ? 0 : -1;
    nvs_close(h);
    return rc;
}

void wallet_duress_forget(void) { (void)wallet_duress_set(WDG_NONE, WDG_NONE); }

#else   // host (sim + desktop tests)

static int s_real = WDG_NONE, s_decoy = WDG_NONE;

int wallet_duress_real(void)  { return s_real; }
int wallet_duress_decoy(void) { return s_decoy; }

int wallet_duress_set(int real, int decoy)
{
    if (!valid_pair(real, decoy))
        return -1;
    s_real = real;
    s_decoy = decoy;
    return 0;
}

void wallet_duress_forget(void) { s_real = WDG_NONE; s_decoy = WDG_NONE; }

#endif
