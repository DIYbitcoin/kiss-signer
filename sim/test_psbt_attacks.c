// Hostile-PSBT cases: what a coordinator can put in a PSBT to make the signer
// report something other than the truth. Two families, each adapted from a
// published signer's attack suite and rebuilt here on this signer's own
// fixtures:
//
//   - extra or contradictory key derivations naming this wallet's fingerprint
//     on an input or an output;
//   - a fingerprint that claims (or disclaims) ownership falsely.
//
// Every fixture is built with libwally directly, independent of the code
// under test, from the BIP84 dev mnemonic the rest of test_crypto.c uses.
// Signatures are checked here by recomputing the sighash and verifying, not by
// asking kiss_psbt whether it signed.
#include <stdio.h>
#include <string.h>

#include "kiss_crypto.h"
#include "kiss_psbt.h"

#include <wally_bip32.h>
#include <wally_bip39.h>
#include <wally_crypto.h>
#include <wally_map.h>
#include <wally_psbt.h>
#include <wally_psbt_members.h>
#include <wally_script.h>
#include <wally_transaction.h>
#include <wally_transaction_members.h>

#define H BIP32_INITIAL_HARDENED_CHILD

static int fails;

static void chkb(const char *name, int ok)
{
    if (ok) printf("PASS: %s\n", name);
    else { printf("FAIL: %s\n", name); fails++; }
}

static struct ext_key a_master;
static uint8_t a_fp[4];

static void derive(const uint32_t *path, size_t n, struct ext_key *o)
{
    bip32_key_from_parent_path(&a_master, path, n, BIP32_FLAG_KEY_PRIVATE, o);
}

static void p2wpkh(const uint8_t pub[33], uint8_t spk[22])
{
    size_t w = 0;
    wally_witness_program_from_bytes(pub, 33, WALLY_SCRIPT_HASH160, spk, 22, &w);
}

// Replace the keypath map of input (out == 0) or output (out == 1) `idx` with
// these entries, in exactly this order. wally keeps insertion order through a
// serialize/parse round trip, and the order is what two of the cases turn on.
static void set_keypaths(struct wally_psbt *p, int out, size_t idx, int n,
                         const uint8_t (*pubs)[33], const uint8_t (*fps)[4],
                         const uint32_t (*paths)[5])
{
    struct wally_map *m = NULL;
    int ok = wally_map_keypath_public_key_init_alloc(2, &m) == WALLY_OK;
    for (int i = 0; ok && i < n; i++)
        ok = wally_map_keypath_add(m, pubs[i], 33, fps[i], 4, paths[i], 5) == WALLY_OK;
    ok = ok && (out ? wally_psbt_set_output_keypaths(p, idx, m)
                    : wally_psbt_set_input_keypaths(p, idx, m)) == WALLY_OK;
    if (!ok)
        printf("FAIL: attack fixture keypaths\n");
    wally_map_free(m);
}

static void kp1(struct wally_psbt *p, int out, size_t idx, const uint8_t pub[33],
                const uint8_t fp[4], const uint32_t path[5])
{
    uint8_t pubs[1][33], fps[1][4];
    uint32_t paths[1][5];
    memcpy(pubs[0], pub, 33);
    memcpy(fps[0], fp, 4);
    memcpy(paths[0], path, sizeof paths[0]);
    set_keypaths(p, out, idx, 1, (const uint8_t (*)[33])pubs,
                 (const uint8_t (*)[4])fps, (const uint32_t (*)[5])paths);
}

// A valid public key nobody in this file holds the derivation for: the point
// for the scalar b..b. A pattern like 02 11..11 is usually not on the curve,
// and libwally refuses it before the signer ever sees it.
static void stranger_pub(uint8_t b, uint8_t out[33])
{
    uint8_t priv[32];
    memset(priv, b, 32);
    if (wally_ec_public_key_from_private_key(priv, 32, out, 33) != WALLY_OK)
        printf("FAIL: stranger key\n");
}

static const uint32_t P_IN[5]  = {H + 84, H, H, 0, 0};
static const uint32_t P_CHG[5] = {H + 84, H, H, 1, 0};

