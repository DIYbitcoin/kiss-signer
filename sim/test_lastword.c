// Host tests for the last word module. The canonical BIP39 zero entropy
// vectors are the proof: "abandon" x11 must admit "about", x23 must admit
// "art", and every candidate must satisfy the real validator.
// Build: sim/build_test.sh -> /tmp/kisstest
#include <stdio.h>
#include <string.h>
#include "wallet_lastword.h"
#include "wallet_seed.h"

static int lfails;
static void ok(const char *n, int c)
{
    if (c) printf("PASS: %s\n", n);
    else { printf("FAIL: %s\n", n); lfails++; }
}

// "abandon abandon ... abandon" (n copies), space separated.
static void abandon_str(char *out, size_t cap, int n)
{
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        strncat(out, i ? " abandon" : "abandon", cap - strlen(out) - 1);
    }
}

static int word_index(const char *w)
{
    for (int i = 0; i < 2048; i++) {
        const char *c = wallet_lastword_word((uint16_t)i);
        if (c && strcmp(c, w) == 0) return i;
    }
    return -1;
}

static int set_has(const uint16_t *set, int n, int idx)
{
    for (int i = 0; i < n; i++) if (set[i] == idx) return 1;
    return 0;
}

// Append candidate word `idx` to prefix and run the real validator.
static int completes(const char *prefix, uint16_t idx)
{
    char m[WSEED_MAX_MNEMONIC];
    snprintf(m, sizeof m, "%s %s", prefix, wallet_lastword_word(idx));
    return wallet_seed_validate(m) == 0;
}

int test_lastword(void)
{
    lfails = 0;
    printf("\n-- last word (cards) --\n");

    char prefix[WSEED_MAX_MNEMONIC];
    static uint16_t cand[WLAST_MAX];

    // ---- wordlist access ----
    ok("word 0 is abandon", wallet_lastword_word(0) &&
       strcmp(wallet_lastword_word(0), "abandon") == 0);
    ok("word 2047 is zoo", wallet_lastword_word(2047) &&
       strcmp(wallet_lastword_word(2047), "zoo") == 0);
    ok("word 2048 is NULL", wallet_lastword_word(2048) == NULL);

    // ---- canonical zero entropy vectors ----
    abandon_str(prefix, sizeof prefix, 11);
    int n = wallet_lastword_candidates(prefix, cand);
    ok("11x abandon: 128 candidates", n == 128);
    ok("11x abandon: contains about", set_has(cand, n, word_index("about")));

    abandon_str(prefix, sizeof prefix, 23);
    n = wallet_lastword_candidates(prefix, cand);
    ok("23x abandon: 8 candidates", n == 8);
    ok("23x abandon: contains art", set_has(cand, n, word_index("art")));

    // ---- every candidate really validates; indices strictly ascending ----
    {
        int all_valid = 1, ascending = 1;
        for (int i = 0; i < n; i++) {
            if (!completes(prefix, cand[i])) all_valid = 0;
            if (i && cand[i] <= cand[i - 1]) ascending = 0;
        }
        ok("23x abandon: all 8 validate", all_valid);
        ok("23x abandon: ascending, distinct", ascending);
    }

    // ---- round trip: a full mnemonic's own last word is in the set ----
    {
        uint8_t e16[16], e32[32];
        memset(e16, 0x5A, sizeof e16);
        memset(e32, 0xC3, sizeof e32);
        char words[WSEED_MAX_MNEMONIC];

        ok("from_entropy 16 rc", wallet_seed_from_entropy(e16, 16, words, sizeof words) == 0);
        char *last = strrchr(words, ' ');
        *last = '\0';
        n = wallet_lastword_candidates(words, cand);
        ok("16 byte round trip: 128 candidates", n == 128);
        ok("16 byte round trip: own last word present",
           set_has(cand, n, word_index(last + 1)));

        ok("from_entropy 32 rc", wallet_seed_from_entropy(e32, 32, words, sizeof words) == 0);
        last = strrchr(words, ' ');
        *last = '\0';
        n = wallet_lastword_candidates(words, cand);
        ok("32 byte round trip: 8 candidates", n == 8);
        ok("32 byte round trip: own last word present",
           set_has(cand, n, word_index(last + 1)));
    }

    // ---- the inverse, and the premise its binary search rests on ----
    // wallet_lastword_index binary searches, which is only correct while the
    // English list is lexicographic. That is asserted here rather than assumed,
    // and word_index above stays a linear scan so the two never share a bug.
    {
        int round = 0, ascending = 1;
        for (int i = 0; i < 2048; i++) {
            const char *c = wallet_lastword_word((uint16_t)i);
            if (c && wallet_lastword_index(c) == i) round++;
            if (i && strcmp(c, wallet_lastword_word((uint16_t)(i - 1))) <= 0)
                ascending = 0;
        }
        ok("index: round trips for all 2048 words", round == 2048);
        ok("index: the list is strictly ascending (the search's premise)", ascending);
    }
    ok("index: off list word -> -1", wallet_lastword_index("zzzz") == -1);
    ok("index: NULL -> -1", wallet_lastword_index(NULL) == -1);
    ok("index: empty -> -1", wallet_lastword_index("") == -1);

    // ---- rejections ----
    ok("NULL -> -1", wallet_lastword_candidates(NULL, cand) == -1);
    abandon_str(prefix, sizeof prefix, 10);
    ok("10 words -> -1", wallet_lastword_candidates(prefix, cand) == -1);
    abandon_str(prefix, sizeof prefix, 12);
    ok("12 words -> -1", wallet_lastword_candidates(prefix, cand) == -1);
    abandon_str(prefix, sizeof prefix, 22);
    ok("22 words -> -1", wallet_lastword_candidates(prefix, cand) == -1);
    abandon_str(prefix, sizeof prefix, 24);
    ok("24 words -> -1", wallet_lastword_candidates(prefix, cand) == -1);
    abandon_str(prefix, sizeof prefix, 10);
    strncat(prefix, " zzzz", sizeof prefix - strlen(prefix) - 1);
    ok("off list word -> 0", wallet_lastword_candidates(prefix, cand) == 0);

    return lfails;
}
