// The trimmed copy a signed PSBT leaves by QR (kiss_psbt_export_copy).
//
// What has to hold, checked independently with libwally on every script type
// the signer signs and on both PSBT versions:
//   - the full and the trimmed copy finalize to the SAME transaction;
//   - a coordinator that combines the trimmed copy into its own unsigned
//     PSBT finalizes to that same transaction too;
//   - the trimmed copy still carries what a finalizer reads, and every
//     signature in it verifies from the trimmed copy's own fields;
//   - it survives the UR and pMofN encoders unchanged;
//   - a silent payment PSBT is never trimmed.
// The sizes and frame counts it prints are the before/after numbers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kiss_crypto.h"
#include "kiss_psbt.h"
#include "qr_transport.h"

#include <wally_bip32.h>
#include <wally_bip39.h>
#include <wally_crypto.h>
#include <wally_map.h>
#include <wally_psbt.h>
#include <wally_psbt_members.h>
#include <wally_script.h>
#include <wally_transaction.h>

#define H BIP32_INITIAL_HARDENED_CHILD

static int fails;

static void chkb(const char *name, int ok)
{
    if (ok) printf("PASS: %s\n", name);
    else { printf("FAIL: %s\n", name); fails++; }
}

static struct ext_key e_master;
static uint8_t e_fp[4];

static void spk_for(int purpose, const uint8_t pub[33], uint8_t *out, size_t *len)
{
    size_t w = 0;
    if (purpose == 44) {
        wally_scriptpubkey_p2pkh_from_bytes(pub, 33, WALLY_SCRIPT_HASH160, out, 25, len);
    } else if (purpose == 49) {
        uint8_t r[22];
        wally_witness_program_from_bytes(pub, 33, WALLY_SCRIPT_HASH160, r, 22, &w);
        wally_scriptpubkey_p2sh_from_bytes(r, 22, WALLY_SCRIPT_HASH160, out, 23, len);
    } else {
        wally_witness_program_from_bytes(pub, 33, WALLY_SCRIPT_HASH160, out, 22, len);
    }
}

// A previous transaction paying `amount` to `spk` at output 0, shaped like a
// real one: a segwit input with a 107-byte witness, and `n_out` outputs in
// all (2 for a wallet spend, 20 for an exchange payout).
static struct wally_tx *prev_tx(int seed, const uint8_t *spk, size_t spk_len,
                                uint64_t amount, int n_out)
{
    struct wally_tx *t = NULL;
    uint8_t txid[32], other[22] = {0x00, 0x14};
    memset(txid, 0x60 + seed, 32);
    memset(other + 2, 0x70 + seed, 20);
    wally_tx_init_alloc(2, 0, 1, (size_t)n_out, &t);
    struct wally_tx_witness_stack *ws = NULL;
    uint8_t sig[72], pub[33];
    memset(sig, 0x30, sizeof sig);
    memset(pub, 0x02, sizeof pub);
    wally_tx_witness_stack_init_alloc(2, &ws);
    wally_tx_witness_stack_add(ws, sig, sizeof sig);
    wally_tx_witness_stack_add(ws, pub, sizeof pub);
    wally_tx_add_raw_input(t, txid, 32, 1, 0xFFFFFFFD, NULL, 0, ws, 0);
    wally_tx_witness_stack_free(ws);
    wally_tx_add_raw_output(t, amount, spk, spk_len, 0);
    for (int k = 1; k < n_out; k++)
        wally_tx_add_raw_output(t, 50000 + (uint64_t)k, other, 22, 0);
    return t;
}

typedef struct {
    const char *name;
    int n_in;
    int purpose[4];      // per input: 44 / 49 / 84
    int prev_outs;       // outputs in each previous transaction
    int version;         // PSBT version: 0 or 2
    int xpub;            // carry a global xpub, as Sparrow does
} shape_t;

