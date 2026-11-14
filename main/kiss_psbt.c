// KISS Signer — step 5: PSBT parse / verify / sign (libwally only, no UI).
// Verify order follows the spec's safety model: anything that could lose coins
// is a STOP (sign refuses); anything odd-but-signable is a CAUTION the user
// must accept on the verify screen. v1 scope: BIP84 P2WPKH, SIGHASH_ALL,
// every input must be ours (single-sig, no multi-party PSBTs). One exception:
// a silent payment paid together with another signer, where the inputs that
// are not ours are counted and never signed. See sp_shared.
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

#include "sha256/sha256.h"   // streaming sha256, bundled with components/cUR
#include "kiss_crypto.h"
#include "kiss_cardwallet.h"
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

// A shared send: a silent payment this signer pays together with another one,
// each adding a BIP375 share for its own inputs. Set at load by sp_sort_inputs
// and sp_shared, read by everything after them.
//   on          this load is one;
//   share_only  and a share of another signer is still missing, so the silent
//               payment output has no script and nothing can be signed;
//   foreign[i]  input i is another signer's: counted, never signed;
//   locktime    share only: the lock time an extract would have computed.
// All zero for every other PSBT, which is what leaves their paths as they were.
static struct {
    bool on, share_only;
    bool foreign[WPSBT_MAX_INS];
    uint32_t locktime;
} s_sh;


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

// ---- the transaction, as the review reads it --------------------------------
// Every path that can sign reads the transaction itself: a v0's own, or the
// view a v2 extract built. A share only pass has neither. Its silent payment
// output has no script, so there is no transaction to extract, which is also
// what makes a signature impossible there and not merely unasked for. The
// review then reads the same facts from the PSBTv2 fields a later extract will
// build that transaction from. tx is NULL on that pass and on no other.
static size_t rv_n_in(const struct wally_tx *tx)
{
    return tx ? tx->num_inputs : s_psbt->num_inputs;
}

static size_t rv_n_out(const struct wally_tx *tx)
{
    return tx ? tx->num_outputs : s_psbt->num_outputs;
}

static uint32_t rv_version(const struct wally_tx *tx)
{
    return tx ? tx->version : s_psbt->tx_version;
}

static uint32_t rv_locktime(const struct wally_tx *tx)
{
    return tx ? tx->locktime : s_sh.locktime;
}

static uint32_t rv_sequence(const struct wally_tx *tx, size_t i)
{
    return tx ? tx->inputs[i].sequence : s_psbt->inputs[i].sequence;
}

static const uint8_t *rv_txhash(const struct wally_tx *tx, size_t i)
{
    return tx ? tx->inputs[i].txhash : s_psbt->inputs[i].txhash;
}

static uint32_t rv_vout(const struct wally_tx *tx, size_t i)
{
    return tx ? tx->inputs[i].index : s_psbt->inputs[i].index;
}

static uint64_t rv_out_sats(const struct wally_tx *tx, size_t j)
{
    return tx ? tx->outputs[j].satoshi : s_psbt->outputs[j].amount;
}

static const uint8_t *rv_out_script(const struct wally_tx *tx, size_t j, size_t *len)
{
    *len = tx ? tx->outputs[j].script_len : s_psbt->outputs[j].script_len;
    return tx ? tx->outputs[j].script : s_psbt->outputs[j].script;
}

// Consensus cap (21M BTC in sats). wally 1.6.0 already refuses bigger amounts
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
    // Bitcoin Core's GetDustThreshold at the default 3000 sat/kvB relay fee.
    // Taproot is its own number: a p2tr output is larger than a p2wpkh one and
    // its spending input smaller, and the two do not cancel. It used to fall
    // through to the segwit 294, so a 300 sat change output read as ordinary
    // on the one script type where it cannot be relayed.
    return purpose == 44 ? 546 : purpose == 49 ? 540 : purpose == 86 ? 330 : 294;
}

