// Perceptual colour distance, shared by the two gates that ask about colour.
//
// sim/themecheck.c asks it of the palette: does an accent land on top of a
// status colour. sim/overlapcheck.c asks it of rendered objects: is this
// element wearing an accent and a status colour at once. Same question about
// two colours, so the same measurement, in one place rather than two copies
// that drift.
//
// CIE76 dE in Lab, not RGB: two colours can be far apart in RGB and still read
// the same to an eye at arm's length.
#ifndef COLOUR_DE_H
#define COLOUR_DE_H

#include <math.h>
#include <stdint.h>

// Below this, treat the two as the same colour on a 4.3 inch panel at arm's
// length. dE 25 is well above the ~2.3 "just noticeable" threshold on purpose:
// the question here is not whether a careful eye can tell them apart side by
// side, it is whether they still read as DIFFERENT KINDS OF THING across a
// screen. The measured palette pairs sit at 0, 30 and 83, so nothing lands near
// the line and the exact value is not load bearing.
#define DE_SAME 25.0

// sRGB hex -> CIE Lab (D65).
static inline void cde_to_lab(uint32_t hex, double *L, double *a, double *b)
{
    double c[3] = { ((hex >> 16) & 0xFF) / 255.0,
                    ((hex >>  8) & 0xFF) / 255.0,
                    ( hex        & 0xFF) / 255.0 };
    for (int i = 0; i < 3; i++)
        c[i] = c[i] <= 0.04045 ? c[i] / 12.92
                               : pow((c[i] + 0.055) / 1.055, 2.4);
    double X = (0.4124 * c[0] + 0.3576 * c[1] + 0.1805 * c[2]) / 0.95047;
    double Y = (0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]);
    double Z = (0.0193 * c[0] + 0.1192 * c[1] + 0.9505 * c[2]) / 1.08883;
    double f[3], v[3] = { X, Y, Z };
    for (int i = 0; i < 3; i++)
        f[i] = v[i] > 0.008856 ? cbrt(v[i])
                               : (7.787 * v[i]) + (16.0 / 116.0);
    *L = 116.0 * f[1] - 16.0;
    *a = 500.0 * (f[0] - f[1]);
    *b = 200.0 * (f[1] - f[2]);
}

static inline double cde_delta_e(uint32_t p, uint32_t q)
{
    double L1, a1, b1, L2, a2, b2;
    cde_to_lab(p, &L1, &a1, &b1);
    cde_to_lab(q, &L2, &a2, &b2);
    return sqrt((L1 - L2) * (L1 - L2) + (a1 - a2) * (a1 - a2) +
                (b1 - b2) * (b1 - b2));
}

static inline int cde_same(uint32_t p, uint32_t q)
{
    return cde_delta_e(p, q) < DE_SAME;
}

#endif