// One P2WPKH input of ours at 0/0 (100000 sats, previous transaction attached
// so every amount is proven), out0 60000 to a stranger, out1 39000 change at
// 1/0. Returned unserialized so each case can bend one thing.
static struct wally_psbt *base_psbt(void)
{
    struct ext_key kin, kchg;
    derive(P_IN, 5, &kin);
    derive(P_CHG, 5, &kchg);
    uint8_t in_spk[22], chg_spk[22], ext_spk[22] = {0x00, 0x14};
    memset(ext_spk + 2, 0x11, 20);
    p2wpkh(kin.pub_key, in_spk);
    p2wpkh(kchg.pub_key, chg_spk);

    struct wally_tx *prev = NULL, *tx = NULL;
    uint8_t dummy[32], txid[32];
    memset(dummy, 0xBB, 32);
    wally_tx_init_alloc(2, 0, 1, 1, &prev);
    wally_tx_add_raw_input(prev, dummy, 32, 0, 0xFFFFFFFF, NULL, 0, NULL, 0);
    wally_tx_add_raw_output(prev, 100000, in_spk, 22, 0);
    wally_tx_get_txid(prev, txid, 32);

    wally_tx_init_alloc(2, 0, 1, 2, &tx);
    wally_tx_add_raw_input(tx, txid, 32, 0, 0xFFFFFFFD, NULL, 0, NULL, 0);
    wally_tx_add_raw_output(tx, 60000, ext_spk, 22, 0);
    wally_tx_add_raw_output(tx, 39000, chg_spk, 22, 0);

    struct wally_psbt *p = NULL;
    wally_psbt_init_alloc(0, 1, 2, 0, 0, &p);
    wally_psbt_set_global_tx(p, tx);
    wally_psbt_set_input_utxo(p, 0, prev);
    struct wally_tx_output *u = NULL;
    wally_tx_output_init_alloc(100000, in_spk, 22, &u);
    wally_psbt_set_input_witness_utxo(p, 0, u);
    wally_tx_output_free(u);
    kp1(p, 0, 0, kin.pub_key, a_fp, P_IN);
    kp1(p, 1, 1, kchg.pub_key, a_fp, P_CHG);

    wally_tx_free(prev);
    wally_tx_free(tx);
    wally_bzero(&kin, sizeof kin);
    wally_bzero(&kchg, sizeof kchg);
    return p;
}

static size_t ser(struct wally_psbt *p, uint8_t *out, size_t cap)
{
    size_t w = 0;
    if (wally_psbt_to_bytes(p, 0, out, cap, &w) != WALLY_OK)
        w = 0;
    wally_psbt_free(p);
    return w;
}


// Does input `i` of a SIGNED psbt carry a signature under `pub` that verifies
// against the sighash a finalizer will check? Recomputed from the signed bytes
// with wally alone.
static int input_sig_verifies(const uint8_t *psbt, size_t len, size_t i,
                              const uint8_t pub[33])
{
    struct wally_psbt *p = NULL;
    struct wally_tx *tx = NULL;
    int ok = 0;
    if (wally_psbt_from_bytes(psbt, len, 0, &p) != WALLY_OK)
        return 0;
    size_t idx = 0;
    if (wally_map_find(&p->inputs[i].signatures, pub, 33, &idx) == WALLY_OK && idx) {
        const struct wally_map_item *it = &p->inputs[i].signatures.items[idx - 1];
        uint8_t code[32], h[32], sig[64];
        size_t cl = 0;
        if (it->value_len >= 9 && it->value[it->value_len - 1] == WALLY_SIGHASH_ALL &&
            wally_psbt_get_input_scriptcode(p, i, p->inputs[i].witness_utxo->script,
                                            p->inputs[i].witness_utxo->script_len,
                                            code, sizeof code, &cl) == WALLY_OK &&
            wally_psbt_extract(p, WALLY_PSBT_EXTRACT_NON_FINAL, &tx) == WALLY_OK &&
            wally_psbt_get_input_signature_hash(p, i, tx, code, cl, 0, h, 32) == WALLY_OK &&
            wally_ec_sig_from_der(it->value, it->value_len - 1, sig, 64) == WALLY_OK &&
            wally_ec_sig_verify(pub, 33, h, 32, EC_FLAG_ECDSA, sig, 64) == WALLY_OK)
            ok = 1;
    }
    wally_tx_free(tx);
    wally_psbt_free(p);
    return ok;
}

static uint8_t pb[4096], sb[4096];

// ---- 1. extra and contradictory derivations --------------------------------