// Every input ours at m/<purpose>h/0h/0h/0/i worth 100000, one output to a
// stranger and one change output at 1/0, fee 2000.
static size_t build(const shape_t *sh, uint8_t *out, size_t cap)
{
    uint8_t ext[22] = {0x00, 0x14};
    memset(ext + 2, 0x11, 20);
    const int cp = sh->purpose[0];
    const uint32_t pchg[5] = {H + (uint32_t)cp, H, H, 1, 0};
    struct ext_key kchg;
    bip32_key_from_parent_path(&e_master, pchg, 5, BIP32_FLAG_KEY_PRIVATE, &kchg);
    uint8_t chg_spk[25];
    size_t chg_len = 0;
    spk_for(cp, kchg.pub_key, chg_spk, &chg_len);

    struct wally_tx *tx = NULL;
    wally_tx_init_alloc(2, 0, (size_t)sh->n_in, 2, &tx);
    struct wally_tx *prevs[4] = {0};
    struct ext_key kin[4];
    uint8_t in_spk[4][25];
    size_t in_len[4];
    for (int i = 0; i < sh->n_in; i++) {
        const uint32_t pin[5] = {H + (uint32_t)sh->purpose[i], H, H, 0, (uint32_t)i};
        bip32_key_from_parent_path(&e_master, pin, 5, BIP32_FLAG_KEY_PRIVATE, &kin[i]);
        spk_for(sh->purpose[i], kin[i].pub_key, in_spk[i], &in_len[i]);
        prevs[i] = prev_tx(i, in_spk[i], in_len[i], 100000, sh->prev_outs);
        uint8_t txid[32];
        wally_tx_get_txid(prevs[i], txid, 32);
        wally_tx_add_raw_input(tx, txid, 32, 0, 0xFFFFFFFD, NULL, 0, NULL, 0);
    }
    const uint64_t total = 100000ULL * (uint64_t)sh->n_in;
    wally_tx_add_raw_output(tx, 60000, ext, 22, 0);
    wally_tx_add_raw_output(tx, total - 60000 - 2000, chg_spk, chg_len, 0);

    struct wally_psbt *p = NULL;
    wally_psbt_init_alloc(0, (size_t)sh->n_in, 2, 0, 0, &p);
    wally_psbt_set_global_tx(p, tx);
    for (int i = 0; i < sh->n_in; i++) {
        const uint32_t pin[5] = {H + (uint32_t)sh->purpose[i], H, H, 0, (uint32_t)i};
        wally_psbt_set_input_utxo(p, (size_t)i, prevs[i]);
        if (sh->purpose[i] != 44) {
            struct wally_tx_output *u = NULL;
            wally_tx_output_init_alloc(100000, in_spk[i], in_len[i], &u);
            wally_psbt_set_input_witness_utxo(p, (size_t)i, u);
            wally_tx_output_free(u);
        }
        if (sh->purpose[i] == 49) {
            uint8_t r[22];
            size_t w = 0;
            wally_witness_program_from_bytes(kin[i].pub_key, 33, WALLY_SCRIPT_HASH160, r, 22, &w);
            wally_psbt_set_input_redeem_script(p, (size_t)i, r, 22);
        }
        struct wally_map *m = NULL;
        wally_map_keypath_public_key_init_alloc(1, &m);
        wally_map_keypath_add(m, kin[i].pub_key, 33, e_fp, 4, pin, 5);
        wally_psbt_set_input_keypaths(p, (size_t)i, m);
        wally_map_free(m);
        wally_tx_free(prevs[i]);
    }
    struct wally_map *m = NULL;
    wally_map_keypath_public_key_init_alloc(1, &m);
    wally_map_keypath_add(m, kchg.pub_key, 33, e_fp, 4, pchg, 5);
    wally_psbt_set_output_keypaths(p, 1, m);
    wally_map_free(m);

    if (sh->xpub) {
        const uint32_t pacct[3] = {H + (uint32_t)cp, H, H};
        struct ext_key acct;
        uint8_t ser[BIP32_SERIALIZED_LEN], val[4 + 12];
        bip32_key_from_parent_path(&e_master, pacct, 3, BIP32_FLAG_KEY_PUBLIC, &acct);
        bip32_key_serialize(&acct, BIP32_FLAG_KEY_PUBLIC, ser, sizeof ser);
        memcpy(val, e_fp, 4);
        for (int d = 0; d < 3; d++) {
            val[4 + 4 * d] = (uint8_t)pacct[d];
            val[5 + 4 * d] = (uint8_t)(pacct[d] >> 8);
            val[6 + 4 * d] = (uint8_t)(pacct[d] >> 16);
            val[7 + 4 * d] = (uint8_t)(pacct[d] >> 24);
        }
        if (wally_map_add(&p->global_xpubs, ser, sizeof ser, val, sizeof val) != WALLY_OK)
            printf("FAIL: export fixture xpub\n");
    }
    if (sh->version == 2 && wally_psbt_set_version(p, 0, 2) != WALLY_OK)
        printf("FAIL: export fixture v2\n");

    size_t wr = 0;
    if (wally_psbt_to_bytes(p, 0, out, cap, &wr) != WALLY_OK)
        wr = 0;
    wally_psbt_free(p);
    wally_tx_free(tx);
    for (int i = 0; i < sh->n_in; i++)
        wally_bzero(&kin[i], sizeof kin[i]);
    wally_bzero(&kchg, sizeof kchg);
    return wr;
}

