// KISS Wallet — step 5: PSBT parse / verify / sign (libwally only, no UI).
// Verify order follows the spec's safety model: anything that could lose coins
// is a STOP (sign refuses); anything odd-but-signable is a CAUTION the user
// must accept on the verify screen. v1 scope: BIP84 P2WPKH, SIGHASH_ALL,
// every input must be ours (single-sig, no multi-party PSBTs).
#include "wallet_psbt.h"

#include <stdio.h>
#include <string.h>

#include <wally_core.h>
#include <wally_bip32.h>
#include <wally_address.h>
#include <wally_crypto.h>
#include <wally_map.h>
#include <wally_psbt.h>
#include <wally_script.h>
#include <wally_transaction.h>

#include "wallet_crypto.h"

static struct wally_psbt *s_psbt;
static wpsbt_status_t s_status = WPSBT_STOP;   // sign gate; STOP until a good load

static void stop(wpsbt_summary_t *s, const char *r)
{
    if (s->status == WPSBT_STOP)
        return;                        // first blocker wins: show the root cause
    s->status = WPSBT_STOP;
    snprintf(s->reason, sizeof s->reason, "%s", r);
}

static void caution(wpsbt_summary_t *s, const char *r)
{
    if (s->status == WPSBT_READY) {    // never downgrade a STOP
        s->status = WPSBT_CAUTION;
        snprintf(s->reason, sizeof s->reason, "%s", r);
    }
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
        (path[3] != 0 && path[3] != 1))
        return 0;
    return purpose;
}

// Purpose of a path on THIS network, or 0 if it isn't one of ours here.
static uint32_t our_purpose(const uint32_t *path, size_t len)
{
    return path_purpose_for_coin(path, len, wallet_testnet() ? 1 : 0);
}