static void test_extra_derivations(void)
{
    struct ext_key kin, kchg, kother, kfar;
    const uint32_t P_OTHER[5] = {H + 84, H, H, 1, 5};
    const uint32_t P_FAKE[5]  = {H + 84, H, H, 0, 99999};
    const uint32_t P_FAR[5]   = {H + 84, H, H, 1, 100000};
    derive(P_IN, 5, &kin);
    derive(P_CHG, 5, &kchg);
    derive(P_OTHER, 5, &kother);
    derive(P_FAR, 5, &kfar);
    uint8_t fake[33];
    stranger_pub(0x11, fake);
    wpsbt_summary_t s;
    size_t pl, sw = 0;
    char nm[96];

    // Input: the real derivation plus a second entry claiming our fingerprint.
    // Both orders, because the verdict must not depend on which one a parser
    // happens to read first. Before the fix, real-first signed and fake-first
    // refused: the same PSBT, two answers.
    for (int order = 0; order < 2; order++) {
        uint8_t pp[2][33];
        const uint8_t fps[2][4] = {{a_fp[0], a_fp[1], a_fp[2], a_fp[3]},
                                   {a_fp[0], a_fp[1], a_fp[2], a_fp[3]}};
        uint32_t paths[2][5];
        memcpy(pp[order], kin.pub_key, 33);
        memcpy(paths[order], P_IN, sizeof P_IN);
        memcpy(pp[!order], fake, 33);
        memcpy(paths[!order], P_FAKE, sizeof P_FAKE);
        struct wally_psbt *p = base_psbt();
        set_keypaths(p, 0, 0, 2, (const uint8_t (*)[33])pp, fps,
                     (const uint32_t (*)[5])paths);
        pl = ser(p, pb, sizeof pb);
        snprintf(nm, sizeof nm, "input + forged twin (%s first): STOP", order ? "forged" : "real");
        chkb(nm, kiss_psbt_load(pb, pl, &s) == 0 && s.status == WPSBT_STOP);
        snprintf(nm, sizeof nm, "input + forged twin (%s first): no signature", order ? "forged" : "real");
        chkb(nm, kiss_psbt_sign(sb, sizeof sb, &sw) != 0);
        kiss_psbt_free();
    }

    // Input: the real derivation plus a GENUINE second key of ours that the
    // script does not spend. Nothing a single-sig input needs.
    for (int order = 0; order < 2; order++) {
        uint8_t pubs[2][33];
        const uint8_t fps[2][4] = {{a_fp[0], a_fp[1], a_fp[2], a_fp[3]},
                                   {a_fp[0], a_fp[1], a_fp[2], a_fp[3]}};
        uint32_t paths[2][5];
        memcpy(pubs[order], kin.pub_key, 33);
        memcpy(paths[order], P_IN, sizeof P_IN);
        memcpy(pubs[!order], kother.pub_key, 33);
        memcpy(paths[!order], P_OTHER, sizeof P_OTHER);
        struct wally_psbt *p = base_psbt();
        set_keypaths(p, 0, 0, 2, (const uint8_t (*)[33])pubs, fps,
                     (const uint32_t (*)[5])paths);
        pl = ser(p, pb, sizeof pb);
        snprintf(nm, sizeof nm, "input + second real key (%s first): STOP", order ? "second" : "spent");
        chkb(nm, kiss_psbt_load(pb, pl, &s) == 0 && s.status == WPSBT_STOP);
        kiss_psbt_free();
    }

    // Change: the real derivation plus a forged entry claiming our
    // fingerprint. Both orders again.
    for (int order = 0; order < 2; order++) {
        uint8_t pubs[2][33];
        const uint8_t fps[2][4] = {{a_fp[0], a_fp[1], a_fp[2], a_fp[3]},
                                   {a_fp[0], a_fp[1], a_fp[2], a_fp[3]}};
        uint32_t paths[2][5];
        memcpy(pubs[order], kchg.pub_key, 33);
        memcpy(paths[order], P_CHG, sizeof P_CHG);
        memcpy(pubs[!order], fake, 33);
        memcpy(paths[!order], P_FAKE, sizeof P_FAKE);
        struct wally_psbt *p = base_psbt();
        set_keypaths(p, 1, 1, 2, (const uint8_t (*)[33])pubs, fps,
                     (const uint32_t (*)[5])paths);
        pl = ser(p, pb, sizeof pb);
        snprintf(nm, sizeof nm, "change + forged twin (%s first): STOP", order ? "forged" : "real");
        chkb(nm, kiss_psbt_load(pb, pl, &s) == 0 && s.status == WPSBT_STOP);
        kiss_psbt_free();
    }

    // A derivation naming our fingerprint with a path deeper than any key
    // this wallet has. On an output it used to fall through to "a send",
    // quietly, while a 5-deep path with the wrong pubkey refused: one lie,
    // two verdicts. It is a claim of ours that cannot be ours.
    {
        uint8_t v[4 + 4 * 12];
        memcpy(v, a_fp, 4);
        for (int d = 0; d < 12; d++) {
            v[4 + 4 * d] = 5; v[5 + 4 * d] = 0; v[6 + 4 * d] = 0; v[7 + 4 * d] = 0;
        }
        struct wally_psbt *p = base_psbt();
        struct wally_map *m = NULL;
        if (wally_map_keypath_public_key_init_alloc(1, &m) != WALLY_OK ||
            wally_map_add(m, kchg.pub_key, 33, v, sizeof v) != WALLY_OK ||
            wally_psbt_set_output_keypaths(p, 1, m) != WALLY_OK)
            printf("FAIL: attack fixture deep keypath\n");
        wally_map_free(m);
        pl = ser(p, pb, sizeof pb);
        chkb("change claims our fp at depth 12: STOP",
             kiss_psbt_load(pb, pl, &s) == 0 && s.status == WPSBT_STOP);
        kiss_psbt_free();
    }

    // Legitimate: an input that also carries a derivation under somebody
    // else's fingerprint (a coordinator's own bookkeeping, a cosigner's key)
    // is ours exactly once and still signs.
    {
        uint8_t pubs[2][33];
        const uint8_t fps[2][4] = {{a_fp[0], a_fp[1], a_fp[2], a_fp[3]},
                                   {0xde, 0xad, 0xbe, 0xef}};
        const uint32_t paths[2][5] = {{H + 84, H, H, 0, 0}, {H + 84, H, H, 0, 7}};
        memcpy(pubs[0], kin.pub_key, 33);
        memcpy(pubs[1], fake, 33);
        struct wally_psbt *p = base_psbt();
        set_keypaths(p, 0, 0, 2, (const uint8_t (*)[33])pubs, fps, paths);
        pl = ser(p, pb, sizeof pb);
        chkb("input + foreign-fp entry: READY",
             kiss_psbt_load(pb, pl, &s) == 0 && s.status == WPSBT_READY);
        chkb("input + foreign-fp entry: signs valid",
             kiss_psbt_sign(sb, sizeof sb, &sw) == 0 &&
             input_sig_verifies(sb, sw, 0, kin.pub_key));
        kiss_psbt_free();
    }

    // Change parked far out, with a forged input derivation riding along to
    // widen whatever range a signer infers from the inputs. This signer
    // infers none -- the gap bar is fixed -- so the far change must still be
    // flagged, never passed off as ordinary change. The forged entry refuses
    // the whole thing now; without it the gap caution is what fires.
    {
        uint8_t far_spk[22];
        p2wpkh(kfar.pub_key, far_spk);
        for (int with_fake = 0; with_fake < 2; with_fake++) {
            struct wally_psbt *p = base_psbt();
            wally_tx_set_output_script(p->tx, 1, far_spk, 22);
            kp1(p, 1, 1, kfar.pub_key, a_fp, P_FAR);
            if (with_fake) {
                uint8_t pubs[2][33];
                const uint8_t fps[2][4] = {{a_fp[0], a_fp[1], a_fp[2], a_fp[3]},
                                           {a_fp[0], a_fp[1], a_fp[2], a_fp[3]}};
                const uint32_t paths[2][5] = {{H + 84, H, H, 0, 0}, {H + 84, H, H, 0, 99999}};
                memcpy(pubs[0], kin.pub_key, 33);
                memcpy(pubs[1], fake, 33);
                set_keypaths(p, 0, 0, 2, (const uint8_t (*)[33])pubs, fps, paths);
            }
            pl = ser(p, pb, sizeof pb);
            int ok = kiss_psbt_load(pb, pl, &s) == 0;
            if (with_fake)
                chkb("far change + forged input entry: STOP", ok && s.status == WPSBT_STOP);
            else
                chkb("far change alone: gap caution, not plain change",
                     ok && s.status == WPSBT_CAUTION && (s.caution_flags & WPSBT_C_GAP_CHANGE));
            kiss_psbt_free();
        }
    }
    wally_bzero(&kin, sizeof kin);
    wally_bzero(&kchg, sizeof kchg);
    wally_bzero(&kother, sizeof kother);
    wally_bzero(&kfar, sizeof kfar);
}

