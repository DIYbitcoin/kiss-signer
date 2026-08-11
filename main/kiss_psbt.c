// KISS Signer — step 5: PSBT parse / verify / sign (libwally only, no UI).
// Verify order follows the spec's safety model: anything that could lose coins
// is a STOP (sign refuses); anything odd-but-signable is a CAUTION the user
// must accept on the verify screen. v1 scope: BIP84 P2WPKH, SIGHASH_ALL,
// every input must be ours (single-sig, no multi-party PSBTs).
#include "kiss_psbt.h"

#include <stdio.h>
#include <string.h>

#include <wally_core.h>
#include <wally_bip32.h>
#include <wally_address.h>
#include <wally_crypto.h>
#include <wally_map.h>
#include <wally_psbt.h>
#include <wally_psbt_members.h>
#include <wally_script.h>
#include <wally_transaction.h>

#include "kiss_crypto.h"
#include "kiss_sp.h"

static struct wally_psbt *s_psbt;
static struct wally_tx *s_txv;                 // v2 only: extracted tx view
static wpsbt_status_t s_status = WPSBT_STOP;   // sign gate; STOP until a good load

// Silent payment outputs found by sp_scan (BIP375), consumed by sp_fill.
static struct {
    uint32_t n;
    struct { uint32_t idx; uint8_t scan[33], spend[33]; } o[WPSBT_MAX_OUTS];
} s_sp;

// BIP376 inputs that spend a received silent payment: which inputs carry a
// PSBT_IN_SP_TWEAK (0x20) and its 32-byte tweak. Ownership is re-verified from
// our own spend key at load; the tweaked key signs at sign time.
static struct {
    bool present[WPSBT_MAX_INS];
    uint8_t tweak[WPSBT_MAX_INS][32];
} s_sp_in;

static uint8_t s_psbt_hash[32];   // sha256(psbt bytes): deterministic-sign aux seed

// v0 keeps its embedded global tx; v2 uses the view extracted after sp_fill.
// A v2 psbt is authoritative ONLY through s_txv (built from its own fields, the
// same data libwally signs). A global unsigned tx must never speak for a v2:
// that hybrid is exactly how an attacker would show one tx and sign another, so
// v2 never falls back to s_psbt->tx here (and the load path rejects the hybrid).
static const struct wally_tx *psbt_tx(void)
{
    if (!s_psbt)
        return NULL;
    if (s_psbt->version == 2)
        return s_txv;
    return s_psbt->tx;
}

// Consensus cap (21M BTC in sats). wally 1.5.4 already refuses bigger amounts
// at parse (psbt_from_bytes rc=-2, verified) — this cap is defense-in-depth so
// the sums below can never wrap uint64 even if a future wally lets one through
// (16 ins + 16 outs * MAX_MONEY is still < 2^63).
#define MAX_MONEY 2100000000000000ULL

static void stop(wpsbt_summary_t *s, const char *r)
{
    if (s->status == WPSBT_STOP)
        return;                        // first blocker wins: show the root cause
    s->status = WPSBT_STOP;
    snprintf(s->reason, sizeof s->reason, "%s", r);
}

static void caution(wpsbt_summary_t *s, uint16_t flag, const char *r)
{
    s->caution_flags |= flag;          // accumulate: fee + dust + ... can coexist
    if (s->status == WPSBT_READY) {    // never downgrade a STOP
        s->status = WPSBT_CAUTION;
        snprintf(s->reason, sizeof s->reason, "%s", r);   // first caution seeds the light
    }
}

// Standardness dust floor by output type (sats): an output below this is
// nonstandard and the tx may not relay. Distinct from the privacy threshold.
static uint64_t dust_floor(uint32_t purpose)
{
    return purpose == 44 ? 546 : purpose == 49 ? 540 : 294;   // p2pkh / nested / segwit
}

// Find OUR keypath in a PSBT keypath map (master fingerprint match) and parse
// its derivation path. Returns 1 if found, 0 if the map holds no key of ours.
static int our_keypath(const struct wally_map *m, const uint8_t fp[4],
                       uint32_t *path, size_t *path_len /* in: max, out: got */)
{
    for (size_t i = 0; i < m->num_items; i++) {
        const struct wally_map_item *it = &m->items[i];
        if (it->value_len < 4 || (it->value_len - 4) % 4 != 0)
            continue;
        if (memcmp(it->value, fp, 4) != 0)
            continue;
        size_t depth = (it->value_len - 4) / 4;
        if (depth > *path_len)
            return 0;                              // absurd depth: not ours
        for (size_t d = 0; d < depth; d++) {
            const uint8_t *v = it->value + 4 + d * 4;
            path[d] = (uint32_t)v[0] | ((uint32_t)v[1] << 8) |
                      ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
        }
        *path_len = depth;
        return 1;
    }
    return 0;
}

// The three script types this wallet can sign, by BIP purpose.
static int purpose_supported(uint32_t p) { return p == 44 || p == 49 || p == 84; }

// Path must be m/<purpose>h/<coin>h/0h/{0,1}/i for one of our supported script
// types. Returns the purpose (44/49/84) if it matches at the given coin, else 0.
//
// The signer is TYPE-AGNOSTIC: it accepts ANY of its script types because the
// PSBT's own path declares which one — the user never pre-selects a type to
// sign (the ADDRESS TYPE setting is only for Receive/Export). The NETWORK stays
// pinned though: the coin must match the device's testnet/mainnet setting, and a
// path that would match on the OTHER coin is flagged as a wrong-network STOP.
static uint32_t path_purpose_for_coin(const uint32_t *path, size_t len, uint32_t coin)
{
    if (len != 5 || path[0] < BIP32_INITIAL_HARDENED_CHILD)
        return 0;
    uint32_t purpose = path[0] - BIP32_INITIAL_HARDENED_CHILD;
    if (!purpose_supported(purpose) ||
        path[1] != (BIP32_INITIAL_HARDENED_CHILD + coin) ||
        path[2] != BIP32_INITIAL_HARDENED_CHILD ||
        (path[3] != 0 && path[3] != 1) ||
        // The address index must NOT be hardened. Nothing stopped it before,
        // and the key re-derives perfectly well from the private master here --
        // which is the trap. The descriptor this device exports is an xpub
        // (kiss_crypto.h), and a hardened child cannot be derived from an
        // xpub by anyone, ever. Change sent to m/84h/0h/0h/1/2147483648h is
        // provably ours, invisible to every watch-only wallet the owner has,
        // and unrecoverable from the backup this device tells them to keep.
        // A coordinator has no legitimate reason to ask for one.
        //
        // The index is NOT bounded above beyond that. A gap-limit caution was
        // considered and left out: the row stack is full at five (see
        // SG_ROW_MAX in kiss_sign.c) and a sixth reason does not fit the
        // page, and a large index still re-derives to a key the seed owns.
        // Worth revisiting if a row ever frees up.
        path[4] >= BIP32_INITIAL_HARDENED_CHILD)
        return 0;
    return purpose;
}

