// Host tests for the post quantum signature layer: SLH-DSA-SHA2-128s, as
// vendored in components/slhdsa.
//
// Every vector here is NIST's, out of usnistgov/ACVP-Server, because the thing
// being tested is a copy of somebody else's library. Numbers it generated itself
// would prove the copy is intact and nothing else; these prove the copy computes
// what FIPS 205 says. See tools/gen_pq_kat.py.
//
// What no test on a laptop can reach: the accelerator. pq_hw_sha.c's hardware
// path is inside #ifdef ESP_PLATFORM, so everything below exercises the portable
// C. main/pq_selftest_vectors.h is the answer to that -- one case the device
// checks against its own SHA engine at startup.
// Build: sim/build_test.sh -> kisstest
#include <stdio.h>
#include <string.h>

#include "slh_dsa.h"
#include "sha2_api.h"
#include "pq_hw_sha.h"
#include "pq_kat_vectors.h"

static int fails;

static void ok(const char *n, int c)
{
    if (c) printf("PASS: %s\n", n);
    else { printf("FAIL: %s\n", n); fails++; }
}

#define NELEM(a) (sizeof(a) / sizeof((a)[0]))

// Signing is ~2.2 million compressions and takes a quarter of a second here, so
// the suite pays about a second for the five cases. That is the price of testing
// the path the release tool uses; verification, which is what the DEVICE runs,
// is 2199 compressions and free.
static uint8_t sig[8192];

