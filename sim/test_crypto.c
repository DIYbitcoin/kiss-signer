// Desktop test runner for the wallet crypto layer.
// Build: sim/build_test.sh -> /tmp/kisstest
// Step 1: BIP39/BIP32 selftest. Step 4: BIP84 session addresses + descriptor,
// checked against the vectors published in BIP84 itself (dev mnemonic, no passphrase).
#include <stdio.h>
#include <string.h>
#include "kiss_crypto.h"
#include "kiss_sp.h"   // sp_schnorr_sign: the second secp context
#include "kiss_psbt.h"
#include "kiss_usage.h"
#include "sign_vectors.h"   // golden signatures, independently computed (embit)
#include "boot_sign_vectors.h"  // the two the device re-signs at boot

#include <wally_bip32.h>
#include <wally_bip39.h>
#include <wally_address.h>
#include <wally_crypto.h>
#include <wally_map.h>
#include <wally_psbt.h>
#include <wally_psbt_members.h>
#include <wally_script.h>
#include <wally_transaction.h>

static int fails;

// sim/test_qr.c — step 6 QR transport tests (returns its own fail count)
int test_qr_transport(const uint8_t *psbt, size_t psbt_len);
// sim/test_seed.c — step 7 seed layer (runs FIRST; leaves the dev seed stored)
int test_seed_layer(void);
int test_backup_layer(void);
// sim/test_sp.c — step 8 silent payments (BIP352/374/375)
int test_sp(void);
// sim/test_sdseed.c — the sealed blob SD-card storage writes
int test_sdseed_layer(void);
// sim/test_duress.c — the duress unlock stroke classifier
int test_duress(void);
int test_gword(void);
int test_coverword(void);
// sim/test_passedit.c: insert/delete at the passphrase caret
int test_passedit(void);
// sim/test_tapent.c — the tap-entropy fold, debounce and three-way mix
int test_tapent(void);
// sim/test_art.c — the baked-art RLE decoder, cross-checked against the baker
int test_art(void);
// sim/test_dice.c — the dice-entropy digit buffer + SHA256 recipe
int test_dice(void);

// sim/test_lastword.c — the checksum valid last word enumeration (cards mode)
int test_lastword(void);
// sim/test_cards_q.c — whether the owner's own words are worth a seed
int test_cards_q(void);
// sim/test_proof.c — the CAMERA AUDIT frame -> file -> hash -> words pipeline
int test_proof(void);
// sim/test_fw.c — SD firmware update: version ordering and image descriptors
int test_fw(void);

static void chk(const char *name, const char *got, const char *want) {
    if (got && strcmp(got, want) == 0) {
        printf("PASS: %s -> %s\n", name, got);
    } else {
        printf("FAIL: %s\n  got  %s\n  want %s\n", name, got ? got : "(null)", want);
        fails++;
    }
}

static void chki(const char *name, long long got, long long want) {
    if (got == want) {
        printf("PASS: %s -> %lld\n", name, got);
    } else {
        printf("FAIL: %s\n  got  %lld\n  want %lld\n", name, got, want);
        fails++;
    }
}

static void chkb(const char *name, int ok) {
    if (ok) printf("PASS: %s\n", name);
    else { printf("FAIL: %s\n", name); fails++; }
}

// ---- step 5 fixtures: build test PSBTs with libwally itself (independent of
// the code under test). Dev wallet, 1 P2WPKH input at 0/0 (100000 sats),
// out0 = 60000 to an external p2wpkh, out1 = change at 1/0. ----
#define H BIP32_INITIAL_HARDENED_CHILD
enum {
    MUT_NONE      = 0,
    MUT_NO_UTXO   = 1,   // strip witness_utxo -> input amount unverifiable -> STOP
    MUT_FAKE_CHG  = 2,   // change keypath claims 1/0 but script pays elsewhere -> STOP
    MUT_SIGHASH   = 4,   // SIGHASH_SINGLE -> STOP (v1 signs ALL only)
    MUT_UNKNOWN   = 8,   // proprietary input field -> CAUTION + counted
    MUT_HIGH_FEE  = 16,  // shrink change so fee dwarfs the send -> CAUTION
    MUT_TESTNET   = 32,  // keypaths at m/84h/1h/0h -> valid only in testnet mode
    MUT_OPRETURN  = 64,  // out0 is an OP_RETURN blob -> no address to verify -> STOP
    MUT_HARD_IDX  = 128, // change at m/84h/0h/0h/1/0h -- OUR key, re-derives fine,
                         // and no xpub on earth can derive it back -> STOP
    MUT_CONSOLID  = 256, // every output is change: a self-consolidation, the one
                         // shape the old fee-share test skipped entirely
};

static struct ext_key t_master, t_k00, t_k10;   // full-path keys (pub_key valid)
static struct ext_key t_k00t, t_k10t;           // testnet: m/84h/1h/0h/{0,1}/0
static uint8_t t_fp[4];
static uint8_t t_ext_spk[22];
static char    t_ext_addr[92];

static int fixture_keys(void) {
    uint8_t seed[BIP39_SEED_LEN_512]; size_t sl = 0;
    if (bip39_mnemonic_to_seed(
            "abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon abandon abandon about",
            NULL, seed, sizeof seed, &sl) != WALLY_OK) return 1;
    if (bip32_key_from_seed(seed, sizeof seed, BIP32_VER_MAIN_PRIVATE, 0, &t_master) != WALLY_OK) return 2;
    if (bip32_key_get_fingerprint(&t_master, t_fp, sizeof t_fp) != WALLY_OK) return 3;
    const uint32_t p00[5] = {H + 84, H, H, 0, 0}, p10[5] = {H + 84, H, H, 1, 0};
    if (bip32_key_from_parent_path(&t_master, p00, 5, BIP32_FLAG_KEY_PRIVATE, &t_k00) != WALLY_OK) return 4;
    if (bip32_key_from_parent_path(&t_master, p10, 5, BIP32_FLAG_KEY_PRIVATE, &t_k10) != WALLY_OK) return 5;
    const uint32_t q00[5] = {H + 84, H + 1, H, 0, 0}, q10[5] = {H + 84, H + 1, H, 1, 0};
    if (bip32_key_from_parent_path(&t_master, q00, 5, BIP32_FLAG_KEY_PRIVATE, &t_k00t) != WALLY_OK) return 7;
    if (bip32_key_from_parent_path(&t_master, q10, 5, BIP32_FLAG_KEY_PRIVATE, &t_k10t) != WALLY_OK) return 8;
    t_ext_spk[0] = 0x00; t_ext_spk[1] = 0x14;
    memset(t_ext_spk + 2, 0x11, 20);
    char *a = NULL;
    if (wally_addr_segwit_from_bytes(t_ext_spk, 22, "bc", 0, &a) != WALLY_OK) return 6;
    snprintf(t_ext_addr, sizeof t_ext_addr, "%s", a);
    wally_free_string(a);
    return 0;
}

static void p2wpkh_spk(const uint8_t pub[33], uint8_t spk[22]) {
    size_t w = 0;
    wally_witness_program_from_bytes(pub, 33, WALLY_SCRIPT_HASH160, spk, 22, &w);
}

static size_t mk_psbt(int mut, uint8_t *out, size_t outsz) {
    uint8_t txid[32]; memset(txid, 0xAA, sizeof txid);
    const struct ext_key *k00 = (mut & MUT_TESTNET) ? &t_k00t : &t_k00;
    const struct ext_key *k10 = (mut & MUT_TESTNET) ? &t_k10t : &t_k10;
    uint32_t coin = (mut & MUT_TESTNET) ? H + 1 : H;
    uint8_t in_spk[22], chg_spk[22];
    p2wpkh_spk(k00->pub_key, in_spk);
    if (mut & MUT_FAKE_CHG) {
        chg_spk[0] = 0x00; chg_spk[1] = 0x14; memset(chg_spk + 2, 0x22, 20);
    } else {
        p2wpkh_spk(k10->pub_key, chg_spk);
    }
    uint64_t chg_sats = (mut & (MUT_HIGH_FEE | MUT_CONSOLID)) ? 5000 : 39000;  // fee 35000 vs 1000

    // A hardened address index. The key IS ours and bip32 derives it happily
    // from the private master, which is exactly why the path check has to be
    // the thing that refuses it.
    struct ext_key khard;
    const uint32_t coin_h = (mut & MUT_TESTNET) ? H + 1 : H;
    const uint32_t p10h[5] = {H + 84, coin_h, H, 1, H};
    if (mut & MUT_HARD_IDX) {
        bip32_key_from_parent_path(&t_master, p10h, 5, BIP32_FLAG_KEY_PRIVATE, &khard);
        p2wpkh_spk(khard.pub_key, chg_spk);
    }

    struct wally_tx *tx = NULL;
    wally_tx_init_alloc(2, 0, 1, 2, &tx);
    wally_tx_add_raw_input(tx, txid, 32, 0, 0xFFFFFFFD, NULL, 0, NULL, 0);  // RBF seq
    if (mut & MUT_OPRETURN) {            // OP_RETURN blob: no address to show
        const uint8_t opret[10] = {0x6A, 0x08, 0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD, 0xBE, 0xEF};
        wally_tx_add_raw_output(tx, 60000, opret, sizeof opret, 0);
    } else if (mut & MUT_CONSOLID) {     // out0 pays our own receive branch too
        wally_tx_add_raw_output(tx, 60000, in_spk, 22, 0);
    } else {
        wally_tx_add_raw_output(tx, 60000, t_ext_spk, 22, 0);
    }
    wally_tx_add_raw_output(tx, chg_sats, chg_spk, 22, 0);

    struct wally_psbt *p = NULL;
    wally_psbt_init_alloc(0, 1, 2, 1, 0, &p);
    wally_psbt_set_global_tx(p, tx);

    if (!(mut & MUT_NO_UTXO)) {
        struct wally_tx_output *utxo = NULL;
        wally_tx_output_init_alloc(100000, in_spk, 22, &utxo);
        wally_psbt_set_input_witness_utxo(p, 0, utxo);
        wally_tx_output_free(utxo);
    }

    const uint32_t p00[5] = {H + 84, coin, H, 0, 0}, p10[5] = {H + 84, coin, H, 1, 0};
    struct wally_map *m = NULL;
    wally_map_keypath_public_key_init_alloc(1, &m);
    wally_map_keypath_add(m, k00->pub_key, 33, t_fp, 4, p00, 5);
    wally_psbt_set_input_keypaths(p, 0, m);
    wally_map_free(m);
    if (mut & MUT_CONSOLID) {            // out0 needs its own keypath to count
        m = NULL;                        // as change rather than as a recipient
        wally_map_keypath_public_key_init_alloc(1, &m);
        wally_map_keypath_add(m, k00->pub_key, 33, t_fp, 4, p00, 5);
        wally_psbt_set_output_keypaths(p, 0, m);
        wally_map_free(m);
    }
    m = NULL;
    wally_map_keypath_public_key_init_alloc(1, &m);
    if (mut & MUT_HARD_IDX)
        wally_map_keypath_add(m, khard.pub_key, 33, t_fp, 4, p10h, 5);
    else
        wally_map_keypath_add(m, k10->pub_key, 33, t_fp, 4, p10, 5);
    wally_psbt_set_output_keypaths(p, 1, m);
    wally_map_free(m);

    if (mut & MUT_SIGHASH)
        wally_psbt_set_input_sighash(p, 0, WALLY_SIGHASH_SINGLE);
    if (mut & MUT_UNKNOWN) {
        struct wally_map *u = NULL;
        const uint8_t k[3] = {0xFC, 0x01, 0x02}, v[1] = {0x01};
        wally_map_init_alloc(1, NULL, &u);
        wally_map_add(u, k, 3, v, 1);
        wally_psbt_set_input_unknowns(p, 0, u);
        wally_map_free(u);
    }

    size_t wr = 0;
    wally_psbt_to_bytes(p, 0, out, outsz, &wr);
    wally_psbt_free(p);
    wally_tx_free(tx);
    return wr;
}

