// Desktop tests for main/pass_edit.h: insert and delete at the caret.
//
// This suite exists because the passphrase field shows dots. Every other
// screen in this firmware fails loudly: a layout bug is visible, a bad PSBT is
// refused, a mismatched fingerprint is red. An off-by-one in these two
// functions is invisible by construction. The owner types what they meant to
// type, sees the right number of dots, gets a wallet, funds it, and discovers
// months later that the passphrase they wrote down opens nothing.
//
// So the tests below do not check that the functions behave plausibly. They
// check the buffer byte for byte against a reference model that is written a
// different way on purpose: a naive shift built from single-character moves,
// which is obviously correct and far too slow to ship.
#include <stdio.h>
#include <string.h>

#include "pass_edit.h"

static int pfails;

static void pchk(const char *name, int ok) {
    if (ok) printf("PASS: %s\n", name);
    else { printf("FAIL: %s\n", name); pfails++; }
}

#define CAP 128

// ---- reference model: the same two edits, written the obvious slow way ----

static void ref_insert(char *b, int *len, int *caret, char c) {
    for (int i = *len; i > *caret; i--) b[i] = b[i - 1];
    b[*caret] = c;
    (*len)++;
    (*caret)++;
    b[*len] = 0;
}

static void ref_delete(char *b, int *len, int *caret) {
    for (int i = *caret - 1; i < *len - 1; i++) b[i] = b[i + 1];
    (*len)--;
    (*caret)--;
    b[*len] = 0;
}

// ---- a tiny deterministic sequence generator, so failures are reproducible ----

static unsigned rng_s = 0x5EED1234u;
static unsigned rnd(unsigned n) {
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 17; rng_s ^= rng_s << 5;
    return rng_s % n;
}

int test_passedit(void);

int test_passedit(void) {
    printf("\n-- pass_edit --\n");
    pfails = 0;

    // Typing straight through, which is the ordinary case and has to stay
    // byte-identical to the append-only behaviour this replaced.
    {
        char b[CAP + 1] = {0};
        int len = 0, caret = 0;
        const char *want = "correct horse battery staple";
        for (const char *p = want; *p; p++)
            pass_edit_insert(b, CAP, &len, &caret, *p);
        pchk("append only: contents", strcmp(b, want) == 0);
        pchk("append only: length", len == (int)strlen(want));
        pchk("append only: caret at end", caret == len);
    }

    // Insert in the middle. The caret follows the character it inserted, so
    // typing a word into a gap comes out in order rather than reversed.
    {
        char b[CAP + 1] = "ac";
        int len = 2, caret = 1;
        pass_edit_insert(b, CAP, &len, &caret, 'b');
        pchk("mid insert: contents", strcmp(b, "abc") == 0);
        pchk("mid insert: caret after the new char", caret == 2);

        char b2[CAP + 1] = "ad";
        int l2 = 2, c2 = 1;
        pass_edit_insert(b2, CAP, &l2, &c2, 'b');
        pass_edit_insert(b2, CAP, &l2, &c2, 'c');
        pchk("mid insert: two in a row keep their order",
             strcmp(b2, "abcd") == 0);
    }

    // Backspace removes what is BEHIND the caret, so tapping a character and
    // pressing backspace takes that character and not its neighbour.
    {
        char b[CAP + 1] = "abc";
        int len = 3, caret = 2;          // caret sits just after 'b'
        pass_edit_delete(b, &len, &caret);
        pchk("mid delete: contents", strcmp(b, "ac") == 0);
        pchk("mid delete: caret", caret == 1);
        pchk("mid delete: length", len == 2);
    }

    // Boundaries.
    {
        char b[CAP + 1] = "abc";
        int len = 3, caret = 0;
        pchk("delete at start refuses", !pass_edit_delete(b, &len, &caret));
        pchk("delete at start changes nothing",
             strcmp(b, "abc") == 0 && len == 3 && caret == 0);

        char full[CAP + 1];
        memset(full, 'x', CAP);
        full[CAP] = 0;
        int flen = CAP, fcaret = 0;
        pchk("insert into a full buffer refuses",
             !pass_edit_insert(full, CAP, &flen, &fcaret, 'y'));
        pchk("insert into a full buffer changes nothing",
             flen == CAP && fcaret == 0 && full[0] == 'x');

        // Inserting at the very front of a full-but-one buffer is the last
        // move that must still work, and it is the one that walks the most
        // bytes, so it is where a bad length would run off the end.
        char nearly[CAP + 1];
        memset(nearly, 'a', CAP - 1);
        nearly[CAP - 1] = 0;
        int nlen = CAP - 1, ncaret = 0;
        pchk("insert at front of a nearly full buffer",
             pass_edit_insert(nearly, CAP, &nlen, &ncaret, 'z'));
        pchk("insert at front: terminator survives",
             nlen == CAP && (int)strlen(nearly) == CAP && nearly[0] == 'z');
    }

    // The real test: 20000 random edits against the reference model, checking
    // the whole buffer every step. A divergence anywhere is a passphrase that
    // does not read back as it was typed.
    {
        char a[CAP + 2] = {0}, r[CAP + 2] = {0};
        int alen = 0, acaret = 0, rlen = 0, rcaret = 0;
        int diverged = -1;

        for (int step = 0; step < 20000 && diverged < 0; step++) {
            unsigned op = rnd(10);
            if (op < 6) {                       // mostly typing
                char c = (char)(0x20 + rnd(0x5F));   // printable ASCII, as the keyboard emits
                if (alen < CAP) {
                    pass_edit_insert(a, CAP, &alen, &acaret, c);
                    ref_insert(r, &rlen, &rcaret, c);
                }
            } else if (op < 9) {                // backspace
                if (acaret > 0) {
                    pass_edit_delete(a, &alen, &acaret);
                    ref_delete(r, &rlen, &rcaret);
                }
            } else {                            // a tap moves the caret
                int pos = alen ? (int)rnd((unsigned)alen + 1) : 0;
                acaret = pos;
                rcaret = pos;
            }
            if (alen != rlen || acaret != rcaret || strcmp(a, r) != 0)
                diverged = step;
        }
        if (diverged >= 0)
            printf("  diverged at step %d: got \"%s\" (len %d caret %d), "
                   "want \"%s\" (len %d caret %d)\n",
                   diverged, a, alen, acaret, r, rlen, rcaret);
        pchk("20000 random edits match the reference model", diverged < 0);
    }

    printf(pfails ? "pass_edit: %d FAILED\n" : "pass_edit: all passed\n", pfails);
    return pfails;
}