int test_pq(void)
{
    const slh_param_t *p = &slh_dsa_sha2_128s;

    ok("param set is SLH-DSA-SHA2-128s", strcmp(slh_alg_id(p), "SLH-DSA-SHA2-128s") == 0);
    ok("public key is 32 bytes", slh_pk_sz(p) == 32);
    ok("private key is 64 bytes", slh_sk_sz(p) == 64);
    ok("signature is 7856 bytes", slh_sig_sz(p) == 7856);

    // ---- keyGen: the top tree, hashed all the way to its root ----
    {
        int bad = 0;
        for (size_t i = 0; i < NELEM(PQ_KAT_KEYGEN); i++) {
            const pq_kat_keygen_t *k = &PQ_KAT_KEYGEN[i];
            uint8_t sk[64], pk[32];
            if (slh_keygen_internal(sk, pk, k->sk_seed, k->sk_prf, k->pk_seed, p) != 0 ||
                memcmp(sk, k->sk, 64) != 0 || memcmp(pk, k->pk, 32) != 0)
                bad++;
        }
        char nm[80];
        snprintf(nm, sizeof nm, "ACVP keyGen, %zu cases", NELEM(PQ_KAT_KEYGEN));
        ok(nm, bad == 0);
    }

    // ---- sigGen: deterministic, so the bytes are an answer and not a sample ----
    for (size_t i = 0; i < NELEM(PQ_KAT_SIGGEN); i++) {
        const pq_kat_siggen_t *t = &PQ_KAT_SIGGEN[i];
        char nm[96];

        pq_hw_sha_begin();
        size_t n = t->external
            ? slh_sign(sig, t->msg, t->msg_len, t->ctx, t->ctx_len, t->sk, NULL, p)
            : slh_sign_internal(sig, t->msg, t->msg_len, t->sk, NULL, p);
        pq_hw_sha_end();

        uint8_t d[32];
        sha2_256(d, sig, n);
        snprintf(nm, sizeof nm, "ACVP sigGen tc%d (%s) signs to the expected bytes",
                 t->tc_id, t->external ? "ctx" : "internal");
        ok(nm, n == 7856 && memcmp(d, t->sig_sha256, 32) == 0);

        // The signature just produced is byte identical to ACVP's, so verifying
        // it is verifying theirs.
        int v = t->external
            ? slh_verify(t->msg, t->msg_len, sig, n, t->ctx, t->ctx_len, t->pk, p)
            : slh_verify_internal(t->msg, t->msg_len, sig, n, t->pk, p);
        snprintf(nm, sizeof nm, "ACVP sigGen tc%d verifies", t->tc_id);
        ok(nm, v == 1);
    }

    // ---- refusals ----
    //
    // One accepted signature proves the arithmetic runs; it says nothing about
    // whether anything is CHECKED. These are the ways a firmware image gets
    // swapped under a signature, so each one has to come back 0.
    {
        const pq_kat_siggen_t *t = &PQ_KAT_SIGGEN[1];   // tc161: 1 byte message
        pq_hw_sha_begin();
        size_t n = slh_sign(sig, t->msg, t->msg_len, t->ctx, t->ctx_len, t->sk, NULL, p);
        pq_hw_sha_end();
        ok("refusal fixture signs", n == 7856);

        // A byte in each structural region of the signature: the randomizer R at
        // the front, the FORS trees behind it, and the hypertree auth path at the
        // back. A verifier that only really looks at one of them passes a single
        // flipped bit and fails here.
        static const size_t at[] = {0, 16, 2000, 5000, 7855};
        for (size_t i = 0; i < NELEM(at); i++) {
            sig[at[i]] ^= 0x01;
            int v = slh_verify(t->msg, t->msg_len, sig, n, t->ctx, t->ctx_len, t->pk, p);
            sig[at[i]] ^= 0x01;
            char nm[80];
            snprintf(nm, sizeof nm, "flipped bit at byte %zu is refused", at[i]);
            ok(nm, v == 0);
        }

        uint8_t msg2[1]; msg2[0] = (uint8_t)(t->msg[0] ^ 0x01);
        ok("a different message is refused",
           slh_verify(msg2, 1, sig, n, t->ctx, t->ctx_len, t->pk, p) == 0);

        uint8_t pk2[32]; memcpy(pk2, t->pk, 32); pk2[31] ^= 0x01;
        ok("a different public key is refused",
           slh_verify(t->msg, t->msg_len, sig, n, t->ctx, t->ctx_len, pk2, p) == 0);

        // The context is what stops a signature made for one purpose being
        // replayed as another. If it were ignored, this would pass.
        ok("a different context is refused",
           slh_verify(t->msg, t->msg_len, sig, n, (const uint8_t *)"nope", 4, t->pk, p) == 0);
        ok("no context at all is refused",
           slh_verify(t->msg, t->msg_len, sig, n, NULL, 0, t->pk, p) == 0);

        ok("a short signature is refused",
           slh_verify(t->msg, t->msg_len, sig, n - 1, t->ctx, t->ctx_len, t->pk, p) == 0);
        ok("a long signature is refused",
           slh_verify(t->msg, t->msg_len, sig, n + 1, t->ctx, t->ctx_len, t->pk, p) == 0);
    }

    // ---- the compression seam ----
    //
    // pq_hw_sha.c owns sha2_256_compress on both platforms, and on the device it
    // decides per call whether the peripheral is in hand. Neither decision may
    // change a digest. Here both branches are software, so what this actually
    // holds is that the dispatcher is wired at all and that the switch is inert.
    {
        const pq_kat_siggen_t *t = &PQ_KAT_SIGGEN[1];
        uint8_t d1[32], d2[32];

        pq_hw_sha_force_sw(false);
        pq_hw_sha_begin();
        pq_hw_sha_reset_compressions();
        size_t n = slh_sign(sig, t->msg, t->msg_len, t->ctx, t->ctx_len, t->sk, NULL, p);
        uint32_t c_sign = pq_hw_sha_compressions();
        pq_hw_sha_end();
        sha2_256(d1, sig, n);

        pq_hw_sha_force_sw(true);
        n = slh_sign(sig, t->msg, t->msg_len, t->ctx, t->ctx_len, t->sk, NULL, p);
        pq_hw_sha_force_sw(false);
        sha2_256(d2, sig, n);
        ok("forcing software changes nothing", memcmp(d1, d2, 32) == 0);

        pq_hw_sha_reset_compressions();
        slh_verify(t->msg, t->msg_len, sig, n, t->ctx, t->ctx_len, t->pk, p);
        uint32_t c_verify = pq_hw_sha_compressions();

        // Verifying is a walk down one path; signing rebuilds the trees. If
        // these ever come out close, verification has started doing work it has
        // no business doing -- which on this device is the difference between an
        // update screen that pauses and one that looks hung.
        printf("      sign %u compressions, verify %u\n", c_sign, c_verify);
        ok("verify is at least 100x cheaper than sign", c_verify * 100 < c_sign);

        // A missed end() must not leave the next operation holding a lock. On
        // the device this is what stops a forgotten bracket becoming a deadlock
        // rather than a slow signature.
        pq_hw_sha_begin();
        pq_hw_sha_begin();
        pq_hw_sha_end();
        pq_hw_sha_end();
        pq_hw_sha_end();   // one too many, deliberately
        pq_hw_sha_begin();
        pq_hw_sha_end();
        ok("brackets nest and survive an unbalanced end", 1);
    }

    return fails;
}
