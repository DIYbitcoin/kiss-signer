// Parser fuzz harness — junk in must NEVER crash, NEVER sign, NEVER show READY.
// Build: sim/build_fuzz.sh -> /tmp/kissfuzz (ASAN+UBSAN so memory bugs actually
// fail instead of silently corrupting). Deterministic (fixed xorshift seed), so
// a failure reproduces exactly.
//
// Three attack surfaces, matching what a hostile coordinator can reach:
//   1. wallet_psbt_load on pure-random bytes      -> must reject or STOP
//   2. wallet_psbt_load on truncated/bit-flipped
//      REAL psbts                                  -> may parse, must not crash;
//                                                     status stays well-formed
//   3. qrt_parser_feed on random text + corrupted
//      UR parts                                    -> must not crash or complete
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "wallet_crypto.h"
#include "wallet_psbt.h"
#include "wallet_seed.h"
#include "qr_transport.h"

#include <wally_bip32.h>
#include <wally_bip39.h>
#include <wally_map.h>
#include <wally_psbt.h>
#include <wally_psbt_members.h>
#include <wally_script.h>
#include <wally_transaction.h>

#define H BIP32_INITIAL_HARDENED_CHILD

static int fails;
static void chkb(const char *name, int ok) {
    if (!ok) { printf("FAIL: %s\n", name); fails++; }
}

// xorshift32: deterministic junk (re-seeded in main for reproducibility)
static uint32_t s_rng = 0xC0FFEE01u;
static uint32_t rnd(void) {
    uint32_t x = s_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return s_rng = x;
}

// a real, valid PSBT to mutate (dev seed, 1-in 2-out p2wpkh — same fixture
// shape the main tests use)
static size_t mk_valid_psbt(uint8_t *out, size_t outsz) {
    uint8_t seed[BIP39_SEED_LEN_512]; size_t sl = 0;
    struct ext_key master, k00, k10;
    bip39_mnemonic_to_seed(
        "abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon abandon abandon about", NULL, seed, sizeof seed, &sl);
    bip32_key_from_seed(seed, sizeof seed, BIP32_VER_MAIN_PRIVATE, 0, &master);
    uint8_t fp[4];
    bip32_key_get_fingerprint(&master, fp, sizeof fp);
    const uint32_t p00[5] = {H + 84, H, H, 0, 0}, p10[5] = {H + 84, H, H, 1, 0};
    bip32_key_from_parent_path(&master, p00, 5, BIP32_FLAG_KEY_PRIVATE, &k00);
    bip32_key_from_parent_path(&master, p10, 5, BIP32_FLAG_KEY_PRIVATE, &k10);

    uint8_t in_spk[22], chg_spk[22], ext_spk[22] = {0x00, 0x14};
    size_t w = 0;
    memset(ext_spk + 2, 0x11, 20);
    wally_witness_program_from_bytes(k00.pub_key, 33, WALLY_SCRIPT_HASH160, in_spk, 22, &w);
    wally_witness_program_from_bytes(k10.pub_key, 33, WALLY_SCRIPT_HASH160, chg_spk, 22, &w);

    uint8_t txid[32]; memset(txid, 0xAA, 32);
    struct wally_tx *tx = NULL;
    wally_tx_init_alloc(2, 0, 1, 2, &tx);
    wally_tx_add_raw_input(tx, txid, 32, 0, 0xFFFFFFFD, NULL, 0, NULL, 0);
    wally_tx_add_raw_output(tx, 60000, ext_spk, 22, 0);
    wally_tx_add_raw_output(tx, 39000, chg_spk, 22, 0);

    struct wally_psbt *p = NULL;
    wally_psbt_init_alloc(0, 1, 2, 1, 0, &p);
    wally_psbt_set_global_tx(p, tx);
    struct wally_tx_output *utxo = NULL;
    wally_tx_output_init_alloc(100000, in_spk, 22, &utxo);
    wally_psbt_set_input_witness_utxo(p, 0, utxo);
    wally_tx_output_free(utxo);
    struct wally_map *m = NULL;
    wally_map_keypath_public_key_init_alloc(1, &m);
    wally_map_keypath_add(m, k00.pub_key, 33, fp, 4, p00, 5);
    wally_psbt_set_input_keypaths(p, 0, m);
    wally_map_free(m); m = NULL;
    wally_map_keypath_public_key_init_alloc(1, &m);
    wally_map_keypath_add(m, k10.pub_key, 33, fp, 4, p10, 5);
    wally_psbt_set_output_keypaths(p, 1, m);
    wally_map_free(m);
    size_t wr = 0;
    wally_psbt_to_bytes(p, 0, out, outsz, &wr);
    wally_psbt_free(p);
    wally_tx_free(tx);
    wally_bzero(&master, sizeof master);
    wally_bzero(&k00, sizeof k00);
    wally_bzero(&k10, sizeof k10);
    return wr;
}

