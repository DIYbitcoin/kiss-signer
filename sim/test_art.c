// Host tests for the baked-art RLE decoder (main/kiss_art_rle.c).
// Build: sim/build_test.sh -> /tmp/kisstest
//
// The decoder is the only thing between a 3.1 MB flash saving and a screen
// full of garbage, and it runs on every image at every boot. The fixture below
// is the cross-check that matters: FIX_RAW compressed by tools/bake_art.py's
// Python encoder IS FIX_RLE, so if the two ever disagree about the format this
// fails here rather than shipping art nothing can read.
#include <stdio.h>
#include <string.h>
#include "kiss_art_rle.h"

static int fails;

static void ok(const char *name, int cond)
{
    if (cond) { printf("PASS: %s\n", name); }
    else      { printf("FAIL: %s\n", name); fails++; }
}

// A pattern carrying every shape the encoder has to split on: a 300 block run
// (over the 127 ceiling), 200 distinct blocks (a literal stretch over the same
// ceiling), a minimal 2 block run and a lone literal block.
#include "art_fixture.inc"

static void test_fixture(void)
{
    uint8_t out[sizeof FIX_RAW + 16];
    memset(out, 0xEE, sizeof out);

    uint32_t n = art_rle_decompress(FIX_RLE, sizeof FIX_RLE, out,
                                    sizeof FIX_RAW, 2);
    ok("art: fixture decodes to the full length", n == sizeof FIX_RAW);
    ok("art: fixture matches the python encoder byte for byte",
       memcmp(out, FIX_RAW, sizeof FIX_RAW) == 0);

    // The canary is the real assertion: a decoder that got the length right by
    // overrunning and being clipped would pass both checks above.
    int clean = 1;
    for (size_t i = sizeof FIX_RAW; i < sizeof out; i++)
        if (out[i] != 0xEE) clean = 0;
    ok("art: fixture wrote nothing past the output length", clean);

    ok("art: compression is worth doing at all",
       sizeof FIX_RLE < sizeof FIX_RAW / 2);
}

static void test_shapes(void)
{
    uint8_t out[64];

    const uint8_t lit[] = { 0x81, 0xAA, 0xBB };
    memset(out, 0, sizeof out);
    ok("art: literal of one block",
       art_rle_decompress(lit, sizeof lit, out, 2, 2) == 2 &&
       out[0] == 0xAA && out[1] == 0xBB);

    const uint8_t run[] = { 5, 0x12, 0x34 };
    memset(out, 0, sizeof out);
    ok("art: run of five blocks",
       art_rle_decompress(run, sizeof run, out, 10, 2) == 10 &&
       out[0] == 0x12 && out[9] == 0x34);

    // blk 1 takes the memset path, which is the one an A8 plane hits
    const uint8_t run1[] = { 7, 0x5A };
    memset(out, 0, sizeof out);
    uint32_t n1 = art_rle_decompress(run1, sizeof run1, out, 7, 1);
    int all = (n1 == 7);
    for (int i = 0; i < 7; i++) if (out[i] != 0x5A) all = 0;
    ok("art: run of seven bytes at block size 1", all);

    // a control byte of 0 consumes a block and emits nothing: it must advance
    const uint8_t zero[] = { 0, 0x11, 0x22, 0x81, 0x33, 0x44 };
    memset(out, 0, sizeof out);
    ok("art: zero length run does not stall",
       art_rle_decompress(zero, sizeof zero, out, 2, 2) == 2 && out[0] == 0x33);
}

static void test_refusals(void)
{
    uint8_t out[32];
    static const uint8_t one_lit[] = { 0x81, 0x01, 0x02 };

    ok("art: empty input writes nothing",
       art_rle_decompress(one_lit, 0, out, 32, 2) == 0);
    ok("art: null input is refused", art_rle_decompress(NULL, 4, out, 32, 2) == 0);
    ok("art: null output is refused",
       art_rle_decompress(one_lit, sizeof one_lit, NULL, 32, 2) == 0);
    ok("art: zero block size is refused",
       art_rle_decompress(one_lit, sizeof one_lit, out, 32, 0) == 0);

    // truncated literal: the control byte promises 2 blocks, 1 arrives
    const uint8_t trunc[] = { 0x82, 0xAA, 0xBB };
    memset(out, 0xEE, sizeof out);
    ok("art: truncated literal stops instead of reading on",
       art_rle_decompress(trunc, sizeof trunc, out, 32, 2) == 0 && out[0] == 0xEE);

    // truncated run: the block being repeated is missing
    const uint8_t trunc2[] = { 4, 0xAA };
    ok("art: truncated run stops short",
       art_rle_decompress(trunc2, sizeof trunc2, out, 32, 2) == 0);

    // output too small: stop at the boundary, do not write past it
    const uint8_t big[] = { 20, 0x12, 0x34 };
    memset(out, 0xEE, sizeof out);
    ok("art: undersized output stops at the boundary",
       art_rle_decompress(big, sizeof big, out, 4, 2) == 0 && out[0] == 0xEE);
}

int test_art(void)
{
    fails = 0;
    printf("\n-- baked art rle --\n");
    test_fixture();
    test_shapes();
    test_refusals();
    return fails;
}