// Finalize and extract with libwally alone; the network transaction's bytes.
static size_t final_tx(struct wally_psbt *p, uint8_t *out, size_t cap)
{
    struct wally_tx *tx = NULL;
    size_t w = 0;
    if (wally_psbt_finalize(p, 0) != WALLY_OK ||
        wally_psbt_extract(p, 0, &tx) != WALLY_OK ||
        wally_tx_to_bytes(tx, WALLY_TX_FLAG_USE_WITNESS, out, cap, &w) != WALLY_OK)
        w = 0;
    wally_tx_free(tx);
    return w;
}

static size_t final_from_bytes(const uint8_t *psbt, size_t len, uint8_t *out, size_t cap)
{
    struct wally_psbt *p = NULL;
    size_t w = 0;
    if (wally_psbt_from_bytes(psbt, len, 0, &p) == WALLY_OK)
        w = final_tx(p, out, cap);
    wally_psbt_free(p);
    return w;
}

// The coordinator's side: its own unsigned PSBT, combined with what came
// back from the signer, then finalized.
static size_t final_merged(const uint8_t *mine, size_t ml, const uint8_t *back,
                           size_t bl, uint8_t *out, size_t cap)
{
    struct wally_psbt *a = NULL, *b = NULL;
    size_t w = 0;
    if (wally_psbt_from_bytes(mine, ml, 0, &a) == WALLY_OK &&
        wally_psbt_from_bytes(back, bl, 0, &b) == WALLY_OK &&
        wally_psbt_combine(a, b) == WALLY_OK)
        w = final_tx(a, out, cap);
    wally_psbt_free(a);
    wally_psbt_free(b);
    return w;
}

// Every partial signature in the trimmed copy verifies against a sighash
// computed from the trimmed copy's OWN fields: it carries enough to be
// checked, not just enough to be glued into something that can.
static int sigs_verify_alone(const uint8_t *psbt, size_t len)
{
    struct wally_psbt *p = NULL;
    struct wally_tx *tx = NULL;
    int ok = wally_psbt_from_bytes(psbt, len, 0, &p) == WALLY_OK &&
             wally_psbt_extract(p, WALLY_PSBT_EXTRACT_NON_FINAL, &tx) == WALLY_OK;
    for (size_t i = 0; ok && i < p->num_inputs; i++) {
        const struct wally_map *sigs = &p->inputs[i].signatures;
        uint8_t script[64], code[64], h[32], sig[64];
        size_t sl = 0, cl = 0;
        ok = sigs->num_items == 1 &&
             wally_psbt_get_input_signing_script(p, i, script, sizeof script, &sl) == WALLY_OK &&
             wally_psbt_get_input_scriptcode(p, i, script, sl, code, sizeof code, &cl) == WALLY_OK &&
             wally_psbt_get_input_signature_hash(p, i, tx, code, cl, 0, h, 32) == WALLY_OK &&
             wally_ec_sig_from_der(sigs->items[0].value, sigs->items[0].value_len - 1,
                                   sig, 64) == WALLY_OK &&
             wally_ec_sig_verify(sigs->items[0].key, 33, h, 32, EC_FLAG_ECDSA, sig, 64) == WALLY_OK;
    }
    wally_tx_free(tx);
    wally_psbt_free(p);
    return ok;
}

