// Desktop test runner for the wallet crypto layer.
// Build: sim/build_test.sh -> /tmp/kisstest
// Step 1: BIP39/BIP32 selftest. Step 4: BIP84 session addresses + descriptor,
// checked against the vectors published in BIP84 itself (dev mnemonic, no passphrase).
#include <stdio.h>
#include <string.h>
#include "wallet_crypto.h"
#include "wallet_psbt.h"
#include "wallet_usage.h"

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
// sim/test_sp.c — step 8 silent payments (BIP352/374/375)
int test_sp(void);

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
    uint64_t chg_sats = (mut & MUT_HIGH_FEE) ? 5000 : 39000;   // fee 35000 vs 1000

    struct wally_tx *tx = NULL;
    wally_tx_init_alloc(2, 0, 1, 2, &tx);
    wally_tx_add_raw_input(tx, txid, 32, 0, 0xFFFFFFFD, NULL, 0, NULL, 0);  // RBF seq
    if (mut & MUT_OPRETURN) {            // OP_RETURN blob: no address to show
        const uint8_t opret[10] = {0x6A, 0x08, 0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD, 0xBE, 0xEF};
        wally_tx_add_raw_output(tx, 60000, opret, sizeof opret, 0);
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
    m = NULL;
    wally_map_keypath_public_key_init_alloc(1, &m);
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
    wallet_set_network(0);
    wallet_set_script(script);
    snprintf(nm, sizeof nm, "%s script reads back", label);
    chki(nm, wallet_script(), script);

    // receive address: right prefix AND equal to an independent derivation
    snprintf(nm, sizeof nm, "%s addr rc", label);
    chki(nm, wallet_session_address(0, 0, addr, sizeof addr), 0);
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
        chki("descriptor rc", wallet_session_descriptor(desc, sizeof desc), 0);
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
        chki(nm, wallet_session_bw_export(bw, sizeof bw), 0);
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
        chki(nm, wallet_psbt_load(pb, pl, &sum), 0);
        snprintf(nm, sizeof nm, "%s psbt READY", label);
        chki(nm, sum.status, WPSBT_READY);
        snprintf(nm, sizeof nm, "%s change re-derived", label);
        chkb(nm, sum.outs[1].is_change);
        snprintf(nm, sizeof nm, "%s in/send/change", label);
        chkb(nm, sum.in_sats == 100000 && sum.send_sats == 60000 &&
                 sum.change_sats == 39000 && sum.fee_sats == 1000);
        snprintf(nm, sizeof nm, "%s sign rc", label);
        chki(nm, wallet_psbt_sign(sb, sizeof sb, &sw), 0);
        struct wally_psbt *sp = NULL;
        snprintf(nm, sizeof nm, "%s signed parses", label);
        chkb(nm, wally_psbt_from_bytes(sb, sw, 0, &sp) == WALLY_OK);
        if (sp) {
            snprintf(nm, sizeof nm, "%s signed finalizes", label);
            chkb(nm, wally_psbt_finalize(sp, 0) == WALLY_OK);
            struct wally_tx *stx = NULL;
            snprintf(nm, sizeof nm, "%s signed extracts", label);
            chkb(nm, wally_psbt_extract(sp, 0, &stx) == WALLY_OK);
            if (stx) wally_tx_free(stx);
            wally_psbt_free(sp);
        }
        wallet_psbt_free();
    }
    wallet_set_script(WSCRIPT_NATIVE);
}