// Purpose of a path on THIS network, or 0 if it isn't one of ours here.
static uint32_t our_purpose(const uint32_t *path, size_t len)
{
    return path_purpose_for_coin(path, len, kiss_testnet() ? 1 : 0);
}

// Same path shape but the OTHER network's coin: a fine transaction on the wrong
// network — deserves a clearer STOP than "unsupported path".
static int path_is_other_network(const uint32_t *path, size_t len)
{
    return path_purpose_for_coin(path, len, kiss_testnet() ? 0 : 1) != 0;
}

// Build the expected scriptPubKey for a given BIP purpose from a pubkey.
// 0 on success; *out_len set. out must be >= 25.
static int expected_spk(const uint8_t pub[33], uint32_t purpose,
                        uint8_t *out, size_t cap, size_t *out_len)
{
    size_t w = 0;
    if (purpose == 44) {               // p2pkh: 76 a9 14 <h160> 88 ac
        return wally_scriptpubkey_p2pkh_from_bytes(pub, 33, WALLY_SCRIPT_HASH160,
                                                   out, cap, out_len) == WALLY_OK ? 0 : 1;
    }
    if (purpose == 49) {               // p2sh of the p2wpkh redeem script
        uint8_t redeem[22];
        if (wally_witness_program_from_bytes(pub, 33, WALLY_SCRIPT_HASH160,
                                             redeem, sizeof redeem, &w) != WALLY_OK || w != 22)
            return 1;
        return wally_scriptpubkey_p2sh_from_bytes(redeem, 22, WALLY_SCRIPT_HASH160,
                                                  out, cap, out_len) == WALLY_OK ? 0 : 1;
    }
    // 84 native p2wpkh: 00 14 <h160>
    return wally_witness_program_from_bytes(pub, 33, WALLY_SCRIPT_HASH160,
                                            out, cap, out_len) == WALLY_OK ? 0 : 1;
}

// Re-derive the key at path and compare its expected scriptPubKey with spk.
// 0 = match, nonzero = mismatch/error. This is THE anti-theft check.
static int rederive_matches(const uint32_t *path, size_t path_len,
                            const uint8_t *spk, size_t spk_len)
{
    uint32_t purpose = our_purpose(path, path_len);   // build the spk for the path's OWN type
    if (!purpose)
        return 1;
    const struct ext_key *master = kiss_session_master();
    struct ext_key k;
    uint8_t want[25];
    size_t wl = 0;
    int rc = 1;
    if (master &&
        bip32_key_from_parent_path(master, path, path_len,
                                   BIP32_FLAG_KEY_PRIVATE, &k) == WALLY_OK) {
        if (expected_spk(k.pub_key, purpose, want, sizeof want, &wl) == 0 &&
            wl == spk_len && memcmp(want, spk, wl) == 0)
            rc = 0;
        wally_bzero(&k, sizeof k);
    }
    return rc;
}

static void spk_to_addr(const uint8_t *spk, size_t spk_len, char *out, size_t out_len,
                        wpsbt_summary_t *s)
{
    char *a = NULL;
    const char *hrp = kiss_testnet() ? "tb" : "bc";
    int net = kiss_testnet() ? WALLY_NETWORK_BITCOIN_TESTNET : WALLY_NETWORK_BITCOIN_MAINNET;
    if (wally_addr_segwit_from_bytes(spk, spk_len, hrp, 0, &a) == WALLY_OK ||
        wally_scriptpubkey_to_address(spk, spk_len, net, &a) == WALLY_OK) {
        snprintf(out, out_len, "%s", a);
        wally_free_string(a);
    } else {
        // no address to show = a destination the user cannot verify = refuse.
        // (taproot/segwit/legacy all render above; this is OP_RETURN/garbage)
        snprintf(out, out_len, "(nonstandard script)");
        stop(s, "nonstandard output script");
    }
}

// Remove every unknowns entry whose key starts with `keytype` (and matches
// key_len when nonzero). Returns how many were removed.
static int wipe_unknowns(struct wally_map *m, uint8_t keytype, size_t key_len)
{
    int n = 0;
    for (;;) {
        const struct wally_map_item *hit = NULL;
        for (size_t i = 0; i < m->num_items; i++)
            if (m->items[i].key_len >= 1 && m->items[i].key[0] == keytype &&
                (!key_len || m->items[i].key_len == key_len)) { hit = &m->items[i]; break; }
        if (!hit)
            break;
        if (wally_map_remove(m, hit->key, hit->key_len) != WALLY_OK)
            break;
        n++;
    }
    return n;
}

// BIP375 pass over the unknowns maps: record SP outputs, WIPE any incoming
// ECDH shares/proofs (this signer never endorses foreign crypto - it always
// recomputes), refuse BIP376 receive-side fields, and count what remains as
// truly unknown. Returns 0, or -1 after stop() on a malformed/unsupported SP
// field.
static int sp_scan(wpsbt_summary_t *s)
{
    memset(&s_sp, 0, sizeof s_sp);
    memset(&s_sp_in, 0, sizeof s_sp_in);
    uint32_t unknown = 0;

    // globals: 0x07/0x08 = SP ECDH share/DLEQ (34-byte keys) -> wipe
    wipe_unknowns(&s_psbt->unknowns, 0x07, 34);
    wipe_unknowns(&s_psbt->unknowns, 0x08, 34);
    unknown += (uint32_t)s_psbt->unknowns.num_items;

    for (size_t i = 0; i < s_psbt->num_inputs; i++) {
        struct wally_map *m = &s_psbt->inputs[i].unknowns;
        wipe_unknowns(m, 0x1d, 34);        // send per-input share  -> recompute, never trust
        wipe_unknowns(m, 0x1e, 34);        // send per-input proof  -> recompute, never trust
        for (size_t j = 0; j < m->num_items; j++) {
            const struct wally_map_item *it = &m->items[j];
            uint8_t k0 = it->key_len ? it->key[0] : 0xff;
            // BIP376: spending a RECEIVED silent-payment coin.
            if (k0 == 0x1f) {                 // PSBT_IN_SP_SPEND_BIP32_DERIVATION
                // key = 0x1f || 33-byte spend pubkey; value = fp || LE32 path.
                // We ignore the coordinator's key/path and derive our own spend
                // key, so only the shape is checked here (record via the tweak).
                if (it->key_len != 34 || it->value_len < 4 || (it->value_len - 4) % 4 != 0) {
                    stop(s, "malformed SP spend derivation");
                    return -1;
                }
                continue;
            }
            if (k0 == 0x20) {                 // PSBT_IN_SP_TWEAK (32-byte tweak)
                if (it->key_len != 1 || it->value_len != 32 || i >= WPSBT_MAX_INS) {
                    stop(s, "malformed SP tweak");
                    return -1;
                }
                s_sp_in.present[i] = true;
                memcpy(s_sp_in.tweak[i], it->value, 32);
                continue;
            }
            unknown++;
        }
    }

    for (size_t i = 0; i < s_psbt->num_outputs; i++) {
        const struct wally_map *m = &s_psbt->outputs[i].unknowns;
        bool have_sp_info = false;
        bool have_sp_label = false;
        for (size_t j = 0; j < m->num_items; j++) {
            const struct wally_map_item *it = &m->items[j];
            if (it->key_len == 1 && it->key[0] == 0x09) {
                if (it->value_len != 66 || s_sp.n >= WPSBT_MAX_OUTS ||
                    i >= WPSBT_MAX_OUTS) {
                    stop(s, "malformed SP output info");
                    return -1;
                }
                s_sp.o[s_sp.n].idx = (uint32_t)i;
                memcpy(s_sp.o[s_sp.n].scan, it->value, 33);
                memcpy(s_sp.o[s_sp.n].spend, it->value + 33, 33);
                s_sp.n++;
                have_sp_info = true;
            } else if (it->key_len == 1 && it->key[0] == 0x0a) {
                if (it->value_len != 4) {
                    stop(s, "malformed SP output label");
                    return -1;
                }
                // label: informational for the recipient; nothing to do here
                have_sp_label = true;
            } else {
                unknown++;
            }
        }
        // A BIP375 label only describes the Silent Payment recipient carried
        // by 0x09 on this same output. Alone it is meaningless/malformed, and
        // silently accepting it would let coordinator metadata evade review.
        if (have_sp_label && !have_sp_info) {
            stop(s, "SP label without output info");
            return -1;
        }
    }
    s->n_unknown = unknown;
    return 0;
}