static size_t redeem_len(const struct wally_psbt *p, size_t i)
{
    size_t n = 0;
    if (wally_psbt_get_input_redeem_script_len(p, i, &n) != WALLY_OK)
        n = 0;
    return n;
}

// What the trimmed copy keeps and drops, field by field.
static int fields_ok(const uint8_t *psbt, size_t len, const shape_t *sh)
{
    struct wally_psbt *p = NULL;
    if (wally_psbt_from_bytes(psbt, len, 0, &p) != WALLY_OK)
        return 0;
    int ok = p->global_xpubs.num_items == 0 && (uint32_t)p->version == (uint32_t)sh->version;
    for (int i = 0; ok && i < sh->n_in; i++) {
        const struct wally_psbt_input *in = &p->inputs[i];
        const int legacy = sh->purpose[i] == 44;
        ok = in->keypaths.num_items == 0 &&
             in->signatures.num_items == 1 &&
             (legacy ? in->utxo != NULL && in->witness_utxo == NULL
                     : in->utxo == NULL && in->witness_utxo != NULL) &&
             (sh->purpose[i] != 49 || redeem_len(p, (size_t)i) == 22);
    }
    for (size_t j = 0; ok && j < p->num_outputs; j++)
        ok = p->outputs[j].keypaths.num_items == 0;
    wally_psbt_free(p);
    return ok;
}

static int ur_parts(const uint8_t *psbt, size_t len, int frag)
{
    qrt_encoder_t *e = qrt_encoder_new_frag(QRT_FMT_UR, psbt, len, frag);
    int n = e ? qrt_encoder_parts(e) : -1;
    qrt_encoder_free(e);
    return n;
}

static int pmofn_parts(const uint8_t *psbt, size_t len)
{
    qrt_encoder_t *e = qrt_encoder_new_frag(QRT_FMT_PMOFN, psbt, len, 0);
    int n = e ? qrt_encoder_parts(e) : -1;
    qrt_encoder_free(e);
    return n;
}

// Out through an encoder and back in through the scan parser.
static int qr_round_trip(int fmt, const uint8_t *psbt, size_t len)
{
    qrt_encoder_t *e = qrt_encoder_new_frag(fmt, psbt, len, 0);
    qrt_parser_t *q = qrt_parser_new();
    static char part[4096];
    static uint8_t got[QRT_MAX_SIGNED_PSBT];
    size_t gl = 0;
    int ok = e && q;
    for (int k = 0; ok && !qrt_parser_complete(q) && k < 4 * qrt_encoder_parts(e) + 8; k++)
        ok = qrt_encoder_next(e, part, sizeof part) == 0 &&
             qrt_parser_feed(q, part, strlen(part)) == 0;
    ok = ok && qrt_parser_complete(q) &&
         qrt_parser_result(q, got, sizeof got, &gl) == 0 &&
         gl == len && memcmp(got, psbt, len) == 0;
    qrt_encoder_free(e);
    qrt_parser_free(q);
    return ok;
}