// ---- 2. fingerprints that lie ----------------------------------------------

static void test_false_fingerprints(void)
{
    struct ext_key kchg;
    derive(P_CHG, 5, &kchg);
    uint8_t fake[33];
    stranger_pub(0x44, fake);
    const uint8_t foreign_fp[4] = {0xde, 0xad, 0xbe, 0xef};
    wpsbt_summary_t s;
    size_t pl;

    // An output claims our fingerprint and change path under a pubkey that
    // is not the one that path derives.
    struct wally_psbt *p = base_psbt();
    kp1(p, 1, 1, fake, a_fp, P_CHG);
    pl = ser(p, pb, sizeof pb);
    chkb("output claims our fp, forged key: STOP",
         kiss_psbt_load(pb, pl, &s) == 0 && s.status == WPSBT_STOP);
    kiss_psbt_free();

    // The right path and key, paid to the LEGACY address of that key. The
    // key is ours; the address is not one a BIP84 wallet watches.
    {
        uint8_t pkh[25];
        size_t w = 0;
        wally_scriptpubkey_p2pkh_from_bytes(kchg.pub_key, 33, WALLY_SCRIPT_HASH160, pkh, 25, &w);
        p = base_psbt();
        wally_tx_set_output_script(p->tx, 1, pkh, w);
        pl = ser(p, pb, sizeof pb);
        chkb("change key paid to its 1... address: STOP",
             kiss_psbt_load(pb, pl, &s) == 0 && s.status == WPSBT_STOP);
        kiss_psbt_free();
    }

    // Our real change key, at another account of the same seed. Its own key,
    // but not this wallet's account: never passed off as change.
    {
        struct ext_key k999;
        const uint32_t P999[5] = {H + 84, H, H + 999, 0, 0};
        uint8_t spk[22];
        derive(P999, 5, &k999);
        p2wpkh(k999.pub_key, spk);
        p = base_psbt();
        wally_tx_set_output_script(p->tx, 1, spk, 22);
        kp1(p, 1, 1, k999.pub_key, a_fp, P999);
        pl = ser(p, pb, sizeof pb);
        int ok = kiss_psbt_load(pb, pl, &s) == 0;
        chkb("change at account 999h: not counted as change",
             ok && (s.status == WPSBT_STOP || !s.outs[1].is_change));
        kiss_psbt_free();
        wally_bzero(&k999, sizeof k999);
    }

    // The other direction: an output that pays our real change key while its
    // derivation names somebody else's fingerprint. A coordinator that only
    // had the bare zpub writes exactly this. It is shown as a send -- the
    // address is on the screen and the amount counts as leaving -- which
    // over-reports, never hides. Not refused: that would break the bare-zpub
    // coordinators the fingerprint panel in kiss_psbt.h exists for.
    p = base_psbt();
    kp1(p, 1, 1, kchg.pub_key, foreign_fp, P_CHG);
    pl = ser(p, pb, sizeof pb);
    chkb("our change under a foreign fp: shown as a send",
         kiss_psbt_load(pb, pl, &s) == 0 && s.status != WPSBT_STOP &&
         !s.outs[1].is_change && s.send_sats == 99000);
    kiss_psbt_free();

    wally_bzero(&kchg, sizeof kchg);
}

int test_psbt_attacks(void)
{
    fails = 0;
    printf("---- hostile PSBTs ----\n");
    uint8_t seed[BIP39_SEED_LEN_512];
    size_t sl = 0;
    if (bip39_mnemonic_to_seed("abandon abandon abandon abandon abandon abandon "
                               "abandon abandon abandon abandon abandon about",
                               NULL, seed, sizeof seed, &sl) != WALLY_OK ||
        bip32_key_from_seed(seed, sizeof seed, BIP32_VER_MAIN_PRIVATE, 0, &a_master) != WALLY_OK ||
        bip32_key_get_fingerprint(&a_master, a_fp, 4) != WALLY_OK) {
        printf("FAIL: attack fixture keys\n");
        return 1;
    }
    wally_bzero(seed, sizeof seed);
    kiss_set_network(0);
    if (kiss_session_open(NULL) != 0) {
        printf("FAIL: attack session open\n");
        return 1;
    }
    test_extra_derivations();
    test_false_fingerprints();
    wally_bzero(&a_master, sizeof a_master);
    return fails;
}
