// Host tests for the generated backup password (main/kiss_pwgen.h).
// Build: sim/build_test.sh -> $KISS_SIM_TMP/kisstest
#include <stdio.h>
#include <string.h>
#include "kiss_pwgen.h"
#include "kiss_seed.h"

static int fails;
static void ok(const char *n, int c)
{
    if (c) printf("PASS: %s\n", n);
    else { printf("FAIL: %s\n", n); fails++; }
}

static const uint8_t ONES[PWGEN_BYTES] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};
static const uint8_t ZEROS[PWGEN_BYTES] = { 0 };

// The draw's inverse, so the round trip below is a property and not a table
// of magic bytes: five indices into the 56-bit register, low bit unused.
static void pack(const uint16_t idx[PWGEN_WORDS], uint8_t out[PWGEN_BYTES])
{
    uint64_t bits = 0;
    for (int w = 0; w < PWGEN_WORDS; w++) bits = (bits << 11) | idx[w];
    bits <<= 1;                       // the unused low bit
    for (int i = 0; i < PWGEN_BYTES; i++)
        out[i] = (uint8_t)(bits >> (48 - 8 * i));
}

int test_pwgen(void);
int test_pwgen(void)
{
    fails = 0;
    uint16_t idx[PWGEN_WORDS];

    ok("draw refuses a short stream", kiss_pwgen_draw(ONES, PWGEN_BYTES - 1, idx) == -1);
    ok("draw refuses NULL", kiss_pwgen_draw(NULL, PWGEN_BYTES, idx) == -1);

    ok("all ones draws", kiss_pwgen_draw(ONES, sizeof ONES, idx) == 0);
    int all_max = 1;
    for (int w = 0; w < PWGEN_WORDS; w++) if (idx[w] != 2047) all_max = 0;
    ok("all ones is 2047 five times", all_max);

    ok("all zeros draws", kiss_pwgen_draw(ZEROS, sizeof ZEROS, idx) == 0);
    int all_min = 1;
    for (int w = 0; w < PWGEN_WORDS; w++) if (idx[w] != 0) all_min = 0;
    ok("all zeros is 0 five times", all_min);

    // The property the whole thing rests on: 11 bits IS a word index, so
    // nothing is folded, biased or dropped between bytes and words.
    const uint16_t want[PWGEN_WORDS] = { 1, 2, 4, 2047, 1024 };
    uint8_t buf[PWGEN_BYTES];
    pack(want, buf);
    ok("packed indices draw back", kiss_pwgen_draw(buf, sizeof buf, idx) == 0);
    ok("round trip is exact", memcmp(idx, want, sizeof want) == 0);

    int in_range = 1;
    for (int v = 0; v < 2048; v++) {
        const uint16_t one[PWGEN_WORDS] = { (uint16_t)v, 0, 0, 0, (uint16_t)v };
        pack(one, buf);
        if (kiss_pwgen_draw(buf, sizeof buf, idx) != 0) { in_range = 0; break; }
        if (idx[0] != (uint16_t)v || idx[4] != (uint16_t)v) { in_range = 0; break; }
        for (int w = 0; w < PWGEN_WORDS; w++)
            if (idx[w] > 2047) { in_range = 0; break; }
    }
    ok("every 11-bit value survives both ends", in_range);

    char pw[PWGEN_MAX];
    const uint16_t known[PWGEN_WORDS] = { 0, 1, 2, 3, 2047 };
    ok("join succeeds", kiss_pwgen_join(known, pw, sizeof pw) == 0);
    ok("join is abandon ability able about zoo",
       strcmp(pw, "abandon ability able about zoo") == 0);

    int spaces = 0;
    for (const char *p = pw; *p; p++) if (*p == ' ') spaces++;
    ok("four separators", spaces == PWGEN_WORDS - 1);

    // Refuse, never truncate: half a password opens nothing and an owner
    // cannot tell by looking at it.
    char small[PWGEN_MAX - 1];
    ok("join refuses a short buffer", kiss_pwgen_join(known, small, sizeof small) == -1);
    ok("join refuses NULL", kiss_pwgen_join(known, NULL, PWGEN_MAX) == -1);

    // What PWGEN_MAX was sized for: five 8-letter words and the NUL.
    uint16_t longest[PWGEN_WORDS];
    int found = 0;
    for (int i = 0; i < 2048 && found < PWGEN_WORDS; i++) {
        const char *s = NULL;
        if (kiss_seed_word(i, &s) == 0 && s && strlen(s) == 8)
            longest[found++] = (uint16_t)i;
    }
    ok("wordlist has five 8-letter words", found == PWGEN_WORDS);
    if (found == PWGEN_WORDS) {
        ok("longest possible password joins",
           kiss_pwgen_join(longest, pw, sizeof pw) == 0);
        ok("and is 44 characters", strlen(pw) == 44);
    }

    return fails;
}
