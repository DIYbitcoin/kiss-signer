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
#include "kiss_pqsig.h"

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

// The bytes kiss_fw_install would have handed esp_ota_write, collected so the
// splitter can be checked against the image itself and not only its digest.
#define PQ_TEST_IMG 100000u
static uint8_t pq_seen[PQ_TEST_IMG + KISS_PQSIG_TRAILER_LEN];
static size_t pq_seen_len;
static int pq_sink_refuse_after = -1;   // >= 0: refuse once this many bytes are through

static int pq_sink(const uint8_t *d, size_t n, void *ud)
{
    (void)ud;
    if (pq_sink_refuse_after >= 0 && pq_seen_len + n > (size_t)pq_sink_refuse_after)
        return -77;
    memcpy(pq_seen + pq_seen_len, d, n);
    pq_seen_len += n;
    return 0;
}

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



    // ---- the firmware trailer ----
    //
    // Everything above is the vendored library. This is what this repository
    // wrote, and it is what decides whether an image becomes bootable.
    {
        // A throwaway release key. Deterministic, so a failure here is the same
        // failure twice rather than a new one each run.
        static uint8_t sk[64], pk[32], seed[16], prf[16], pseed[16];
        for (int i = 0; i < 16; i++) {
            seed[i] = (uint8_t)i; prf[i] = (uint8_t)(i + 64); pseed[i] = (uint8_t)(i + 128);
        }
        ok("test release key", slh_keygen_internal(sk, pk, seed, prf, pseed, p) == 0);

        // An explicit all zero key, not whatever main/pq_release_pubkey.h
        // happens to hold. That header carries the real release key now, so a
        // test asserting the placeholder was a test that passed only until
        // somebody minted one -- which is exactly what it did.
        static const uint8_t nokey[32] = {0};
        kiss_pqsig_test_set_pubkey(nokey);
        ok("no key means the check refuses rather than passes",
           kiss_pqsig_available() == false);
        kiss_pqsig_test_set_pubkey(pk);
        ok("a key makes the check available", kiss_pqsig_available());

        // A stand-in image, long enough that the trailer window fills and drains
        // many times. Not a repeat: a pattern that lined up with the window
        // would hide a splitter handing back the wrong 8 KB.
        static uint8_t image[PQ_TEST_IMG];
        for (size_t i = 0; i < PQ_TEST_IMG; i++) image[i] = (uint8_t)(i * 31 + (i >> 8));
        uint8_t want[32];
        sha2_256(want, image, PQ_TEST_IMG);

        static uint8_t trailer[KISS_PQSIG_TRAILER_LEN];
        memcpy(trailer, KISS_PQSIG_MAGIC, KISS_PQSIG_MAGIC_LEN);
        trailer[4] = KISS_PQSIG_SCHEME_SLH_DSA_SHA2_128S;
        trailer[5] = 0;
        trailer[6] = (uint8_t)(KISS_PQSIG_SIG_LEN & 0xFF);
        trailer[7] = (uint8_t)(KISS_PQSIG_SIG_LEN >> 8);
        pq_hw_sha_begin();
        size_t sn = slh_sign(trailer + KISS_PQSIG_HDR_LEN, want, 32,
                             (const uint8_t *)KISS_PQSIG_CTX, sizeof KISS_PQSIG_CTX - 1,
                             sk, NULL, p);
        pq_hw_sha_end();
        ok("the trailer holds a full signature", sn == KISS_PQSIG_SIG_LEN);
        ok("what the release tool signs is what the device checks",
           kiss_pqsig_check(want, trailer, sizeof trailer) == KISS_PQSIG_OK);

        static uint8_t file[PQ_TEST_IMG + KISS_PQSIG_TRAILER_LEN];
        memcpy(file, image, PQ_TEST_IMG);
        memcpy(file + PQ_TEST_IMG, trailer, KISS_PQSIG_TRAILER_LEN);
        const size_t FLEN = PQ_TEST_IMG + KISS_PQSIG_TRAILER_LEN;

        // ---- the splitter, at every chunk size that could get it wrong ----
        //
        // The card hands over whatever platform_sd's chunk happens to be and the
        // trailer does not respect it. These are the sizes where an off by one
        // lives: either side of the window, either side of a power of two, one
        // byte at a time, and the whole file in one go.
        static const size_t chunks[] = {
            1, 2, 3, 7, 63, 64, 4095, 4096,
            KISS_PQSIG_TRAILER_LEN - 1, KISS_PQSIG_TRAILER_LEN, KISS_PQSIG_TRAILER_LEN + 1,
            PQ_TEST_IMG - 1, PQ_TEST_IMG, PQ_TEST_IMG + KISS_PQSIG_TRAILER_LEN,
        };
        static kiss_pqsig_stream_t st;
        int bad = 0;
        for (size_t ci = 0; ci < NELEM(chunks); ci++) {
            pq_seen_len = 0;
            pq_sink_refuse_after = -1;
            kiss_pqsig_stream_init(&st);
            for (size_t off = 0; off < FLEN; ) {
                size_t n = chunks[ci] > FLEN - off ? FLEN - off : chunks[ci];
                if (kiss_pqsig_stream_feed(&st, file + off, n, pq_sink, NULL) != 0) {
                    printf("  chunk %zu: feed refused\n", chunks[ci]); bad++; break;
                }
                off += n;
            }
            uint8_t got[32]; const uint8_t *tr = NULL; size_t trl = 0;
            if (kiss_pqsig_stream_end(&st, got, &tr, &trl) != KISS_PQSIG_OK) {
                printf("  chunk %zu: end\n", chunks[ci]); bad++; continue;
            }
            if (memcmp(got, want, 32) != 0) { printf("  chunk %zu: digest\n", chunks[ci]); bad++; }
            if (trl != KISS_PQSIG_TRAILER_LEN || memcmp(tr, trailer, trl) != 0) {
                printf("  chunk %zu: trailer bytes\n", chunks[ci]); bad++;
            }
            if (kiss_pqsig_stream_image_len(&st) != PQ_TEST_IMG) {
                printf("  chunk %zu: image length\n", chunks[ci]); bad++;
            }
            // The digest agreeing is not enough. The writer has to have been
            // given the image and nothing else -- 8 KB of trailer written into
            // the slot is an image esp_ota_end would then judge.
            if (pq_seen_len != PQ_TEST_IMG || memcmp(pq_seen, image, PQ_TEST_IMG) != 0) {
                printf("  chunk %zu: bytes handed to the writer\n", chunks[ci]); bad++;
            }
            if (kiss_pqsig_check(got, tr, trl) != KISS_PQSIG_OK) {
                printf("  chunk %zu: check\n", chunks[ci]); bad++;
            }
        }
        char nm[96];
        snprintf(nm, sizeof nm, "the trailer splits off cleanly at all %zu chunk sizes", NELEM(chunks));
        ok(nm, bad == 0);

        // ---- refusals ----
        //
        // Each of these is a way an image gets swapped under a signature that
        // still looks like a signature.
        static uint8_t t2[KISS_PQSIG_TRAILER_LEN];
        #define WITH_TRAILER(mut, code, why) do {                                  \
            memcpy(t2, trailer, sizeof t2); { mut; }                               \
            ok(why, kiss_pqsig_check(want, t2, sizeof t2) == (code));               \
        } while (0)

        WITH_TRAILER(t2[0] ^= 0x01, KISS_PQSIG_ERR_MAGIC, "a file with no trailer is refused");
        WITH_TRAILER(t2[4] = 2, KISS_PQSIG_ERR_SCHEME, "an unknown scheme is refused");
        WITH_TRAILER(t2[6] = 0, KISS_PQSIG_ERR_LENGTH, "a wrong signature length is refused");
        // The slack after the signature reaches flash inside a signed release.
        // Anything alive in it is a channel whether or not it was meant as one.
        WITH_TRAILER(t2[KISS_PQSIG_TRAILER_LEN - 1] = 0xFF, KISS_PQSIG_ERR_PADDING,
                     "a nonzero byte in the padding is refused");
        WITH_TRAILER(t2[KISS_PQSIG_HDR_LEN + 4000] ^= 0x01, KISS_PQSIG_ERR_VERIFY,
                     "a flipped bit in the signature is refused");
        #undef WITH_TRAILER

        uint8_t other[32]; memcpy(other, want, 32); other[0] ^= 0x01;
        ok("a different image under the same trailer is refused",
           kiss_pqsig_check(other, trailer, sizeof trailer) == KISS_PQSIG_ERR_VERIFY);
        ok("a short trailer is refused",
           kiss_pqsig_check(want, trailer, sizeof trailer - 1) == KISS_PQSIG_ERR_LENGTH);

        // A file too small to hold a trailer never filled the window, so there is
        // no image under it. It has to come back as a length refusal and not as
        // a digest over the fragment.
        pq_seen_len = 0; pq_sink_refuse_after = -1;
        kiss_pqsig_stream_init(&st);
        kiss_pqsig_stream_feed(&st, file, KISS_PQSIG_TRAILER_LEN - 1, pq_sink, NULL);
        uint8_t d0[32];
        ok("a file shorter than the trailer is refused",
           kiss_pqsig_stream_end(&st, d0, NULL, NULL) == KISS_PQSIG_ERR_LENGTH);
        ok("and nothing was handed to the writer", pq_seen_len == 0);

        // A card pulled mid write shows up as the sink refusing. The stream has
        // to stay refused after that rather than resuming into a digest over a
        // hole.
        pq_seen_len = 0; pq_sink_refuse_after = 40000;
        kiss_pqsig_stream_init(&st);
        int rc = 0;
        for (size_t off = 0; off < FLEN && rc == 0; off += 4096) {
            size_t n = 4096 > FLEN - off ? FLEN - off : 4096;
            rc = kiss_pqsig_stream_feed(&st, file + off, n, pq_sink, NULL);
        }
        ok("a refusing writer stops the stream", rc == -77);
        ok("and it stays stopped",
           kiss_pqsig_stream_feed(&st, file, 4096, pq_sink, NULL) == -77 &&
           kiss_pqsig_stream_end(&st, d0, NULL, NULL) == -77);
        pq_sink_refuse_after = -1;

        kiss_pqsig_test_set_pubkey(nokey);
        ok("without a key nothing verifies at all",
           kiss_pqsig_check(want, trailer, sizeof trailer) == KISS_PQSIG_ERR_NO_KEY);
        kiss_pqsig_test_set_pubkey(NULL);   // back to the compiled in key

        // The selftest the device runs at startup, run here too. It cannot see
        // the accelerator from a laptop, but it can catch a vector header that
        // was regenerated against a different parameter set.
        ok("the startup selftest passes", kiss_pqsig_selftest() == 0);
    }

    return fails;
}