// Collect scan-key group g into recips/idxs, already in BIP375 k order (see
// sp_sort_group). Both the fill and the self-check go through this one helper
// on purpose: if they disagreed about ordering the self-check would happily
// bless outputs a conforming coordinator rejects, which is the opposite of
// what it is for. Returns how many recipients the group holds.
static uint32_t sp_group_collect(int g, const int *group_of,
                                 sp_recip_t *recips, uint32_t *idxs)
{
    uint32_t gn = 0;
    for (uint32_t o = 0; o < s_sp.n; o++)
        if (group_of[o] == g) {
            memcpy(recips[gn].scan, s_sp.o[o].scan, 33);
            memcpy(recips[gn].spend, s_sp.o[o].spend, 33);
            idxs[gn++] = s_sp.o[o].idx;
        }
    sp_sort_group(recips, idxs, gn);
    return gn;
}

// The signer role of BIP375, all inputs ours: derive every input scalar, build
// the global ECDH share + DLEQ proof per scan key, derive and set the P2TR
// output scripts, lock the modifiable flags - then re-verify the whole result
// the way the coordinator will before trusting it. Any failure is a STOP.
static void sp_fill(wpsbt_summary_t *s, const struct ext_key *master,
                    const uint8_t fp[4], const uint8_t psbt_hash[32])
{
    uint8_t privs[WPSBT_MAX_INS][32];
    bool xf[WPSBT_MAX_INS] = { false };
    uint8_t op[WPSBT_MAX_INS][36];
    size_t n_in = s_psbt->num_inputs;
    if (n_in == 0 || n_in > WPSBT_MAX_INS) {
        stop(s, "too many inputs for silent payments");
        return;
    }

    for (size_t i = 0; i < n_in; i++) {
        const struct wally_psbt_input *in = &s_psbt->inputs[i];

        // A BIP376 input -- a coin that arrived at OUR OWN silent-payment
        // address -- has no BIP32 keypath, and never will: its key is
        // spend_priv + tweak, not a bip32 child. Demanding a derivation here
        // rejected the entire transaction as "input is not this wallet's",
        // which is both wrong and the most misleading thing the device could
        // have said: the coin is ours, it just isn't reached by a path.
        //
        // This fires whenever a wallet spends a received silent payment and
        // sends change back to its own silent-payment address, which is the
        // ordinary shape of an SP wallet's transaction, not a corner case:
        // BIP376 on the input, BIP375 on the output, in one PSBT. sp_fill
        // needs every input's private key to build a_sum, so it has to know
        // both ways of getting one.
        //
        // Ownership is still proven, not assumed: sp_spend_signing_key
        // recomputes b_spend + t and MUST match the P2TR key actually being
        // spent, so a foreign or tampered tweak fails here exactly as it does
        // on the signing path.
        if (i < WPSBT_MAX_INS && s_sp_in.present[i]) {
            const struct wally_tx_output *u = in->witness_utxo;
            if (!u || u->script_len != 34 || u->script[0] != 0x51 ||
                u->script[1] != 0x20) {
                stop(s, "silent-payment input must be taproot");
                goto out;
            }
            uint8_t spend_priv[32];
            int owned = sp_spend_privkey(master, kiss_testnet(), spend_priv) == 0 &&
                        sp_spend_signing_key(spend_priv, s_sp_in.tweak[i],
                                             u->script + 2, privs[i]) == 0;
            wally_bzero(spend_priv, sizeof spend_priv);
            if (!owned) {
                stop(s, "silent-payment input is not this wallet's");
                goto out;
            }
            xf[i] = true;              // P2TR: BIP352 counts its even-Y key
            memcpy(op[i], in->txhash, 32);
            op[i][32] = (uint8_t)in->index;
            op[i][33] = (uint8_t)(in->index >> 8);
            op[i][34] = (uint8_t)(in->index >> 16);
            op[i][35] = (uint8_t)(in->index >> 24);
            continue;
        }

        uint32_t path[8];
        size_t path_len = 8;
        if (!our_keypath(&in->keypaths, fp, path, &path_len)) {
            stop(s, "input is not this wallet's");
            goto out;
        }
        if (our_purpose(path, path_len) != 84) {
            // eligibility exists for 44/49 in the BIP, but this signer's SP
            // scope is its native type; other types never co-sign SP sends
            stop(s, path_is_other_network(path, path_len)
                     ? (kiss_testnet() ? "wrong network: mainnet transaction"
                                         : "wrong network: testnet transaction")
                     : "silent payments need native segwit inputs");
            goto out;
        }
        struct ext_key k;
        if (bip32_key_from_parent_path(master, path, path_len,
                                       BIP32_FLAG_KEY_PRIVATE, &k) != WALLY_OK) {
            stop(s, "silent payment key derivation failed");
            goto out;
        }
        memcpy(privs[i], k.priv_key + 1, 32);   // ext_key priv_key[0] is 0x00
        wally_bzero(&k, sizeof k);
        memcpy(op[i], in->txhash, 32);
        op[i][32] = (uint8_t)in->index;
        op[i][33] = (uint8_t)(in->index >> 8);
        op[i][34] = (uint8_t)(in->index >> 16);
        op[i][35] = (uint8_t)(in->index >> 24);
    }

    uint8_t a_sum[32], a_pub[33], ih[32], aux[32];
    if (sp_sum_privkeys(&privs[0][0], xf, n_in, a_sum, a_pub) != 0 ||
        sp_input_hash(&op[0][0], n_in, a_pub, ih) != 0) {
        stop(s, "silent payment derivation failed");
        goto out;
    }
    {   // deterministic DLEQ aux: same psbt + same wallet = same signature bytes
        uint8_t seed[32 + 4 + 32];
        memcpy(seed, master->priv_key + 1, 32);
        memcpy(seed + 32, fp, 4);
        memcpy(seed + 36, psbt_hash, 32);
        wally_sha256(seed, sizeof seed, aux, 32);
        wally_bzero(seed, sizeof seed);
    }

    // group SP outputs by scan key (first appearance order; k = group position)
    int group_of[WPSBT_MAX_OUTS], order[WPSBT_MAX_OUTS], n_groups = 0;
    for (uint32_t o = 0; o < s_sp.n; o++) {
        int g = -1;
        for (int j = 0; j < n_groups; j++)
            if (memcmp(s_sp.o[order[j]].scan, s_sp.o[o].scan, 33) == 0) { g = j; break; }
        if (g < 0) { g = n_groups++; order[g] = (int)o; }
        group_of[o] = g;
    }
    for (int g = 0; g < n_groups; g++) {
        sp_recip_t recips[WPSBT_MAX_OUTS];
        uint32_t idxs[WPSBT_MAX_OUTS];
        uint32_t gn = sp_group_collect(g, group_of, recips, idxs);
        uint8_t share[33], proof[64];
        if (sp_ecdh_share(a_sum, recips[0].scan, share) != 0 ||
            sp_derive_group(share, ih, recips, gn) != 0 ||
            sp_dleq_prove(a_sum, recips[0].scan, aux, NULL, NULL, proof) != 0) {
            stop(s, "silent payment derivation failed");
            goto out;
        }
        for (uint32_t j = 0; j < gn; j++) {
            uint8_t scr[34] = { 0x51, 0x20 };
            memcpy(scr + 2, recips[j].xonly_out, 32);
            if (wally_psbt_set_output_script(s_psbt, idxs[j], scr, 34) != WALLY_OK) {
                stop(s, "silent payment derivation failed");
                goto out;
            }
        }
        uint8_t key[34];
        key[0] = 0x07;
        memcpy(key + 1, recips[0].scan, 33);
        if (wally_map_add(&s_psbt->unknowns, key, 34, share, 33) != WALLY_OK) {
            stop(s, "silent payment derivation failed");
            goto out;
        }
        key[0] = 0x08;
        if (wally_map_add(&s_psbt->unknowns, key, 34, proof, 64) != WALLY_OK) {
            stop(s, "silent payment derivation failed");
            goto out;
        }
    }
    // BIP375: once output scripts are set, nothing may be added or removed
    if (wally_psbt_set_tx_modifiable_flags(s_psbt, 0) != WALLY_OK) {
        stop(s, "silent payment derivation failed");
        goto out;
    }

    // SELF-VERIFY as the coordinator will: take only the psbt's stored share +
    // proof, check the proof against A_sum, re-derive every script, compare.
    for (int g = 0; g < n_groups; g++) {
        sp_recip_t recips[WPSBT_MAX_OUTS];
        uint32_t idxs[WPSBT_MAX_OUTS];
        uint32_t gn = sp_group_collect(g, group_of, recips, idxs);
        uint8_t key[34], share[33], proof[64];
        size_t item = 0, wr = 0;
        key[0] = 0x07;
        memcpy(key + 1, recips[0].scan, 33);
        int ok = wally_map_find(&s_psbt->unknowns, key, 34, &item) == WALLY_OK && item &&
                 s_psbt->unknowns.items[item - 1].value_len == 33;
        if (ok) memcpy(share, s_psbt->unknowns.items[item - 1].value, 33);
        key[0] = 0x08;
        ok = ok && wally_map_find(&s_psbt->unknowns, key, 34, &item) == WALLY_OK && item &&
             s_psbt->unknowns.items[item - 1].value_len == 64;
        if (ok) memcpy(proof, s_psbt->unknowns.items[item - 1].value, 64);
        ok = ok && sp_dleq_verify(a_pub, recips[0].scan, share, proof, NULL, NULL) == 0 &&
             sp_derive_group(share, ih, recips, gn) == 0;
        for (uint32_t j = 0; ok && j < gn; j++) {
            const struct wally_psbt_output *po = &s_psbt->outputs[idxs[j]];
            ok = po->script && po->script_len == 34 &&
                 po->script[0] == 0x51 && po->script[1] == 0x20 &&
                 memcmp(po->script + 2, recips[j].xonly_out, 32) == 0;
        }
        (void)wr;
        if (!ok) {
            stop(s, "silent payment self-check failed");
            goto out;
        }
    }

out:
    wally_bzero(privs, sizeof privs);
    wally_bzero(a_sum, sizeof a_sum);
    wally_bzero(aux, sizeof aux);
}