int main(int argc, char **argv) {
    // step 7 first: ends with the dev seed stored, which everything below uses
    fails += test_seed_layer();

    uint8_t fp[4] = {0};
    int rc = wallet_selftest(fp);
    printf("fingerprint: %02X%02X%02X%02X\n", fp[0], fp[1], fp[2], fp[3]);
    if (rc != 0) {
        printf("FAIL: wallet_selftest stage %d\n", rc);
        return 1;
    }
    printf("PASS: BIP39 test vector -> 73C5DA0A\n");

    // ---- entropy mixing: seed = SHA256(camera_hash || TRNG bytes), so neither
    // a predictable scene nor a weak chip RNG can weaken the seed alone ----
    {
        uint8_t a[32], b[32], c[32], m1[32], m2[32], m3[32], want[32], cat[64];
        memset(a, 0xAA, 32); memset(b, 0xBB, 32); memset(c, 0xCC, 32);
        chki("entropy mix rc", wallet_entropy_mix(a, b, m1), 0);
        memcpy(cat, a, 32); memcpy(cat + 32, b, 32);
        chkb("entropy mix is SHA256(a||b)",
             wally_sha256(cat, 64, want, 32) == WALLY_OK && memcmp(m1, want, 32) == 0);
        chkb("entropy mix != camera hash alone", memcmp(m1, a, 32) != 0);
        wallet_entropy_mix(a, c, m2);
        chkb("TRNG bytes change the result", memcmp(m1, m2, 32) != 0);
        wallet_entropy_mix(c, b, m3);
        chkb("camera bytes change the result", memcmp(m1, m3, 32) != 0);
    }

    // ---- step 4: session + BIP84 (vectors straight from the BIP84 document) ----
    if (wallet_session_open(NULL) != 0) { printf("FAIL: wallet_session_open\n"); return 1; }

    char addr[91];
    if (wallet_session_address(0, 0, addr, sizeof addr) != 0) { printf("FAIL: address 0/0 rc\n"); return 1; }
    chk("m/84h/0h/0h/0/0", addr, "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu");
    if (wallet_session_address(0, 1, addr, sizeof addr) == 0)
        chk("m/84h/0h/0h/0/1", addr, "bc1qnjg0jd8228aq7egyzacy8cys3knf9xvrerkf9g");
    else { printf("FAIL: address 0/1 rc\n"); fails++; }
    if (wallet_session_address(1, 0, addr, sizeof addr) == 0)
        chk("m/84h/0h/0h/1/0", addr, "bc1q8c6fshw2dlwun7ekn9qwf37cu2rn755upcp6el");
    else { printf("FAIL: address 1/0 rc\n"); fails++; }

    char desc[256];
    if (wallet_session_descriptor(desc, sizeof desc) != 0) { printf("FAIL: descriptor rc\n"); return 1; }
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
        if (wallet_session_bw_export(bw, sizeof bw) != 0) {
            printf("FAIL: bw export rc\n"); fails++;
        } else {
            chk("bw export = origin + BIP84 vector zpub", bw,
                "[73c5da0a/84'/0'/0']"
                "zpub6rFR7y4Q2AijBEqTUquhVz398htDFrtymD9xYYfG1m4wAcvPhXNfE3EfH1r1"
                "ADqtfSdVCToUG868RvUUkgDKf31mGDtKsAYz2oz2AGutZYs");
        }
        // testnet: vpub prefix (no published vector; prefix + origin checked)
        wallet_set_network(1);
        if (wallet_session_bw_export(bw, sizeof bw) != 0) {
            printf("FAIL: bw export testnet rc\n"); fails++;
        } else {
            chkb("bw export testnet origin+vpub",
                 strncmp(bw, "[73c5da0a/84'/1'/0']vpub", 24) == 0);
        }
        wallet_set_network(0);
    }

    // ---- step 5: PSBT parse / verify / sign ----
    printf("---- step 5: PSBT ----\n");
    if (fixture_keys() != 0) { printf("FAIL: psbt fixture keys\n"); return 1; }

    uint8_t pb[1024], sb[2048];
    size_t pl, sw = 0;
    wpsbt_summary_t sum;

    pl = mk_psbt(MUT_NONE, pb, sizeof pb);
    chkb("psbt fixture serializes", pl > 100);
    chki("psbt load rc", wallet_psbt_load(pb, pl, &sum), 0);
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
        chki("psbt details rc", wallet_psbt_details(&det), 0);
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
        chki("details stop-load rc", wallet_psbt_load(sb2, sl2, &stopsum), 0);
        chki("details stop status", stopsum.status, WPSBT_STOP);
        chkb("details refused on STOP", wallet_psbt_details(&det) != 0);
        chki("details reload rc", wallet_psbt_load(pb, pl, &sum), 0);  // restore READY
    }

    chki("psbt sign rc", wallet_psbt_sign(sb, sizeof sb, &sw), 0);
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
    wallet_psbt_free();

    {   // coordinators hand users base64 as often as binary — loader must sniff it
        char *b64 = NULL;
        pl = mk_psbt(MUT_NONE, pb, sizeof pb);
        chkb("fixture base64-encodes", wally_base64_from_bytes(pb, pl, 0, &b64) == WALLY_OK);
        if (b64) {
            chki("base64 psbt load rc", wallet_psbt_load((const uint8_t *)b64, strlen(b64), &sum), 0);
            chki("base64 psbt READY", sum.status, WPSBT_READY);
            chki("base64 psbt fee", (long long)sum.fee_sats, 1000);
            wally_free_string(b64);
        }
        wallet_psbt_free();
    }

    pl = mk_psbt(MUT_NO_UTXO, pb, sizeof pb);
    chki("no-utxo load rc", wallet_psbt_load(pb, pl, &sum), 0);
    chki("no-utxo STOP", sum.status, WPSBT_STOP);
    chkb("no-utxo reason says amount", strstr(sum.reason, "amount") != NULL);
    chkb("no-utxo sign refused", wallet_psbt_sign(sb, sizeof sb, &sw) != 0);
    wallet_psbt_free();

    pl = mk_psbt(MUT_FAKE_CHG, pb, sizeof pb);
    chki("fake-change load rc", wallet_psbt_load(pb, pl, &sum), 0);
    chki("fake-change STOP", sum.status, WPSBT_STOP);
    chkb("fake-change reason says change", strstr(sum.reason, "change") != NULL);
    chkb("fake-change sign refused", wallet_psbt_sign(sb, sizeof sb, &sw) != 0);
    wallet_psbt_free();

    pl = mk_psbt(MUT_SIGHASH, pb, sizeof pb);
    chki("sighash load rc", wallet_psbt_load(pb, pl, &sum), 0);
    chki("sighash STOP", sum.status, WPSBT_STOP);
    chkb("sighash reason says sighash", strstr(sum.reason, "sighash") != NULL);
    wallet_psbt_free();

    pl = mk_psbt(MUT_UNKNOWN, pb, sizeof pb);
    chki("unknown load rc", wallet_psbt_load(pb, pl, &sum), 0);
    chki("unknown STOP", sum.status, WPSBT_STOP);              // don't sign past unknowns
    chki("unknown count", sum.n_unknown, 1);
    chkb("unknown reason says unknown", strstr(sum.reason, "unknown") != NULL);
    chkb("unknown sign refused", wallet_psbt_sign(sb, sizeof sb, &sw) != 0);
    wallet_psbt_free();

    pl = mk_psbt(MUT_HIGH_FEE, pb, sizeof pb);
    chki("high-fee load rc", wallet_psbt_load(pb, pl, &sum), 0);
    chki("high-fee CAUTION", sum.status, WPSBT_CAUTION);
    chkb("high-fee reason says fee", strstr(sum.reason, "fee") != NULL);
    chkb("high-fee flag set", (sum.caution_flags & WPSBT_C_HIGHFEE) != 0);
    wallet_psbt_free();

    // ---- dust / privacy warnings (CAUTION, never STOP; several can stack) ----
    // clean spend: normal input, normal change, moderate fee -> no cautions
    pl = mk_val_psbt(100000, 60000, 38000, 1, pb, sizeof pb);
    chki("clean load rc", wallet_psbt_load(pb, pl, &sum), 0);
    chki("clean READY", sum.status, WPSBT_READY);
    chki("clean no caution flags", sum.caution_flags, 0);
    wallet_psbt_free();

    // spending a tiny KISS-owned coin: dust-input privacy warn, still signable
    pl = mk_val_psbt(3000, 2700, 0, 0, pb, sizeof pb);
    chki("dust-input load rc", wallet_psbt_load(pb, pl, &sum), 0);
    chki("dust-input CAUTION", sum.status, WPSBT_CAUTION);
    chkb("dust-input flag set", (sum.caution_flags & WPSBT_C_DUST_INPUT) != 0);
    chkb("dust-input not STOP-signable", wallet_psbt_sign(sb, sizeof sb, &sw) == 0);
    wallet_psbt_free();

    // small (but above dust) change: privacy warn, not the loud dust-change flag
    pl = mk_val_psbt(100000, 90000, 4000, 1, pb, sizeof pb);
    chki("small-change load rc", wallet_psbt_load(pb, pl, &sum), 0);
    chkb("small-change flag set", (sum.caution_flags & WPSBT_C_SMALL_CHANGE) != 0);
    chkb("small-change not dust-change", (sum.caution_flags & WPSBT_C_DUST_CHANGE) == 0);
    wallet_psbt_free();

    // change below the standardness dust floor (<294 segwit): loud dust-change
    pl = mk_val_psbt(100000, 99500, 200, 1, pb, sizeof pb);
    chki("dust-change load rc", wallet_psbt_load(pb, pl, &sum), 0);
    chkb("dust-change flag set", (sum.caution_flags & WPSBT_C_DUST_CHANGE) != 0);
    chkb("dust-change not small-change", (sum.caution_flags & WPSBT_C_SMALL_CHANGE) == 0);
    wallet_psbt_free();

    // fee-rate backstop (~300 sat/vB): a big send at a fat-finger rate trips the
    // rate check even though the fee is a small SHARE of the send
    pl = mk_val_psbt(2000000, 1900000, 40000, 1, pb, sizeof pb);   // fee 60000 -> ~425 sat/vB
    chki("high-rate load rc", wallet_psbt_load(pb, pl, &sum), 0);
    chki("high-rate CAUTION", sum.status, WPSBT_CAUTION);
    chkb("high-rate flags high-fee", (sum.caution_flags & WPSBT_C_HIGHFEE) != 0);
    chkb("high-rate share is small", sum.fee_sats * 10 < sum.send_sats);   // not the % check
    wallet_psbt_free();

    // an elevated-but-normal rate below the backstop stays clean (no congestion
    // fatigue): ~140 sat/vB, well under the 300 bar and a small share
    pl = mk_val_psbt(2000000, 1900000, 80000, 1, pb, sizeof pb);   // fee 20000 -> ~140 sat/vB
    chki("moderate-rate load rc", wallet_psbt_load(pb, pl, &sum), 0);
    chki("moderate-rate READY", sum.status, WPSBT_READY);
    chki("moderate-rate no cautions", sum.caution_flags, 0);
    wallet_psbt_free();

    // combo: tiny input + tiny change + high fee -> all three flags coexist
    pl = mk_val_psbt(4000, 3000, 200, 1, pb, sizeof pb);
    chki("combo load rc", wallet_psbt_load(pb, pl, &sum), 0);
    chki("combo CAUTION", sum.status, WPSBT_CAUTION);
    chkb("combo has high-fee", (sum.caution_flags & WPSBT_C_HIGHFEE) != 0);
    chkb("combo has dust-input", (sum.caution_flags & WPSBT_C_DUST_INPUT) != 0);
    chkb("combo has dust-change", (sum.caution_flags & WPSBT_C_DUST_CHANGE) != 0);
    wallet_psbt_free();

    // ---- amount sanity: consensus cap + no unsigned wraparound ----
    // outputs > inputs must STOP with fee_sats untouched (display safety: the
    // verify screen renders send+fee, which stays sane on this path)
    pl = mk_val_psbt(50000, 60000, 10000, 1, pb, sizeof pb);
    chki("exceed load rc", wallet_psbt_load(pb, pl, &sum), 0);
    chki("exceed STOP", sum.status, WPSBT_STOP);
    chkb("exceed reason", strstr(sum.reason, "exceed") != NULL);
    chkb("exceed fee zero", sum.fee_sats == 0);
    chkb("exceed display no underflow", sum.send_sats + sum.fee_sats == 60000);
    wallet_psbt_free();

    // absurd per-amount values (> 21M BTC): wally's own builders AND parser
    // refuse them (verified: from_bytes rc=-2 on a patched amount), so such a
    // PSBT must never reach the verifier — otherwise send+change could wrap
    // uint64 and sneak past the outputs-exceed-inputs check. Patch a valid
    // PSBT's LE64 amount to near-2^64 and require load to reject the bytes.
    // (wallet_psbt.c ALSO caps per-amount at MAX_MONEY as defense-in-depth.)
    pl = mk_val_psbt(100000, 60000, 38000, 1, pb, sizeof pb);
    {
        const uint8_t old[8] = {0x60, 0xEA, 0, 0, 0, 0, 0, 0};        // 60000 LE
        const uint8_t evil[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        int patched = 0;
        for (size_t i = 0; i + 8 <= pl; i++)
            if (memcmp(pb + i, old, 8) == 0) { memcpy(pb + i, evil, 8); patched = 1; break; }
        chkb("absurd-out amount patched", patched);
        chkb("absurd-out bytes rejected", wallet_psbt_load(pb, pl, &sum) != 0);
    }
    wallet_psbt_free();

    // ---- receive reuse guard (wallet_usage) ----
    {
        uint8_t fp[4] = {0xEC, 0x5A, 0x45, 0x95};
        wallet_usage_wipe();
        chki("usage fresh -> -1", wallet_usage_high(fp, 0, WSCRIPT_NATIVE), -1);
        wallet_usage_mark(fp, 0, WSCRIPT_NATIVE, 3);
        chki("usage marks 3", wallet_usage_high(fp, 0, WSCRIPT_NATIVE), 3);
        wallet_usage_mark(fp, 0, WSCRIPT_NATIVE, 1);         // lower: ignored
        chki("usage monotonic", wallet_usage_high(fp, 0, WSCRIPT_NATIVE), 3);
        wallet_usage_mark(fp, 0, WSCRIPT_NATIVE, 7);
        chki("usage advances to 7", wallet_usage_high(fp, 0, WSCRIPT_NATIVE), 7);
        chki("usage isolates network", wallet_usage_high(fp, 1, WSCRIPT_NATIVE), -1);
        chki("usage isolates type", wallet_usage_high(fp, 0, WSCRIPT_LEGACY), -1);
        uint8_t fp2[4] = {0x11, 0x22, 0x33, 0x44};
        chki("usage isolates wallet", wallet_usage_high(fp2, 0, WSCRIPT_NATIVE), -1);
        wallet_usage_wipe();
        chki("usage wipe clears", wallet_usage_high(fp, 0, WSCRIPT_NATIVE), -1);
    }

    // an output the device can't render as an address = a destination the user
    // can't verify = refuse to sign (STOP, not caution)
    pl = mk_psbt(MUT_OPRETURN, pb, sizeof pb);
    chki("opreturn load rc", wallet_psbt_load(pb, pl, &sum), 0);
    chki("nonstandard output STOP", sum.status, WPSBT_STOP);
    chkb("nonstandard reason says nonstandard", strstr(sum.reason, "nonstandard") != NULL);
    chkb("nonstandard sign refused", wallet_psbt_sign(sb, sizeof sb, &sw) != 0);
    wallet_psbt_free();

    chkb("garbage refuses to load", wallet_psbt_load((const uint8_t *)"nope", 4, &sum) != 0);
    wallet_psbt_free();

    wallet_session_close();
    if (wallet_session_address(0, 0, addr, sizeof addr) != 0) {
        printf("PASS: closed session refuses to derive\n");
    } else {
        printf("FAIL: session still derives after close\n");
        fails++;
    }
    pl = mk_psbt(MUT_NONE, pb, sizeof pb);
    chkb("closed session refuses PSBT", wallet_psbt_load(pb, pl, &sum) != 0);

    // step 6: QR transport (pure data layer, session not needed)
    fails += test_qr_transport(pb, pl);

    // ---- testnet mode: same seed, coin 1h, tb1, tpub, wrong-network STOP ----
    wallet_set_network(1);
    chki("testnet mode reads back", wallet_testnet(), 1);
    chki("testnet session open", wallet_session_open(""), 0);
    chki("testnet addr rc", wallet_session_address(0, 0, addr, sizeof addr), 0);
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
        chki("testnet descriptor rc", wallet_session_descriptor(desc, sizeof desc), 0);
        chkb("testnet descriptor path 84h/1h/0h", strstr(desc, "/84h/1h/0h]") != NULL);
        chkb("testnet descriptor tpub", strstr(desc, "]tpub") != NULL);
    }

    // a mainnet PSBT must be blocked while in testnet mode, loudly
    pl = mk_psbt(MUT_NONE, pb, sizeof pb);
    chki("mainnet psbt on testnet load rc", wallet_psbt_load(pb, pl, &sum), 0);
    chki("mainnet psbt on testnet STOP", sum.status, WPSBT_STOP);
    chkb("wrong-network reason", strstr(sum.reason, "network") != NULL);
    chkb("summary flags testnet", sum.testnet);
    chkb("wrong-network sign refused", wallet_psbt_sign(sb, sizeof sb, &sw) != 0);
    wallet_psbt_free();

    // a native testnet PSBT verifies READY with tb1 addresses and signs
    pl = mk_psbt(MUT_TESTNET, pb, sizeof pb);
    chki("testnet psbt load rc", wallet_psbt_load(pb, pl, &sum), 0);
    chki("testnet psbt READY", sum.status, WPSBT_READY);
    chkb("testnet out0 addr is tb1", strncmp(sum.outs[0].addr, "tb1", 3) == 0);
    chkb("testnet change re-derived", sum.outs[1].is_change);
    chkb("testnet change addr is tb1", strncmp(sum.outs[1].addr, "tb1", 3) == 0);
    chki("testnet sign rc", wallet_psbt_sign(sb, sizeof sb, &sw), 0);
    wallet_psbt_free();

    // and the testnet PSBT is blocked back on mainnet (no state leaks either way)
    wallet_set_network(0);
    chki("mainnet mode reads back", wallet_testnet(), 0);
    chki("mainnet addr again rc", wallet_session_address(0, 0, addr, sizeof addr), 0);
    chk("m/84h/0h/0h/0/0 again", addr, "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu");
    pl = mk_psbt(MUT_TESTNET, pb, sizeof pb);
    chki("testnet psbt on mainnet load rc", wallet_psbt_load(pb, pl, &sum), 0);
    chki("testnet psbt on mainnet STOP", sum.status, WPSBT_STOP);
    chkb("wrong-network reason (mainnet side)", strstr(sum.reason, "network") != NULL);
    wallet_psbt_free();
    pl = mk_psbt(MUT_NONE, pb, sizeof pb);
    chki("mainnet psbt READY again", wallet_psbt_load(pb, pl, &sum), 0);
    chki("mainnet psbt status again", sum.status, WPSBT_READY);
    wallet_psbt_free();

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
        wallet_set_network(0);
        for (int a = 0; a < 3; a++)          // a = the PSBT's actual input type
          for (int b = 0; b < 3; b++) {      // b = the (possibly different) selected type
            size_t pl2 = mk_typed_psbt(cs[a].script, cs[a].purpose, pb2, sizeof pb2);
            wallet_set_script(cs[b].script);
            snprintf(nm, sizeof nm, "%s psbt signs while %s selected", cs[a].name, cs[b].name);
            chki(nm, wallet_psbt_load(pb2, pl2, &sm), 0);
            chki(nm, sm.status, WPSBT_READY);
            snprintf(nm, sizeof nm, "%s psbt detected purpose %u", cs[a].name, cs[a].purpose);
            chki(nm, sm.purpose, cs[a].purpose);   // UI shows the PSBT's own type
            wallet_psbt_free();
        }
        // network guard still holds: a mainnet-coin PSBT is refused on testnet
        size_t pl3 = mk_typed_psbt(WSCRIPT_NATIVE, 84, pb2, sizeof pb2);  // coin 0h
        wallet_set_network(1);
        chki("mainnet psbt loads on testnet", wallet_psbt_load(pb2, pl3, &sm), 0);
        chki("mainnet psbt STOPs on testnet (wrong network)", sm.status, WPSBT_STOP);
        wallet_psbt_free();
        wallet_set_network(0);

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
            chki("legacy witness-only load rc", wallet_psbt_load(pb2, wl2, &sm), 0);
            chki("legacy witness-only STOP", sm.status, WPSBT_STOP);
            chkb("legacy witness-only reason says previous",
                 strstr(sm.reason, "previous") != NULL);
            wallet_psbt_free();
        }

        // MIXED input types (1 native + 1 legacy) in one PSBT: legit (e.g. a
        // consolidation), must verify READY, report purpose 0 (mixed), and the
        // fee estimate must count each input at its own type's weight
        size_t plm = mk_mixed_psbt(pb2, sizeof pb2);
        chkb("mixed psbt builds", plm > 0);
        chki("mixed psbt load rc", wallet_psbt_load(pb2, plm, &sm), 0);
        chki("mixed psbt READY", sm.status, WPSBT_READY);
        chki("mixed psbt purpose 0 (mixed)", sm.purpose, 0);
        chki("mixed psbt n_in", sm.n_in, 2);
        // legacy sig ≈107 vB full weight + native witness ≈28 vB: the estimate
        // must be well ABOVE an all-native guess and below an all-legacy one
        chkb("mixed fee estimate counts per type",
             sm.est_vsize > 200 && sm.est_vsize < 400);
        wallet_psbt_free();
    }
    // native 0/0 still equals the published BIP84 vector (no leakage)
    wallet_set_script(WSCRIPT_NATIVE);
    chki("native addr after type sweep rc", wallet_session_address(0, 0, addr, sizeof addr), 0);
    chk("m/84h/0h/0h/0/0 after sweep", addr, "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu");

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