// ---- script-type fixtures (legacy BIP44 / nested BIP49 / native BIP84) ----
static void derive5(uint32_t purpose, uint32_t chg, uint32_t idx, struct ext_key *o) {
    const uint32_t p[5] = {H + purpose, H, H, chg, idx};
    bip32_key_from_parent_path(&t_master, p, 5, BIP32_FLAG_KEY_PRIVATE, o);
}

static void build_spk(int script, const uint8_t pub[33], uint8_t *out, size_t *len) {
    size_t w = 0;
    if (script == WSCRIPT_LEGACY) {
        wally_scriptpubkey_p2pkh_from_bytes(pub, 33, WALLY_SCRIPT_HASH160, out, 25, len);
    } else if (script == WSCRIPT_NESTED) {
        uint8_t r[22];
        wally_witness_program_from_bytes(pub, 33, WALLY_SCRIPT_HASH160, r, 22, &w);
        wally_scriptpubkey_p2sh_from_bytes(r, 22, WALLY_SCRIPT_HASH160, out, 23, len);
    } else {
        wally_witness_program_from_bytes(pub, 33, WALLY_SCRIPT_HASH160, out, 22, len);
    }
}

// A 1-in (ours) 2-out (external 60k + change 39k) PSBT for the given script type.
static size_t mk_typed_psbt(int script, uint32_t purpose, uint8_t *out, size_t cap) {
    struct ext_key kin, kchg;
    derive5(purpose, 0, 0, &kin);
    derive5(purpose, 1, 0, &kchg);
    uint8_t in_spk[25], chg_spk[25];
    size_t in_len = 0, chg_len = 0;
    build_spk(script, kin.pub_key, in_spk, &in_len);
    build_spk(script, kchg.pub_key, chg_spk, &chg_len);
    uint8_t ext_spk[22] = {0x00, 0x14};
    memset(ext_spk + 2, 0x11, 20);

    struct wally_tx *prev = NULL;
    uint8_t txid[32];
    if (script == WSCRIPT_LEGACY) {          // legacy needs the real prev-tx txid
        wally_tx_init_alloc(2, 0, 1, 1, &prev);
        uint8_t dt[32]; memset(dt, 0xBB, 32);
        wally_tx_add_raw_input(prev, dt, 32, 0, 0xFFFFFFFF, NULL, 0, NULL, 0);
        wally_tx_add_raw_output(prev, 100000, in_spk, in_len, 0);
        wally_tx_get_txid(prev, txid, 32);
    } else {
        memset(txid, 0xAA, 32);
    }

    struct wally_tx *tx = NULL;
    wally_tx_init_alloc(2, 0, 1, 2, &tx);
    wally_tx_add_raw_input(tx, txid, 32, 0, 0xFFFFFFFD, NULL, 0, NULL, 0);
    wally_tx_add_raw_output(tx, 60000, ext_spk, 22, 0);
    wally_tx_add_raw_output(tx, 39000, chg_spk, chg_len, 0);

    struct wally_psbt *p = NULL;
    wally_psbt_init_alloc(0, 1, 2, 1, 0, &p);
    wally_psbt_set_global_tx(p, tx);

    if (script == WSCRIPT_LEGACY) {
        wally_psbt_set_input_utxo(p, 0, prev);
        wally_tx_free(prev);
    } else {
        struct wally_tx_output *u = NULL;
        wally_tx_output_init_alloc(100000, in_spk, in_len, &u);
        wally_psbt_set_input_witness_utxo(p, 0, u);
        wally_tx_output_free(u);
        if (script == WSCRIPT_NESTED) {      // redeem script needed to sign p2sh-p2wpkh
            uint8_t redeem[22]; size_t w = 0;
            wally_witness_program_from_bytes(kin.pub_key, 33, WALLY_SCRIPT_HASH160,
                                             redeem, 22, &w);
            wally_psbt_set_input_redeem_script(p, 0, redeem, 22);
        }
    }

    const uint32_t pin[5] = {H + purpose, H, H, 0, 0}, pchg[5] = {H + purpose, H, H, 1, 0};
    struct wally_map *m = NULL;
    wally_map_keypath_public_key_init_alloc(1, &m);
    wally_map_keypath_add(m, kin.pub_key, 33, t_fp, 4, pin, 5);
    wally_psbt_set_input_keypaths(p, 0, m);
    wally_map_free(m); m = NULL;
    wally_map_keypath_public_key_init_alloc(1, &m);
    wally_map_keypath_add(m, kchg.pub_key, 33, t_fp, 4, pchg, 5);
    wally_psbt_set_output_keypaths(p, 1, m);
    wally_map_free(m);

    size_t wr = 0;
    wally_psbt_to_bytes(p, 0, out, cap, &wr);
    wally_psbt_free(p);
    wally_tx_free(tx);
    wally_bzero(&kin, sizeof kin);
    wally_bzero(&kchg, sizeof kchg);
    return wr;
}

// Native-segwit 1-in PSBT with explicit values: input `in_val` (ours, 84h/0/0),
// one external output `ext_val`, and optionally a change output `chg_val`
// (84h/1/0). Fee is the remainder. For the dust/privacy warning tests.
static size_t mk_val_psbt(uint64_t in_val, uint64_t ext_val, uint64_t chg_val,
                          int with_change, uint8_t *out, size_t cap) {
    struct ext_key kin, kchg;
    derive5(84, 0, 0, &kin);
    derive5(84, 1, 0, &kchg);
    uint8_t in_spk[22], chg_spk[22]; size_t in_len = 0, chg_len = 0;
    build_spk(WSCRIPT_NATIVE, kin.pub_key, in_spk, &in_len);
    build_spk(WSCRIPT_NATIVE, kchg.pub_key, chg_spk, &chg_len);
    uint8_t ext_spk[22] = {0x00, 0x14};
    memset(ext_spk + 2, 0x11, 20);

    uint8_t txid[32]; memset(txid, 0xAA, 32);
    int nout = with_change ? 2 : 1;
    struct wally_tx *tx = NULL;
    wally_tx_init_alloc(2, 0, 1, nout, &tx);
    wally_tx_add_raw_input(tx, txid, 32, 0, 0xFFFFFFFD, NULL, 0, NULL, 0);
    wally_tx_add_raw_output(tx, ext_val, ext_spk, 22, 0);
    if (with_change)
        wally_tx_add_raw_output(tx, chg_val, chg_spk, chg_len, 0);

    struct wally_psbt *p = NULL;
    wally_psbt_init_alloc(0, 1, nout, 1, 0, &p);
    wally_psbt_set_global_tx(p, tx);
    struct wally_tx_output *u = NULL;
    wally_tx_output_init_alloc(in_val, in_spk, in_len, &u);
    wally_psbt_set_input_witness_utxo(p, 0, u);
    wally_tx_output_free(u);

    const uint32_t pin[5] = {H + 84, H, H, 0, 0}, pchg[5] = {H + 84, H, H, 1, 0};
    struct wally_map *m = NULL;
    wally_map_keypath_public_key_init_alloc(1, &m);
    wally_map_keypath_add(m, kin.pub_key, 33, t_fp, 4, pin, 5);
    wally_psbt_set_input_keypaths(p, 0, m);
    wally_map_free(m); m = NULL;
    if (with_change) {
        wally_map_keypath_public_key_init_alloc(1, &m);
        wally_map_keypath_add(m, kchg.pub_key, 33, t_fp, 4, pchg, 5);
        wally_psbt_set_output_keypaths(p, 1, m);
        wally_map_free(m);
    }

    size_t wr = 0;
    wally_psbt_to_bytes(p, 0, out, cap, &wr);
    wally_psbt_free(p);
    wally_tx_free(tx);
    wally_bzero(&kin, sizeof kin);
    wally_bzero(&kchg, sizeof kchg);
    return wr;
}