static void one_shape(const shape_t *sh)
{
    static uint8_t unsigned_b[4096], full[QRT_MAX_SIGNED_PSBT], trim[QRT_MAX_SIGNED_PSBT];
    static uint8_t tx_full[8192], tx_trim[8192], tx_merge[8192], tx_merge_full[8192];
    char nm[128];
    wpsbt_summary_t s;
    size_t ul = build(sh, unsigned_b, sizeof unsigned_b), fl = 0, tl = 0;

    snprintf(nm, sizeof nm, "export %s: loads and signs", sh->name);
    int ok = ul > 0 && kiss_psbt_load(unsigned_b, ul, &s) == 0 && s.status != WPSBT_STOP &&
             kiss_psbt_sign(full, sizeof full, &fl) == 0;
    chkb(nm, ok);
    kiss_psbt_free();
    if (!ok) {
        printf("  status %d reason %s\n", s.status, s.reason);
        return;
    }
    if (sh->version == 2) {
        // A v2 coordinator may finalize the returned copy without combining,
        // so it needs every field: sent whole.
        snprintf(nm, sizeof nm, "export %s: goes whole", sh->name);
        chkb(nm, kiss_psbt_export_copy(full, fl, trim, sizeof trim, &tl) == 0 &&
                 tl == fl && memcmp(trim, full, fl) == 0);
        return;
    }
    snprintf(nm, sizeof nm, "export %s: trims", sh->name);
    chkb(nm, kiss_psbt_export_copy(full, fl, trim, sizeof trim, &tl) == 0 && tl > 0 && tl < fl);

    size_t a = final_from_bytes(full, fl, tx_full, sizeof tx_full);
    size_t b = final_from_bytes(trim, tl, tx_trim, sizeof tx_trim);
    size_t c = final_merged(unsigned_b, ul, trim, tl, tx_merge, sizeof tx_merge);
    size_t d = final_merged(unsigned_b, ul, full, fl, tx_merge_full, sizeof tx_merge_full);
    snprintf(nm, sizeof nm, "export %s: full and trimmed finalize to one tx", sh->name);
    chkb(nm, a > 0 && a == b && memcmp(tx_full, tx_trim, a) == 0);
    snprintf(nm, sizeof nm, "export %s: coordinator merge of trimmed, same tx", sh->name);
    chkb(nm, a > 0 && c == a && memcmp(tx_full, tx_merge, a) == 0);
    snprintf(nm, sizeof nm, "export %s: coordinator merge of full, same tx", sh->name);
    chkb(nm, a > 0 && d == a && memcmp(tx_full, tx_merge_full, a) == 0);
    snprintf(nm, sizeof nm, "export %s: signatures verify from the trimmed copy alone", sh->name);
    chkb(nm, sigs_verify_alone(trim, tl));
    snprintf(nm, sizeof nm, "export %s: keeps what a finalizer reads, drops the rest", sh->name);
    chkb(nm, fields_ok(trim, tl, sh));
    char fa[9], fb[9];
    snprintf(nm, sizeof nm, "export %s: same signature fingerprint", sh->name);
    chkb(nm, kiss_psbt_sig_fingerprint(full, fl, fa) == 0 &&
             kiss_psbt_sig_fingerprint(trim, tl, fb) == 0 && strcmp(fa, fb) == 0);
    snprintf(nm, sizeof nm, "export %s: UR and pMofN carry it intact", sh->name);
    chkb(nm, qr_round_trip(QRT_FMT_UR, trim, tl) && qr_round_trip(QRT_FMT_PMOFN, trim, tl));

    printf("SIZE: %-34s %5zu -> %5zu bytes | UR frames %2d -> %2d (3.5in %2d -> %2d) | "
           "pMofN %2d -> %2d\n", sh->name, fl, tl,
           ur_parts(full, fl, 0), ur_parts(trim, tl, 0),
           ur_parts(full, fl, 42), ur_parts(trim, tl, 42),
           pmofn_parts(full, fl), pmofn_parts(trim, tl));
}