int kiss_psbt_load(const uint8_t *bytes, size_t len, wpsbt_summary_t *s)
{
    const struct ext_key *master = kiss_session_master();
    if (!s || !master)
        return -1;

    kiss_psbt_free();
    // Cleared BEFORE the parse, not after it. The memset used to sit below four
    // early `return -2` paths, so a rejected PSBT left the PREVIOUS one's
    // amounts and caution flags in the caller's summary. Both call sites happen
    // to bail to an error screen without reading it, which is one careless edit
    // away from drawing the last transaction's numbers under this filename.
    //
    // And it fails CLOSED: WPSBT_READY is 0, so a plain memset would hand a
    // zeroed-but-READY summary to any caller that ignored the return code.
    memset(s, 0, sizeof *s);
    s->status = WPSBT_STOP;

    // Coordinators hand out base64 as often as binary ("cHNidP" = b64("psbt")).
    //
    // STATIC, not automatic. These two are 4096 + 5462 = 9558 bytes in one
    // frame, and this runs on the LVGL task, whose stack is 20480
    // (CONFIG_ESP_MAIN_TASK_STACK_SIZE). Nearly half the stack, in the function
    // that parses a file an attacker hands you on an SD card or over the
    // camera, under whatever LVGL has already pushed to get here -- and the
    // canaries this repo just turned on add to every frame in that chain
    // rather than subtracting from this one.
    //
    // Safe because one task parses: kiss_psbt_load is called from the UI task
    // and there is a single s_psbt behind it, so a second concurrent parse was
    // never possible. Wiped rather than left sitting: a PSBT is not key
    // material, but it is every address and amount the owner is about to sign,
    // and .bss outlives the session that read it. Same reason camera_spike
    // wipes its static decode result.
    static uint8_t b64buf[4096];
    if (len >= 6 && memcmp(bytes, "cHNidP", 6) == 0) {
        static char txt[5462];                     // 4096 bytes of PSBT, padded
        size_t tl = 0;
        for (size_t i = 0; i < len && tl + 1 < sizeof txt; i++)
            if (bytes[i] != '\r' && bytes[i] != '\n' && bytes[i] != ' ')
                txt[tl++] = (char)bytes[i];
        txt[tl] = 0;
        size_t wr = 0;
        int b64rc = wally_base64_to_bytes(txt, 0, b64buf, sizeof b64buf, &wr);
        wally_bzero(txt, sizeof txt);              // done with it either way
        if (b64rc != WALLY_OK || !wr) {
            wally_bzero(b64buf, sizeof b64buf);
            return -2;
        }
        bytes = b64buf;
        len = wr;
    }
    // BIP375 PSBTv2s carry script-less SP outputs, which BIP370's mandatory
    // PSBT_OUT_SCRIPT makes a strict-parse failure in wally 1.5.4 - retry
    // loose, then require exactly that shape below (anything else that needed
    // loose to parse is malformed and stops).
    bool loose = false;
    if (wally_psbt_from_bytes(bytes, len, 0, &s_psbt) != WALLY_OK) {
        kiss_psbt_free();
        if (wally_psbt_from_bytes(bytes, len, WALLY_PSBT_PARSE_FLAG_LOOSE,
                                  &s_psbt) != WALLY_OK) {
            kiss_psbt_free();
            wally_bzero(b64buf, sizeof b64buf);
            return -2;
        }
        loose = true;
    }
    if (!(s_psbt->version == 2 || s_psbt->tx)) {
        kiss_psbt_free();
        return -2;
    }
    // A PSBTv2 must not carry a global unsigned tx (BIP370). One present means a
    // malformed or hostile hybrid: display would read s_psbt->tx while signing
    // builds the tx from the v2 fields. Refuse it outright, loose parse or not.
    if (s_psbt->version == 2 && s_psbt->tx) {
        kiss_psbt_free();
        return -2;
    }
    uint8_t psbt_hash[32];                         // deterministic-DLEQ / -sign seed
    wally_sha256(bytes, len, psbt_hash, 32);
    memcpy(s_psbt_hash, psbt_hash, 32);            // BIP376 sign-time aux uses it too
    // NOW the decode buffer is dead, and not one line sooner: `bytes` still
    // points INTO it for a base64 PSBT, and the hash above is the seed for the
    // deterministic signature. Wiping before this point silently reseeded every
    // signature off 4096 zero bytes -- which is what the golden BIP340 vectors
    // in sim/test_sp.c caught, and the only thing that would have.
    //
    // Every later return is an error path that would otherwise leave a whole
    // PSBT in .bss for the rest of the boot.
    wally_bzero(b64buf, sizeof b64buf);

    s->status = WPSBT_READY;                       // cleared at entry; earned here
    s->testnet = kiss_testnet() != 0;

    uint8_t fp[BIP32_KEY_FINGERPRINT_LEN];
    struct ext_key m = *master;                    // fingerprint API wants non-const
    bip32_key_get_fingerprint(&m, fp, sizeof fp);
    wally_bzero(&m, sizeof m);
    memcpy(s->our_fp, fp, 4);

    // Both sides of the ownership memcmp, captured before any verdict. Costs
    // nothing and turns "not this wallet's" from a dead end into a diff:
    // in0_keypaths == 0 means the coordinator sent no derivation at all, while
    // a non-zero in0_fp that differs from our_fp names the mismatch outright.
    if (s_psbt->num_inputs > 0) {
        const struct wally_map *k0 = &s_psbt->inputs[0].keypaths;
        s->in0_keypaths = (uint32_t)k0->num_items;
        if (k0->num_items > 0 && k0->items[0].value_len >= 4)
            memcpy(s->in0_fp, k0->items[0].value, 4);
    }

    s->n_in = (uint32_t)s_psbt->num_inputs;
    s->n_out = (uint32_t)s_psbt->num_outputs;
    if (s->n_out > WPSBT_MAX_OUTS)
        stop(s, "too many outputs");

    // silent payments: find SP outputs, wipe foreign shares, refuse BIP376
    if (sp_scan(s) == 0 && s_sp.n > 0) {
        if (s_psbt->version != 2)
            stop(s, "SP output needs PSBTv2");
        // sighash gate must hold BEFORE we derive/fill anything
        for (size_t i = 0; i < s_psbt->num_inputs && s->status != WPSBT_STOP; i++) {
            uint32_t sh = s_psbt->inputs[i].sighash;
            if (sh != 0 && sh != WALLY_SIGHASH_ALL)
                stop(s, "sighash is not ALL");
        }
        if (s->status != WPSBT_STOP)
            sp_fill(s, master, fp, psbt_hash);
    }
    if (s->status == WPSBT_STOP) {
        s_status = s->status;
        return 0;
    }
    if (s_psbt->version == 2) {
        // every output must now have a script (SP ones were just derived) and
        // an amount; a loose parse that hid any other gap dies here
        for (size_t j = 0; j < s_psbt->num_outputs; j++) {
            const struct wally_psbt_output *po = &s_psbt->outputs[j];
            if (!po->script || !po->has_amount) {
                stop(s, "malformed transaction (v2 fields)");
                s_status = s->status;
                return 0;
            }
        }
        if (wally_psbt_extract(s_psbt, WALLY_PSBT_EXTRACT_NON_FINAL, &s_txv) != WALLY_OK) {
            stop(s, "malformed transaction (v2 extract)");
            s_status = s->status;
            return 0;
        }
    } else if (loose) {
        // v0 that needed loose parsing has a real defect somewhere
        stop(s, "malformed transaction");
        s_status = s->status;
        return 0;
    }

    const struct wally_tx *tx = psbt_tx();
    s->locktime = tx->locktime;

    if (tx->num_inputs != s_psbt->num_inputs || tx->num_outputs != s_psbt->num_outputs)
        stop(s, "malformed: tx/psbt count mismatch");

    // ---- inputs: verifiable amount + our re-derived script, or no signature ----
    uint32_t n44 = 0, n49 = 0, n84 = 0, ntap = 0;   // inputs per type: fee estimate + UI label
    uint32_t nunproven = 0;   // amount taken from a witness_utxo with no prev tx behind it
    for (size_t i = 0; i < s_psbt->num_inputs && i < tx->num_inputs; i++) {
        const struct wally_psbt_input *in = &s_psbt->inputs[i];
        if (tx->inputs[i].sequence < 0xFFFFFFFE)
            s->rbf = true;

        if (in->sighash != 0 && in->sighash != WALLY_SIGHASH_ALL) {
            stop(s, "sighash is not ALL");
            continue;
        }

        // BIP376: an input spending a RECEIVED silent payment. It carries no
        // BIP32 keypath (the key is spend + tweak, not a bip32 child), so prove
        // ownership by recomputing the tweaked spend key from OUR OWN spend key
        // and matching the P2TR output key being spent (never trust the tweak
        // blindly - a wrong tweak would steer a signature onto a foreign key).
        if (i < WPSBT_MAX_INS && s_sp_in.present[i]) {
            const struct wally_tx_output *u = in->witness_utxo;
            if (!u || u->script_len != 34 || u->script[0] != 0x51 || u->script[1] != 0x20) {
                stop(s, "silent-payment input must be taproot");
                continue;
            }
            uint8_t spend_priv[32], d[32];
            int owned = sp_spend_privkey(master, kiss_testnet(), spend_priv) == 0 &&
                        sp_spend_signing_key(spend_priv, s_sp_in.tweak[i],
                                             u->script + 2, d) == 0;
            wally_bzero(spend_priv, sizeof spend_priv);
            wally_bzero(d, sizeof d);
            if (!owned) {
                stop(s, "silent-payment input is not this wallet's");
                continue;
            }
            if (u->satoshi > MAX_MONEY) {
                stop(s, "input amount over 21M BTC (corrupt)");
                continue;
            }
            s->in_sats += u->satoshi;
            s->n_sp_in++;
            ntap++;
            nunproven++;   // witness_utxo only; BIP341 is what covers it, see below
            if (u->satoshi > 0 && u->satoshi < WPSBT_PRIVACY_SATS)
                caution(s, WPSBT_C_DUST_INPUT, "possible dust attack (privacy)");
            continue;
        }

        uint32_t path[8];
        size_t path_len = 8;
        if (!our_keypath(&in->keypaths, fp, path, &path_len)) {
            stop(s, "input is not this wallet's");
            continue;
        }
        uint32_t purpose = our_purpose(path, path_len);
        if (!purpose) {
            stop(s, path_is_other_network(path, path_len)
                     ? (kiss_testnet() ? "wrong network: mainnet transaction"
                                         : "wrong network: testnet transaction")
                     : "unsupported input derivation path");
            continue;
        }

        // Where the amount + scriptPubKey come from. The full previous
        // transaction is the only source that PROVES either one: it has to hash
        // to the outpoint being spent, so a lie about the amount is a lie about
        // a txid and cannot survive. A witness_utxo is a claim, nothing more.
        //
        // So the prev tx wins whenever it is there, for every script type, and a
        // witness_utxo that contradicts it is the coordinator disagreeing with
        // itself about a coin -- refuse rather than pick a side. The LEGACY
        // sighash does not commit to amounts at all, so purpose 44 has no second
        // source to fall back to and REQUIRES the prev tx. Segwit may fall back
        // (BIP143 commits to the amount of the input being signed) but the coin
        // is then only as honest as one signing session: see nunproven below.
        const uint8_t *utxo_spk = NULL;
        size_t utxo_spk_len = 0;
        uint64_t utxo_val = 0;
        bool proven = false;
        if (in->utxo) {                 // full prev tx: its txid must match
            uint8_t ptxid[32];
            uint32_t vout = tx->inputs[i].index;
            if (wally_tx_get_txid(in->utxo, ptxid, sizeof ptxid) == WALLY_OK &&
                memcmp(ptxid, tx->inputs[i].txhash, 32) == 0 &&
                vout < in->utxo->num_outputs) {
                utxo_spk = in->utxo->outputs[vout].script;
                utxo_spk_len = in->utxo->outputs[vout].script_len;
                utxo_val = in->utxo->outputs[vout].satoshi;
                proven = true;
            } else {
                stop(s, "input's previous transaction does not match");
                continue;
            }
            const struct wally_tx_output *w = in->witness_utxo;
            if (w && (w->satoshi != utxo_val || w->script_len != utxo_spk_len ||
                      memcmp(w->script, utxo_spk, utxo_spk_len) != 0)) {
                stop(s, "input's previous transaction does not match");
                continue;
            }
        } else if (purpose != 44 && in->witness_utxo) {
            utxo_spk = in->witness_utxo->script;
            utxo_spk_len = in->witness_utxo->script_len;
            utxo_val = in->witness_utxo->satoshi;
        }
        if (!utxo_spk) {
            stop(s, purpose == 44
                     ? "legacy input needs its full previous transaction"
                     : "input amount unverifiable");   // fake-fee theft vector
            continue;
        }
        if (!proven) nunproven++;
        if (utxo_val > MAX_MONEY) {
            stop(s, "input amount over 21M BTC (corrupt)");
            continue;
        }
        s->in_sats += utxo_val;
        // spending a tiny KISS-owned coin is the classic dust-attack tell: a
        // stranger sends dust hoping you consolidate it and link your coins
        if (utxo_val > 0 && utxo_val < WPSBT_PRIVACY_SATS)
            caution(s, WPSBT_C_DUST_INPUT, "possible dust attack (privacy)");
        if (purpose == 44) n44++; else if (purpose == 49) n49++; else n84++;
        // re-derive our scriptPubKey for THIS input's own type and require an
        // exact match — the amount above is only trustworthy if this spk is ours
        if (rederive_matches(path, path_len, utxo_spk, utxo_spk_len) != 0)
            stop(s, "input script does not re-derive");
    }
    // detected type for the UI: one uniform purpose, or 0 when inputs mix types
    s->purpose = (n44 && !n49 && !n84) ? 44
               : (n49 && !n44 && !n84) ? 49
               : (n84 && !n44 && !n49) ? 84 : 0;

    s->n_unproven_in = nunproven;
    // The fee on the screen is a subtraction, and every input amount is a term in
    // it. BIP143 commits only to the amount of the input being signed, so with
    // two or more inputs a coordinator can run two signing sessions, declare a
    // different but individually truthful amount in each, and combine one valid
    // signature per input. Both signatures verify. The tx that broadcasts pays a
    // fee neither screen showed, and the difference goes to a miner.
    //
    // Nothing in the PSBT can rule that out: each session, on its own, is honest.
    // BIP341 hashes EVERY input amount into the sighash, so an all-taproot spend
    // is immune, and with one input the lie lands in that input's own sighash and
    // invalidates it. Everything else is the owner's call, which is what a
    // CAUTION is for -- and a coordinator that attaches the previous transactions
    // clears it outright, because then there is nothing left to lie about.
    if (s->n_in >= 2 && ntap < s->n_in && nunproven > 0)
        caution(s, WPSBT_C_UNPROVEN_IN, "input amounts not proven - fee may be higher");

    // Merging coins is the one privacy loss a signer can see coming and the one
    // it can never take back: the moment this broadcasts, every input is public
    // proof the rest belong to the same owner. Counted over ALL our inputs, not
    // the dust ones — WPSBT_C_DUST_INPUT is about who sent the coin, this is
    // about how many are being tied together, and a sweep of perfectly ordinary
    // coins does the same damage. Soft CAUTION: consolidating is often the right
    // call, and the signer has no UTXO set to propose a better selection with.
    if (s->n_in >= WPSBT_MERGE_INS)
        caution(s, WPSBT_C_MERGE_INS, "merging many coins (privacy)");

    // ---- outputs: re-derive change ourselves; never trust "this is change" ----
    // Refuse rather than verify a subset: an output we don't show is an output
    // the user can't approve, and skipping it would corrupt the fee math.
    if (tx->num_outputs > WPSBT_MAX_OUTS)
        stop(s, "too many outputs to verify safely");
    for (size_t j = 0; j < tx->num_outputs && j < WPSBT_MAX_OUTS; j++) {
        const struct wally_tx_output *o = &tx->outputs[j];
        wpsbt_out_t *so = &s->outs[j];
        so->sats = o->satoshi;
        // SP outputs display as their sp1/tsp1 address, not the derived bc1p:
        // the user must approve the DESTINATION they were given, and the
        // self-verified derivation is what guarantees the script honors it
        for (uint32_t si = 0; si < s_sp.n; si++)
            if (s_sp.o[si].idx == (uint32_t)j) {
                so->is_sp = true;
                sp_address_encode(s_sp.o[si].scan, s_sp.o[si].spend,
                                  kiss_testnet() != 0, so->addr, sizeof so->addr);
                s->n_sp++;
            }
        if (!so->is_sp)
            spk_to_addr(o->script, o->script_len, so->addr, sizeof so->addr, s);
        if (o->satoshi > MAX_MONEY) {
            stop(s, "output amount over 21M BTC (corrupt)");
            continue;              // don't let it wrap the sums below
        }
        if (so->is_sp) {           // never change; always part of the send
            s->send_sats += o->satoshi;
            continue;
        }

        uint32_t path[8];
        size_t path_len = 8;
        if (our_keypath(&s_psbt->outputs[j].keypaths, fp, path, &path_len)) {
            if (!our_purpose(path, path_len) ||
                rederive_matches(path, path_len, o->script, o->script_len) != 0) {
                stop(s, "change address does not re-derive");  // active attack marker
            } else {
                so->is_change = true;
                s->change_sats += o->satoshi;
                // a tiny change output fragments your coins (privacy); below the
                // standardness dust floor it is also likely a coordinator slip
                if (o->satoshi > 0 && o->satoshi < dust_floor(our_purpose(path, path_len)))
                    caution(s, WPSBT_C_DUST_CHANGE, "dust change output");
                else if (o->satoshi > 0 && o->satoshi < WPSBT_PRIVACY_SATS)
                    caution(s, WPSBT_C_SMALL_CHANGE, "tiny change (privacy)");
            }
        } else {
            s->send_sats += o->satoshi;
        }
    }

    // ---- fee + estimated rate (P2WPKH witness ≈ 108 WU per input + marker) ----
    if (s->in_sats < s->send_sats + s->change_sats) {
        stop(s, "outputs exceed inputs");
    } else {
        s->fee_sats = s->in_sats - s->send_sats - s->change_sats;
    }
    // signed-size estimate for the fee RATE (informational only). Per input:
    // legacy p2pkh signature (~107 B) is full-weight base data; segwit puts
    // ~108 WU in the witness (¼ weight). Nested adds a small p2sh scriptSig.
    size_t base_vsize = 0;
    wally_tx_get_vsize(tx, &base_vsize);           // unsigned tx: vsize == size
    // per-input-type estimate (handles mixed types): legacy sigs are full-weight
    // base data (~107 vB each); nested adds a small scriptSig (~23 vB) plus its
    // witness; every segwit witness weighs 1/4 (~108 WU each + 2 marker bytes)
    s->est_vsize = (uint32_t)(base_vsize + 107 * n44 + 23 * n49);
    // taproot key-path witness ~= 66 WU (1 item + 1 len + 64-byte schnorr sig)
    if (n49 + n84 + ntap)
        s->est_vsize += (uint32_t)((2 + 108 * (n49 + n84) + 66 * ntap + 3) / 4);
    if (s->est_vsize)
        s->fee_rate_x10 = (uint32_t)(s->fee_sats * 10 / s->est_vsize);

    // Unknown/proprietary fields are a refusal, not a warning: v1 will not sign
    // past data it doesn't understand (spec safety model).
    if (s->n_unknown > 0)
        stop(s, "unknown data in this transaction");
    // high fee = a tenth or more of what the fee is being paid to move, or an
    // outsized rate regardless.
    //
    // The share test used to be skipped entirely when send_sats was zero, which
    // left a pure self-consolidation -- every output change, and the exact shape
    // a fee-inflation attack wants, since nothing appears to leave the wallet --
    // judged on the rate alone. A consolidation still has something the fee is a
    // share OF: the coins being consolidated. Ordinary sends keep the identical
    // test and the identical threshold; only the previously untested case gains
    // one.
    uint64_t fee_base = s->send_sats > 0 ? s->send_sats : s->in_sats;
    if ((fee_base > 0 && s->fee_sats * 10 >= fee_base) ||
        s->fee_rate_x10 > WPSBT_HIGH_RATE_X10)
        caution(s, WPSBT_C_HIGHFEE, "unusually high fee - check it before signing");

    s_status = s->status;
    return 0;
}

