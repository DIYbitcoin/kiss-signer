// The wallet's picture is FROZEN. This file is what freezes it.
//
// A fingerprint drawn as a pattern is something an owner learns by sight and
// then checks by sight, on the screen where "that is not my wallet" has to be
// noticeable. So the pattern a given fingerprint produces is a compatibility
// surface, exactly like an address derivation: if a firmware update changed it,
// every owner who had learned their picture would open their own wallet and see
// the wrong one. There is no error message for that and no way to take it back.
//
// The rasters below were generated from components/bitsquiggle32 at the pinned
// upstream commit (see that component's README.md) and are written out as
// pictures on purpose, so a reviewer can see what is being promised rather than
// trust a hash of it.
//
// If this test fails, the encoding moved. That is a BREAKING change to every
// wallet already in the world, not a vector in need of re-blessing. Fix the
// cause or keep the pin.
#include <stdio.h>
#include <string.h>

#include "bitsquiggle32.h"

#define SQ_W 16
#define SQ_H 22

typedef struct {
    uint32_t value;
    const char *rows[SQ_H];
} vector_t;

static const vector_t VECTORS[] = {
    // The dev seed's own fingerprint, so this vector is the picture that shows
    // up in every simulator screenshot and on every bench device.
    { 0xEC5A4595u, {
        "................",
        "....#####.#####.",
        "....#####.#####.",
        "....#####....##.",
        "....#####....##.",
        "....#####....##.",
        "....##.##.......",
        ".#####.##.......",
        ".#####.##.......",
        ".##.............",
        ".##.......#####.",
        ".##.......#####.",
        ".##..........##.",
        ".##.......#####.",
        ".##.......#####.",
        ".##.......##....",
        ".###########....",
        ".###########....",
        "....##.##.......",
        ".#####.#####....",
        ".#####.#####....",
        "................",
    } },
    // The smallest value the UI will ever draw: wt_squiggle refuses 00000000,
    // because a zeroed buffer is a failed derivation and not a wallet.
    { 0x00000001u, {
        "................",
        ".#####....#####.",
        ".#####....#####.",
        "....##....##....",
        ".#####....#####.",
        ".#####....#####.",
        ".#####....#####.",
        ".#####....#####.",
        ".#####....#####.",
        "................",
        ".##.########.##.",
        ".##.########.##.",
        ".##.##.##.##.##.",
        ".#####.##.#####.",
        ".#####.##.#####.",
        "....##....##....",
        ".#####....#####.",
        ".#####....#####.",
        ".#####....#####.",
        ".##############.",
        ".##############.",
        "................",
    } },
    { 0xFFFFFFFFu, {
        "................",
        "....########....",
        "....########....",
        "....##....##....",
        ".#####.##.#####.",
        ".#####.##.#####.",
        ".##.##.##.##.##.",
        ".##.########.##.",
        ".##.########.##.",
        ".##.##.##.##.##.",
        ".##.##.##.##.##.",
        ".##.##.##.##.##.",
        ".##.##.##.##.##.",
        ".##.##.##.##.##.",
        ".##.##.##.##.##.",
        ".##.##....##.##.",
        ".##.##....##.##.",
        ".##.##....##.##.",
        "................",
        ".##############.",
        ".##############.",
        "................",
    } },
    { 0xDEADBEEFu, {
        "................",
        ".##....#####.##.",
        ".##....#####.##.",
        ".##....##....##.",
        ".##....#####.##.",
        ".##....#####.##.",
        ".##.............",
        ".##....#####.##.",
        ".##....#####.##.",
        ".##.......##.##.",
        ".########.##.##.",
        ".########.##.##.",
        ".##....##.......",
        ".#####.##.......",
        ".#####.##.......",
        "....##.##.......",
        ".##.###########.",
        ".##.###########.",
        ".##.#####.##.##.",
        ".##.#####.##.##.",
        ".##.#####.##.##.",
        "................",
    } },
};

static int sfails;

static void chkb(const char *name, int ok)
{
    if (ok) printf("PASS: %s\n", name);
    else { printf("FAIL: %s\n", name); sfails++; }
}

static void dump(const Bitsquiggle32PixelGrid *g)
{
    for (int y = 0; y < SQ_H; y++) {
        printf("  ");
        for (int x = 0; x < SQ_W; x++)
            putchar(g->pixels[y * SQ_W + x] ? '#' : '.');
        putchar('\n');
    }
}

int test_squiggle(void)
{
    // The grid the UI expands into a canvas. If these ever move, wt_squiggle's
    // buffer arithmetic is wrong before any pattern is.
    chkb("squiggle grid is 16x22",
         BITSQUIGGLE32_PIXEL_WIDTH == SQ_W && BITSQUIGGLE32_PIXEL_HEIGHT == SQ_H);

    for (unsigned i = 0; i < sizeof VECTORS / sizeof VECTORS[0]; i++) {
        const vector_t *v = &VECTORS[i];
        Bitsquiggle32PixelGrid g;
        char name[64];

        snprintf(name, sizeof name, "squiggle %08X is unchanged", v->value);
        if (bitsquiggle32_pixels(v->value, BITSQUIGGLE32_MONOCHROME, &g) != 0) {
            printf("FAIL: %s (encoder refused the value)\n", name);
            sfails++;
            continue;
        }
        int same = 1;
        for (int y = 0; y < SQ_H && same; y++)
            for (int x = 0; x < SQ_W; x++)
                if ((v->rows[y][x] == '#') != (g.pixels[y * SQ_W + x] != 0)) {
                    same = 0;
                    break;
                }
        chkb(name, same);
        if (!same) {
            printf("  the encoding MOVED. every wallet that learned this\n"
                   "  picture now draws a different one. got:\n");
            dump(&g);
        }

        // wt_squiggle paints in wt_accent() and throws the library's derived
        // colours away, which is only safe while the raster is the same whatever
        // style is asked for. Assert the thing that is being relied on.
        Bitsquiggle32PixelGrid bw;
        snprintf(name, sizeof name, "squiggle %08X ignores style", v->value);
        chkb(name,
             bitsquiggle32_pixels(v->value, BITSQUIGGLE32_BLACK_AND_WHITE, &bw) == 0
             && memcmp(bw.pixels, g.pixels, sizeof g.pixels) == 0);
    }

    return sfails;
}
