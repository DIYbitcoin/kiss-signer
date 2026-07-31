// Do the wallet pictures actually look different from each other, and do they
// survive being small?
//
// The pattern is only worth drawing if an owner can tell theirs from somebody
// else's at a glance. 32 bits into a 16x22 grid says nothing about that on its
// own: what matters is whether two DIFFERENT fingerprints produce two pictures a
// person would call different, at the size the screen actually draws them.
//
// So this prints a wall of them and counts the near misses. It is a judgement
// aid, not a gate — the eye makes the call, and the numbers just say where to
// look. Run it before moving a squiggle to a smaller size anywhere in the UI.
//
// Build: bash sim/build_squigglecheck.sh   Run: /tmp/kisssquiggle [count] [seed]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bitsquiggle32.h"

#define W BITSQUIGGLE32_PIXEL_WIDTH
#define H BITSQUIGGLE32_PIXEL_HEIGHT

#define MAXN 512

static uint8_t grids[MAXN][W * H];
static uint32_t values[MAXN];

// The sizes the UI draws at today, plus the one phase 2 would need.
static const struct { int scale; const char *where; } SIZES[] = {
    { 6, "the fingerprint explainer card" },
    { 3, "beside the code on the reveal screen" },
    { 2, "the home chip, if it ever ships" },
};

// xorshift, so a run is reproducible from its seed and a suspicious pair can be
// reproduced by anyone reading the output.
static uint32_t rng_state = 0x9E3779B9u;
static uint32_t rng(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

// How many of the 352 cells differ. Not a perceptual measure and not pretending
// to be one; it is a cheap way to surface the pairs worth looking at.
static int distance(const uint8_t *a, const uint8_t *b)
{
    int d = 0;
    for (int i = 0; i < W * H; i++) if (a[i] != b[i]) d++;
    return d;
}

// Print n patterns side by side.
static void print_row(int from, int n)
{
    for (int y = 0; y < H; y++) {
        for (int i = from; i < from + n; i++) {
            // Two characters per pattern pixel, because a terminal cell is
            // about twice as tall as it is wide and a 16x22 grid printed one
            // for one comes out squeezed, which is not what the panel shows.
            for (int x = 0; x < W; x++) {
                char c = grids[i][y * W + x] ? '#' : ' ';
                putchar(c); putchar(c);
            }
            printf("   ");
        }
        putchar('\n');
    }
    for (int i = from; i < from + n; i++) printf("%-*s%08X   ", W * 2 - 8, "", values[i]);
    printf("\n\n");
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 200;
    if (n < 2) n = 2;
    if (n > MAXN) n = MAXN;
    if (argc > 2) rng_state = (uint32_t)strtoul(argv[2], NULL, 0);
    const uint32_t seed = rng_state;

    for (int i = 0; i < n; i++) {
        uint32_t v = rng();
        if (v == 0) v = 1;                 // the UI never draws a zero
        Bitsquiggle32PixelGrid g;
        if (bitsquiggle32_pixels(v, BITSQUIGGLE32_MONOCHROME, &g) != 0) {
            printf("FAIL: encoder refused %08X\n", v);
            return 1;
        }
        values[i] = v;
        memcpy(grids[i], g.pixels, sizeof grids[i]);
    }

    printf("%d pictures, seed 0x%08X\n\n", n, seed);

    // A wall of them, for the eye.
    for (int i = 0; i + 4 <= n && i < 16; i += 4) print_row(i, 4);

    // Identical pairs would be a real problem at any size: two wallets, one
    // picture, and nothing on screen to tell them apart but the code.
    int identical = 0, close = 0, worst = W * H;
    uint32_t wa = 0, wb = 0;
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            int d = distance(grids[i], grids[j]);
            if (d == 0) {
                identical++;
                printf("IDENTICAL: %08X and %08X\n", values[i], values[j]);
            } else if (d <= 8) {
                close++;
                if (d < worst) { worst = d; wa = values[i]; wb = values[j]; }
            }
        }
    }
    printf("pairs checked      %d\n", n * (n - 1) / 2);
    printf("identical pairs    %d\n", identical);
    printf("within 8 cells     %d\n", close);
    if (close) printf("closest non-equal  %08X vs %08X, %d cells\n", wa, wb, worst);

    // How much of the grid is lit, which is a property of the encoding and not
    // of the size. Printed once, for context on the sizes below.
    long on = 0;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < W * H; k++) on += grids[i][k] ? 1 : 0;
    printf("ink                %d%% of the grid\n",
           (int)(on * 100 / ((long)n * W * H)));

    // What each size costs in real pixels. The number that decides whether a
    // size works is the LAST one: a pattern's smallest feature is one grid cell,
    // so at 2x the finest thing on the glass is two pixels, and two pixels at
    // arm's length is the question no desktop run can answer. Take these to a
    // device; do not conclude anything about legibility from this program.
    printf("\nsize on the panel:\n");
    for (unsigned s = 0; s < sizeof SIZES / sizeof SIZES[0]; s++)
        printf("  %dx  %3dx%3d px  finest feature %dpx  %s\n", SIZES[s].scale,
               W * SIZES[s].scale, H * SIZES[s].scale, SIZES[s].scale,
               SIZES[s].where);

    // Never a non-zero exit: this reports, the reviewer decides.
    return 0;
}