// 32-byte hash (internal little-endian) -> the big-endian hex people compare
static void txid_hex(const uint8_t h[32], char out[65])
{
    for (int i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", h[31 - i]);
}

int kiss_psbt_details(wpsbt_details_t *d)
{
    // a STOPped transaction failed verification — its raw fields must not be
    // presented under a page that says "verified" (and there is nothing to
    // decide: the signer already refused)
    if (!d || !s_psbt || !psbt_tx() || s_status == WPSBT_STOP)
        return -1;
    const struct ext_key *master = kiss_session_master();
    if (!master)
        return -1;
    memset(d, 0, sizeof *d);

    const struct wally_tx *tx = psbt_tx();
    d->version = tx->version;
    d->locktime = tx->locktime;

    uint8_t fp[BIP32_KEY_FINGERPRINT_LEN];
    struct ext_key m = *master;
    bip32_key_get_fingerprint(&m, fp, sizeof fp);
    wally_bzero(&m, sizeof m);

    uint8_t h[32];
    if (wally_tx_get_txid((struct wally_tx *)tx, h, sizeof h) == WALLY_OK)
        txid_hex(h, d->txid);

    d->n_total = (uint32_t)s_psbt->num_inputs;
    bool any_legacy = false;
    for (size_t i = 0; i < s_psbt->num_inputs && i < tx->num_inputs; i++) {
        const struct wally_psbt_input *in = &s_psbt->inputs[i];
        if (d->n_in >= WPSBT_MAX_INS)
            break;
        wpsbt_in_t *di = &d->ins[d->n_in++];
        txid_hex(tx->inputs[i].txhash, di->txid);
        di->vout = tx->inputs[i].index;
        bool is_sp = i < WPSBT_MAX_INS && s_sp_in.present[i];
        // Same precedence as load, for the same reason: the prev tx is the only
        // source that proves the number, and load already checked it hashes to
        // this outpoint -- for the inputs it checked. It reaches that check by
        // way of a keypath, so a SILENT-PAYMENT input never gets there: its
        // branch proves ownership from the tweak, takes the amount from the
        // witness_utxo (BIP341 is what covers it) and continues. Reading
        // in->utxo here would therefore read a transaction NOTHING has looked
        // at, and stamp the one word this screen exists to say on it.
        if (!is_sp && in->utxo && di->vout < in->utxo->num_outputs) {
            di->sats = in->utxo->outputs[di->vout].satoshi;
            di->proven = true;
        } else if (in->witness_utxo) {
            di->sats = in->witness_utxo->satoshi;
        }
        if (is_sp) {
            di->is_sp = true;          // BIP376: spends a received silent payment
            di->purpose = 352;         // taproot key-path; signing can't change the txid
        } else {
            uint32_t path[8];
            size_t path_len = 8;
            if (our_keypath(&in->keypaths, fp, path, &path_len) && path_len == 5) {
                di->purpose = our_purpose(path, path_len);
                di->change = path[3];
                di->index = path[4];
            }
        }
        if (di->purpose == 44)
            any_legacy = true;
    }
    // legacy scriptSigs live inside the txid preimage, so signing changes the
    // txid; segwit signatures live in the witness, which the txid ignores
    d->txid_final = !any_legacy;
    return 0;
}

// BIP376: sign every input that spends a received silent payment, using the
// tweaked spend key d = b_spend + tweak. wally can't do this (the key is not a
// bip32 child), so compute the taproot key-path sighash + Schnorr-sign here.
// aux is deterministic per (wallet, psbt) so signing is reproducible. Returns 0,
// or negative on any failure (the whole sign then fails - no partial result).
static int sign_sp_spends(const struct ext_key *master)
{
    bool any = false;
    for (size_t i = 0; i < s_psbt->num_inputs && i < WPSBT_MAX_INS; i++)
        if (s_sp_in.present[i]) { any = true; break; }
    if (!any)
        return 0;

    uint8_t spend_priv[32], aux[32];
    int rc = -1;
    if (sp_spend_privkey(master, kiss_testnet(), spend_priv) != 0)
        return -1;
    {   // aux = sha256(spend_priv || psbt_hash): deterministic + wallet-specific
        uint8_t seed[32 + 32];
        memcpy(seed, spend_priv, 32);
        memcpy(seed + 32, s_psbt_hash, 32);
        wally_sha256(seed, sizeof seed, aux, 32);
        wally_bzero(seed, sizeof seed);
    }
    for (size_t i = 0; i < s_psbt->num_inputs && i < WPSBT_MAX_INS; i++) {
        if (!s_sp_in.present[i])
            continue;
        const struct wally_tx_output *u = s_psbt->inputs[i].witness_utxo;
        uint8_t d[32], sh[32], sig[65];
        // BIP341 signature encoding: 64 bytes means SIGHASH_DEFAULT, full stop.
        // wally computes the sighash over whatever PSBT_IN_SIGHASH_TYPE says,
        // and the load gate lets an explicit SIGHASH_ALL through, so when one
        // is present the hash type MUST be appended -- otherwise a verifier
        // reads the bare 64 bytes as DEFAULT, hashes a different message, and
        // the signature fails. wally appends it on its own bip32 path; this
        // one is hand-rolled and has to do the same.
        size_t siglen = 64;
        uint32_t sh_type = s_psbt->inputs[i].sighash;
        rc = -2;
        if (u && u->script_len == 34 &&
            sp_spend_signing_key(spend_priv, s_sp_in.tweak[i], u->script + 2, d) == 0 &&
            wally_psbt_get_input_signature_hash(s_psbt, i, s_txv, NULL, 0, 0, sh, 32) == WALLY_OK &&
            sp_schnorr_sign(d, sh, aux, sig) == 0) {
            if (sh_type != 0)
                sig[siglen++] = (uint8_t)(sh_type & 0xff);
            if (wally_psbt_input_set_taproot_signature(&s_psbt->inputs[i],
                                                       sig, siglen) == WALLY_OK)
                rc = 0;
        }
        wally_bzero(d, sizeof d);
        wally_bzero(sh, sizeof sh);
        wally_bzero(sig, sizeof sig);
        if (rc != 0)
            break;
    }
    wally_bzero(spend_priv, sizeof spend_priv);
    wally_bzero(aux, sizeof aux);
    return rc;
}

int kiss_psbt_sign(uint8_t *out, size_t out_len, size_t *written)
{
    const struct ext_key *master = kiss_session_master();
    if (!s_psbt || !master || s_status == WPSBT_STOP)
        return -1;
    // The curve code must have reproduced the golden vectors on THIS chip
    // before it is trusted with a real key. Cached after the first run, so the
    // cost is the boot call; a unit that fails signs nothing at all rather
    // than emitting a signature whose nonce nobody has ever checked.
    if (kiss_sign_selftest() != 0)
        return -6;
    if (wally_psbt_sign_bip32(s_psbt, master, EC_FLAG_GRIND_R) != WALLY_OK)
        return -2;
    if (sign_sp_spends(master) != 0)
        return -5;
    size_t need = 0;
    if (wally_psbt_get_length(s_psbt, 0, &need) != WALLY_OK || need > out_len)
        return -3;
    return wally_psbt_to_bytes(s_psbt, 0, out, out_len, written) == WALLY_OK ? 0 : -4;
}

int kiss_psbt_sig_fingerprint(const uint8_t *signed_psbt, size_t len,
                                char out[9])
{
    if (!signed_psbt || !out)
        return -1;
    struct wally_psbt *p = NULL;
    if (wally_psbt_from_bytes(signed_psbt, len, 0, &p) != WALLY_OK || !p)
        return -1;
    // Accumulate the signature bytes in input order, then hash once. Signatures
    // only: the whole point is that a signer producing the same signatures
    // agrees, whatever its PSBT framing.
    uint8_t acc[4096];
    size_t n = 0;
    int any = 0, overflow = 0;
    for (size_t i = 0; i < p->num_inputs && !overflow; i++) {
        const struct wally_map *sigs = &p->inputs[i].signatures;
        for (size_t j = 0; j < sigs->num_items; j++) {
            const struct wally_map_item *it = &sigs->items[j];
            if (n + it->value_len > sizeof acc) { overflow = 1; break; }
            memcpy(acc + n, it->value, it->value_len);
            n += it->value_len; any = 1;
        }
        const struct wally_map_item *tap =
            wally_map_get_integer(&p->inputs[i].psbt_fields, 0x13);
        if (!overflow && tap && (tap->value_len == 64 || tap->value_len == 65)) {
            if (n + tap->value_len > sizeof acc) { overflow = 1; }
            else { memcpy(acc + n, tap->value, tap->value_len); n += tap->value_len; any = 1; }
        }
    }
    wally_psbt_free(p);
    if (overflow || !any) { wally_bzero(acc, sizeof acc); return -1; }
    uint8_t h[32];
    int rc = wally_sha256(acc, n, h, 32);
    wally_bzero(acc, sizeof acc);
    if (rc != WALLY_OK)
        return -1;
    static const char HEX[] = "0123456789abcdef";
    for (int k = 0; k < 4; k++) {
        out[k * 2]     = HEX[h[k] >> 4];
        out[k * 2 + 1] = HEX[h[k] & 0x0f];
    }
    out[8] = 0;
    return 0;
}

void kiss_psbt_free(void)
{
    if (s_psbt) {
        wally_psbt_free(s_psbt);
        s_psbt = NULL;
    }
    if (s_txv) {
        wally_tx_free(s_txv);
        s_txv = NULL;
    }
    memset(&s_sp, 0, sizeof s_sp);
    memset(&s_sp_in, 0, sizeof s_sp_in);
    memset(s_psbt_hash, 0, sizeof s_psbt_hash);
    s_status = WPSBT_STOP;
}