int main(void)
{
    s_rng = 0xC0FFEE01u;
    wallet_seed_store("abandon abandon abandon abandon abandon abandon "
                      "abandon abandon abandon abandon abandon about");
    wallet_set_network(0);
    wallet_set_script(WSCRIPT_NATIVE);
    if (wallet_session_open(NULL) != 0) { printf("FAIL: session open\n"); return 1; }

    static uint8_t valid[4096], buf[4096], sig[4096];
    size_t vlen = mk_valid_psbt(valid, sizeof valid);
    chkb("valid fixture builds", vlen > 0);
    wpsbt_summary_t sum;

    // the fixture itself must be READY (so the mutations below start from good)
    chkb("fixture loads READY",
         wallet_psbt_load(valid, vlen, &sum) == 0 && sum.status == WPSBT_READY);
    wallet_psbt_free();

    // ---- 1. pure random bytes: must never come out READY ----
    for (int i = 0; i < 2000; i++) {
        size_t n = 1 + rnd() % 3000;
        for (size_t j = 0; j < n; j += 4) {
            uint32_t r = rnd();
            memcpy(buf + j, &r, (n - j) < 4 ? (n - j) : 4);
        }
        memset(&sum, 0, sizeof sum);
        int rc = wallet_psbt_load(buf, n, &sum);
        if (rc == 0 && sum.status == WPSBT_READY) {
            printf("FAIL: random junk parsed READY (iter %d len %zu)\n", i, n);
            fails++;
        }
        // a rejected load must also refuse to sign
        size_t sw = 0;
        if (rc != 0 && wallet_psbt_sign(sig, sizeof sig, &sw) == 0) {
            printf("FAIL: sign succeeded after rejected load (iter %d)\n", i);
            fails++;
        }
        wallet_psbt_free();
    }
    printf("PASS: 2000 random-junk PSBTs never READY, never signable\n");

    // ---- 2a. every truncation of a real PSBT ----
    for (size_t n = 0; n < vlen; n++) {
        memcpy(buf, valid, n);
        memset(&sum, 0, sizeof sum);
        int rc = wallet_psbt_load(buf, n, &sum);
        if (rc == 0 && sum.status == WPSBT_READY) {
            printf("FAIL: truncated PSBT (%zu of %zu bytes) READY\n", n, vlen);
            fails++;
        }
        wallet_psbt_free();
    }
    printf("PASS: all %zu truncations never READY\n", vlen);

    // ---- 2b. bit-flipped real PSBTs: may legitimately still verify (a flipped
    // amount is still a valid tx) — the guarantee is no crash and a status that
    // is always one of the three defined values ----
    for (int i = 0; i < 4000; i++) {
        memcpy(buf, valid, vlen);
        int flips = 1 + rnd() % 8;
        for (int f = 0; f < flips; f++)
            buf[rnd() % vlen] ^= (uint8_t)(1u << (rnd() % 8));
        memset(&sum, 0, sizeof sum);
        int rc = wallet_psbt_load(buf, vlen, &sum);
        if (rc == 0 && sum.status != WPSBT_READY && sum.status != WPSBT_CAUTION &&
            sum.status != WPSBT_STOP) {
            printf("FAIL: bit-flip produced undefined status %d (iter %d)\n", sum.status, i);
            fails++;
        }
        wallet_psbt_free();
    }
    printf("PASS: 4000 bit-flipped PSBTs, no crash, status always defined\n");

    // ---- 3. QR/UR chunk fuzz: random text + corrupted real UR parts ----
    for (int i = 0; i < 2000; i++) {
        qrt_parser_t *p = qrt_parser_new();
        if (!p) { printf("FAIL: parser alloc\n"); fails++; break; }
        int feeds = 1 + rnd() % 6;
        char part[600];
        for (int f = 0; f < feeds; f++) {
            size_t n = 1 + rnd() % 590;
            for (size_t j = 0; j < n; j++) {
                // mix printable junk with UR-looking prefixes
                uint32_t r = rnd();
                part[j] = (char)(0x20 + r % 95);
            }
            if (rnd() % 3 == 0) memcpy(part, "UR:CRYPTO-PSBT/", n < 15 ? n : 15);
            qrt_parser_feed(p, part, n);
            if (qrt_parser_complete(p)) {
                uint8_t out[4096]; size_t ol = 0;
                // if it claims complete, result must at least not crash;
                // and junk must never yield a "psbt"-magic payload
                if (qrt_parser_result(p, out, sizeof out, &ol) == 0 &&
                    ol >= 5 && memcmp(out, "psbt\xff", 5) == 0) {
                    printf("FAIL: junk UR completed to a psbt payload (iter %d)\n", i);
                    fails++;
                }
            }
        }
        qrt_parser_free(p);
    }
    printf("PASS: 2000 junk QR/UR feeds, no crash, no false completion\n");

    // ---- 3b. real UR stream with one corrupted part mid-flight ----
    for (int i = 0; i < 200; i++) {
        qrt_encoder_t *e = qrt_encoder_new(QRT_FMT_UR, valid, vlen);
        qrt_parser_t *p = qrt_parser_new();
        char part[600];
        int guard = 0;
        while (e && p && !qrt_parser_complete(p) && guard++ < 64) {
            if (qrt_encoder_next(e, part, sizeof part) != 0) break;
            size_t n = strlen(part);
            if (guard == 2 && n > 20) {          // corrupt one part in the middle
                part[10 + rnd() % (n - 15)] = '!';
            }
            qrt_parser_feed(p, part, n);
        }
        if (p && qrt_parser_complete(p)) {       // if it completed anyway, the
            uint8_t out[4096]; size_t ol = 0;    // payload must be the REAL psbt
            if (qrt_parser_result(p, out, sizeof out, &ol) == 0 &&
                (ol != vlen || memcmp(out, valid, vlen) != 0)) {
                printf("FAIL: corrupted UR stream completed to a DIFFERENT payload (iter %d)\n", i);
                fails++;
            }
        }
        qrt_parser_free(p);
        qrt_encoder_free(e);
    }
    printf("PASS: 200 corrupted-part UR streams never stitched a wrong payload\n");

    wallet_session_close();
    printf(fails ? "\n%d FUZZ FAIL\n" : "\nALL FUZZ PASS\n", fails);
    return fails ? 1 : 0;
}