// n-in native-segwit PSBT, every input ours (84h/0/i) and worth `per` sats, one
// external output taking the lot minus `fee`. No change: this is the shape a
// consolidation or a wallet sweep actually has, which is what the merge caution
// is about. Each input gets its own outpoint so they are distinct.
//
// `mode` decides what backs each input's amount. That is the axis
// WPSBT_C_UNPROVEN_IN turns on, and it is a parameter rather than four builders
// so every case below spends the same coins in the same transaction:
//
//   NIN_CLAIM  witness_utxo only, invented outpoint    -> amount claimed
//   NIN_PROVE  witness_utxo + the real previous tx     -> amount proven
//   NIN_OMIT   same outpoint as PROVE, prev tx dropped -> byte-identical tx to
//              PROVE with the proof removed, which is what makes the
//              signature-unchanged regression mean anything
//   NIN_LIE    real previous tx, witness_utxo overstating it by one sat
#define NIN_MAX 16
enum { NIN_CLAIM = 0, NIN_PROVE, NIN_OMIT, NIN_LIE };
static size_t mk_nin_psbt_ex(int n_in, uint64_t per, uint64_t fee, int mode,
                             uint8_t *out, size_t cap) {
    uint8_t ext_spk[22] = {0x00, 0x14};
    memset(ext_spk + 2, 0x11, 20);
    if (n_in > NIN_MAX) return 0;

    // build every previous tx first: every mode but CLAIM makes the outer tx
    // spend THEIR txids, which is the whole point -- a lie about the amount
    // would change the txid and stop being a lie about this coin
    struct wally_tx *prev[NIN_MAX] = {0};
    uint8_t spks[NIN_MAX][22]; size_t spklen[NIN_MAX];
    uint8_t txids[NIN_MAX][32];
    for (int i = 0; i < n_in; i++) {
        struct ext_key kin;
        derive5(84, 0, (uint32_t)i, &kin);
        spklen[i] = 0;
        build_spk(WSCRIPT_NATIVE, kin.pub_key, spks[i], &spklen[i]);
        wally_bzero(&kin, sizeof kin);
        if (mode != NIN_CLAIM) {
            uint8_t dt[32]; memset(dt, 0xB0 + i, 32);
            wally_tx_init_alloc(2, 0, 1, 1, &prev[i]);
            wally_tx_add_raw_input(prev[i], dt, 32, 0, 0xFFFFFFFF, NULL, 0, NULL, 0);
            wally_tx_add_raw_output(prev[i], per, spks[i], spklen[i], 0);
            wally_tx_get_txid(prev[i], txids[i], 32);
        } else {
            memset(txids[i], 0xA0 + i, 32);
        }
    }

    struct wally_tx *tx = NULL;
    wally_tx_init_alloc(2, 0, n_in, 1, &tx);
    for (int i = 0; i < n_in; i++)
        wally_tx_add_raw_input(tx, txids[i], 32, 0, 0xFFFFFFFD, NULL, 0, NULL, 0);
    wally_tx_add_raw_output(tx, per * (uint64_t)n_in - fee, ext_spk, 22, 0);

    struct wally_psbt *p = NULL;
    wally_psbt_init_alloc(0, n_in, 1, 1, 0, &p);
    wally_psbt_set_global_tx(p, tx);
    for (int i = 0; i < n_in; i++) {
        struct ext_key kin;
        derive5(84, 0, (uint32_t)i, &kin);
        struct wally_tx_output *u = NULL;
        wally_tx_output_init_alloc(mode == NIN_LIE ? per + 1 : per,
                                   spks[i], spklen[i], &u);
        wally_psbt_set_input_witness_utxo(p, i, u);
        wally_tx_output_free(u);
        if (mode == NIN_PROVE || mode == NIN_LIE)
            wally_psbt_set_input_utxo(p, i, prev[i]);
        if (prev[i]) wally_tx_free(prev[i]);

        const uint32_t pin[5] = {H + 84, H, H, 0, (uint32_t)i};
        struct wally_map *m = NULL;
        wally_map_keypath_public_key_init_alloc(1, &m);
        wally_map_keypath_add(m, kin.pub_key, 33, t_fp, 4, pin, 5);
        wally_psbt_set_input_keypaths(p, i, m);
        wally_map_free(m);
        wally_bzero(&kin, sizeof kin);
    }

    size_t wr = 0;
    wally_psbt_to_bytes(p, 0, out, cap, &wr);
    wally_psbt_free(p);
    wally_tx_free(tx);
    // libwally reports the length it WANTED when the buffer is short, and out
    // then holds nothing. Say so rather than handing back a phantom length.
    return wr > cap ? 0 : wr;
}

// The merge tests are about how many coins are tied together, not about who
// proved what, so they spend PROVEN coins and keep asserting the merge flag
// alone. The unproven cases below call mk_nin_psbt_ex directly.
static size_t mk_nin_psbt(int n_in, uint64_t per, uint64_t fee,
                          uint8_t *out, size_t cap) {
    return mk_nin_psbt_ex(n_in, per, fee, NIN_PROVE, out, cap);
}

// 2-in (native 84h + legacy 44h, both ours) / 2-out mixed-type PSBT: input0
// carries a witness_utxo, input1 a full prev tx. 100k + 100k in, 60k out +
// 139k change (native), fee 1000.
static size_t mk_mixed_psbt(uint8_t *out, size_t cap) {
    struct ext_key kn, kl, kchg;
    derive5(84, 0, 0, &kn);
    derive5(44, 0, 0, &kl);
    derive5(84, 1, 0, &kchg);
    uint8_t n_spk[25], l_spk[25], chg_spk[25];
    size_t n_len = 0, l_len = 0, chg_len = 0;
    build_spk(WSCRIPT_NATIVE, kn.pub_key, n_spk, &n_len);
    build_spk(WSCRIPT_LEGACY, kl.pub_key, l_spk, &l_len);
    build_spk(WSCRIPT_NATIVE, kchg.pub_key, chg_spk, &chg_len);
    uint8_t ext_spk[22] = {0x00, 0x14};
    memset(ext_spk + 2, 0x11, 20);

    struct wally_tx *prev = NULL;            // legacy input's full previous tx
    uint8_t ptxid[32], ntxid[32];
    wally_tx_init_alloc(2, 0, 1, 1, &prev);
    uint8_t dt[32]; memset(dt, 0xBB, 32);
    wally_tx_add_raw_input(prev, dt, 32, 0, 0xFFFFFFFF, NULL, 0, NULL, 0);
    wally_tx_add_raw_output(prev, 100000, l_spk, l_len, 0);
    wally_tx_get_txid(prev, ptxid, 32);
    memset(ntxid, 0xAA, 32);

    struct wally_tx *tx = NULL;
    wally_tx_init_alloc(2, 0, 2, 2, &tx);
    wally_tx_add_raw_input(tx, ntxid, 32, 0, 0xFFFFFFFD, NULL, 0, NULL, 0);
    wally_tx_add_raw_input(tx, ptxid, 32, 0, 0xFFFFFFFD, NULL, 0, NULL, 0);
    wally_tx_add_raw_output(tx, 60000, ext_spk, 22, 0);
    wally_tx_add_raw_output(tx, 139000, chg_spk, chg_len, 0);

    struct wally_psbt *p = NULL;
    wally_psbt_init_alloc(0, 2, 2, 1, 0, &p);
    wally_psbt_set_global_tx(p, tx);

    struct wally_tx_output *u = NULL;        // input0: native, witness_utxo
    wally_tx_output_init_alloc(100000, n_spk, n_len, &u);
    wally_psbt_set_input_witness_utxo(p, 0, u);
    wally_tx_output_free(u);
    wally_psbt_set_input_utxo(p, 1, prev);   // input1: legacy, full prev tx
    wally_tx_free(prev);

    const uint32_t pn[5] = {H + 84, H, H, 0, 0}, plg[5] = {H + 44, H, H, 0, 0},
                   pc[5] = {H + 84, H, H, 1, 0};
    struct wally_map *m = NULL;
    wally_map_keypath_public_key_init_alloc(1, &m);
    wally_map_keypath_add(m, kn.pub_key, 33, t_fp, 4, pn, 5);
    wally_psbt_set_input_keypaths(p, 0, m);
    wally_map_free(m); m = NULL;
    wally_map_keypath_public_key_init_alloc(1, &m);
    wally_map_keypath_add(m, kl.pub_key, 33, t_fp, 4, plg, 5);
    wally_psbt_set_input_keypaths(p, 1, m);
    wally_map_free(m); m = NULL;
    wally_map_keypath_public_key_init_alloc(1, &m);
    wally_map_keypath_add(m, kchg.pub_key, 33, t_fp, 4, pc, 5);
    wally_psbt_set_output_keypaths(p, 1, m);
    wally_map_free(m);

    size_t wr = 0;
    wally_psbt_to_bytes(p, 0, out, cap, &wr);
    wally_psbt_free(p);
    wally_tx_free(tx);
    wally_bzero(&kn, sizeof kn);
    wally_bzero(&kl, sizeof kl);
    wally_bzero(&kchg, sizeof kchg);
    return wr;
}

