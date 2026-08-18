// Pins the backup rehearsal's decision core (main/kiss_rehearse.c).
//
// Every vector here is a fault that shipped, or the boundary beside one. The
// no-passphrase prompt and the 00000000 render both lived in kiss_ui.c, which
// no test binary links; these are the decisions moved out of it.
#include <stdio.h>
#include <string.h>

#include "../main/kiss_rehearse.h"

static int fails;

static void ck(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

int test_rehearse(void)
{
    fails = 0;

    // Zero is the "no keys open" placeholder and must never read as a code.
    const uint8_t z[4] = {0, 0, 0, 0};
    ck(!kiss_fp_known(z), "fp_known: zeroed fingerprint is not a code");
    ck(!kiss_fp_known(NULL), "fp_known: NULL is not a code");
    for (int i = 0; i < 4; i++) {
        uint8_t fp[4] = {0, 0, 0, 0};
        fp[i] = 1;
        char what[48];
        snprintf(what, sizeof what, "fp_known: byte %d alone is enough", i);
        ck(kiss_fp_known(fp), what);
    }

    // A wallet with no passphrase rehearses its words and nothing else. The
    // shipped fault asked it for "the exact backup passphrase", a prompt with
    // no answerable input.
    ck(kiss_rehearse_after_words(1) == KISS_REHEARSE_VERIFIED,
       "after_words: no passphrase -> words are the whole backup");
    ck(kiss_rehearse_after_words(0) == KISS_REHEARSE_NEED_PASSPHRASE,
       "after_words: passphrase wallets rehearse it too");

    // The passphrase verdict. Equal and real verifies; a zeroed session
    // identity never does, whatever the retyped passphrase derived -- a wiped
    // session must not be verifiable against.
    const uint8_t a[4] = {0x12, 0xA4, 0xBB, 0x6B};
    const uint8_t b[4] = {0x12, 0xA4, 0xBB, 0x6C};
    ck(kiss_rehearse_pass_ok(a, a), "pass_ok: exact match verifies");
    ck(!kiss_rehearse_pass_ok(b, a), "pass_ok: one byte off refuses");
    ck(!kiss_rehearse_pass_ok(z, z), "pass_ok: zero against zero refuses");
    ck(!kiss_rehearse_pass_ok(a, z), "pass_ok: a wiped session verifies nothing");
    ck(!kiss_rehearse_pass_ok(NULL, a), "pass_ok: no derivation refuses");

    return fails;
}
