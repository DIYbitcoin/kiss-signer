// See kiss_cryptobench.h.
#include "kiss_cryptobench.h"

#if defined(ESP_PLATFORM) && !defined(KISS_RELEASE)

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "wally_bip39.h"
#include "wally_crypto.h"

#include "kiss_kef.h"
#include "kiss_pbkdf2.h"
#include "pq_hw_sha.h"   // force_sw: the same input down both paths

static const char *TAG = "kissbench";

// Published BIP39 vector. This never reaches kiss_seed_stage -- it is a
// stopwatch input, not a seed -- and the whole file is compiled out of
// release, so no mnemonic string ships.
static const char *BENCH_MNEMONIC =
    "abandon abandon abandon abandon abandon abandon "
    "abandon abandon abandon abandon abandon about";

static void report(const char *what, const char *shape, int64_t us)
{
    ESP_LOGI(TAG, "%-28s %-18s %8lld us%s", what, shape, (long long)us,
             us > KISS_BENCH_BUDGET_US ? "   SLOW" : "");
}

void kiss_cryptobench_run(void)
{
    const uint8_t *pw = (const uint8_t *)"a passphrase of ordinary length";
    const size_t   pwl = 31;
    const uint8_t *salt = (const uint8_t *)"73C5DA0A";   // a fingerprint id
    uint8_t key[32];
    int64_t t0;

    // 1. What opening or making a locked backup costs, at the iteration count
    //    a KEF envelope this device writes actually carries.
    const uint32_t iters = KEF_ITER_STORED * 10000u;
    t0 = esp_timer_get_time();
    int rc = kiss_pbkdf2_sha256(pw, pwl, salt, 8, iters, key);
    int64_t hw_us = esp_timer_get_time() - t0;
    report("KEF key derivation", rc == 0 ? "100000 iters" : "FAILED", hw_us);

    // 2. The same derivation with the accelerator refused, so the number above
    //    has something to be a ratio of and the fallback is proved to work at
    //    all. A tenth of the iterations, because the point is the ratio and
    //    the software path is the expensive one to sit through.
    const uint32_t few = iters / 10u;
    t0 = esp_timer_get_time();
    rc = kiss_pbkdf2_sha256(pw, pwl, salt, 8, few, key);
    int64_t hw10_us = esp_timer_get_time() - t0;

    pq_hw_sha_force_sw(true);
    t0 = esp_timer_get_time();
    int rc_sw = kiss_pbkdf2_sha256(pw, pwl, salt, 8, few, key);
    int64_t sw10_us = esp_timer_get_time() - t0;
    pq_hw_sha_force_sw(false);

    if (rc == 0 && rc_sw == 0 && hw10_us > 0)
        ESP_LOGI(TAG, "%-28s %-18s %8lld us   vs %lld us software, %lld.%01llux",
                 "  ...at a tenth", "10000 iters", (long long)hw10_us,
                 (long long)sw10_us, (long long)(sw10_us / hw10_us),
                 (long long)(sw10_us * 10 / hw10_us % 10));

    // 3. Every seed load and every passphrase change pays this one, and it is
    //    SHA-512, which nothing here accelerates. Measured so that stays a
    //    decision rather than an assumption.
    uint8_t seed[BIP39_SEED_LEN_512];
    size_t seed_len = 0;
    t0 = esp_timer_get_time();
    int brc = bip39_mnemonic_to_seed(BENCH_MNEMONIC, NULL, seed,
                                     sizeof seed, &seed_len);
    report("BIP39 seed derivation",
           brc == WALLY_OK ? "2048 iters, sha512" : "FAILED",
           esp_timer_get_time() - t0);

    // 4. One signature, on the chip that will sign. Cheap, and the line is
    //    here so a regression in the curve code has a number too.
    uint8_t priv[32], sig[EC_SIGNATURE_LEN], msg[32];
    memcpy(priv, seed, 32);
    priv[0] |= 1;                       // any valid scalar; this signs nothing
    memset(msg, 0x5a, sizeof msg);
    t0 = esp_timer_get_time();
    int src = wally_ec_sig_from_bytes(priv, sizeof priv, msg, sizeof msg,
                                      EC_FLAG_ECDSA | EC_FLAG_GRIND_R,
                                      sig, sizeof sig);
    report("one ECDSA signature", src == WALLY_OK ? "secp256k1" : "FAILED",
           esp_timer_get_time() - t0);

    wally_bzero(seed, sizeof seed);
    wally_bzero(priv, sizeof priv);
    wally_bzero(key, sizeof key);
    ESP_LOGI(TAG, "budget is %d us of dead screen; SLOW means make it faster "
                  "or move it off the UI task", KISS_BENCH_BUDGET_US);
}

#else

void kiss_cryptobench_run(void) {}

#endif