static void test_one_script(int script, uint32_t purpose, const char *label,
                            const char *prefix, const char *wrapper,
                            const char *bwpre) {
    char nm[64], addr[92];
    kiss_set_network(0);
    kiss_set_script(script);
    snprintf(nm, sizeof nm, "%s script reads back", label);
    chki(nm, kiss_script(), script);

    // receive address: right prefix AND equal to an independent derivation
    snprintf(nm, sizeof nm, "%s addr rc", label);
    chki(nm, kiss_session_address(0, 0, addr, sizeof addr), 0);
    snprintf(nm, sizeof nm, "%s addr prefix %s", label, prefix);
    chkb(nm, strncmp(addr, prefix, strlen(prefix)) == 0);
    {
        struct ext_key k; derive5(purpose, 0, 0, &k);
        char *ia = NULL;
        int ok;
        if (script == WSCRIPT_NATIVE) ok = wally_bip32_key_to_addr_segwit(&k, "bc", 0, &ia) == WALLY_OK;
        else ok = wally_bip32_key_to_address(&k,
                     script == WSCRIPT_LEGACY ? WALLY_ADDRESS_TYPE_P2PKH : WALLY_ADDRESS_TYPE_P2SH_P2WPKH,
                     script == WSCRIPT_LEGACY ? 0x00 : 0x05, &ia) == WALLY_OK;
        snprintf(nm, sizeof nm, "%s addr matches independent derive", label);
        chkb(nm, ok && ia && strcmp(addr, ia) == 0);
        if (ia) wally_free_string(ia);
    }

    // descriptor: right wrapper + purpose
    {
        char desc[256];
        chki("descriptor rc", kiss_session_descriptor(desc, sizeof desc), 0);
        snprintf(nm, sizeof nm, "%s descriptor wrapper", label);
        chkb(nm, strncmp(desc, wrapper, strlen(wrapper)) == 0);
        char want[16]; snprintf(want, sizeof want, "/%uh/0h/0h]", purpose);
        snprintf(nm, sizeof nm, "%s descriptor purpose", label);
        chkb(nm, strstr(desc, want) != NULL);
    }

    // BlueWallet export: "[fp/purpose'/0'/0']" origin + the SLIP-132 prefix
    // BlueWallet wants (zpub/ypub, plain xpub for legacy)
    {
        char bw[192], worig[24];
        snprintf(nm, sizeof nm, "%s bw export rc", label);
        chki(nm, kiss_session_bw_export(bw, sizeof bw), 0);
        snprintf(worig, sizeof worig, "[73c5da0a/%u'/0'/0']", purpose);
        snprintf(nm, sizeof nm, "%s bw export origin", label);
        chkb(nm, strncmp(bw, worig, strlen(worig)) == 0);
        snprintf(nm, sizeof nm, "%s bw export prefix %s", label, bwpre);
        chkb(nm, strncmp(bw + strlen(worig), bwpre, strlen(bwpre)) == 0);
    }

    // sign roundtrip
    {
        uint8_t pb[4096], sb[4096]; size_t sw = 0;
        wpsbt_summary_t sum;
        size_t pl = mk_typed_psbt(script, purpose, pb, sizeof pb);
        snprintf(nm, sizeof nm, "%s psbt load rc", label);
        chki(nm, kiss_psbt_load(pb, pl, &sum), 0);
        snprintf(nm, sizeof nm, "%s psbt READY", label);
        chki(nm, sum.status, WPSBT_READY);
        snprintf(nm, sizeof nm, "%s change re-derived", label);
        chkb(nm, sum.outs[1].is_change);
        snprintf(nm, sizeof nm, "%s in/send/change", label);
        chkb(nm, sum.in_sats == 100000 && sum.send_sats == 60000 &&
                 sum.change_sats == 39000 && sum.fee_sats == 1000);
        snprintf(nm, sizeof nm, "%s sign rc", label);
        chki(nm, kiss_psbt_sign(sb, sizeof sb, &sw), 0);
        struct wally_psbt *sp = NULL;
        snprintf(nm, sizeof nm, "%s signed parses", label);
        chkb(nm, wally_psbt_from_bytes(sb, sw, 0, &sp) == WALLY_OK);
        if (sp) {
            // Golden vector: the input-0 signature must equal the byte string an
            // INDEPENDENT signer (embit, tools/sign_fixtures/gen_sign_vectors.py)
            // produced for the same PSBT + dev seed. A change to nonce derivation
            // or low-R grinding fails here. This is the Dark Skippy tell caught
            // at the source: the nonce is not free, and CI proves it did not move.
            const char *want = strcmp(label, "legacy") == 0 ? SV_ECDSA_LEGACY
                             : strcmp(label, "nested") == 0 ? SV_ECDSA_NESTED
                             : SV_ECDSA_NATIVE;
            char got[160] = {0};
            if (sp->inputs[0].signatures.num_items == 1) {
                const struct wally_map_item *it = &sp->inputs[0].signatures.items[0];
                char *gh = NULL;
                wally_hex_from_bytes(it->value, it->value_len, &gh);
                if (gh) { snprintf(got, sizeof got, "%s", gh); wally_free_string(gh); }
            }
            snprintf(nm, sizeof nm, "%s signature is the golden byte string", label);
            chk(nm, got, want);
            snprintf(nm, sizeof nm, "%s signed finalizes", label);
            chkb(nm, wally_psbt_finalize(sp, 0) == WALLY_OK);
            struct wally_tx *stx = NULL;
            snprintf(nm, sizeof nm, "%s signed extracts", label);
            chkb(nm, wally_psbt_extract(sp, 0, &stx) == WALLY_OK);
            if (stx) wally_tx_free(stx);
            wally_psbt_free(sp);
        }
        kiss_psbt_free();
        if (strcmp(label, "native") == 0) {
            // Signature fingerprint: sha256 of the input's signature bytes, first
            // 4 bytes. Computed independently from SV_ECDSA_NATIVE:
            //   python3 -c "import hashlib;print(hashlib.sha256(bytes.fromhex('<native sig>')).hexdigest()[:8])"
            char fp[9] = {0};
            chki("sig fingerprint rc", kiss_psbt_sig_fingerprint(sb, sw, fp), 0);
            chk("sig fingerprint is the golden code", fp, "a1e0d4c5");

            // Determinism localizer: the same PSBT signs to the same bytes every
            // time. A stray RNG in the nonce path breaks this even where a golden
            // vector might still match by luck. Reload, re-sign, require identical
            // bytes AND an identical fingerprint.
            wpsbt_summary_t s2; uint8_t sb2[4096]; size_t sw2 = 0;
            kiss_psbt_load(pb, pl, &s2);
            chki("native re-sign rc", kiss_psbt_sign(sb2, sizeof sb2, &sw2), 0);
            chkb("native signing is deterministic (byte-identical)",
                 sw2 == sw && memcmp(sb2, sb, sw) == 0);
            char fp2[9] = {0};
            kiss_psbt_sig_fingerprint(sb2, sw2, fp2);
            chkb("sig fingerprint stable across re-sign", strcmp(fp, fp2) == 0);
            kiss_psbt_free();
        }
    }
    kiss_set_script(WSCRIPT_NATIVE);
}

// The boot signing selftest (kiss_sign_selftest) reproduces two golden
// signatures with the frozen rules. Here we check that it passes AND that the
// vectors it pins actually discriminate: a golden vector a weaker rule also
// satisfies would sit in the binary proving nothing.
static void test_boot_sign_selftest(void) {
    chki("boot sign selftest rc", kiss_sign_selftest(), 0);

    uint8_t sig[64];
    // Discrimination 1: drop the low-R grinding. BSV_MSG was chosen so the
    // counter-0 RFC6979 nonce gives a high R, so plain ECDSA must differ.
    chkb("ECDSA vector is unreachable without grind-R",
         wally_ec_sig_from_bytes(BSV_KEY, 32, BSV_MSG, 32,
                                 EC_FLAG_ECDSA, sig, sizeof sig) == WALLY_OK &&
         memcmp(sig, BSV_ECDSA, sizeof sig) != 0);

    // Discrimination 2: BIP340 with the default all-zero aux is a different
    // signature, so the vector pins our aux rule and not merely "some BIP340".
    uint8_t zero_aux[32] = {0};
    chkb("Schnorr vector is unreachable with a zero aux",
         wally_ec_sig_from_bytes_aux(BSV_KEY, 32, BSV_MSG, 32,
                                     zero_aux, sizeof zero_aux,
                                     EC_FLAG_SCHNORR, sig, sizeof sig) == WALLY_OK &&
         memcmp(sig, BSV_SCHNORR, sizeof sig) != 0);
}

// Blinding. secp256k1 multiplies the secret by a random scalar and divides it
// back out, so the power and timing traces of a signature stop being a
// function of the key alone. It costs one call and neither context had ever
// had it: libwally's global, and the separate one kiss_sp builds for BIP340.
//
// The whole risk of turning it on is that a signature stops being reproducible,
// which on this device would be a worse bug than the one being fixed -- so the
// test is not "does randomize return 0", it is "sign the same thing either side
// of a re-randomize and get the same bytes". Both signing paths, because they
// hold different contexts.
static void test_secp_randomize(void) {
    chki("secp randomize rc", kiss_secp_randomize(), 0);
    chki("secp randomize again rc", kiss_secp_randomize(), 0);

    // libwally's context: the boot selftest re-signs golden ECDSA + BIP340
    chki("boot sign selftest survives randomize", kiss_sign_selftest(), 0);

    // kiss_sp's context: BIP340 over a fixed key, message and aux
    uint8_t d[32], msg[32], aux[32], sig1[64], sig2[64];
    memset(d, 0x11, sizeof d);
    memset(msg, 0x22, sizeof msg);
    memset(aux, 0x33, sizeof aux);
    int ok1 = sp_schnorr_sign(d, msg, aux, sig1) == 0;
    chki("secp randomize between signings", kiss_secp_randomize(), 0);
    int ok2 = sp_schnorr_sign(d, msg, aux, sig2) == 0;
    chkb("schnorr is identical either side of a randomize",
         ok1 && ok2 && memcmp(sig1, sig2, sizeof sig1) == 0);
}

// Locking drops the transaction, not just the key. kiss_psbt_load returns -1
// before reaching its own free() when there is no session, so lock-then-open
// was the sequence that kept the last PSBT parsed in RAM -- every address and
// amount in it -- until something happened to load another.
static void test_lock_drops_psbt(const uint8_t *psbt, size_t len)
{
    wpsbt_summary_t sum;
    wpsbt_details_t det;

    chki("lock: psbt loads READY first",
         kiss_psbt_load(psbt, len, &sum) == 0 && sum.status == WPSBT_READY ? 0 : 1, 0);
    chkb("lock: details are available while open", kiss_psbt_details(&det) == 0);

    chkb("lock: a transaction is held while open", kiss_psbt_held());

    kiss_session_close();
    // The readers already refused without a session, so nothing could be shown.
    // Refusing to SHOW it and not HOLDING it are different claims, and this is
    // the second one: the parsed transaction -- every address and amount in it
    // -- must not still be in RAM after the device locks.
    chkb("lock: no transaction is held after the lock", !kiss_psbt_held());
    chkb("lock: and no reader offers one", kiss_psbt_details(&det) != 0);

    // and a load attempted with no session must not resurrect the old one
    chkb("lock: a load with no session is refused",
         kiss_psbt_load(psbt, len, &sum) != 0);
    chkb("lock: the refused load resurrected nothing", !kiss_psbt_held());

    chki("lock: reopen for the rest of the suite", kiss_session_open(NULL), 0);
}

// A selftest whose failure changes nothing is decoration. Force it to fail and
// prove the signer refuses: the same shape as OVERLAPCHECK_SELFTEST, which
// exists because a clean sweep means nothing without proof the gate can fire.
static void test_sign_refused_when_selftest_fails(const uint8_t *psbt, size_t len) {
    wpsbt_summary_t sum;
    uint8_t out[4096];
    size_t written = 0;

    kiss_sign_selftest_force_fail(1);
    chki("selftest reports the forced failure", kiss_sign_selftest(), 99);
    chki("psbt load still works", kiss_psbt_load(psbt, len, &sum), 0);
    chkb("signing is REFUSED while the selftest fails",
         kiss_psbt_sign(out, sizeof out, &written) != 0);
    chki("nothing was written", (long long)written, 0);
    kiss_psbt_free();

    kiss_sign_selftest_force_fail(0);
    chki("selftest passes again once un-forced", kiss_sign_selftest(), 0);
    chki("psbt load works", kiss_psbt_load(psbt, len, &sum), 0);
    chki("signing works again", kiss_psbt_sign(out, sizeof out, &written), 0);
    kiss_psbt_free();
}