// Find OUR keypath in a PSBT keypath map (master fingerprint match) and parse
// its derivation path. If item_out is non-NULL it receives the matching entry,
// whose map key is the public key the coordinator claims that path derives.
// Returns 1 if exactly one entry is ours, 0 if none is, and -1 if the map
// says something about this wallet that cannot be checked as one claim.
//
// -1 covers two shapes. Two or more entries naming our fingerprint: a
// single-sig input spends one key and a single-sig output pays one, so a
// second entry is either a forgery or a key the script does not use. This
// used to return the FIRST match, which made the verdict depend on entry
// order -- the real key first signed, the same forgery first refused. And an
// entry naming our fingerprint at a depth no key of ours has: that used to
// read as "not ours", so on an output a claim of ownership that cannot be
// true was shown as an ordinary send, while the same claim at a plausible
// depth with the wrong pubkey refused. Both are one claim the device cannot
// verify, and every other such claim in this file is a STOP.
static int our_keypath(const struct wally_map *m, const uint8_t fp[4],
                       uint32_t *path, size_t *path_len /* in: max, out: got */,
                       const struct wally_map_item **item_out)
{
    if (item_out)
        *item_out = NULL;
    const struct wally_map_item *hit = NULL;
    for (size_t i = 0; i < m->num_items; i++) {
        const struct wally_map_item *it = &m->items[i];
        if (it->value_len < 4 || (it->value_len - 4) % 4 != 0)
            continue;
        if (memcmp(it->value, fp, 4) != 0)
            continue;
        if (hit || (it->value_len - 4) / 4 > *path_len)
            return -1;
        hit = it;
    }
    if (!hit)
        return 0;
    size_t depth = (hit->value_len - 4) / 4;
    for (size_t d = 0; d < depth; d++) {
        const uint8_t *v = hit->value + 4 + d * 4;
        path[d] = (uint32_t)v[0] | ((uint32_t)v[1] << 8) |
                  ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
    }
    *path_len = depth;
    if (item_out)
        *item_out = hit;
    return 1;
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
        // The index is NOT bounded above HERE, and deliberately so: a path is
        // ours or it is not, and m/../1/99999 re-derives to a key this seed
        // owns however far out it sits. Refusing it in this function would
        // refuse the INPUT too, and an old coin at a high index is a coin.
        //
        // What a high index does cost is that no coordinator scans far enough
        // to find change left there. That is a property of the OUTPUT and it
        // is raised as WPSBT_C_GAP_CHANGE where the output is checked, so the
        // owner is told and can still sign. This paragraph used to say the
        // caution was left out because the row stack was full at five; the
        // rows have their own page now and a fifth one fits.
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

// How many distinct input addresses are tracked exactly. Beyond it the caller
// falls back to the input count, which is the most it could be.
#define WPSBT_ADDR_TRACK 40

// FNV-1a over a scriptPubKey, for "have I seen this address already". Not a
// security hash and it is not asked to be one: a collision would merge two
// addresses into one and UNDERSTATE a consolidation by one, which is the same
// answer the old count-the-coins version gave for every reused address. At 64
// bits it will not happen on forty scripts.
static uint64_t spk_key(const uint8_t *spk, size_t len)
{
    uint64_t h = 1469598103934665603ULL;              // FNV offset basis
    for (size_t i = 0; i < len; i++) {
        h ^= spk[i];
        h *= 1099511628211ULL;                        // FNV prime
    }
    return h;
}

// Re-derive the key at path and compare both claims the PSBT makes about it:
// the keypath map's public-key key (when keypath is non-NULL), and the UTXO's
// scriptPubKey. A correct fingerprint/path paired with somebody else's pubkey
// otherwise passes review, but libwally silently skips it at sign time and
// returns success with no signature.
// 0 = match, 1 = script/path mismatch/error, 2 = keypath pubkey mismatch.
// The public key at one of our paths, from whichever holds the wallet: the
// master in RAM, or the account key a Satochip card handed out at unlock (the
// path is m/purpose'/coin'/0'/change/index, so the last two steps are public
// derivations anyone holding the account key can make). 0 on success.
static int path_pubkey(const uint32_t *path, size_t path_len, uint8_t pub[33])
{
    const struct ext_key *master = kiss_session_master();
    struct ext_key k;
    int rc = 1;
    if (master) {
        if (bip32_key_from_parent_path(master, path, path_len,
                                       BIP32_FLAG_KEY_PRIVATE, &k) == WALLY_OK) {
            memcpy(pub, k.pub_key, 33);
            rc = 0;
        }
    } else if (kiss_session_card() && path_len == 5) {
        const struct ext_key *acct =
            kiss_cardwallet_account(path[0] - BIP32_INITIAL_HARDENED_CHILD,
                                    path[1] == BIP32_INITIAL_HARDENED_CHILD + 1);
        if (acct && bip32_key_from_parent_path(acct, path + 3, 2,
                                               BIP32_FLAG_KEY_PUBLIC, &k) == WALLY_OK) {
            memcpy(pub, k.pub_key, 33);
            rc = 0;
        }
    }
    wally_bzero(&k, sizeof k);
    return rc;
}

static int rederive_matches(const uint32_t *path, size_t path_len,
                            const uint8_t *spk, size_t spk_len,
                            const struct wally_map_item *keypath)
{
    uint32_t purpose = our_purpose(path, path_len);   // build the spk for the path's OWN type
    if (!purpose)
        return 1;
    uint8_t pub[33], want[25];
    size_t wl = 0;
    int rc = 1;
    if (path_pubkey(path, path_len, pub) == 0) {
        if (keypath && (keypath->key_len != sizeof pub ||
                        memcmp(keypath->key, pub, sizeof pub) != 0))
            rc = 2;
        else if (expected_spk(pub, purpose, want, sizeof want, &wl) == 0 &&
            wl == spk_len && memcmp(want, spk, wl) == 0)
            rc = 0;
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

// A BIP375 share or its proof, by where it lives: 0x07/0x08 on the
// transaction, 0x1d/0x1e on an input, each keyed by the 33 byte scan key.
static bool is_share_field(const struct wally_map_item *it, uint8_t share,
                           uint8_t proof)
{
    return it->key_len == 34 && (it->key[0] == share || it->key[0] == proof);
}

// Every share and proof that arrived, gone: this signer never endorses foreign
// crypto, it recomputes. This is the wipe sp_scan used to make as it went. It
// moved here, unchanged, because one kind of PSBT now has to be recognised
// BEFORE anything is wiped: a shared send keeps the other signers' shares on
// their own inputs, as the only way a share this signer cannot compute can
// reach it, and checks each one against its proof instead (sp_shared). Every
// other PSBT comes through here before a line of it is trusted, as before.
static void sp_wipe_shares(void)
{
    wipe_unknowns(&s_psbt->unknowns, 0x07, 34);
    wipe_unknowns(&s_psbt->unknowns, 0x08, 34);
    for (size_t i = 0; i < s_psbt->num_inputs; i++) {
        struct wally_map *m = &s_psbt->inputs[i].unknowns;
        wipe_unknowns(m, 0x1d, 34);        // send per-input share  -> recompute, never trust
        wipe_unknowns(m, 0x1e, 34);        // send per-input proof  -> recompute, never trust
    }
}

// BIP375 pass over the unknowns maps: record SP outputs, refuse BIP376
// receive-side fields, and count what remains as truly unknown. Incoming ECDH
// shares and proofs are not counted: they are wiped (sp_wipe_shares) or, on a
// shared send, judged one by one (sp_shared), and either way they are fields
// this file knows. Returns 0, or -1 after stop() on a malformed/unsupported SP
// field.
static int sp_scan(wpsbt_summary_t *s)
{
    memset(&s_sp, 0, sizeof s_sp);
    memset(&s_sp_in, 0, sizeof s_sp_in);
    uint32_t unknown = 0;

    // globals: 0x07/0x08 = SP ECDH share/DLEQ (34-byte keys)
    for (size_t j = 0; j < s_psbt->unknowns.num_items; j++)
        if (!is_share_field(&s_psbt->unknowns.items[j], 0x07, 0x08))
            unknown++;

    for (size_t i = 0; i < s_psbt->num_inputs; i++) {
        const struct wally_map *m = &s_psbt->inputs[i].unknowns;
        for (size_t j = 0; j < m->num_items; j++) {
            const struct wally_map_item *it = &m->items[j];
            if (is_share_field(it, 0x1d, 0x1e))   // send per-input share + proof
                continue;
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
        int kp = our_keypath(&in->keypaths, fp, path, &path_len, NULL);
        if (kp != 1) {
            stop(s, kp < 0 ? "input key derivations conflict"
                           : "input is not this wallet's");
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

// ---- a silent payment paid together with another signer --------------------
//
// One signer holding every input key makes one share for the whole
// transaction, which is sp_fill above. Two signers cannot: neither holds the
// other's keys, so neither has an a_sum to multiply. BIP375 has the answer,
// and nothing here is a format of our own: every input carries its own share
// C_i = a_i*B_scan and a DLEQ proof that it was made with the key that input
// spends. The shares add up to a_sum*B_scan, so the output script comes out of
// public data and no key leaves anybody's signer.
//
// What that asks of this signer, and nothing more:
//   - its own inputs get a share and a proof, computed here. Whatever arrived
//     for them is wiped first, as it always was;
//   - another signer's input is never signed, and its key is never taken on
//     the coordinator's word: the public key is read off the input itself
//     (sp_input_pubkey) and a share counts only with a proof that verifies
//     against that key;
//   - while a share is missing there is no script, so there is no transaction
//     and nothing to sign. That pass only releases this signer's shares
//     (share only) and the payment comes back once more to be signed.
//
// All of it runs only for a PSBT with a silent payment output, at least one
// input of ours and at least one that is not. Every other PSBT takes the path
// it always took, wipes included.

// Where an input's amount and scriptPubKey come from, by the rule the input
// loop below states in full: a previous transaction that hashes to the
// outpoint proves both and wins, a witness_utxo beside it has to say the same,
// and a witness_utxo alone is a claim. Returns 0 with the outputs set (*proven
// says which of the two it was), 1 when the input carries neither, and 2 when
// the previous transaction contradicts the outpoint or the witness_utxo.
static int input_prevout(const struct wally_psbt_input *in,
                         const uint8_t txhash[32], uint32_t vout,
                         const uint8_t **spk, size_t *spk_len,
                         uint64_t *sats, bool *proven)
{
    *spk = NULL;
    *spk_len = 0;
    *sats = 0;
    *proven = false;
    const struct wally_tx_output *w = in->witness_utxo;
    if (in->utxo) {
        uint8_t ptxid[32];
        if (wally_tx_get_txid(in->utxo, ptxid, sizeof ptxid) != WALLY_OK ||
            memcmp(ptxid, txhash, 32) != 0 || vout >= in->utxo->num_outputs)
            return 2;
        const struct wally_tx_output *o = &in->utxo->outputs[vout];
        if (w && (w->satoshi != o->satoshi || w->script_len != o->script_len ||
                  (o->script_len &&
                   memcmp(w->script, o->script, o->script_len) != 0)))
            return 2;
        *spk = o->script;
        *spk_len = o->script_len;
        *sats = o->satoshi;
        *proven = true;
        return 0;
    }
    if (!w)
        return 1;
    *spk = w->script;
    *spk_len = w->script_len;
    *sats = w->satoshi;
    return 0;
}

// The public key BIP352 counts for an input, read the way a party WITHOUT its
// private key has to read it: for P2WPKH the key among the input's derivations
// whose HASH160 is the witness program, for P2TR the even Y point of the
// x-only output key. Nothing else is eligible in a shared send. Returns 0 with
// the key, 1 for any other script type, and 2 for a P2WPKH input that names no
// key hashing to its program, which leaves nothing to check a share against.
static int sp_input_pubkey(const struct wally_psbt_input *in,
                           const uint8_t *spk, size_t spk_len, uint8_t pub[33])
{
    if (spk && spk_len == 34 && spk[0] == 0x51 && spk[1] == 0x20) {
        pub[0] = 0x02;
        memcpy(pub + 1, spk + 2, 32);
        return 0;
    }
    if (!spk || spk_len != 22 || spk[0] != 0x00 || spk[1] != 0x14)
        return 1;
    for (size_t j = 0; j < in->keypaths.num_items; j++) {
        const struct wally_map_item *it = &in->keypaths.items[j];
        uint8_t h[HASH160_LEN];
        if (it->key_len == EC_PUBLIC_KEY_LEN &&
            wally_hash160(it->key, it->key_len, h, sizeof h) == WALLY_OK &&
            memcmp(h, spk + 2, sizeof h) == 0) {
            memcpy(pub, it->key, 33);
            return 0;
        }
    }
    return 2;
}

// Input i's share and proof for one scan key, as the PSBT holds them. Returns
// how many of the two are there (0, 1 or 2), or -1 when one is there in a
// length BIP375 does not allow. A share and its proof come together or not at
// all, so 1 is a refusal and the caller does not need to know which half.
static int sp_in_share(size_t i, const uint8_t scan[33], uint8_t share[33],
                       uint8_t proof[64])
{
    const struct wally_map *m = &s_psbt->inputs[i].unknowns;
    uint8_t key[34];
    size_t item = 0;
    int n = 0;
    memcpy(key + 1, scan, 33);
    key[0] = 0x1d;
    if (wally_map_find(m, key, sizeof key, &item) == WALLY_OK && item) {
        if (m->items[item - 1].value_len != 33)
            return -1;
        memcpy(share, m->items[item - 1].value, 33);
        n++;
    }
    key[0] = 0x1e;
    item = 0;
    if (wally_map_find(m, key, sizeof key, &item) == WALLY_OK && item) {
        if (m->items[item - 1].value_len != 64)
            return -1;
        memcpy(proof, m->items[item - 1].value, 64);
        n++;
    }
    return n;
}

// The scan key groups, in first appearance order: the same grouping sp_fill
// makes for itself. order[g] is the first silent payment output of group g.
static int sp_groups(int *group_of, int *order)
{
    int n_groups = 0;
    for (uint32_t o = 0; o < s_sp.n; o++) {
        int g = -1;
        for (int j = 0; j < n_groups; j++)
            if (memcmp(s_sp.o[order[j]].scan, s_sp.o[o].scan, 33) == 0) { g = j; break; }
        if (g < 0) { g = n_groups++; order[g] = (int)o; }
        group_of[o] = g;
    }
    return n_groups;
}

// Whose input is this? OWN when it makes the claim the single signer path
// accepts: a derivation under our fingerprint on the silent payment branch
// this signer pays from, or a BIP376 tweak our spend key reproduces. FOREIGN
// when it makes no claim on this wallet at all. BAD is a claim on this wallet
// that does not hold up (a second derivation naming us, a path of ours that is
// not native segwit or not this network, a tweak on a coin that is not
// taproot): every one of those is a refusal today and none of them may be
// read as "somebody else's" to get past it.
enum { SP_IN_OWN, SP_IN_FOREIGN, SP_IN_BAD };

static int sp_input_owner(size_t i, const struct ext_key *master,
                          const uint8_t fp[4])
{
    const struct wally_psbt_input *in = &s_psbt->inputs[i];
    if (s_sp_in.present[i]) {
        const struct wally_tx_output *u = in->witness_utxo;
        if (!u || u->script_len != 34 || u->script[0] != 0x51 ||
            u->script[1] != 0x20)
            return SP_IN_BAD;
        uint8_t spend_priv[32], d[32];
        int owned = sp_spend_privkey(master, kiss_testnet(), spend_priv) == 0 &&
                    sp_spend_signing_key(spend_priv, s_sp_in.tweak[i],
                                         u->script + 2, d) == 0;
        wally_bzero(spend_priv, sizeof spend_priv);
        wally_bzero(d, sizeof d);
        return owned ? SP_IN_OWN : SP_IN_FOREIGN;
    }
    uint32_t path[8];
    size_t path_len = 8;
    int kp = our_keypath(&in->keypaths, fp, path, &path_len, NULL);
    if (kp == 0)
        return SP_IN_FOREIGN;
    if (kp != 1 || our_purpose(path, path_len) != 84)
        return SP_IN_BAD;
    return SP_IN_OWN;
}

// Is this PSBT a shared send? Only with at least one input of ours AND at
// least one of somebody else's, and no claim on this wallet that fails. Called
// once a silent payment output has been found, and that is the gate the whole
// feature hangs on: without such an output there is no share to add, so a
// stranger's input stays what it always was, a refusal.
//
// The two ends of the count go back to sp_fill on purpose. Every input ours
// is the single signer path, untouched. No input ours is the old refusal in
// its own words, "input is not this wallet's", with the fingerprint panel the
// sign screen hangs off that sentence: a signer that owns nothing here has no
// share to give and nothing to sign.
static bool sp_sort_inputs(const struct ext_key *master, const uint8_t fp[4])
{
    const size_t n_in = s_psbt->num_inputs;
    size_t n_own = 0, n_foreign = 0;
    memset(&s_sh, 0, sizeof s_sh);
    if (n_in == 0 || n_in > WPSBT_MAX_INS)
        return false;                  // sp_fill says so in its own words
    for (size_t i = 0; i < n_in; i++) {
        int who = sp_input_owner(i, master, fp);
        if (who == SP_IN_BAD) {
            n_own = 0;
            break;
        }
        if (who == SP_IN_FOREIGN) {
            s_sh.foreign[i] = true;
            n_foreign++;
        } else {
            n_own++;
        }
    }
    if (!n_own || !n_foreign) {
        memset(&s_sh, 0, sizeof s_sh);
        return false;
    }
    s_sh.on = true;
    return true;
}

// One scan key group, derived from nothing but what the PSBT now holds, the
// way a coordinator will do it: each input's stored share checked against its
// stored proof and that input's own public key, the shares summed, the sum
// multiplied by the input hash. With derive false (a share only pass) the
// other signers' shares are not all there yet, so only ours are checked and
// nothing is derived. Returns 0 with recips/idxs/gn filled, nonzero on a
// share that is missing or does not verify, or a sum that has no point.
static int sp_shared_group(int g, const int *group_of,
                           const uint8_t (*pubs)[33], size_t n_in,
                           const uint8_t ih[32], bool derive,
                           sp_recip_t *recips, uint32_t *idxs, uint32_t *gn)
{
    uint8_t share[33], proof[64], csum[33] = { 0 };
    bool cinf = true;
    *gn = sp_group_collect(g, group_of, recips, idxs);
    for (size_t i = 0; i < n_in && i < WPSBT_MAX_INS; i++) {
        if (!derive && s_sh.foreign[i])
            continue;
        if (sp_in_share(i, recips[0].scan, share, proof) != 2 ||
            sp_dleq_verify(pubs[i], recips[0].scan, share, proof, NULL, NULL) != 0 ||
            sp_point_sum(csum, &cinf, share) != 0)
            return -1;
    }
    if (!derive)
        return 0;
    if (cinf || sp_derive_group(csum, ih, recips, *gn) != 0)
        return -2;
    return 0;
}

// The signer role of BIP375 when only SOME inputs are ours. Our inputs get
// their per input share and proof; the other signers' shares are checked, not
// trusted. With every share present the output scripts are derived from their
// sum, set or compared, and re-verified the way a coordinator will; with one
// missing this is a share only pass and nothing more is done. Any failure is
// a STOP. sp_sort_inputs said which inputs are whose before this runs.
static void sp_shared(wpsbt_summary_t *s, const struct ext_key *master,
                      const uint8_t fp[4], const uint8_t psbt_hash[32])
{
    uint8_t privs[WPSBT_MAX_INS][32];    // OUR inputs' scalars, as BIP352 counts them
    uint8_t pubs[WPSBT_MAX_INS][33];     // every input's public key, A_i
    uint8_t op[WPSBT_MAX_INS][36];
    uint8_t k32[32] = { 0 }, aux[32] = { 0 };
    uint8_t share[33], proof[64], key[34], seen[33];
    uint8_t a_pub[33] = { 0 }, ih[32] = { 0 };
    int group_of[WPSBT_MAX_OUTS], order[WPSBT_MAX_OUTS];
    sp_recip_t recips[WPSBT_MAX_OUTS];
    uint32_t idxs[WPSBT_MAX_OUTS], gn = 0;
    const size_t n_in = s_psbt->num_inputs;   // 2..WPSBT_MAX_INS, sp_sort_inputs
    const int n_groups = sp_groups(group_of, order);
    bool complete = true;
    memset(privs, 0, sizeof privs);
    memset(pubs, 0, sizeof pubs);

    // A share made for the WHOLE transaction says one signer holds every input
    // key. Beside an input that is not ours that is false whoever wrote it,
    // and it cannot be checked against anything this signer can add up.
    for (size_t j = 0; j < s_psbt->unknowns.num_items; j++)
        if (is_share_field(&s_psbt->unknowns.items[j], 0x07, 0x08)) {
            stop(s, "shared send carries a global share");
            goto out;
        }

    for (size_t i = 0; i < n_in && i < WPSBT_MAX_INS; i++) {
        struct wally_psbt_input *in = &s_psbt->inputs[i];
        const uint8_t *spk = NULL;
        size_t spk_len = 0;
        uint64_t sats = 0;
        bool proven = false;
        int pr = input_prevout(in, in->txhash, in->index, &spk, &spk_len,
                               &sats, &proven);
        if (pr != 0) {
            stop(s, pr == 2 ? "input's previous transaction does not match"
                            : "input amount unverifiable");
            goto out;
        }
        memcpy(op[i], in->txhash, 32);
        op[i][32] = (uint8_t)in->index;
        op[i][33] = (uint8_t)(in->index >> 8);
        op[i][34] = (uint8_t)(in->index >> 16);
        op[i][35] = (uint8_t)(in->index >> 24);

        if (s_sh.foreign[i]) {
            // Not ours. Its key comes from the input, and a share that came
            // with it is kept for the check below instead of being wiped: it
            // is the one thing in this PSBT this signer cannot recompute.
            int pk = sp_input_pubkey(in, spk, spk_len, pubs[i]);
            if (pk != 0) {
                stop(s, pk == 1 ? "shared send needs P2WPKH or P2TR inputs"
                                : "other signer's input key does not match");
                goto out;
            }
            // The tweak on it is the other signer's to prove, not ours. Left
            // marked, every later pass would try to sign this input as a
            // received silent payment of our own.
            s_sp_in.present[i] = false;
            // A share for a scan key no output pays to is not part of this
            // payment and nothing here could ever check what it is for.
            for (size_t j = 0; j < in->unknowns.num_items; j++) {
                const struct wally_map_item *it = &in->unknowns.items[j];
                bool used = !is_share_field(it, 0x1d, 0x1e);
                for (int g = 0; !used && g < n_groups; g++)
                    used = memcmp(it->key + 1, s_sp.o[order[g]].scan, 33) == 0;
                if (!used) {
                    stop(s, "other signer's share does not verify");
                    goto out;
                }
            }
            continue;
        }

        // Ours. Nothing that arrived for it is trusted: wiped, then recomputed.
        wipe_unknowns(&in->unknowns, 0x1d, 34);
        wipe_unknowns(&in->unknowns, 0x1e, 34);
        bool xonly = false;
        if (s_sp_in.present[i]) {
            // BIP376, as in sp_fill: the key is spend + tweak and must be the
            // P2TR key being spent.
            const struct wally_tx_output *u = in->witness_utxo;
            uint8_t spend_priv[32];
            int owned = u && u->script_len == 34 &&
                        sp_spend_privkey(master, kiss_testnet(), spend_priv) == 0 &&
                        sp_spend_signing_key(spend_priv, s_sp_in.tweak[i],
                                             u->script + 2, k32) == 0;
            wally_bzero(spend_priv, sizeof spend_priv);
            if (!owned) {
                stop(s, "silent-payment input is not this wallet's");
                goto out;
            }
            xonly = true;              // P2TR: BIP352 counts its even-Y key
        } else {
            uint32_t path[8];
            size_t path_len = 8;
            const struct wally_map_item *keypath = NULL;
            if (our_keypath(&in->keypaths, fp, path, &path_len, &keypath) != 1 ||
                our_purpose(path, path_len) != 84) {
                stop(s, "input is not this wallet's");
                goto out;
            }
            // Bound to the script it spends BEFORE a share is made from it.
            // The input loop makes this same check again for the signature;
            // here it keeps a share from being made with a key of ours for a
            // coin that key does not hold.
            int match = rederive_matches(path, path_len, spk, spk_len, keypath);
            if (match != 0) {
                stop(s, match == 2 ? "input derivation pubkey does not match"
                                   : "input script does not re-derive");
                goto out;
            }
            struct ext_key k;
            if (bip32_key_from_parent_path(master, path, path_len,
                                           BIP32_FLAG_KEY_PRIVATE, &k) != WALLY_OK) {
                wally_bzero(&k, sizeof k);
                stop(s, "silent payment key derivation failed");
                goto out;
            }
            memcpy(k32, k.priv_key + 1, 32);    // ext_key priv_key[0] is 0x00
            wally_bzero(&k, sizeof k);
        }
        // The scalar BIP352 counts for this input (negated to even Y when it
        // is taproot) and its point. The point must be the one a coordinator
        // will read off the input, or the proof below proves the wrong thing.
        if (sp_sum_privkeys(k32, &xonly, 1, privs[i], pubs[i]) != 0) {
            stop(s, "silent payment derivation failed");
            goto out;
        }
        wally_bzero(k32, sizeof k32);
        if (sp_input_pubkey(in, spk, spk_len, seen) != 0 ||
            memcmp(seen, pubs[i], 33) != 0) {
            stop(s, "silent payment self-check failed");
            goto out;
        }
    }

    {   // deterministic DLEQ aux, as in sp_fill: same psbt + same wallet = same bytes
        uint8_t seed[32 + 4 + 32];
        memcpy(seed, master->priv_key + 1, 32);
        memcpy(seed + 32, fp, 4);
        memcpy(seed + 36, psbt_hash, 32);
        wally_sha256(seed, sizeof seed, aux, 32);
        wally_bzero(seed, sizeof seed);
    }

    for (int g = 0; g < n_groups; g++) {
        const uint8_t *scan = s_sp.o[order[g]].scan;
        key[0] = 0x1d;
        memcpy(key + 1, scan, 33);
        for (size_t i = 0; i < n_in && i < WPSBT_MAX_INS; i++) {
            if (s_sh.foreign[i]) {
                // A share and its proof come together, and the proof has to
                // hold against the key this input really spends. Neither there
                // yet is not an error: that signer has not had its turn.
                int have = sp_in_share(i, scan, share, proof);
                if (have == 0) {
                    complete = false;
                    continue;
                }
                if (have == 1) {
                    stop(s, "other signer's share has no proof");
                    goto out;
                }
                if (have != 2 ||
                    sp_dleq_verify(pubs[i], scan, share, proof, NULL, NULL) != 0) {
                    stop(s, "other signer's share does not verify");
                    goto out;
                }
                continue;
            }
            struct wally_map *m = &s_psbt->inputs[i].unknowns;
            if (sp_ecdh_share(privs[i], scan, share) != 0 ||
                sp_dleq_prove(privs[i], scan, aux, NULL, NULL, proof) != 0) {
                stop(s, "silent payment derivation failed");
                goto out;
            }
            key[0] = 0x1d;
            if (wally_map_add(m, key, sizeof key, share, 33) != WALLY_OK) {
                stop(s, "silent payment derivation failed");
                goto out;
            }
            key[0] = 0x1e;
            if (wally_map_add(m, key, sizeof key, proof, 64) != WALLY_OK) {
                stop(s, "silent payment derivation failed");
                goto out;
            }
        }
    }
    // The private keys have done everything they are here for. From this line
    // on the function works from the PSBT alone.
    wally_bzero(privs, sizeof privs);
    wally_bzero(aux, sizeof aux);

    // Nothing may be added or removed once a share commits to these inputs.
    if (wally_psbt_set_tx_modifiable_flags(s_psbt, 0) != WALLY_OK) {
        stop(s, "silent payment derivation failed");
        goto out;
    }

    if (!complete) {
        // SHARE ONLY. A script cannot exist before every share does, so one
        // that is already here was not derived from this transaction's
        // inputs, and adding a share beneath it would be endorsing it.
        for (uint32_t o = 0; o < s_sp.n; o++) {
            const struct wally_psbt_output *po = &s_psbt->outputs[s_sp.o[o].idx];
            if (po->script || po->script_len) {
                stop(s, "output script set before all shares");
                goto out;
            }
        }
        // What leaves this signer is its own shares: check them as stored.
        for (int g = 0; g < n_groups; g++)
            if (sp_shared_group(g, group_of, (const uint8_t (*)[33])pubs, n_in,
                                ih, false, recips, idxs, &gn) != 0) {
                stop(s, "silent payment self-check failed");
                goto out;
            }
    } else {
        // COMPLETE. A_sum and the input hash come from every input's public
        // key, and the shared secret from every input's share.
        bool ainf = true;
        for (size_t i = 0; i < n_in && i < WPSBT_MAX_INS; i++)
            if (sp_point_sum(a_pub, &ainf, pubs[i]) != 0) {
                stop(s, "silent payment derivation failed");
                goto out;
            }
        if (ainf || sp_input_hash(&op[0][0], n_in, a_pub, ih) != 0) {
            stop(s, "silent payment derivation failed");
            goto out;
        }
        for (int g = 0; g < n_groups; g++) {
            if (sp_shared_group(g, group_of, (const uint8_t (*)[33])pubs, n_in,
                                ih, true, recips, idxs, &gn) != 0) {
                stop(s, "silent payment derivation failed");
                goto out;
            }
            for (uint32_t j = 0; j < gn; j++) {
                uint8_t scr[34] = { 0x51, 0x20 };
                const struct wally_psbt_output *po = &s_psbt->outputs[idxs[j]];
                memcpy(scr + 2, recips[j].xonly_out, 32);
                if (po->script || po->script_len) {
                    // The other signer derived it on its own pass. It is only
                    // a claim until it equals what these shares derive to.
                    if (po->script_len != 34 || !po->script ||
                        memcmp(po->script, scr, 34) != 0) {
                        stop(s, "silent payment script does not match");
                        goto out;
                    }
                } else if (wally_psbt_set_output_script(s_psbt, idxs[j], scr,
                                                        34) != WALLY_OK) {
                    stop(s, "silent payment derivation failed");
                    goto out;
                }
            }
        }
        // SELF-VERIFY as the coordinator will: only what the PSBT stores, every
        // proof again, every sum again, every script compared.
        for (int g = 0; g < n_groups; g++) {
            int ok = sp_shared_group(g, group_of, (const uint8_t (*)[33])pubs,
                                     n_in, ih, true, recips, idxs, &gn) == 0;
            for (uint32_t j = 0; ok && j < gn; j++) {
                const struct wally_psbt_output *po = &s_psbt->outputs[idxs[j]];
                ok = po->script && po->script_len == 34 &&
                     po->script[0] == 0x51 && po->script[1] == 0x20 &&
                     memcmp(po->script + 2, recips[j].xonly_out, 32) == 0;
            }
            if (!ok) {
                stop(s, "silent payment self-check failed");
                goto out;
            }
        }
    }

    s_sh.share_only = !complete;
    s->share_only = !complete;
    // The owner accepts this before the slide arms, on either pass: that part
    // of the inputs is not theirs, and on a share only pass that this step
    // signs nothing.
    caution(s, WPSBT_C_SHARED, complete ? "shared send: some inputs are another signer's"
                                        : "share only: nothing is signed in this step");

out:
    wally_bzero(privs, sizeof privs);
    wally_bzero(k32, sizeof k32);
    wally_bzero(aux, sizeof aux);
}

// Does output j stand without a script, and rightly? Only a silent payment
// output on a share only pass: its script is what the missing shares will
// derive, and sp_shared has already refused one that came with a script.
static bool sp_awaits_script(size_t j)
{
    if (!s_sh.share_only)
        return false;
    for (uint32_t o = 0; o < s_sp.n; o++)
        if (s_sp.o[o].idx == (uint32_t)j)
            return true;
    return false;
}

int kiss_psbt_load(const uint8_t *bytes, size_t len, wpsbt_summary_t *s)
{
    const struct ext_key *master = kiss_session_master();   // NULL on a card
    if (!s || (!master && !kiss_session_card()))
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
        static char txt[5465];                     // 5464 base64 chars for 4096 bytes + NUL
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
    // PSBT_OUT_SCRIPT makes a strict-parse failure in wally 1.6.0 - retry
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
    uint8_t psbt_hash[32];                         // deterministic-DLEQ seed
    wally_sha256(bytes, len, psbt_hash, 32);
    // NOW the decode buffer is dead, and not one line sooner: `bytes` still
    // points INTO it for a base64 PSBT, and the hash above is the seed for the
    // deterministic DLEQ proof. Wiping before this point silently reseeded every
    // proof off 4096 zero bytes -- which is what the golden BIP340 vectors
    // in sim/test_sp.c caught, and the only thing that would have.
    //
    // Every later return is an error path that would otherwise leave a whole
    // PSBT in .bss for the rest of the boot -- so the wipe sits on the line
    // after the last read of `bytes`, and the two shape checks below moved
    // BELOW it rather than the wipe being repeated in each. Both of them
    // returned -2 with the decoded transaction still in the buffer: every
    // address and amount of a rejected PSBT, readable for the rest of the
    // boot. Neither is exotic -- a PSBTv0 with no global tx and a v2/v0
    // hybrid are the two shapes a hostile file takes to get here.
    wally_bzero(b64buf, sizeof b64buf);
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

    s->status = WPSBT_READY;                       // cleared at entry; earned here
    s->testnet = kiss_testnet() != 0;
    s->net     = (uint8_t)kiss_network();

    uint8_t fp[BIP32_KEY_FINGERPRINT_LEN];
    if (kiss_session_fingerprint(fp) != 0)
        return -1;
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
    bool shared = false;
    if (sp_scan(s) == 0 && s_sp.n > 0) {
        if (s_psbt->version != 2)
            stop(s, "SP output needs PSBTv2");
        // sighash gate must hold BEFORE we derive/fill anything
        for (size_t i = 0; i < s_psbt->num_inputs && s->status != WPSBT_STOP; i++) {
            uint32_t sh = s_psbt->inputs[i].sighash;
            if (sh != 0 && sh != WALLY_SIGHASH_ALL)
                stop(s, "sighash is not ALL");
        }
        // Paying one takes every input's private key (the ECDH share), which
        // a card never hands out.
        if (s->status != WPSBT_STOP && !master)
            stop(s, "silent payments cannot be signed by a Satochip");
        // Ours alone, or paid together with another signer? Decided before
        // the wipe, because the second kind is the one PSBT that keeps a share
        // it did not make. Only here, under a silent payment output: without
        // one sp_sort_inputs is never asked and nothing is shared.
        if (s->status != WPSBT_STOP)
            shared = sp_sort_inputs(master, fp);
    }
    if (!shared)
        sp_wipe_shares();
    if (s_sp.n > 0 && s->status != WPSBT_STOP) {
        if (shared)
            sp_shared(s, master, fp, psbt_hash);
        else
            sp_fill(s, master, fp, psbt_hash);
    }
    if (s->status == WPSBT_STOP) {
        s_status = s->status;
        return 0;
    }
    if (s_psbt->version == 2) {
        // every output must now have a script (SP ones were just derived) and
        // an amount; a loose parse that hid any other gap dies here. The one
        // output allowed to lack a script is a silent payment's on a share
        // only pass, where sp_shared has just required exactly that.
        for (size_t j = 0; j < s_psbt->num_outputs; j++) {
            const struct wally_psbt_output *po = &s_psbt->outputs[j];
            if (!po->has_amount || (!po->script && !sp_awaits_script(j))) {
                stop(s, "malformed transaction (v2 fields)");
                s_status = s->status;
                return 0;
            }
        }
        if (s_sh.share_only) {
            // No transaction can be built without that script, so there is
            // none to extract and s_txv stays NULL: this pass reviews the
            // PSBT's own fields (rv_*) and every signing path finds nothing
            // to hash. The lock time is the one fact an extract computes
            // rather than copies, so it is computed here by the same rule.
            size_t lt = 0;
            if (wally_psbt_get_locktime(s_psbt, &lt) != WALLY_OK) {
                stop(s, "malformed transaction (v2 extract)");
                s_status = s->status;
                return 0;
            }
            s_sh.locktime = (uint32_t)lt;
        } else if (wally_psbt_extract(s_psbt, WALLY_PSBT_EXTRACT_NON_FINAL, &s_txv) != WALLY_OK) {
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
    // A v0 PSBT carries its transaction and wally refuses one without it, and
    // the v2 extract above is checked -- so this should not be reachable. It
    // is one branch in the parser every hostile file goes through, and the
    // same call is already NULL checked in kiss_psbt_details(), so the two
    // sites now agree rather than one of them being a crash.
    //
    // A share only pass is the one load that gets past here without one, and
    // on purpose: see the rv_* readers, which then read s_psbt itself.
    if (!s_psbt || (!tx && !s_sh.share_only)) {
        stop(s, "malformed transaction");
        s_status = s->status;
        return 0;
    }
    s->locktime = rv_locktime(tx);

    // TERMINAL, not a flag on the way past. This used to stop() and carry on
    // building the summary, and the output loop below is bounded by
    // tx->num_outputs while indexing s_psbt->outputs[j] -- so the very
    // disagreement being reported was what let j run past the shorter array.
    //
    // Nothing signable came of it (status is STOP and the sign gate reads it),
    // but the summary was assembled from a transaction whose two halves do not
    // describe the same thing, which is not a summary of anything. There is
    // nothing to show, so it returns.
    if (rv_n_in(tx) != s_psbt->num_inputs || rv_n_out(tx) != s_psbt->num_outputs) {
        stop(s, "malformed: tx/psbt count mismatch");
        s_status = s->status;
        return 0;
    }

    // One coin named by two inputs is a transaction no node accepts, and with
    // amounts that are only claimed it is also two different answers to what
    // that coin holds. Refused before anything is summed from it.
    for (size_t i = 1; i < rv_n_in(tx); i++)
        for (size_t k = 0; k < i; k++)
            if (rv_vout(tx, i) == rv_vout(tx, k) &&
                memcmp(rv_txhash(tx, i), rv_txhash(tx, k), 32) == 0) {
                stop(s, "input spends the same coin twice");
                i = rv_n_in(tx);
                break;
            }

    // ---- inputs: verifiable amount + our re-derived script, or no signature ----
    uint32_t n44 = 0, n49 = 0, n84 = 0, ntap = 0;   // inputs per type: fee estimate + UI label
    uint32_t nunproven = 0;   // amount taken from a witness_utxo with no prev tx behind it
    uint32_t nunproven_foreign = 0;   // of those, inputs another signer signs
    // Distinct input scriptPubKeys, hashed rather than kept: the answer needed
    // is only "seen before", and 40 scripts at up to 34 bytes each is a lot of
    // stack for a question a 64-bit key settles.
    uint64_t seen[WPSBT_ADDR_TRACK];
    uint32_t nseen = 0;
    bool     lock_enforced = false;   // some input leaves the locktime live
    for (size_t i = 0; i < s_psbt->num_inputs && i < rv_n_in(tx); i++) {
        const struct wally_psbt_input *in = &s_psbt->inputs[i];
        if (rv_sequence(tx, i) < 0xFFFFFFFE)
            s->rbf = true;
        // A locktime only binds when an input asks for it. Every sequence at
        // 0xFFFFFFFF and the field is decoration a node ignores, so warning
        // about it would be warning about nothing -- and that is not rare:
        // coordinators leave a stale locktime on a final transaction.
        if (rv_sequence(tx, i) != 0xFFFFFFFF)
            lock_enforced = true;
        // Read before any refusal below can `continue` past it, so the count
        // is every input's and not only the ones that verified.
        {
            uint32_t age;
            const int k = wpsbt_rel_lock(rv_version(tx), rv_sequence(tx, i), &age);
            if (k != WPSBT_REL_NONE) {
                s->n_rel_lock++;
                uint32_t *mx = k == WPSBT_REL_TIME ? &s->rel_secs_max
                                                   : &s->rel_blocks_max;
                if (age > *mx) *mx = age;
            }
        }

        if (in->sighash != 0 && in->sighash != WALLY_SIGHASH_ALL) {
            stop(s, "sighash is not ALL");
            continue;
        }

        // A shared send's other half: an input sp_shared sorted as another
        // signer's. It is never signed here and nothing about it is ours, so
        // none of the ownership checks below apply. What still applies is the
        // fee: its amount is a term in the subtraction the screen shows, so it
        // is read by exactly the rule our own inputs follow (input_prevout),
        // and an amount with no previous transaction behind it counts as
        // unproven and is flagged below with the rest.
        //
        // It is not one of our addresses, so it is left out of the merge and
        // dust counts: those are about coins of this wallet being linked.
        if (i < WPSBT_MAX_INS && s_sh.foreign[i]) {
            const uint8_t *fspk = NULL;
            size_t fspk_len = 0;
            uint64_t fval = 0;
            bool fproven = false;
            int pr = input_prevout(in, rv_txhash(tx, i), rv_vout(tx, i), &fspk,
                                   &fspk_len, &fval, &fproven);
            if (pr != 0) {
                stop(s, pr == 2 ? "input's previous transaction does not match"
                                : "input amount unverifiable");
                continue;
            }
            if (fval > MAX_MONEY) {
                stop(s, "input amount over 21M BTC (corrupt)");
                continue;
            }
            if (!fproven) { nunproven++; nunproven_foreign++; }
            s->in_sats += fval;
            s->n_foreign_in++;
            // sp_shared admitted P2WPKH and P2TR and nothing else
            if (fspk_len == 34) ntap++; else n84++;
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
            if (!master) {             // the spend private key is on the card
                stop(s, "silent payments cannot be signed by a Satochip");
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
            s->own_in_sats += u->satoshi;
            s->n_sp_in++;
            ntap++;
            nunproven++;   // witness_utxo only; BIP341 is what covers it, see below
            if (u->satoshi > 0 && u->satoshi < WPSBT_PRIVACY_SATS)
                caution(s, WPSBT_C_DUST_INPUT, "possible dust attack (privacy)");
            continue;
        }

        uint32_t path[8];
        size_t path_len = 8;
        const struct wally_map_item *keypath = NULL;
        int kp = our_keypath(&in->keypaths, fp, path, &path_len, &keypath);
        if (kp != 1) {
            stop(s, kp < 0 ? "input key derivations conflict"
                           : "input is not this wallet's");
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
            uint32_t vout = rv_vout(tx, i);
            if (wally_tx_get_txid(in->utxo, ptxid, sizeof ptxid) == WALLY_OK &&
                memcmp(ptxid, rv_txhash(tx, i), 32) == 0 &&
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
        s->own_in_sats += utxo_val;
        {
            const uint64_t key = spk_key(utxo_spk, utxo_spk_len);
            uint32_t j = 0;
            while (j < nseen && seen[j] != key) j++;
            if (j == nseen) {
                // Refuse rather than print a number that was not counted. The
                // old fallback here reported the INPUT count once the table
                // filled, which is an upper bound and not an answer: the card
                // would have said "joins 44 of your addresses" about a spend
                // that joined thirty.
                //
                // Same call the output side makes one screen over -- "too many
                // outputs to verify safely" -- and for the same reason. It is
                // also unreachable: a loadable PSBT is capped at 4096 bytes and
                // the generated fixtures cost ~137 bytes per marginal input
                // (279 at one input, 825 at five), so the ceiling is near
                // thirty against a table of forty. Unreachable is not the same
                // as correct, and this branch is the one that stays true if a
                // buffer ever grows.
                if (nseen >= WPSBT_ADDR_TRACK) {
                    stop(s, "too many input addresses to count safely");
                    continue;
                }
                seen[nseen++] = key;
            }
        }
        // spending a tiny KISS-owned coin is the classic dust-attack tell: a
        // stranger sends dust hoping you consolidate it and link your coins
        if (utxo_val > 0 && utxo_val < WPSBT_PRIVACY_SATS)
            caution(s, WPSBT_C_DUST_INPUT, "possible dust attack (privacy)");
        if (purpose == 44) n44++; else if (purpose == 49) n49++; else n84++;
        // re-derive our scriptPubKey for THIS input's own type and require an
        // exact match — the amount above is only trustworthy if this spk is ours
        int match = rederive_matches(path, path_len, utxo_spk, utxo_spk_len,
                                     keypath);
        if (match == 2)
            stop(s, "input derivation pubkey does not match");
        else if (match != 0)
            stop(s, "input script does not re-derive");
    }
    // detected type for the UI: one uniform purpose, or 0 when inputs mix types
    s->purpose = (n44 && !n49 && !n84) ? 44
               : (n49 && !n44 && !n84) ? 49
               : (n84 && !n44 && !n49) ? 84 : 0;

    s->n_unproven_in = nunproven;
    s->n_in_addr = nseen;
    // BIP376 inputs are exempt from the two checks below, and only them.
    // The format never carries the previous transactions (proving the input
    // amounts is a property of BIP341, not an omission of the coordinator),
    // and the coins being spent together are the ones this signer's own scan
    // already linked, so "merging" adds no information. A silent-payment SEND
    // that also spends ordinary P2WPKH inputs is not exempt: those inputs are
    // neither provably priced nor scan-linked, so both apply to them exactly as
    // they do in any other transaction.
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
    // invalidates it.
    //
    // This was a STOP, and that refused ordinary multi-input sends from Sparrow
    // over QR and from BlueWallet, which leave the previous transactions off
    // segwit inputs to keep the PSBT small. It is a CAUTION now: signable, but
    // the row has to be accepted before the slide arms, and the fee row on the
    // review is marked unverified for as long as this flag is set. What is still
    // refused is everything above that is not merely missing: a previous
    // transaction that does not hash to the outpoint, a witness_utxo that
    // disagrees with it, a legacy input with no previous transaction, an input
    // with no amount at all, an amount over 21M BTC, and outputs that exceed
    // the inputs.
    //
    // The bar is where the lie is possible, and nowhere else, because a
    // warning that fires on spends it cannot be about teaches the owner to
    // tap through the one that is:
    //  - an input THIS signer signs as taproot (a received silent payment)
    //    commits every input amount in its own signature, so nothing can be
    //    lied about and nothing is raised;
    //  - one claimed amount among our own inputs, the rest proven, is safe the
    //    way one input is: the only valid signature for that input comes from
    //    a session where its amount was true, and the proven ones are always
    //    true, so that session showed the real fee;
    //  - two or more claimed amounts among our own inputs is the two session
    //    lie above;
    //  - one claimed amount on ANOTHER signer's input is enough: none of our
    //    signatures commits to it, so no session of ours has to be truthful.
    const uint32_t nunproven_own = nunproven - nunproven_foreign;
    if (s->n_sp_in == 0 && (nunproven_foreign > 0 || nunproven_own >= 2))
        caution(s, WPSBT_C_UNVERIFIED_AMT, "input amounts not verified");

    // Merging coins is the one privacy loss a signer can see coming and the one
    // it can never take back: the moment this broadcasts, every input is public
    // proof the rest belong to the same owner. Counted over ALL our inputs, not
    // the dust ones — WPSBT_C_DUST_INPUT is about who sent the coin, this is
    // about how many are being tied together, and a sweep of perfectly ordinary
    // coins does the same damage. Soft CAUTION: consolidating is often the right
    // call, and the signer has no UTXO set to propose a better selection with.
    //
    // ADDRESSES, not coins. Five coins on one reused address are already one
    // owner as far as anyone watching is concerned, and warning about them
    // spends the owner's attention on a loss that happened when the address was
    // reused. What this counts is what the transaction actually gives away.
    if (s->n_in_addr >= WPSBT_MERGE_INS && s->n_in != s->n_sp_in)
        caution(s, WPSBT_C_MERGE_INS, "merging many coins (privacy)");

    // ---- outputs: re-derive change ourselves; never trust "this is change" ----
    // Refuse rather than verify a subset: an output we don't show is an output
    // the user can't approve, and skipping it would corrupt the fee math.
    if (rv_n_out(tx) > WPSBT_MAX_OUTS)
        stop(s, "too many outputs to verify safely");
    // Bounded by BOTH sides even though the mismatch above now returns: this
    // loop reads tx->outputs[j] and s_psbt->outputs[j] (the keypath lookup
    // further down), and a bound naming only one of them is one edit away from
    // the over-read again.
    for (size_t j = 0; j < rv_n_out(tx) && j < s_psbt->num_outputs &&
                       j < WPSBT_MAX_OUTS; j++) {
        // The output's amount and script, from the transaction or, on a share
        // only pass, from the PSBT fields it will be built from. A silent
        // payment output has no script on that pass, and nothing below reads
        // one for it: it is shown as the address it was given.
        size_t o_script_len = 0;
        const uint8_t *o_script = rv_out_script(tx, j, &o_script_len);
        const uint64_t o_sats = rv_out_sats(tx, j);
        wpsbt_out_t *so = &s->outs[j];
        so->sats = o_sats;
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
            spk_to_addr(o_script, o_script_len, so->addr, sizeof so->addr, s);
        if (o_sats > MAX_MONEY) {
            stop(s, "output amount over 21M BTC (corrupt)");
            continue;              // don't let it wrap the sums below
        }
        if (so->is_sp) {           // never change; always part of the send
            s->send_sats += o_sats;
            continue;
        }

        uint32_t path[8];
        size_t path_len = 8;
        const struct wally_map_item *keypath = NULL;
        int kp = our_keypath(&s_psbt->outputs[j].keypaths, fp, path, &path_len,
                             &keypath);
        if (kp < 0) {
            stop(s, "output key derivations conflict");
            s->send_sats += o_sats;
        } else if (kp == 1) {
            if (!our_purpose(path, path_len) ||
                rederive_matches(path, path_len, o_script, o_script_len,
                                 keypath) != 0) {
                stop(s, "change address does not re-derive");  // active attack marker
            } else {
                so->is_change = true;
                // our_purpose proved the path is 5 long and that path[3] is
                // 0 or 1, so both of these are safe to read.
                so->branch = path[3];
                so->index = path[4];
                s->change_sats += o_sats;
                // Change parked past every scanner's window. Its own `if`, not
                // folded into the pair below: an output can be both dust AND
                // out of reach, and those are different sentences.
                if (path[4] >= WPSBT_GAP_INDEX)
                    caution(s, WPSBT_C_GAP_CHANGE, "change past the scan window");
                // a tiny change output fragments your coins (privacy); below the
                // standardness dust floor it is also likely a coordinator slip
                if (o_sats > 0 && o_sats < dust_floor(our_purpose(path, path_len)))
                    caution(s, WPSBT_C_DUST_CHANGE, "dust change output");
                else if (o_sats > 0 && o_sats < WPSBT_PRIVACY_SATS)
                    caution(s, WPSBT_C_SMALL_CHANGE, "tiny change (privacy)");
            }
        } else {
            s->send_sats += o_sats;
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
    if (tx) {
        wally_tx_get_vsize(tx, &base_vsize);       // unsigned tx: vsize == size
    } else {
        // Share only: the same number with no transaction to measure. An
        // unsigned transaction is 4 bytes of version, the two counts, 41 a
        // input (outpoint, an empty scriptSig, sequence), 8 of amount and the
        // script with its length a output, and 4 of lock time. The silent
        // payment output will be a 34 byte P2TR script once it is derived, so
        // that is what it weighs here. Both counts are one byte: a shared
        // send has at most WPSBT_MAX_INS inputs and WPSBT_MAX_OUTS outputs.
        base_vsize = 4 + 1 + 41 * s_psbt->num_inputs + 1 + 4;
        for (size_t j = 0; j < s_psbt->num_outputs && j < WPSBT_MAX_OUTS; j++) {
            const struct wally_psbt_output *po = &s_psbt->outputs[j];
            const size_t sl = po->script ? po->script_len : 34;
            base_vsize += 8 + (sl < 0xfd ? 1 : 3) + sl;
        }
    }
    // per-input-type estimate (handles mixed types): legacy sigs are full-weight
    // base data (~107 vB each); nested adds a small scriptSig (~23 vB) plus its
    // witness; every segwit witness weighs 1/4 (~108 WU each + 2 marker bytes)
    s->est_vsize = (uint32_t)(base_vsize + 107 * n44 + 23 * n49);
    // taproot key-path witness ~= 66 WU (1 item + 1 len + 64-byte schnorr sig)
    if (n49 + n84 + ntap)
        s->est_vsize += (uint32_t)((2 + 108 * (n49 + n84) + 66 * ntap + 3) / 4);
    s->fee_rate_x10 = wpsbt_rate_x10(s->fee_sats, s->est_vsize);

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
    if (wpsbt_fee_share_high(s) || s->fee_rate_x10 > WPSBT_HIGH_RATE_X10)
        caution(s, WPSBT_C_HIGHFEE, "unusually high fee - check it before signing");

    // Stated, not flagged -- see wpsbt_summary_t.lock_binds for why a signer
    // that cannot see the chain tip must not turn this into a caution.
    s->lock_binds = (s->locktime != 0) && lock_enforced;
    if (s->n_rel_lock)
        stop(s, "an input asks its coin for a minimum age");

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
    const struct wally_tx *tx = psbt_tx();
    // a STOPped transaction failed verification — its raw fields must not be
    // presented under a page that says "verified" (and there is nothing to
    // decide: the signer already refused)
    //
    // A share only pass has no transaction and still has a details page: the
    // owner is asked to release a share for these inputs and outputs, and
    // they read the same from the PSBT fields (rv_*). What it cannot have is
    // an id, which stays empty.
    if (!d || !s_psbt || (!tx && !s_sh.share_only) || s_status == WPSBT_STOP)
        return -1;
    if (!kiss_session_master() && !kiss_session_card())
        return -1;
    memset(d, 0, sizeof *d);

    d->version = rv_version(tx);
    d->locktime = rv_locktime(tx);

    uint8_t fp[BIP32_KEY_FINGERPRINT_LEN];
    if (kiss_session_fingerprint(fp) != 0)
        return -1;

    uint8_t h[32];
    if (tx && wally_tx_get_txid((struct wally_tx *)tx, h, sizeof h) == WALLY_OK)
        txid_hex(h, d->txid);

    d->n_total = (uint32_t)s_psbt->num_inputs;
    bool any_scriptsig = false;
    for (size_t i = 0; i < s_psbt->num_inputs && i < rv_n_in(tx); i++) {
        const struct wally_psbt_input *in = &s_psbt->inputs[i];
        bool is_sp = i < WPSBT_MAX_INS && s_sp_in.present[i];
        // Another signer's input in a shared send: no path of ours to show.
        bool foreign = i < WPSBT_MAX_INS && s_sh.foreign[i];
        uint32_t path[8];
        size_t path_len = 8;
        bool have_path = !is_sp && !foreign &&
            our_keypath(&in->keypaths, fp, path, &path_len, NULL) == 1 && path_len == 5;
        uint32_t purpose = is_sp ? 352 : have_path ? our_purpose(path, path_len) : 0;
        // This describes the whole transaction, including inputs beyond the
        // DETAILS list's display limit.
        if (purpose == 44 || purpose == 49)
            any_scriptsig = true;
        if (d->n_in >= WPSBT_MAX_INS)
            continue;
        wpsbt_in_t *di = &d->ins[d->n_in++];
        txid_hex(rv_txhash(tx, i), di->txid);
        di->vout = rv_vout(tx, i);
        di->rel_kind = (uint8_t)wpsbt_rel_lock(rv_version(tx), rv_sequence(tx, i),
                                               &di->rel_age);
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
        di->is_sp = is_sp;
        di->foreign = foreign;
        di->purpose = purpose;
        if (have_path) {
            di->change = path[3];
            di->index = path[4];
        }
    }
    // Legacy signatures and nested-SegWit's redeem script are added to
    // scriptSig during finalization, changing the unsigned transaction's ID.
    // Native SegWit and taproot add only witness data, which txid excludes.
    d->txid_final = !any_scriptsig;
    return 0;
}

// BIP376: sign every input that spends a received silent payment, using the
// tweaked spend key d = b_spend + tweak. wally can't do this (the key is not a
// bip32 child), so compute the taproot key-path sighash + Schnorr-sign here.
// The signature is plain BIP340 with a zero aux, so any conforming signer
// holding the same key reproduces it byte for byte. Returns 0, or negative on
// any failure (the whole sign then fails - no partial result).
static int sign_sp_spends(const struct ext_key *master)
{
    bool any = false;
    for (size_t i = 0; i < s_psbt->num_inputs && i < WPSBT_MAX_INS; i++)
        if (s_sp_in.present[i]) { any = true; break; }
    if (!any)
        return 0;

    uint8_t spend_priv[32];
    int rc = -1;
    if (sp_spend_privkey(master, kiss_testnet(), spend_priv) != 0)
        return -1;
    // No aux is derived here on purpose. sp_schnorr_sign takes none: the nonce
    // is BIP340's standard deterministic one, so a second signer that has never
    // heard of KISS produces the same 64 bytes. That comparison is the only
    // check that catches firmware choosing its own nonce.
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
            sp_schnorr_sign(d, sh, sig) == 0) {
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
    return rc;
}

// The ECDSA sighash input i's signature must cover, by the same three steps
// wally_psbt_sign_bip32 takes: signing script, scriptCode, sighash.
static int input_sighash(size_t i, const struct wally_tx *tx, uint8_t h[32])
{
    uint8_t script[64], code[64];
    size_t sl = 0, cl = 0;
    if (wally_psbt_get_input_signing_script(s_psbt, i, script, sizeof script, &sl) != WALLY_OK ||
        sl > sizeof script ||
        wally_psbt_get_input_scriptcode(s_psbt, i, script, sl, code, sizeof code, &cl) != WALLY_OK ||
        cl > sizeof code ||
        wally_psbt_get_input_signature_hash(s_psbt, i, tx, code, cl, 0, h, 32) != WALLY_OK)
        return -1;
    return 0;
}

// libwally signs nothing for a key that already has a signature in the input,
// by design, and still reports success; the card path's add is ignored the
// same way. So a PSBT arriving with any bytes at all filed under one of our
// keys came back "signed" carrying those bytes instead of ours, and the old
// gate here -- some input has some signature -- passed on the very junk that
// replaced the signature. Clear ours first so the fresh one always lands.
// Signatures under other keys are not this signer's to judge and stay.
static void drop_our_sigs(void)
{
    uint8_t fp[4];
    if (kiss_session_fingerprint(fp) != 0)
        return;
    for (size_t i = 0; i < s_psbt->num_inputs; i++) {
        uint32_t path[8];
        size_t path_len = 8;
        const struct wally_map_item *kp = NULL;
        if (our_keypath(&s_psbt->inputs[i].keypaths, fp, path, &path_len, &kp) == 1)
            wally_map_remove(&s_psbt->inputs[i].signatures, kp->key, kp->key_len);
    }
}

// What another signer's input carries by way of signatures: its partial
// signatures and its taproot key signature. Counted before this signer signs
// and again after, see kiss_psbt_sign.
static size_t foreign_sig_count(size_t i)
{
    const struct wally_psbt_input *in = &s_psbt->inputs[i];
    return in->signatures.num_items +
           (wally_map_get_integer(&in->psbt_fields, 0x13) ? 1 : 0);
}

// The last gate, end to end: EVERY input (load admits nothing but ours) holds
// a signature under the key load verified, and it verifies against the
// sighash a finalizer will check. Nothing short of that is "signed".
//
// In a shared send "every input" is every input of OURS. Another signer's are
// skipped here because no signature of this signer belongs on them, and
// kiss_psbt_sign checks the other half of that sentence itself: that none
// landed there.
static int our_sigs_verify(void)
{
    const struct wally_tx *tx = psbt_tx();
    uint8_t fp[4];
    if (!tx || kiss_session_fingerprint(fp) != 0)
        return -1;
    for (size_t i = 0; i < s_psbt->num_inputs; i++) {
        const struct wally_psbt_input *in = &s_psbt->inputs[i];
        uint8_t h[32], sig[64];
        if (i < WPSBT_MAX_INS && s_sh.foreign[i])
            continue;
        if (i < WPSBT_MAX_INS && s_sp_in.present[i]) {
            // BIP376: key-path Schnorr under the output key being spent
            const struct wally_tx_output *u = in->witness_utxo;
            const struct wally_map_item *tap = wally_map_get_integer(&in->psbt_fields, 0x13);
            if (!u || u->script_len != 34 || !tap ||
                (tap->value_len != 64 && tap->value_len != 65) ||
                wally_psbt_get_input_signature_hash(s_psbt, i, tx, NULL, 0, 0, h, 32) != WALLY_OK ||
                wally_ec_sig_verify(u->script + 2, 32, h, 32, EC_FLAG_SCHNORR,
                                    tap->value, 64) != WALLY_OK)
                return -1;
            continue;
        }
        uint32_t path[8];
        size_t path_len = 8, idx = 0;
        const struct wally_map_item *kp = NULL;
        if (our_keypath(&in->keypaths, fp, path, &path_len, &kp) != 1 ||
            kp->key_len != EC_PUBLIC_KEY_LEN ||
            wally_map_find(&in->signatures, kp->key, kp->key_len, &idx) != WALLY_OK || !idx)
            return -1;
        const struct wally_map_item *it = &in->signatures.items[idx - 1];
        const uint32_t want = in->sighash ? in->sighash : WALLY_SIGHASH_ALL;
        if (it->value_len < 2 || it->value[it->value_len - 1] != (uint8_t)want ||
            wally_ec_sig_from_der(it->value, it->value_len - 1, sig, sizeof sig) != WALLY_OK ||
            input_sighash(i, tx, h) != 0 ||
            wally_ec_sig_verify(kp->key, kp->key_len, h, 32, EC_FLAG_ECDSA,
                                sig, sizeof sig) != WALLY_OK)
            return -1;
    }
    return 0;
}

// The card's path through the same steps wally_psbt_sign_bip32 takes for each
// input: signing script, scriptCode, sighash. Only the signature itself comes
// from the card, and it is checked against the input's own key before it goes
// into the PSBT. The load gate admitted nothing but inputs at our paths, so an
// input with no keypath of ours here is an error, not one to skip.
static int sign_with_card(void)
{
    const struct wally_tx *tx = psbt_tx();
    uint8_t fp[4];
    if (!tx || kiss_session_fingerprint(fp) != 0)
        return -2;
    for (size_t i = 0; i < s_psbt->num_inputs; i++) {
        const struct wally_psbt_input *in = &s_psbt->inputs[i];
        uint32_t path[8];
        size_t path_len = 8;
        const struct wally_map_item *kp = NULL;
        if (our_keypath(&in->keypaths, fp, path, &path_len, &kp) != 1 || !kp ||
            kp->key_len != EC_PUBLIC_KEY_LEN)
            return -2;

        uint8_t h[32], der[EC_SIGNATURE_DER_MAX_LEN + 1], sig[64];
        size_t dl = 0;
        if (input_sighash(i, tx, h) != 0)
            return -2;
        int rc = kiss_cardwallet_sign(path, (int)path_len, h, der, &dl);
        if (rc != 0)
            return rc == SC_ERR_IO ? -9 : -10;
        if (wally_ec_sig_from_der(der, dl, sig, sizeof sig) != WALLY_OK ||
            wally_ec_sig_verify(kp->key, kp->key_len, h, sizeof h, EC_FLAG_ECDSA,
                                sig, sizeof sig) != WALLY_OK)
            return -10;
        der[dl++] = (uint8_t)(in->sighash ? in->sighash : WALLY_SIGHASH_ALL);
        if (wally_psbt_add_input_signature(s_psbt, i, kp->key, kp->key_len, der, dl) != WALLY_OK)
            return -2;
    }
    return 0;
}

int kiss_psbt_sign(uint8_t *out, size_t out_len, size_t *written)
{
    const struct ext_key *master = kiss_session_master();   // NULL on a card
    if (!s_psbt || (!master && !kiss_session_card()) || s_status == WPSBT_STOP)
        return -1;
    // A share only pass signs nothing, and this line is the least of the
    // reasons. Its silent payment output has no script, so load built no
    // transaction (s_txv is NULL) and libwally cannot build one either: there
    // is no sighash for any signing path below to sign. kiss_psbt_share is the
    // way out of that pass.
    if (s_sh.share_only || (s_psbt->version == 2 && !s_txv))
        return -1;
    // The curve code must have reproduced the golden vectors on THIS chip
    // before it is trusted with a real key. Cached after the first run, so the
    // cost is the boot call; a unit that fails signs nothing at all rather
    // than emitting a signature whose nonce nobody has ever checked.
    if (kiss_sign_selftest() != 0)
        return -6;
    // A shared send: what the other signers' inputs carry before this signer
    // touches anything, so the gate below can show it left them alone.
    size_t fsigs[WPSBT_MAX_INS] = { 0 };
    for (size_t i = 0; i < s_psbt->num_inputs && i < WPSBT_MAX_INS; i++)
        if (s_sh.foreign[i])
            fsigs[i] = foreign_sig_count(i);
    drop_our_sigs();
    if (!master) {
        int rc = sign_with_card();
        if (rc != 0)
            return rc;
    } else {
        if (wally_psbt_sign_bip32(s_psbt, master, EC_FLAG_GRIND_R) != WALLY_OK)
            return -2;
        if (sign_sp_spends(master) != 0)
            return -5;
    }
    if (our_sigs_verify() != 0)
        return -7;
    // Never a signature of ours on an input that is not ours. libwally signs
    // only where a derivation of this seed matches, and a foreign input has
    // none, so this cannot fire. It is checked because "never signed" is the
    // one promise a shared send makes about those inputs.
    for (size_t i = 0; i < s_psbt->num_inputs && i < WPSBT_MAX_INS; i++)
        if (s_sh.foreign[i] && foreign_sig_count(i) != fsigs[i])
            return -7;
    // BIP370 signer rule: once a non-ANYONECANPAY signature exists the inputs
    // are closed, and once a non-NONE one does the outputs are. The load gate
    // above accepts nothing but sighash 0 or ALL, so both bits are always due
    // to be cleared here and zero is the whole answer.
    //
    // wally 1.6.0 clears these for every signature it stores, on the
    // wally_psbt_sign_bip32 path and the card's wally_psbt_add_input_signature
    // path alike. 1.5.4 missed the first, so a signed PSBTv2 left here still
    // saying TX_MODIFIABLE=0x03 -- telling the next coordinator it may still
    // add inputs and outputs, over a signature that any such edit would
    // silently invalidate. The call stays so that answer never rests on the
    // vendored version. sp_fill already made this call for silent payments;
    // every v2 gets it now.
    if (s_psbt->version == 2 &&
        wally_psbt_set_tx_modifiable_flags(s_psbt, 0) != WALLY_OK)
        return -8;
    size_t need = 0;
    if (wally_psbt_get_length(s_psbt, 0, &need) != WALLY_OK || need > out_len)
        return -3;
    return wally_psbt_to_bytes(s_psbt, 0, out, out_len, written) == WALLY_OK ? 0 : -4;
}

// ---- the share only exit ------------------------------------------------------
// A PSBT whose silent payment output has no script is what a share only pass
// has to hand back, and it is the one PSBT the vendored libwally will not
// write: BIP370 makes PSBT_OUT_SCRIPT mandatory, BIP375 relaxes that, and
// wally_psbt_to_bytes predates the relaxation (sim/test_sp.c pins it). So the
// bytes are written with a placeholder where the script will be, and the
// placeholder is cut out of them again.
//
// The placeholder never touches the held PSBT. It goes into a copy that lives
// for the length of one call and is reachable from nowhere else, so there is
// no moment at which the PSBT a signing path could be handed has a script on
// that output.

// One compact size at b[*at]. False when it runs off the end.
static bool cs_read(const uint8_t *b, size_t n, size_t *at, uint64_t *v)
{
    if (*at >= n)
        return false;
    const uint8_t c = b[(*at)++];
    const size_t w = c < 0xfd ? 0 : c == 0xfd ? 2 : c == 0xfe ? 4 : 8;
    uint64_t x = c;
    if (w) {
        if (n - *at < w)
            return false;
        x = 0;
        for (size_t k = 0; k < w; k++)
            x |= (uint64_t)b[*at + k] << (8 * k);
        *at += w;
    }
    *v = x;
    return true;
}

// Walk one PSBT map from *at to just past its separator: key and value pairs,
// each length a compact size, ended by a zero key length. With cut_script the
// PSBT_OUT_SCRIPT pair (a one byte key, 0x04) is removed from the buffer and
// *n shrinks by its size. False on a map that runs off the end.
static bool map_walk(uint8_t *b, size_t *n, size_t *at, bool cut_script)
{
    for (;;) {
        const size_t start = *at;
        uint64_t kl = 0, vl = 0;
        if (!cs_read(b, *n, at, &kl))
            return false;
        if (kl == 0)
            return true;                               // the separator
        if (kl > *n - *at)
            return false;
        const bool hit = cut_script && kl == 1 && b[*at] == 0x04;
        *at += (size_t)kl;
        if (!cs_read(b, *n, at, &vl) || vl > *n - *at)
            return false;
        *at += (size_t)vl;
        if (hit) {
            memmove(b + start, b + *at, *n - *at);
            *n -= *at - start;
            *at = start;
        }
    }
}

int kiss_psbt_share(uint8_t *out, size_t out_len, size_t *written)
{
    // Only the pass that has nothing to sign, and only with the keys that made
    // the shares still in the session: the same conditions a signature has.
    if (!out || !written)
        return -1;
    *written = 0;
    if (!s_psbt || !kiss_session_master() || !s_sh.share_only ||
        s_status == WPSBT_STOP)
        return -1;
    // The shares were made by the same curve code a signature is, so a unit
    // that fails its vectors releases none (see kiss_psbt_sign).
    if (kiss_sign_selftest() != 0)
        return -6;
    // Whatever sits under one of our keys is not a signature this pass made.
    // It goes, for the reason kiss_psbt_sign drops it: nothing leaves here
    // looking like ours unless it is.
    drop_our_sigs();

    struct wally_psbt *copy = NULL, *back = NULL;
    static const uint8_t HOLD[1] = { 0x6a };           // OP_RETURN: pays no one
    size_t n = 0, at = 5;
    int rc = -2;
    if (wally_psbt_clone_alloc(s_psbt, 0, &copy) != WALLY_OK || !copy)
        return -2;
    for (uint32_t o = 0; o < s_sp.n; o++)
        if (wally_psbt_set_output_script(copy, s_sp.o[o].idx, HOLD,
                                         sizeof HOLD) != WALLY_OK)
            goto out;
    // wally answers OK with the length it NEEDED when the buffer is short.
    if (wally_psbt_to_bytes(copy, 0, out, out_len, &n) != WALLY_OK || n > out_len) {
        rc = -3;
        goto out;
    }
    wally_psbt_free(copy);
    copy = NULL;

    // Cut the placeholder out of each silent payment output's map, found by
    // walking the maps in the order they are written and never by searching
    // for its bytes: globals, one map an input, one map an output.
    rc = -4;
    if (n < at || !map_walk(out, &n, &at, false))
        goto out;
    for (size_t i = 0; i < s_psbt->num_inputs; i++)
        if (!map_walk(out, &n, &at, false))
            goto out;
    for (size_t j = 0; j < s_psbt->num_outputs; j++)
        if (!map_walk(out, &n, &at, sp_awaits_script(j)))
            goto out;
    if (at != n)
        goto out;

    // Read back what is about to leave, the way the coordinator will: it must
    // parse, describe the same inputs and outputs, and carry no script on any
    // silent payment output. Bytes that fail this go nowhere.
    rc = -5;
    if (wally_psbt_from_bytes(out, n, WALLY_PSBT_PARSE_FLAG_LOOSE, &back) != WALLY_OK ||
        !back || back->num_inputs != s_psbt->num_inputs ||
        back->num_outputs != s_psbt->num_outputs)
        goto out;
    for (size_t j = 0; j < back->num_outputs; j++) {
        const struct wally_psbt_output *po = &back->outputs[j];
        if (sp_awaits_script(j) ? (po->script || po->script_len)
                                : (!po->script || !po->script_len))
            goto out;
    }
    *written = n;
    rc = 0;
out:
    if (copy)
        wally_psbt_free(copy);
    if (back)
        wally_psbt_free(back);
    if (rc != 0)
        wally_bzero(out, out_len);
    return rc;
}

// The size is the previous transactions. A coordinator attaches one per
// input so the signer can PROVE each amount (see the fee-lie note above), and
// a payout from a large transaction is kilobytes; on the way back that proof
// has done its work. The coordinator combines our signatures into its own
// copy, which still has every field, and a coordinator that finalizes this
// copy directly needs only what is kept: BIP143 signs the witness_utxo
// amount and script, and the finalizer reads the pubkey off the signature.
// SeedSigner returns less than this (the transaction and the signatures);
// Krux drops the same metadata but keeps the previous transactions.
int kiss_psbt_export_copy(const uint8_t *signed_psbt, size_t len,
                          uint8_t *out, size_t cap, size_t *written)
{
    if (!signed_psbt || !out || !written)
        return -1;
    *written = 0;
    struct wally_psbt *p = NULL;
    // Strict, and a share only PSBT ends here: its silent payment output has
    // no script, which this parse refuses, so the caller sends it whole. That
    // is the right answer for it and not an accident to rely on. It is a
    // PSBTv2, and the next paragraph sends every one of those whole anyway.
    if (wally_psbt_from_bytes(signed_psbt, len, 0, &p) != WALLY_OK || !p)
        return -1;
    // A PSBTv2 goes whole. A coordinator can only combine a returned PSBT
    // into its own when both describe the same transaction, and a silent
    // payment's does not (the signer writes the output scripts), so a v2
    // coordinator takes the signer's copy as the transaction and checks it
    // alone. kiss-bdk does that for every v2, and its check reads the input
    // public keys from the derivations this would drop.
    bool whole = p->version == 2 || p->unknowns.num_items != 0;
    for (size_t i = 0; !whole && i < p->num_inputs; i++)
        whole = p->inputs[i].unknowns.num_items != 0;
    for (size_t j = 0; !whole && j < p->num_outputs; j++)
        whole = p->outputs[j].unknowns.num_items != 0;
    if (!whole) {
        wally_map_clear(&p->global_xpubs);
        for (size_t i = 0; i < p->num_inputs; i++) {
            struct wally_psbt_input *in = &p->inputs[i];
            wally_map_clear(&in->keypaths);
            if (in->witness_utxo && in->utxo &&
                wally_psbt_input_set_utxo(in, NULL) != WALLY_OK) {
                wally_psbt_free(p);
                return -1;
            }
        }
        for (size_t j = 0; j < p->num_outputs; j++)
            wally_map_clear(&p->outputs[j].keypaths);
    }
    // wally answers OK with the length it NEEDED when cap is short, and
    // writes nothing past cap: a written over cap is a refusal, not a copy.
    int rc = wally_psbt_to_bytes(p, 0, out, cap, written) == WALLY_OK &&
             *written <= cap ? 0 : -1;
    wally_psbt_free(p);
    if (rc != 0)
        *written = 0;
    return rc;
}

int kiss_psbt_sig_fingerprint(const uint8_t *signed_psbt, size_t len,
                                char out[9])
{
    if (!signed_psbt || !out)
        return -1;
    struct wally_psbt *p = NULL;
    if (wally_psbt_from_bytes(signed_psbt, len, 0, &p) != WALLY_OK || !p)
        return -1;
    // Hash the signature bytes in input order, fed in as they are found.
    // Signatures only: the whole point is that a signer producing the same
    // signatures agrees, whatever its PSBT framing.
    //
    // DECIDED: this concatenated into a 4096 byte automatic and hashed once,
    // with an overflow flag returning -1 if the signatures did not fit. The
    // buffer was a fifth of the main task's 20KB stack, claimed in the frame
    // that runs immediately after signing, where libwally is already deep --
    // and it was sized for a PSBT this device cannot be handed: the sign
    // screen reads into QRT_MAX_PSBT (4096) TOTAL, framing included, so the
    // signature bytes alone can never come near 4096 and the overflow branch
    // was unreachable. Streaming spends 112 bytes, has nothing to overflow,
    // and drops the branch. Byte-identical output, held by the golden vector
    // in test_crypto.c.
    CRYAL_SHA256_CTX sha;
    ur_bundled_sha256_init(&sha);
    int any = 0;
    for (size_t i = 0; i < p->num_inputs; i++) {
        const struct wally_map *sigs = &p->inputs[i].signatures;
        for (size_t j = 0; j < sigs->num_items; j++) {
            const struct wally_map_item *it = &sigs->items[j];
            ur_bundled_sha256_update(&sha, it->value, it->value_len);
            any = 1;
        }
        const struct wally_map_item *tap =
            wally_map_get_integer(&p->inputs[i].psbt_fields, 0x13);
        if (tap && (tap->value_len == 64 || tap->value_len == 65)) {
            ur_bundled_sha256_update(&sha, tap->value, tap->value_len);
            any = 1;
        }
    }
    wally_psbt_free(p);
    if (!any) { wally_bzero(&sha, sizeof sha); return -1; }
    uint8_t h[32];
    ur_bundled_sha256_final(&sha, h);
    wally_bzero(&sha, sizeof sha);
    static const char HEX[] = "0123456789abcdef";
    for (int k = 0; k < 4; k++) {
        out[k * 2]     = HEX[h[k] >> 4];
        out[k * 2 + 1] = HEX[h[k] & 0x0f];
    }
    out[8] = 0;
    return 0;
}

// Is a transaction still parsed in RAM? The accessors above all refuse without
// a session, so nothing on screen could show it -- but refusing to SHOW it and
// not HOLDING it are different claims, and only the second one survives a lock.
bool kiss_psbt_held(void)
{
    return s_psbt != NULL;
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
    memset(&s_sh, 0, sizeof s_sh);
    s_status = WPSBT_STOP;
}