// A PSBT carrying fields this signer does not trim through -- the silent
// payment shares, proofs and output info -- goes out byte for byte.
static void untouched_when_unknown(void)
{
    static uint8_t b[4096], out[4096];
    const shape_t sh = {"sp guard", 1, {84}, 2, 2, 0};
    size_t bl = build(&sh, b, sizeof b), ol = 0;
    struct wally_psbt *p = NULL;
    uint8_t key[34] = {0x07}, share[33];
    memset(key + 1, 0x02, 33);
    memset(share, 0x03, 33);
    int ok = wally_psbt_from_bytes(b, bl, 0, &p) == WALLY_OK &&
             wally_map_add(&p->unknowns, key, sizeof key, share, sizeof share) == WALLY_OK &&
             wally_psbt_to_bytes(p, 0, b, sizeof b, &bl) == WALLY_OK;
    wally_psbt_free(p);
    chkb("export: a PSBT with silent payment fields is copied whole",
         ok && kiss_psbt_export_copy(b, bl, out, sizeof out, &ol) == 0 &&
         ol == bl && memcmp(out, b, bl) == 0);
    // A buffer too small for the copy is a refusal, never a short copy
    // reported as whole: wally answers OK with the length it needed.
    {
        static uint8_t sb[4096], signed_b[QRT_MAX_SIGNED_PSBT], small[64];
        const shape_t one = {"small cap", 1, {84}, 2, 0, 0};
        size_t ul = build(&one, sb, sizeof sb), fl = 0, cl = 1;
        wpsbt_summary_t s;
        int ok2 = ul > 0 && kiss_psbt_load(sb, ul, &s) == 0 &&
                  kiss_psbt_sign(signed_b, sizeof signed_b, &fl) == 0;
        kiss_psbt_free();
        chkb("export: a buffer too small is refused",
             ok2 && kiss_psbt_export_copy(signed_b, fl, small, sizeof small, &cl) != 0 &&
             cl == 0);
    }
    chkb("export: garbage is refused, so the caller sends the full PSBT",
         kiss_psbt_export_copy((const uint8_t *)"not a psbt", 10, out, sizeof out, &ol) != 0 &&
         ol == 0);
}

int test_psbt_export(void)
{
    fails = 0;
    printf("---- signed PSBT export copy ----\n");
    uint8_t seed[BIP39_SEED_LEN_512];
    size_t sl = 0;
    if (bip39_mnemonic_to_seed("abandon abandon abandon abandon abandon abandon "
                               "abandon abandon abandon abandon abandon about",
                               NULL, seed, sizeof seed, &sl) != WALLY_OK ||
        bip32_key_from_seed(seed, sizeof seed, BIP32_VER_MAIN_PRIVATE, 0, &e_master) != WALLY_OK ||
        bip32_key_get_fingerprint(&e_master, e_fp, 4) != WALLY_OK) {
        printf("FAIL: export fixture keys\n");
        return 1;
    }
    wally_bzero(seed, sizeof seed);
    kiss_set_network(0);
    if (kiss_session_open(NULL) != 0) {
        printf("FAIL: export session open\n");
        return 1;
    }
    static const shape_t shapes[] = {
        {"native 1-in",                  1, {84},             2,  0, 1},
        {"native 2-in",                  2, {84, 84},         2,  0, 1},
        {"native 4-in, payout prev txs", 4, {84, 84, 84, 84}, 20, 0, 1},
        {"nested 1-in",                  1, {49},             2,  0, 1},
        {"nested 2-in",                  2, {49, 49},         2,  0, 1},
        {"legacy 1-in",                  1, {44},             2,  0, 1},
        {"mixed legacy+native",          2, {44, 84},         2,  0, 0},
        {"native 2-in, PSBTv2",          2, {84, 84},         2,  2, 1},
        {"nested 1-in, PSBTv2",          1, {49},             2,  2, 0},
    };
    for (size_t i = 0; i < sizeof shapes / sizeof shapes[0]; i++)
        one_shape(&shapes[i]);
    untouched_when_unknown();
    wally_bzero(&e_master, sizeof e_master);
    return fails;
}