int main(int argc, char **argv) {
    // step 7 first: ends with the dev seed stored, which everything below uses
    fails += test_seed_layer();
    fails += test_backup_layer();
    fails += test_sdseed_layer();
    fails += test_duress();
    fails += test_gword();
    fails += test_coverword();
    fails += test_passedit();
    fails += test_tapent();
    fails += test_art();
    fails += test_dice();
    fails += test_lastword();
    fails += test_cards_q();
    fails += test_proof();
    fails += test_fw();

    test_boot_sign_selftest();
    test_secp_randomize();

    uint8_t fp[4] = {0};
    int rc = kiss_selftest(fp);
    printf("fingerprint: %02X%02X%02X%02X\n", fp[0], fp[1], fp[2], fp[3]);
    if (rc != 0) {
        printf("FAIL: kiss_selftest stage %d\n", rc);
        return 1;
    }
    printf("PASS: BIP39 test vector -> 73C5DA0A\n");

    // ---- entropy mixing: seed = SHA256(camera_hash || TRNG bytes), so neither
    // a predictable scene nor a weak chip RNG can weaken the seed alone ----
    {
        uint8_t a[32], b[32], c[32], m1[32], m2[32], m3[32], want[32], cat[64];
        memset(a, 0xAA, 32); memset(b, 0xBB, 32); memset(c, 0xCC, 32);
        chki("entropy mix rc", kiss_entropy_mix(a, b, m1), 0);
        memcpy(cat, a, 32); memcpy(cat + 32, b, 32);
        chkb("entropy mix is SHA256(a||b)",
             wally_sha256(cat, 64, want, 32) == WALLY_OK && memcmp(m1, want, 32) == 0);
        chkb("entropy mix != camera hash alone", memcmp(m1, a, 32) != 0);
        kiss_entropy_mix(a, c, m2);
        chkb("TRNG bytes change the result", memcmp(m1, m2, 32) != 0);
        kiss_entropy_mix(c, b, m3);
        chkb("camera bytes change the result", memcmp(m1, m3, 32) != 0);
    }

    // ---- step 4: session + BIP84 (vectors straight from the BIP84 document) ----
    if (kiss_session_open(NULL) != 0) { printf("FAIL: kiss_session_open\n"); return 1; }

    char addr[91];
    if (kiss_session_address(0, 0, addr, sizeof addr) != 0) { printf("FAIL: address 0/0 rc\n"); return 1; }
    chk("m/84h/0h/0h/0/0", addr, "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu");
    if (kiss_session_address(0, 1, addr, sizeof addr) == 0)
        chk("m/84h/0h/0h/0/1", addr, "bc1qnjg0jd8228aq7egyzacy8cys3knf9xvrerkf9g");
    else { printf("FAIL: address 0/1 rc\n"); fails++; }
    if (kiss_session_address(1, 0, addr, sizeof addr) == 0)
        chk("m/84h/0h/0h/1/0", addr, "bc1q8c6fshw2dlwun7ekn9qwf37cu2rn755upcp6el");
    else { printf("FAIL: address 1/0 rc\n"); fails++; }

    // The on-device verifier distinguishes three materially different cases:
    // ours/current-network, valid but from the other network, and malformed.
    // A current-network address that simply is not ours is handled separately
    // by the UI's bounded 100-address search.
    {
        const char *tb = "tb1q6rz28mcfaxtmd6v789l9rrlrusdprr9pqcpvkl";
        chki("mainnet address validates for mainnet",
             kiss_address_validate(
                 "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu"),
             WADDR_CURRENT_NETWORK);
        chki("testnet address is wrong while on mainnet",
             kiss_address_validate(tb), WADDR_WRONG_NETWORK);
        chki("malformed address is invalid",
             kiss_address_validate("not-an-address"), WADDR_INVALID);
        kiss_set_network(1);
        chki("testnet address validates for testnet",
             kiss_address_validate(tb), WADDR_CURRENT_NETWORK);
        chki("mainnet address is wrong while on testnet",
             kiss_address_validate(
                 "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu"),
             WADDR_WRONG_NETWORK);
        kiss_set_network(0);
    }

    char desc[256];
    if (kiss_session_descriptor(desc, sizeof desc) != 0) { printf("FAIL: descriptor rc\n"); return 1; }
    printf("descriptor: %s\n", desc);
    if (strncmp(desc, "wpkh([73c5da0a/84h/0h/0h]xpub", 29) == 0 &&
        strcmp(desc + strlen(desc) - 9, "/<0;1>/*)") == 0) {
        printf("PASS: descriptor shape\n");
    } else {
        printf("FAIL: descriptor shape\n");
        fails++;
    }

    // roundtrip: the xpub inside the descriptor must re-derive the 0/0 vector address
    // (proves the exported account key is really m/84h/0h/0h of the seed)
    const char *xs = strchr(desc, ']');
    const char *xe = strstr(desc, "/<");
    if (xs && xe && xe > xs + 1) {
        char xpub[128] = {0};
        memcpy(xpub, xs + 1, (size_t)(xe - xs - 1));
        struct ext_key acct, child;
        uint32_t path[2] = {0, 0};
        char *a58 = NULL;
        if (bip32_key_from_base58(xpub, &acct) == WALLY_OK &&
            bip32_key_from_parent_path(&acct, path, 2, BIP32_FLAG_KEY_PUBLIC, &child) == WALLY_OK &&
            wally_bip32_key_to_addr_segwit(&child, "bc", 0, &a58) == WALLY_OK) {
            chk("descriptor xpub re-derives 0/0", a58, "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu");
            wally_free_string(a58);
        } else {
            printf("FAIL: descriptor xpub roundtrip\n");
            fails++;
        }
    } else {
        printf("FAIL: descriptor xpub not found\n");
        fails++;
    }

    // BlueWallet pairing export: key origin + SLIP-132 zpub. The zpub is the
    // BIP84 document's own account-0 vector, so this proves the whole encode.
    {
        char bw[192];
        if (kiss_session_bw_export(bw, sizeof bw) != 0) {
            printf("FAIL: bw export rc\n"); fails++;
        } else {
            chk("bw export = origin + BIP84 vector zpub", bw,
                "[73c5da0a/84'/0'/0']"
                "zpub6rFR7y4Q2AijBEqTUquhVz398htDFrtymD9xYYfG1m4wAcvPhXNfE3EfH1r1"
                "ADqtfSdVCToUG868RvUUkgDKf31mGDtKsAYz2oz2AGutZYs");
        }
        // testnet: vpub prefix (no published vector; prefix + origin checked)
        kiss_set_network(1);
        if (kiss_session_bw_export(bw, sizeof bw) != 0) {
            printf("FAIL: bw export testnet rc\n"); fails++;
        } else {
            chkb("bw export testnet origin+vpub",
                 strncmp(bw, "[73c5da0a/84'/1'/0']vpub", 24) == 0);
        }
        kiss_set_network(0);
    }

    // ---- step 5: PSBT parse / verify / sign ----
    printf("---- step 5: PSBT ----\n");
    if (fixture_keys() != 0) { printf("FAIL: psbt fixture keys\n"); return 1; }

    // 4096, not 1024: a PSBT that carries the full previous transaction for
    // every input -- the shape that proves its own amounts -- is several times
    // the size of one that only claims them. Same ceiling the device has.
    uint8_t pb[4096], sb[4096];
    size_t pl, sw = 0;
    wpsbt_summary_t sum;

    pl = mk_psbt(MUT_NONE, pb, sizeof pb);
    chkb("psbt fixture serializes", pl > 100);
    chki("psbt load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("psbt status READY", sum.status, WPSBT_READY);
    chki("psbt n_in", sum.n_in, 1);
    chki("psbt n_out", sum.n_out, 2);
    chki("psbt in_sats", (long long)sum.in_sats, 100000);
    chki("psbt send", (long long)sum.send_sats, 60000);
    chki("psbt change", (long long)sum.change_sats, 39000);
    chki("psbt fee", (long long)sum.fee_sats, 1000);
    chki("psbt rbf", sum.rbf, 1);
    chki("psbt locktime", sum.locktime, 0);
    chkb("psbt out0 external", !sum.outs[0].is_change);
    chk("psbt out0 addr", sum.outs[0].addr, t_ext_addr);
    chkb("psbt out1 change re-derived", sum.outs[1].is_change);
    chk("psbt out1 addr", sum.outs[1].addr, "bc1q8c6fshw2dlwun7ekn9qwf37cu2rn755upcp6el");
    chkb("psbt est_vsize sane", sum.est_vsize >= 130 && sum.est_vsize <= 150);
    chki("psbt fee rate x10", sum.fee_rate_x10, sum.est_vsize ? 10000 / sum.est_vsize : -1);

    {   // DETAILS accessor: per-input facts + the unsigned txid (final for segwit)
        wpsbt_details_t det;
        chki("psbt details rc", kiss_psbt_details(&det), 0);
        chki("details version", det.version, 2);
        chki("details locktime", det.locktime, 0);
        chki("details n_in", det.n_in, 1);
        chk("details in0 prev txid", det.ins[0].txid,   // fixture prev = 32x 0xAA
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        chki("details in0 vout", det.ins[0].vout, 0);
        chki("details in0 sats", (long long)det.ins[0].sats, 100000);
        chki("details in0 purpose", det.ins[0].purpose, 84);
        chki("details in0 change", det.ins[0].change, 0);
        chki("details in0 index", det.ins[0].index, 0);
        chkb("details txid final (all segwit)", det.txid_final);
        chkb("details txid is 64 hex", strlen(det.txid) == 64 &&
             strspn(det.txid, "0123456789abcdef") == 64);
        chki("details n_total", det.n_total, 1);

        // a STOPped load must refuse the details page (it presents fields as
        // verified; the verifier just said they are not)
        uint8_t sb2[1024];
        size_t sl2 = mk_psbt(MUT_NO_UTXO, sb2, sizeof sb2);
        wpsbt_summary_t stopsum;
        chki("details stop-load rc", kiss_psbt_load(sb2, sl2, &stopsum), 0);
        chki("details stop status", stopsum.status, WPSBT_STOP);
        chkb("details refused on STOP", kiss_psbt_details(&det) != 0);
        chki("details reload rc", kiss_psbt_load(pb, pl, &sum), 0);  // restore READY
    }

    chki("psbt sign rc", kiss_psbt_sign(sb, sizeof sb, &sw), 0);
    chkb("psbt signed bigger", sw > pl);
    {   // the signed PSBT must finalize + extract to a real tx with a 2-item witness
        struct wally_psbt *sp = NULL;
        struct wally_tx *stx = NULL;
        chkb("signed psbt parses", wally_psbt_from_bytes(sb, sw, 0, &sp) == WALLY_OK);
        if (sp) {
            chkb("signed psbt finalizes", wally_psbt_finalize(sp, 0) == WALLY_OK);
            chkb("signed psbt extracts", wally_psbt_extract(sp, 0, &stx) == WALLY_OK);
        }
        if (stx) {
            size_t vs = 0;
            wally_tx_get_vsize(stx, &vs);
            chkb("actual vsize within 4 of estimate",
                 (long long)vs >= (long long)sum.est_vsize - 4 && vs <= sum.est_vsize + 4);
            chkb("witness has sig+pubkey",
                 stx->inputs[0].witness && stx->inputs[0].witness->num_items == 2);
            wally_tx_free(stx);
        }
        if (sp) wally_psbt_free(sp);
    }
    kiss_psbt_free();

    {   // coordinators hand users base64 as often as binary — loader must sniff it
        char *b64 = NULL;
        pl = mk_psbt(MUT_NONE, pb, sizeof pb);
        chkb("fixture base64-encodes", wally_base64_from_bytes(pb, pl, 0, &b64) == WALLY_OK);
        if (b64) {
            chki("base64 psbt load rc", kiss_psbt_load((const uint8_t *)b64, strlen(b64), &sum), 0);
            chki("base64 psbt READY", sum.status, WPSBT_READY);
            chki("base64 psbt fee", (long long)sum.fee_sats, 1000);
            wally_free_string(b64);
        }
        kiss_psbt_free();
    }

    pl = mk_psbt(MUT_NO_UTXO, pb, sizeof pb);
    chki("no-utxo load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("no-utxo STOP", sum.status, WPSBT_STOP);
    chkb("no-utxo reason says amount", strstr(sum.reason, "amount") != NULL);
    chkb("no-utxo sign refused", kiss_psbt_sign(sb, sizeof sb, &sw) != 0);
    kiss_psbt_free();

    pl = mk_psbt(MUT_FAKE_CHG, pb, sizeof pb);
    chki("fake-change load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("fake-change STOP", sum.status, WPSBT_STOP);
    chkb("fake-change reason says change", strstr(sum.reason, "change") != NULL);
    chkb("fake-change sign refused", kiss_psbt_sign(sb, sizeof sb, &sw) != 0);
    kiss_psbt_free();

    pl = mk_psbt(MUT_SIGHASH, pb, sizeof pb);
    chki("sighash load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("sighash STOP", sum.status, WPSBT_STOP);
    chkb("sighash reason says sighash", strstr(sum.reason, "sighash") != NULL);
    kiss_psbt_free();

    pl = mk_psbt(MUT_UNKNOWN, pb, sizeof pb);
    chki("unknown load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("unknown STOP", sum.status, WPSBT_STOP);              // don't sign past unknowns
    chki("unknown count", sum.n_unknown, 1);
    chkb("unknown reason says unknown", strstr(sum.reason, "unknown") != NULL);
    chkb("unknown sign refused", kiss_psbt_sign(sb, sizeof sb, &sw) != 0);
    kiss_psbt_free();

    pl = mk_psbt(MUT_HIGH_FEE, pb, sizeof pb);
    chki("high-fee load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("high-fee CAUTION", sum.status, WPSBT_CAUTION);
    chkb("high-fee reason says fee", strstr(sum.reason, "fee") != NULL);
    chkb("high-fee flag set", (sum.caution_flags & WPSBT_C_HIGHFEE) != 0);
    kiss_psbt_free();

    // ---- dust / privacy warnings (CAUTION, never STOP; several can stack) ----
    // clean spend: normal input, normal change, moderate fee -> no cautions
    pl = mk_val_psbt(100000, 60000, 38000, 1, pb, sizeof pb);
    chki("clean load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("clean READY", sum.status, WPSBT_READY);
    chki("clean no caution flags", sum.caution_flags, 0);
    kiss_psbt_free();

    // spending a tiny KISS-owned coin: dust-input privacy warn, still signable
    pl = mk_val_psbt(3000, 2700, 0, 0, pb, sizeof pb);
    chki("dust-input load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("dust-input CAUTION", sum.status, WPSBT_CAUTION);
    chkb("dust-input flag set", (sum.caution_flags & WPSBT_C_DUST_INPUT) != 0);
    chkb("dust-input not STOP-signable", kiss_psbt_sign(sb, sizeof sb, &sw) == 0);
    kiss_psbt_free();

    // small (but above dust) change: privacy warn, not the loud dust-change flag
    pl = mk_val_psbt(100000, 90000, 4000, 1, pb, sizeof pb);
    chki("small-change load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chkb("small-change flag set", (sum.caution_flags & WPSBT_C_SMALL_CHANGE) != 0);
    chkb("small-change not dust-change", (sum.caution_flags & WPSBT_C_DUST_CHANGE) == 0);
    kiss_psbt_free();

    // change below the standardness dust floor (<294 segwit): loud dust-change
    pl = mk_val_psbt(100000, 99500, 200, 1, pb, sizeof pb);
    chki("dust-change load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chkb("dust-change flag set", (sum.caution_flags & WPSBT_C_DUST_CHANGE) != 0);
    chkb("dust-change not small-change", (sum.caution_flags & WPSBT_C_SMALL_CHANGE) == 0);
    kiss_psbt_free();

    // fee-rate backstop (~300 sat/vB): a big send at a fat-finger rate trips the
    // rate check even though the fee is a small SHARE of the send
    pl = mk_val_psbt(2000000, 1900000, 40000, 1, pb, sizeof pb);   // fee 60000 -> ~425 sat/vB
    chki("high-rate load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("high-rate CAUTION", sum.status, WPSBT_CAUTION);
    chkb("high-rate flags high-fee", (sum.caution_flags & WPSBT_C_HIGHFEE) != 0);
    chkb("high-rate share is small", sum.fee_sats * 10 < sum.send_sats);   // not the % check
    kiss_psbt_free();

    // an elevated-but-normal rate below the backstop stays clean (no congestion
    // fatigue): ~140 sat/vB, well under the 300 bar and a small share
    pl = mk_val_psbt(2000000, 1900000, 80000, 1, pb, sizeof pb);   // fee 20000 -> ~140 sat/vB
    chki("moderate-rate load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("moderate-rate READY", sum.status, WPSBT_READY);
    chki("moderate-rate no cautions", sum.caution_flags, 0);
    kiss_psbt_free();

    // combo: tiny input + tiny change + high fee -> all three flags coexist
    pl = mk_val_psbt(4000, 3000, 200, 1, pb, sizeof pb);
    chki("combo load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("combo CAUTION", sum.status, WPSBT_CAUTION);
    chkb("combo has high-fee", (sum.caution_flags & WPSBT_C_HIGHFEE) != 0);
    chkb("combo has dust-input", (sum.caution_flags & WPSBT_C_DUST_INPUT) != 0);
    chkb("combo has dust-change", (sum.caution_flags & WPSBT_C_DUST_CHANGE) != 0);
    kiss_psbt_free();

    // ---- merging coins: the bar sits above everyday coin selection ----
    // one under the bar: four ordinary coins is still a wallet picking inputs,
    // and warning there would be the fatigue the threshold exists to avoid
    pl = mk_nin_psbt(WPSBT_MERGE_INS - 1, 100000, 8000, pb, sizeof pb);
    chki("under-merge load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("under-merge input count", (int)sum.n_in, WPSBT_MERGE_INS - 1);
    chki("under-merge READY", sum.status, WPSBT_READY);
    chki("under-merge no cautions", sum.caution_flags, 0);
    kiss_psbt_free();

    // at the bar: flagged, still signable, and no other flag rides along (the
    // coins are ordinary and the fee is moderate, so this is the merge alone)
    pl = mk_nin_psbt(WPSBT_MERGE_INS, 100000, 10000, pb, sizeof pb);
    chki("merge load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("merge input count", (int)sum.n_in, WPSBT_MERGE_INS);
    chki("merge CAUTION", sum.status, WPSBT_CAUTION);
    chki("merge flag alone", sum.caution_flags, WPSBT_C_MERGE_INS);
    chkb("merge reason says merging", strstr(sum.reason, "merging") != NULL);
    chkb("merge still signable", kiss_psbt_sign(sb, sizeof sb, &sw) == 0);
    kiss_psbt_free();

    // a sweep of tiny coins stacks the merge flag on the dust-input one: they
    // answer different questions (how many are tied together vs who sent them)
    pl = mk_nin_psbt(WPSBT_MERGE_INS + 2, 3000, 2000, pb, sizeof pb);
    chki("merge-dust load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chkb("merge-dust has merge", (sum.caution_flags & WPSBT_C_MERGE_INS) != 0);
    chkb("merge-dust has dust-input", (sum.caution_flags & WPSBT_C_DUST_INPUT) != 0);
    kiss_psbt_free();

    // ---- amounts declared vs amounts proven --------------------------------
    // BIP143 signs the amount of the input being signed and nothing else, so a
    // coordinator can run two sessions, name a different (individually true)
    // amount in each, and combine one valid signature per input. The tx that
    // broadcasts pays a fee neither screen showed. Nothing in a single PSBT can
    // rule that out -- so with two or more inputs whose amounts are only
    // CLAIMED, say so. See WPSBT_C_UNPROVEN_IN.

    // one input: the lie lands in that input's own sighash and breaks it, so a
    // bare witness_utxo is enough and there is nothing to warn about
    pl = mk_nin_psbt_ex(1, 100000, 1000, NIN_CLAIM, pb, sizeof pb);
    chki("1-in unproven load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("1-in unproven READY", sum.status, WPSBT_READY);
    chki("1-in unproven no cautions", sum.caution_flags, 0);
    chki("1-in unproven count", (int)sum.n_unproven_in, 1);
    kiss_psbt_free();

    // two inputs, amounts claimed and not proven: the case krux warns on
    pl = mk_nin_psbt_ex(2, 100000, 2000, NIN_CLAIM, pb, sizeof pb);
    chki("2-in unproven load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("2-in unproven CAUTION", sum.status, WPSBT_CAUTION);
    chki("2-in unproven flag alone", sum.caution_flags, WPSBT_C_UNPROVEN_IN);
    chki("2-in unproven count", (int)sum.n_unproven_in, 2);
    chkb("2-in unproven reason", strstr(sum.reason, "not proven") != NULL);
    chkb("2-in unproven still signable", kiss_psbt_sign(sb, sizeof sb, &sw) == 0);
    kiss_psbt_free();

    // the same two coins with their previous transactions attached: nothing left
    // to lie about, so the warning goes away. This is the escape hatch, and it
    // is what keeps the caution from being permanent noise.
    pl = mk_nin_psbt_ex(2, 100000, 2000, NIN_PROVE, pb, sizeof pb);
    chkb("2-in proven builds", pl > 0);
    chki("2-in proven load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("2-in proven READY", sum.status, WPSBT_READY);
    chki("2-in proven no cautions", sum.caution_flags, 0);
    chki("2-in proven count zero", (int)sum.n_unproven_in, 0);
    kiss_psbt_free();

    // proving costs bytes, and that is the whole reason a coordinator skips it
    {
        uint8_t pa[4096];
        size_t bare = mk_nin_psbt_ex(2, 100000, 2000, NIN_CLAIM, pa, sizeof pa);
        size_t full = mk_nin_psbt_ex(2, 100000, 2000, NIN_PROVE, pb, sizeof pb);
        chkb("previous transactions make the PSBT bigger", full > bare);
    }

    // A witness_utxo that contradicts the previous transaction is a coordinator
    // disagreeing with itself about a coin. Refuse rather than pick a side --
    // and note the amount reading LOW is the theft direction, so the one sat
    // difference here is not a rounding question.
    pl = mk_nin_psbt_ex(1, 100000, 1000, NIN_LIE, pb, sizeof pb);
    chki("contradiction load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("contradiction STOP", sum.status, WPSBT_STOP);
    chkb("contradiction reason", strstr(sum.reason, "previous transaction") != NULL);
    chkb("contradiction refuses to sign", kiss_psbt_sign(sb, sizeof sb, &sw) != 0);
    kiss_psbt_free();

    // THE regression for the precedence flip: PROVE and OMIT are the same
    // transaction spending the same outpoint for the same amount, differing
    // only in whether the proof rides along. Reading the amount off the
    // previous transaction instead of the witness_utxo must not move a single
    // byte of the signature, or this change quietly broke every signer that
    // ever co-signed with this one.
    {
        uint8_t sa[4096], sbb[4096];
        size_t wa = 0, wb = 0;
        pl = mk_nin_psbt_ex(2, 100000, 2000, NIN_OMIT, pb, sizeof pb);
        chki("omit load rc", kiss_psbt_load(pb, pl, &sum), 0);
        chki("omit is the unproven one", sum.caution_flags, WPSBT_C_UNPROVEN_IN);
        chki("omit sign rc", kiss_psbt_sign(sa, sizeof sa, &wa), 0);
        kiss_psbt_free();

        pl = mk_nin_psbt_ex(2, 100000, 2000, NIN_PROVE, pb, sizeof pb);
        chki("prove load rc", kiss_psbt_load(pb, pl, &sum), 0);
        chki("prove sign rc", kiss_psbt_sign(sbb, sizeof sbb, &wb), 0);
        kiss_psbt_free();

        // the serialized PSBTs differ (one carries the previous transactions),
        // so compare the thing that must not move: the signatures themselves
        char fa[9], fb[9];
        chki("omit sig fingerprint rc", kiss_psbt_sig_fingerprint(sa, wa, fa), 0);
        chki("prove sig fingerprint rc", kiss_psbt_sig_fingerprint(sbb, wb, fb), 0);
        chk("proof does not change the signature", fa, fb);
    }

    // DETAILS says WHICH coin: the per-input mark the verify row cannot carry
    pl = mk_nin_psbt_ex(2, 100000, 2000, NIN_CLAIM, pb, sizeof pb);
    chki("details unproven load rc", kiss_psbt_load(pb, pl, &sum), 0);
    {
        wpsbt_details_t dt;
        chki("details unproven rc", kiss_psbt_details(&dt), 0);
        chki("details unproven n_in", dt.n_in, 2);
        chkb("details in0 not proven", !dt.ins[0].proven);
        chkb("details in1 not proven", !dt.ins[1].proven);
    }
    kiss_psbt_free();
    pl = mk_nin_psbt_ex(2, 100000, 2000, NIN_PROVE, pb, sizeof pb);
    chki("details proven load rc", kiss_psbt_load(pb, pl, &sum), 0);
    {
        wpsbt_details_t dt;
        chki("details proven rc", kiss_psbt_details(&dt), 0);
        chkb("details in0 proven", dt.ins[0].proven);
        chkb("details in1 proven", dt.ins[1].proven);
    }
    kiss_psbt_free();

    // ---- amount sanity: consensus cap + no unsigned wraparound ----
    // outputs > inputs must STOP with fee_sats untouched (display safety: the
    // verify screen renders send+fee, which stays sane on this path)
    pl = mk_val_psbt(50000, 60000, 10000, 1, pb, sizeof pb);
    chki("exceed load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("exceed STOP", sum.status, WPSBT_STOP);
    chkb("exceed reason", strstr(sum.reason, "exceed") != NULL);
    chkb("exceed fee zero", sum.fee_sats == 0);
    chkb("exceed display no underflow", sum.send_sats + sum.fee_sats == 60000);
    kiss_psbt_free();

    // absurd per-amount values (> 21M BTC): wally's own builders AND parser
    // refuse them (verified: from_bytes rc=-2 on a patched amount), so such a
    // PSBT must never reach the verifier — otherwise send+change could wrap
    // uint64 and sneak past the outputs-exceed-inputs check. Patch a valid
    // PSBT's LE64 amount to near-2^64 and require load to reject the bytes.
    // (kiss_psbt.c ALSO caps per-amount at MAX_MONEY as defense-in-depth.)
    pl = mk_val_psbt(100000, 60000, 38000, 1, pb, sizeof pb);
    {
        const uint8_t old[8] = {0x60, 0xEA, 0, 0, 0, 0, 0, 0};        // 60000 LE
        const uint8_t evil[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        int patched = 0;
        for (size_t i = 0; i + 8 <= pl; i++)
            if (memcmp(pb + i, old, 8) == 0) { memcpy(pb + i, evil, 8); patched = 1; break; }
        chkb("absurd-out amount patched", patched);
        chkb("absurd-out bytes rejected", kiss_psbt_load(pb, pl, &sum) != 0);
    }
    kiss_psbt_free();

    // ---- receive reuse guard (kiss_usage) ----
    {
        uint8_t fp[4] = {0xEC, 0x5A, 0x45, 0x95};
        kiss_usage_wipe();
        chki("usage fresh -> -1", kiss_usage_high(fp, 0, WSCRIPT_NATIVE), -1);
        kiss_usage_mark(fp, 0, WSCRIPT_NATIVE, 3);
        chki("usage marks 3", kiss_usage_high(fp, 0, WSCRIPT_NATIVE), 3);
        kiss_usage_mark(fp, 0, WSCRIPT_NATIVE, 1);         // lower: ignored
        chki("usage monotonic", kiss_usage_high(fp, 0, WSCRIPT_NATIVE), 3);
        kiss_usage_mark(fp, 0, WSCRIPT_NATIVE, 7);
        chki("usage advances to 7", kiss_usage_high(fp, 0, WSCRIPT_NATIVE), 7);
        chki("usage isolates network", kiss_usage_high(fp, 1, WSCRIPT_NATIVE), -1);
        chki("usage isolates type", kiss_usage_high(fp, 0, WSCRIPT_LEGACY), -1);
        uint8_t fp2[4] = {0x11, 0x22, 0x33, 0x44};
        chki("usage isolates wallet", kiss_usage_high(fp2, 0, WSCRIPT_NATIVE), -1);
        kiss_usage_wipe();
        chki("usage wipe clears", kiss_usage_high(fp, 0, WSCRIPT_NATIVE), -1);
    }

    // an output the device can't render as an address = a destination the user
    // can't verify = refuse to sign (STOP, not caution)
    pl = mk_psbt(MUT_OPRETURN, pb, sizeof pb);
    chki("opreturn load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("nonstandard output STOP", sum.status, WPSBT_STOP);
    chkb("nonstandard reason says nonstandard", strstr(sum.reason, "nonstandard") != NULL);
    chkb("nonstandard sign refused", kiss_psbt_sign(sb, sizeof sb, &sw) != 0);
    kiss_psbt_free();

    // A hardened change index. The key is genuinely this wallet's and derives
    // from the private master without complaint, so re-derivation alone cannot
    // catch it -- but the descriptor this device exports is an xpub, and no
    // xpub can ever derive a hardened child. Change sent there is provably ours
    // and permanently invisible to every watch-only wallet the owner has.
    pl = mk_psbt(MUT_HARD_IDX, pb, sizeof pb);
    chki("hardened change index load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("hardened change index STOP", sum.status, WPSBT_STOP);
    chkb("hardened change index sign refused",
         kiss_psbt_sign(sb, sizeof sb, &sw) != 0);
    kiss_psbt_free();

    // Self-consolidation: every output is change, so send_sats is 0. The
    // fee-share test used to be skipped outright in that case, leaving a 35%
    // fee judged only by its sat/vB rate -- which at this size passes. The
    // share is measured against the coins being consolidated now.
    pl = mk_psbt(MUT_CONSOLID, pb, sizeof pb);
    chki("consolidation load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("consolidation has no send", (int)sum.send_sats, 0);
    chkb("consolidation rate alone would not have fired",
         sum.fee_rate_x10 <= WPSBT_HIGH_RATE_X10);
    chki("consolidation high fee CAUTION", sum.status, WPSBT_CAUTION);
    chkb("consolidation flags the fee",
         (sum.caution_flags & WPSBT_C_HIGHFEE) != 0);
    kiss_psbt_free();

    chkb("garbage refuses to load", kiss_psbt_load((const uint8_t *)"nope", 4, &sum) != 0);
    kiss_psbt_free();

    pl = mk_psbt(MUT_NONE, pb, sizeof pb);
    test_sign_refused_when_selftest_fails(pb, pl);
    test_lock_drops_psbt(pb, pl);

    kiss_session_close();
    if (kiss_session_address(0, 0, addr, sizeof addr) != 0) {
        printf("PASS: closed session refuses to derive\n");
    } else {
        printf("FAIL: session still derives after close\n");
        fails++;
    }
    pl = mk_psbt(MUT_NONE, pb, sizeof pb);
    chkb("closed session refuses PSBT", kiss_psbt_load(pb, pl, &sum) != 0);

    // step 6: QR transport (pure data layer, session not needed)
    fails += test_qr_transport(pb, pl);

    // ---- testnet mode: same seed, coin 1h, tb1, tpub, wrong-network STOP ----
    kiss_set_network(1);
    chki("testnet mode reads back", kiss_testnet(), 1);
    chki("testnet session open", kiss_session_open(""), 0);
    chki("testnet addr rc", kiss_session_address(0, 0, addr, sizeof addr), 0);
    // independent derivation (fixture keys, coin 1h) must agree with the wallet
    {
        char *ta = NULL;
        chkb("fixture tn addr derives",
             wally_bip32_key_to_addr_segwit(&t_k00t, "tb", 0, &ta) == WALLY_OK);
        if (ta) { chk("m/84h/1h/0h/0/0 vs fixture", addr, ta); wally_free_string(ta); }
    }
    chk("m/84h/1h/0h/0/0 (published vector)", addr,
        "tb1q6rz28mcfaxtmd6v789l9rrlrusdprr9pqcpvkl");
    {
        char desc[240];
        chki("testnet descriptor rc", kiss_session_descriptor(desc, sizeof desc), 0);
        chkb("testnet descriptor path 84h/1h/0h", strstr(desc, "/84h/1h/0h]") != NULL);
        chkb("testnet descriptor tpub", strstr(desc, "]tpub") != NULL);
    }

    // a mainnet PSBT must be blocked while in testnet mode, loudly
    pl = mk_psbt(MUT_NONE, pb, sizeof pb);
    chki("mainnet psbt on testnet load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("mainnet psbt on testnet STOP", sum.status, WPSBT_STOP);
    chkb("wrong-network reason", strstr(sum.reason, "network") != NULL);
    chkb("summary flags testnet", sum.testnet);
    chkb("wrong-network sign refused", kiss_psbt_sign(sb, sizeof sb, &sw) != 0);
    kiss_psbt_free();

    // a native testnet PSBT verifies READY with tb1 addresses and signs
    pl = mk_psbt(MUT_TESTNET, pb, sizeof pb);
    chki("testnet psbt load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("testnet psbt READY", sum.status, WPSBT_READY);
    chkb("testnet out0 addr is tb1", strncmp(sum.outs[0].addr, "tb1", 3) == 0);
    chkb("testnet change re-derived", sum.outs[1].is_change);
    chkb("testnet change addr is tb1", strncmp(sum.outs[1].addr, "tb1", 3) == 0);
    chki("testnet sign rc", kiss_psbt_sign(sb, sizeof sb, &sw), 0);
    kiss_psbt_free();

    // and the testnet PSBT is blocked back on mainnet (no state leaks either way)
    kiss_set_network(0);
    chki("mainnet mode reads back", kiss_testnet(), 0);
    chki("mainnet addr again rc", kiss_session_address(0, 0, addr, sizeof addr), 0);
    chk("m/84h/0h/0h/0/0 again", addr, "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu");
    pl = mk_psbt(MUT_TESTNET, pb, sizeof pb);
    chki("testnet psbt on mainnet load rc", kiss_psbt_load(pb, pl, &sum), 0);
    chki("testnet psbt on mainnet STOP", sum.status, WPSBT_STOP);
    chkb("wrong-network reason (mainnet side)", strstr(sum.reason, "network") != NULL);
    kiss_psbt_free();
    pl = mk_psbt(MUT_NONE, pb, sizeof pb);
    chki("mainnet psbt READY again", kiss_psbt_load(pb, pl, &sum), 0);
    chki("mainnet psbt status again", sum.status, WPSBT_READY);
    kiss_psbt_free();

    // step 8: silent payments (needs the open session; flips network itself
    // and returns in mainnet mode, matching the sweep below)
    fails += test_sp();

    // ---- address types: legacy (BIP44), nested (BIP49), native (BIP84) ----
    test_one_script(WSCRIPT_NATIVE, 84, "native", "bc1", "wpkh(",    "zpub");
    test_one_script(WSCRIPT_NESTED, 49, "nested", "3",   "sh(wpkh(", "ypub");
    test_one_script(WSCRIPT_LEGACY, 44, "legacy", "1",   "pkh(",     "xpub");

    // ---- the signer is TYPE-AGNOSTIC: it signs whatever script type the PSBT's
    // own input paths declare, regardless of the ADDRESS TYPE selected (that
    // setting only governs Receive/Export). No pre-matching a type to sign.
    {
        struct { int script; uint32_t purpose; const char *name; } cs[3] = {
            {WSCRIPT_NATIVE, 84, "native"}, {WSCRIPT_NESTED, 49, "nested"}, {WSCRIPT_LEGACY, 44, "legacy"},
        };
        uint8_t pb2[4096]; wpsbt_summary_t sm; char nm[80];
        kiss_set_network(0);
        for (int a = 0; a < 3; a++)          // a = the PSBT's actual input type
          for (int b = 0; b < 3; b++) {      // b = the (possibly different) selected type
            size_t pl2 = mk_typed_psbt(cs[a].script, cs[a].purpose, pb2, sizeof pb2);
            kiss_set_script(cs[b].script);
            snprintf(nm, sizeof nm, "%s psbt signs while %s selected", cs[a].name, cs[b].name);
            chki(nm, kiss_psbt_load(pb2, pl2, &sm), 0);
            chki(nm, sm.status, WPSBT_READY);
            snprintf(nm, sizeof nm, "%s psbt detected purpose %u", cs[a].name, cs[a].purpose);
            chki(nm, sm.purpose, cs[a].purpose);   // UI shows the PSBT's own type
            kiss_psbt_free();
        }
        // network guard still holds: a mainnet-coin PSBT is refused on testnet
        size_t pl3 = mk_typed_psbt(WSCRIPT_NATIVE, 84, pb2, sizeof pb2);  // coin 0h
        kiss_set_network(1);
        chki("mainnet psbt loads on testnet", kiss_psbt_load(pb2, pl3, &sm), 0);
        chki("mainnet psbt STOPs on testnet (wrong network)", sm.status, WPSBT_STOP);
        kiss_psbt_free();
        kiss_set_network(0);

        // LEGACY input carrying only a witness_utxo: the legacy sighash does
        // NOT commit to amounts, so a witness_utxo amount is exactly the
        // fake-fee lie the full-prev-tx requirement exists to block -> STOP
        {
            struct ext_key kin2, kchg2;
            derive5(44, 0, 0, &kin2);
            derive5(44, 1, 0, &kchg2);
            uint8_t in_spk[25], chg_spk[25];
            size_t in_len = 0, chg_len = 0;
            build_spk(WSCRIPT_LEGACY, kin2.pub_key, in_spk, &in_len);
            build_spk(WSCRIPT_LEGACY, kchg2.pub_key, chg_spk, &chg_len);
            uint8_t ext2[22] = {0x00, 0x14};
            memset(ext2 + 2, 0x11, 20);
            uint8_t txid2[32]; memset(txid2, 0xAA, 32);
            struct wally_tx *t2 = NULL;
            wally_tx_init_alloc(2, 0, 1, 2, &t2);
            wally_tx_add_raw_input(t2, txid2, 32, 0, 0xFFFFFFFD, NULL, 0, NULL, 0);
            wally_tx_add_raw_output(t2, 60000, ext2, 22, 0);
            wally_tx_add_raw_output(t2, 39000, chg_spk, chg_len, 0);
            struct wally_psbt *p2 = NULL;
            wally_psbt_init_alloc(0, 1, 2, 1, 0, &p2);
            wally_psbt_set_global_tx(p2, t2);
            struct wally_tx_output *u2 = NULL;      // witness_utxo on a LEGACY input
            wally_tx_output_init_alloc(100000, in_spk, in_len, &u2);
            wally_psbt_set_input_witness_utxo(p2, 0, u2);
            wally_tx_output_free(u2);
            const uint32_t pi2[5] = {H + 44, H, H, 0, 0}, pc2[5] = {H + 44, H, H, 1, 0};
            struct wally_map *m2 = NULL;
            wally_map_keypath_public_key_init_alloc(1, &m2);
            wally_map_keypath_add(m2, kin2.pub_key, 33, t_fp, 4, pi2, 5);
            wally_psbt_set_input_keypaths(p2, 0, m2);
            wally_map_free(m2); m2 = NULL;
            wally_map_keypath_public_key_init_alloc(1, &m2);
            wally_map_keypath_add(m2, kchg2.pub_key, 33, t_fp, 4, pc2, 5);
            wally_psbt_set_output_keypaths(p2, 1, m2);
            wally_map_free(m2);
            size_t wl2 = 0;
            wally_psbt_to_bytes(p2, 0, pb2, sizeof pb2, &wl2);
            wally_psbt_free(p2);
            wally_tx_free(t2);
            wally_bzero(&kin2, sizeof kin2);
            wally_bzero(&kchg2, sizeof kchg2);
            chkb("legacy witness-only builds", wl2 > 0);
            chki("legacy witness-only load rc", kiss_psbt_load(pb2, wl2, &sm), 0);
            chki("legacy witness-only STOP", sm.status, WPSBT_STOP);
            chkb("legacy witness-only reason says previous",
                 strstr(sm.reason, "previous") != NULL);
            kiss_psbt_free();
        }

        // MIXED input types (1 native + 1 legacy) in one PSBT: legit (e.g. a
        // consolidation), must verify READY, report purpose 0 (mixed), and the
        // fee estimate must count each input at its own type's weight
        size_t plm = mk_mixed_psbt(pb2, sizeof pb2);
        chkb("mixed psbt builds", plm > 0);
        chki("mixed psbt load rc", kiss_psbt_load(pb2, plm, &sm), 0);
        // CAUTION, not READY: the legacy input carries its previous transaction
        // and is proven, the native one carries only a witness_utxo and is not,
        // and two inputs is where the two-session amount lie becomes possible.
        chki("mixed psbt CAUTION", sm.status, WPSBT_CAUTION);
        chki("mixed psbt unproven alone", sm.caution_flags, WPSBT_C_UNPROVEN_IN);
        chki("mixed psbt one unproven", (int)sm.n_unproven_in, 1);
        chki("mixed psbt purpose 0 (mixed)", sm.purpose, 0);
        chki("mixed psbt n_in", sm.n_in, 2);
        // legacy sig ≈107 vB full weight + native witness ≈28 vB: the estimate
        // must be well ABOVE an all-native guess and below an all-legacy one
        chkb("mixed fee estimate counts per type",
             sm.est_vsize > 200 && sm.est_vsize < 400);
        kiss_psbt_free();
    }
    // native 0/0 still equals the published BIP84 vector (no leakage)
    kiss_set_script(WSCRIPT_NATIVE);
    chki("native addr after type sweep rc", kiss_session_address(0, 0, addr, sizeof addr), 0);
    chk("m/84h/0h/0h/0/0 after sweep", addr, "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu");

    // ---- the account key is cached between derivations, so prove the cache
    // cannot outlive the session it came from. A stale one would serve the
    // PREVIOUS wallet's addresses to the next one, and since the difference
    // between the decoy and the real wallet is exactly a passphrase, that is
    // the deniability property silently inverted. The script and network
    // sweeps above already cover the other two things it keys on.
    {
        kiss_set_network(0);
        kiss_set_script(WSCRIPT_NATIVE);
        char a_plain[92], a_pass[92], a_again[92];
        chki("cache: decoy session opens", kiss_session_open(NULL), 0);
        chki("cache: decoy addr rc",
             kiss_session_address(0, 0, a_plain, sizeof a_plain), 0);
        chki("cache: passphrase session opens", kiss_session_open("kiss"), 0);
        chki("cache: passphrase addr rc",
             kiss_session_address(0, 0, a_pass, sizeof a_pass), 0);
        chkb("cache: passphrase gives a DIFFERENT address",
             strcmp(a_plain, a_pass) != 0);
        chki("cache: decoy session reopens", kiss_session_open(NULL), 0);
        chki("cache: decoy addr rc again",
             kiss_session_address(0, 0, a_again, sizeof a_again), 0);
        chk("cache: same passphrase, same address", a_again, a_plain);
        kiss_session_close();
        chkb("cache: closed session derives nothing",
             kiss_session_address(0, 0, addr, sizeof addr) != 0);
        chki("cache: reopen after close", kiss_session_open(NULL), 0);
        chki("cache: addr rc after close+reopen",
             kiss_session_address(0, 0, addr, sizeof addr), 0);
        chk("m/84h/0h/0h/0/0 after close+reopen", addr,
            "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu");
    }

    // optional: also emit device-test PSBT files (argv[1] = target dir, e.g. the
    // SD card). kiss-pay.psbt verifies READY + signs; kiss-stop.psbt must STOP.
    if (argc > 1) {
        char path[512];
        FILE *f;
        pl = mk_psbt(MUT_NONE, pb, sizeof pb);
        snprintf(path, sizeof path, "%s/kiss-pay.psbt", argv[1]);
        if ((f = fopen(path, "wb"))) { fwrite(pb, 1, pl, f); fclose(f); printf("wrote %s\n", path); }
        pl = mk_psbt(MUT_NO_UTXO, pb, sizeof pb);
        snprintf(path, sizeof path, "%s/kiss-stop.psbt", argv[1]);
        if ((f = fopen(path, "wb"))) { fwrite(pb, 1, pl, f); fclose(f); printf("wrote %s\n", path); }
    }

    return fails ? 1 : 0;
}