// Same path shape but the OTHER network's coin: a fine transaction on the wrong
// network — deserves a clearer STOP than "unsupported path".
static int path_is_other_network(const uint32_t *path, size_t len)
{
    return path_purpose_for_coin(path, len, wallet_testnet() ? 0 : 1) != 0;
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
    const struct ext_key *master = wallet_session_master();
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
    const char *hrp = wallet_testnet() ? "tb" : "bc";
    int net = wallet_testnet() ? WALLY_NETWORK_BITCOIN_TESTNET : WALLY_NETWORK_BITCOIN_MAINNET;
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

int wallet_psbt_load(const uint8_t *bytes, size_t len, wpsbt_summary_t *s)
{
    const struct ext_key *master = wallet_session_master();
    if (!s || !master)
        return -1;

    wallet_psbt_free();
    // Coordinators hand out base64 as often as binary ("cHNidP" = b64("psbt")).
    uint8_t b64buf[4096];
    if (len >= 6 && memcmp(bytes, "cHNidP", 6) == 0) {
        char txt[5462];                            // 4096 bytes of PSBT, padded
        size_t tl = 0;
        for (size_t i = 0; i < len && tl + 1 < sizeof txt; i++)
            if (bytes[i] != '\r' && bytes[i] != '\n' && bytes[i] != ' ')
                txt[tl++] = (char)bytes[i];
        txt[tl] = 0;
        size_t wr = 0;
        if (wally_base64_to_bytes(txt, 0, b64buf, sizeof b64buf, &wr) != WALLY_OK || !wr)
            return -2;
        bytes = b64buf;
        len = wr;
    }
    if (wally_psbt_from_bytes(bytes, len, 0, &s_psbt) != WALLY_OK || !s_psbt->tx) {
        wallet_psbt_free();
        return -2;
    }

    memset(s, 0, sizeof *s);
    s->status = WPSBT_READY;
    s->testnet = wallet_testnet() != 0;

    uint8_t fp[BIP32_KEY_FINGERPRINT_LEN];
    struct ext_key m = *master;                    // fingerprint API wants non-const
    bip32_key_get_fingerprint(&m, fp, sizeof fp);
    wally_bzero(&m, sizeof m);

    const struct wally_tx *tx = s_psbt->tx;
    s->n_in = (uint32_t)s_psbt->num_inputs;
    s->n_out = (uint32_t)s_psbt->num_outputs;
    s->locktime = tx->locktime;
    s->n_unknown = (uint32_t)s_psbt->unknowns.num_items;

    if (tx->num_inputs != s_psbt->num_inputs || tx->num_outputs != s_psbt->num_outputs)
        stop(s, "malformed: tx/psbt count mismatch");
    if (s->n_out > WPSBT_MAX_OUTS)
        stop(s, "too many outputs");

    // ---- inputs: verifiable amount + our re-derived script, or no signature ----
    uint32_t n44 = 0, n49 = 0, n84 = 0;   // inputs per type: fee estimate + UI label
    for (size_t i = 0; i < s_psbt->num_inputs && i < tx->num_inputs; i++) {
        const struct wally_psbt_input *in = &s_psbt->inputs[i];
        s->n_unknown += (uint32_t)in->unknowns.num_items;
        if (tx->inputs[i].sequence < 0xFFFFFFFE)
            s->rbf = true;

        if (in->sighash != 0 && in->sighash != WALLY_SIGHASH_ALL) {
            stop(s, "sighash is not ALL");
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
                     ? (wallet_testnet() ? "wrong network: mainnet transaction"
                                         : "wrong network: testnet transaction")
                     : "unsupported input derivation path");
            continue;
        }

        // Where the amount + scriptPubKey come from depends on the type. The
        // LEGACY sighash does not commit to amounts, so a witness_utxo on a
        // legacy input is exactly the fake-fee lie the full-previous-tx rule
        // exists to block: purpose 44 REQUIRES non_witness_utxo, txid-checked.
        // Segwit sighash (BIP143) commits to the amount, so witness_utxo is
        // safe there (a lied-about amount just makes the signature invalid).
        const uint8_t *utxo_spk = NULL;
        size_t utxo_spk_len = 0;
        uint64_t utxo_val = 0;
        if (purpose != 44 && in->witness_utxo) {
            utxo_spk = in->witness_utxo->script;
            utxo_spk_len = in->witness_utxo->script_len;
            utxo_val = in->witness_utxo->satoshi;
        } else if (in->utxo) {          // full prev tx: its txid must match
            uint8_t ptxid[32];
            uint32_t vout = tx->inputs[i].index;
            if (wally_tx_get_txid(in->utxo, ptxid, sizeof ptxid) == WALLY_OK &&
                memcmp(ptxid, tx->inputs[i].txhash, 32) == 0 &&
                vout < in->utxo->num_outputs) {
                utxo_spk = in->utxo->outputs[vout].script;
                utxo_spk_len = in->utxo->outputs[vout].script_len;
                utxo_val = in->utxo->outputs[vout].satoshi;
            } else {
                stop(s, "input's previous transaction does not match");
                continue;
            }
        }
        if (!utxo_spk) {
            stop(s, purpose == 44
                     ? "legacy input needs its full previous transaction"
                     : "input amount unverifiable");   // fake-fee theft vector
            continue;
        }
        s->in_sats += utxo_val;
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

    // ---- outputs: re-derive change ourselves; never trust "this is change" ----
    // Refuse rather than verify a subset: an output we don't show is an output
    // the user can't approve, and skipping it would corrupt the fee math.
    if (tx->num_outputs > WPSBT_MAX_OUTS)
        stop(s, "too many outputs to verify safely");
    for (size_t j = 0; j < tx->num_outputs && j < WPSBT_MAX_OUTS; j++) {
        const struct wally_tx_output *o = &tx->outputs[j];
        wpsbt_out_t *so = &s->outs[j];
        so->sats = o->satoshi;
        spk_to_addr(o->script, o->script_len, so->addr, sizeof so->addr, s);
        s->n_unknown += (uint32_t)s_psbt->outputs[j].unknowns.num_items;

        uint32_t path[8];
        size_t path_len = 8;
        if (our_keypath(&s_psbt->outputs[j].keypaths, fp, path, &path_len)) {
            if (!our_purpose(path, path_len) ||
                rederive_matches(path, path_len, o->script, o->script_len) != 0) {
                stop(s, "change address does not re-derive");  // active attack marker
            } else {
                so->is_change = true;
                s->change_sats += o->satoshi;
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
    if (n49 + n84)
        s->est_vsize += (uint32_t)((2 + 108 * (n49 + n84) + 3) / 4);
    if (s->est_vsize)
        s->fee_rate_x10 = (uint32_t)(s->fee_sats * 10 / s->est_vsize);

    // Unknown/proprietary fields are a refusal, not a warning: v1 will not sign
    // past data it doesn't understand (spec safety model).
    if (s->n_unknown > 0)
        stop(s, "unknown data in this transaction");
    if (s->status == WPSBT_READY &&
        (s->fee_sats * 10 >= s->send_sats || s->fee_rate_x10 > 5000))
        caution(s, "high fee");

    s_status = s->status;
    return 0;
}

// 32-byte hash (internal little-endian) -> the big-endian hex people compare
static void txid_hex(const uint8_t h[32], char out[65])
{
    for (int i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", h[31 - i]);
}

int wallet_psbt_details(wpsbt_details_t *d)
{
    // a STOPped transaction failed verification — its raw fields must not be
    // presented under a page that says "verified" (and there is nothing to
    // decide: the signer already refused)
    if (!d || !s_psbt || !s_psbt->tx || s_status == WPSBT_STOP)
        return -1;
    const struct ext_key *master = wallet_session_master();
    if (!master)
        return -1;
    memset(d, 0, sizeof *d);

    const struct wally_tx *tx = s_psbt->tx;
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
        if (in->witness_utxo) {
            di->sats = in->witness_utxo->satoshi;
        } else if (in->utxo && di->vout < in->utxo->num_outputs) {
            di->sats = in->utxo->outputs[di->vout].satoshi;
        }
        uint32_t path[8];
        size_t path_len = 8;
        if (our_keypath(&in->keypaths, fp, path, &path_len) && path_len == 5) {
            di->purpose = our_purpose(path, path_len);
            di->change = path[3];
            di->index = path[4];
        }
        if (di->purpose == 44)
            any_legacy = true;
    }
    // legacy scriptSigs live inside the txid preimage, so signing changes the
    // txid; segwit signatures live in the witness, which the txid ignores
    d->txid_final = !any_legacy;
    return 0;
}

int wallet_psbt_sign(uint8_t *out, size_t out_len, size_t *written)
{
    const struct ext_key *master = wallet_session_master();
    if (!s_psbt || !master || s_status == WPSBT_STOP)
        return -1;
    if (wally_psbt_sign_bip32(s_psbt, master, EC_FLAG_GRIND_R) != WALLY_OK)
        return -2;
    size_t need = 0;
    if (wally_psbt_get_length(s_psbt, 0, &need) != WALLY_OK || need > out_len)
        return -3;
    return wally_psbt_to_bytes(s_psbt, 0, out, out_len, written) == WALLY_OK ? 0 : -4;
}

void wallet_psbt_free(void)
{
    if (s_psbt) {
        wally_psbt_free(s_psbt);
        s_psbt = NULL;
    }
    s_status = WPSBT_STOP;
}
