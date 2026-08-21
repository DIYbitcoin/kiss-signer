// Parser fuzz harness — junk in must NEVER crash, NEVER sign, NEVER show READY.
// Build: sim/build_fuzz.sh -> /tmp/kissfuzz (ASAN+UBSAN so memory bugs actually
// fail instead of silently corrupting). Deterministic (fixed xorshift seed), so
// a failure reproduces exactly.
//
// Attack surfaces, matching what a hostile coordinator, a printed QR from a
// stranger, or whoever holds the SD card can reach:
//   1. kiss_psbt_load on pure-random bytes      -> must reject or STOP
//   2. kiss_psbt_load on truncated/bit-flipped
//      REAL psbts                                  -> may parse, must not crash;
//                                                     status stays well-formed
//   3. qrt_parser_feed on random text + corrupted
//      UR parts                                    -> must not crash or complete
//   4. kiss_psbt_load on base64 TEXT ("cHNidP")  -> the branch random binary
//      never reaches; junk never READY, whitespace in real b64 still loads
//   5. kiss_seed_from_plaintext on junk/digits/entropy -> returns 0 only for
//      a mnemonic that validates; degenerate entropy refused, and the retired
//      numeric SeedQR shape refused even when it is well formed
//   6. kiss_address_validate on junk + flipped
//      real addresses                              -> always a defined verdict,
//                                                     never a false network claim
//   7. sd_seed_open on random/multi-flipped blobs  -> never a mnemonic that is
//      not the sealed one (single flips/truncations: sim/test_sdseed.c; here
//      the same surface runs under ASAN with random shapes)
//   8. kef_parse/kef_sniff/kiss_kef_open on random and damaged envelopes ->
//      never crash, never a foreign plaintext, and no input is ever claimed
//      by BOTH the KEF sniff and the plaintext reader (the restore router
//      depends on that disjointness)
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "kiss_crypto.h"
#include "kiss_kef.h"
#include "kiss_psbt.h"
#include "kiss_seed.h"
#include "kiss_seed_sd.h"
#include "qr_transport.h"
#include "bytewords.h"

#include <wally_address.h>
#include <wally_bip32.h>
#include <wally_bip39.h>
#include <wally_core.h>
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

// ---- hand-built UR frames --------------------------------------------------
// qrt_encoder_new_frag cannot produce the shape below: a conforming encoder
// zero-pads every fragment to one nominal length, so every frame of a stream
// carries the same data_len. The attack is two frames that are each perfectly
// well formed on their own -- right bytewords CRC, CBOR header agreeing with
// the URI path, seq_len and message_len inside the caps -- and disagree only
// with EACH OTHER. Only a hand-built frame gets there, which is why the
// stream-corrupting loop above never found it.
static size_t cbor_uint(uint8_t *p, uint32_t v) {
    if (v < 24)    { p[0] = (uint8_t)v; return 1; }
    if (v < 256)   { p[0] = 0x18; p[1] = (uint8_t)v; return 2; }
    if (v < 65536) { p[0] = 0x19; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; return 3; }
    p[0] = 0x1a; p[1] = (uint8_t)(v >> 24); p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 8); p[4] = (uint8_t)v; return 5;
}
static size_t cbor_bstr_hdr(uint8_t *p, size_t n) {
    if (n < 24)  { p[0] = (uint8_t)(0x40 | n); return 1; }
    if (n < 256) { p[0] = 0x58; p[1] = (uint8_t)n; return 2; }
    p[0] = 0x59; p[1] = (uint8_t)(n >> 8); p[2] = (uint8_t)n; return 3;
}
static int mk_ur_part(char *out, size_t outsz, uint32_t seq_num, uint32_t seq_len,
                      uint32_t msg_len, uint32_t checksum, size_t frag_len) {
    static uint8_t cbor[1024];
    if (frag_len + 16 > sizeof cbor) return -1;
    size_t o = 0;
    cbor[o++] = 0x85;                       // CBOR array of 5
    o += cbor_uint(cbor + o, seq_num);
    o += cbor_uint(cbor + o, seq_len);
    o += cbor_uint(cbor + o, msg_len);
    o += cbor_uint(cbor + o, checksum);
    o += cbor_bstr_hdr(cbor + o, frag_len);
    for (size_t i = 0; i < frag_len; i++) cbor[o + i] = (uint8_t)(0xA5 ^ i);
    o += frag_len;
    char *bw = NULL;
    if (!bytewords_encode(cbor, o, &bw)) return -1;   // real CRC32, appended here
    int n = snprintf(out, outsz, "ur:crypto-psbt/%u-%u/%s",
                     (unsigned)seq_num, (unsigned)seq_len, bw);
    bytewords_free(bw);
    return (n > 0 && (size_t)n < outsz) ? n : -1;
}

int main(void)
{
    s_rng = 0xC0FFEE01u;
    kiss_seed_store("abandon abandon abandon abandon abandon abandon "
                      "abandon abandon abandon abandon abandon about");
    kiss_set_network(0);
    kiss_set_script(WSCRIPT_NATIVE);
    if (kiss_session_open(NULL) != 0) { printf("FAIL: session open\n"); return 1; }

    static uint8_t valid[4096], buf[4096], sig[4096];
    size_t vlen = mk_valid_psbt(valid, sizeof valid);
    chkb("valid fixture builds", vlen > 0);
    wpsbt_summary_t sum;

    // the fixture itself must be READY (so the mutations below start from good)
    chkb("fixture loads READY",
         kiss_psbt_load(valid, vlen, &sum) == 0 && sum.status == WPSBT_READY);
    kiss_psbt_free();

    // ---- 1. pure random bytes: must never come out READY ----
    for (int i = 0; i < 2000; i++) {
        size_t n = 1 + rnd() % 3000;
        for (size_t j = 0; j < n; j += 4) {
            uint32_t r = rnd();
            memcpy(buf + j, &r, (n - j) < 4 ? (n - j) : 4);
        }
        memset(&sum, 0, sizeof sum);
        int rc = kiss_psbt_load(buf, n, &sum);
        if (rc == 0 && sum.status == WPSBT_READY) {
            printf("FAIL: random junk parsed READY (iter %d len %zu)\n", i, n);
            fails++;
        }
        // a rejected load must also refuse to sign
        size_t sw = 0;
        if (rc != 0 && kiss_psbt_sign(sig, sizeof sig, &sw) == 0) {
            printf("FAIL: sign succeeded after rejected load (iter %d)\n", i);
            fails++;
        }
        kiss_psbt_free();
    }
    printf("PASS: 2000 random-junk PSBTs never READY, never signable\n");

    // ---- 2a. every truncation of a real PSBT ----
    for (size_t n = 0; n < vlen; n++) {
        memcpy(buf, valid, n);
        memset(&sum, 0, sizeof sum);
        int rc = kiss_psbt_load(buf, n, &sum);
        if (rc == 0 && sum.status == WPSBT_READY) {
            printf("FAIL: truncated PSBT (%zu of %zu bytes) READY\n", n, vlen);
            fails++;
        }
        kiss_psbt_free();
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
        int rc = kiss_psbt_load(buf, vlen, &sum);
        if (rc == 0 && sum.status != WPSBT_READY && sum.status != WPSBT_CAUTION &&
            sum.status != WPSBT_STOP) {
            printf("FAIL: bit-flip produced undefined status %d (iter %d)\n", sum.status, i);
            fails++;
        }
        kiss_psbt_free();
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

    // ---- 3c. two CRC-valid UR frames that disagree on fragment length ----
    // The reducer XORs one part's body into another's. With a short simple part
    // and a long mixed part it used to read (long - short) bytes past the short
    // one's heap block: silent corruption the final CRC usually caught, a
    // LoadProhibited reboot when the block sat near the end of a heap region.
    // ASAN turns that into a hard failure here. Both orderings, and seq_num 3..6
    // because the mixed part's degree comes from the PRNG and degree 1 would
    // land on the simple-part path instead.
    {
        static char f_short[512], f_long[1200];
        const uint32_t MSG = 16, SUM = 0x1234ABCDu;
        int ok = 1;
        for (int order = 0; order < 2 && ok; order++) {
            for (uint32_t seq = 3; seq <= 6 && ok; seq++) {
                if (mk_ur_part(f_short, sizeof f_short, order ? seq : 1, 2, MSG, SUM, 8) < 0 ||
                    mk_ur_part(f_long, sizeof f_long, order ? 1 : seq, 2, MSG, SUM, 400) < 0) {
                    ok = 0; break;
                }
                qrt_parser_t *p = qrt_parser_new();
                if (!p) { ok = 0; break; }
                qrt_parser_feed(p, order ? f_long : f_short, strlen(order ? f_long : f_short));
                qrt_parser_feed(p, order ? f_short : f_long, strlen(order ? f_short : f_long));
                // 16 bytes of "message" is not a PSBT, so completing at all
                // would mean the decoder stitched something out of two frames
                // that never described the same message.
                if (qrt_parser_complete(p)) {
                    printf("FAIL: mismatched-fragment UR frames completed (order %d seq %u)\n",
                           order, seq);
                    fails++;
                }
                qrt_parser_free(p);
            }
        }
        chkb("mismatched-fragment UR frames build", ok);
    }
    printf("PASS: mismatched-fragment UR frames rejected, no OOB\n");

    // ---- 3d. a checksum-bad final frame is a terminal transfer error ----
    // cUR correctly completes the decoder with an unsuccessful checksum, but
    // the transport used to report the final frame as generic junk and every
    // later frame as an accepted no-op. The scan UI could then never finish or
    // recover without being closed manually.
    {
        static char first[512], last[512];
        const uint32_t MSG = 16, BAD_SUM = 0x1234ABCDu;
        int built = mk_ur_part(first, sizeof first, 1, 2, MSG, BAD_SUM, 8) > 0 &&
                    mk_ur_part(last, sizeof last, 2, 2, MSG, BAD_SUM, 8) > 0;
        qrt_parser_t *p = qrt_parser_new();
        chkb("checksum-bad UR terminal fixture builds", built && p);
        if (built && p) {
            chkb("checksum-bad UR first part accepted",
                 qrt_parser_feed(p, first, strlen(first)) == 0);
            chkb("checksum-bad UR final part reported",
                 qrt_parser_feed(p, last, strlen(last)) == QRT_FEED_CORRUPT);
            chkb("checksum-bad UR error latched",
                 qrt_parser_feed(p, first, strlen(first)) == QRT_FEED_CORRUPT);
            qrt_parser_reset(p);
            chkb("checksum-bad UR reset permits a new transfer",
                 qrt_parser_feed(p, first, strlen(first)) == 0);
        }
        qrt_parser_free(p);
    }

    // ---- 4. base64 PSBT text ----
    // The loader's "cHNidP" branch strips whitespace and base64-decodes before
    // the binary parser ever runs. Sections 1/2 never reach it: random binary
    // has no chance of starting with those six bytes.
    {
        char *b64 = NULL;
        chkb("b64 fixture encodes",
             wally_base64_from_bytes(valid, vlen, 0, &b64) == WALLY_OK && b64);
        size_t blen = strlen(b64);

        memset(&sum, 0, sizeof sum);
        chkb("real base64 loads READY",
             kiss_psbt_load((const uint8_t *)b64, blen, &sum) == 0 &&
             sum.status == WPSBT_READY);
        kiss_psbt_free();

        // coordinators wrap lines and pad; the strip must survive that
        static char ws[6000];
        size_t wo = 0;
        for (size_t i = 0; i < blen && wo + 3 < sizeof ws; i++) {
            ws[wo++] = b64[i];
            if (i % 40 == 39) { ws[wo++] = '\r'; ws[wo++] = '\n'; }
            else if (i % 17 == 16) ws[wo++] = ' ';
        }
        ws[wo] = 0;
        memset(&sum, 0, sizeof sum);
        chkb("whitespace-injected base64 still READY",
             kiss_psbt_load((const uint8_t *)ws, wo, &sum) == 0 &&
             sum.status == WPSBT_READY);
        kiss_psbt_free();

        static const char B64C[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        static char tb[1300];

        // junk behind the magic prefix: enters the branch, must never be READY
        for (int i = 0; i < 2000; i++) {
            memcpy(tb, "cHNidP", 6);
            size_t n = 6 + rnd() % 1200;
            for (size_t j = 6; j < n; j++) tb[j] = B64C[rnd() % 64];
            memset(&sum, 0, sizeof sum);
            int rc = kiss_psbt_load((const uint8_t *)tb, n, &sum);
            if (rc == 0 && sum.status == WPSBT_READY) {
                printf("FAIL: b64 junk parsed READY (iter %d)\n", i);
                fails++;
            }
            size_t sw = 0;
            if (rc != 0 && kiss_psbt_sign(sig, sizeof sig, &sw) == 0) {
                printf("FAIL: sign succeeded after rejected b64 (iter %d)\n", i);
                fails++;
            }
            kiss_psbt_free();
        }

        // valid base64 of NON-psbt bytes: decode succeeds, parse must not
        for (int i = 0; i < 500; i++) {
            size_t n = 6 + rnd() % 2000;
            memcpy(buf, "psbt\xff", 5);
            for (size_t j = 5; j < n; j++) buf[j] = (uint8_t)rnd();
            char *jb = NULL;
            if (wally_base64_from_bytes(buf, n, 0, &jb) != WALLY_OK || !jb)
                continue;
            memset(&sum, 0, sizeof sum);
            if (kiss_psbt_load((const uint8_t *)jb, strlen(jb), &sum) == 0 &&
                sum.status == WPSBT_READY) {
                printf("FAIL: b64 of junk bytes READY (iter %d)\n", i);
                fails++;
            }
            kiss_psbt_free();
            wally_free_string(jb);
        }

        // mutated REAL base64: like 2b, may still parse; must not crash and
        // any success carries a defined status
        for (int i = 0; i < 4000; i++) {
            memcpy(tb, b64, blen + 1);
            int muts = 1 + rnd() % 8;
            for (int m = 0; m < muts; m++) tb[rnd() % blen] = B64C[rnd() % 64];
            memset(&sum, 0, sizeof sum);
            if (kiss_psbt_load((const uint8_t *)tb, blen, &sum) == 0 &&
                sum.status != WPSBT_READY && sum.status != WPSBT_CAUTION &&
                sum.status != WPSBT_STOP) {
                printf("FAIL: mutated b64 undefined status %d (iter %d)\n",
                       sum.status, i);
                fails++;
            }
            kiss_psbt_free();
        }
        wally_free_string(b64);
    }
    printf("PASS: base64 PSBTs: junk never READY, whitespace survives, no crash\n");

    // ---- 5. kiss_seed_from_plaintext: an opened envelope becomes a wallet ----
    // Two parse paths now (raw entropy, plain text) on the far side of a
    // password. The one invariant across both: returning 0 means `out` holds a
    // mnemonic that validates. Junk may fail, it must never half-succeed.
    {
        // The canonical zero-entropy vector, which this parser used to hand
        // back as a wallet and now refuses (kiss_seed_degenerate). It stays
        // here as the SHAPE for the malformed cases below -- eleven words, a
        // junk word, a short buffer -- because those must be refused for their
        // own reasons and not because of what these particular words are.
        static const char *MN =
            "abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon abandon abandon about";
        char so[512];

        // A mnemonic with real entropy behind it, built rather than pasted so
        // it cannot quietly become another published vector.
        uint8_t good[16];
        char GOOD[512];
        for (size_t i = 0; i < sizeof good; i++) good[i] = (uint8_t)(i * 37 + 11);
        chkb("plaintext: built a non degenerate vector",
             kiss_seed_from_entropy(good, sizeof good, GOOD, sizeof GOOD) == 0);

        chkb("plaintext: real mnemonic accepted",
             kiss_seed_from_plaintext(GOOD, strlen(GOOD), so, sizeof so) == 0 &&
             strcmp(so, GOOD) == 0);

        char pad[600];
        int pn = snprintf(pad, sizeof pad, "  %s\r\n", GOOD);
        chkb("plaintext: padded mnemonic accepted and trimmed",
             kiss_seed_from_plaintext(pad, (size_t)pn, so, sizeof so) == 0 &&
             strcmp(so, GOOD) == 0);

        chkb("plaintext: the abandon vector refused as text",
             kiss_seed_from_plaintext(MN, strlen(MN), so, sizeof so) != 0);

        // The retired numeric SeedQR shape: 4 digits per wordlist index. This
        // reader used to accept it; it must not now, at either length and
        // whatever the indices spell. Built from the good vector rather than
        // the abandon one so this is testing the REMOVAL and not the
        // degenerate gate that would refuse it anyway.
        {
            char dg[97];
            const char *w = GOOD;
            size_t d = 0;
            for (int k = 0; k < 12 && d + 4 < sizeof dg; k++) {
                char one[12]; size_t m = 0;
                while (*w && *w != ' ' && m + 1 < sizeof one) one[m++] = *w++;
                one[m] = 0; if (*w == ' ') w++;
                int idx = -1;
                for (int i = 0; i < 2048; i++) {
                    const char *lw = NULL;
                    if (kiss_seed_word(i, &lw) == 0 && lw && strcmp(lw, one) == 0) {
                        idx = i; break;
                    }
                }
                if (idx < 0) break;
                d += (size_t)snprintf(dg + d, sizeof dg - d, "%04d", idx);
            }
            chkb("plaintext: built a well formed 48 digit SeedQR", d == 48);
            chkb("plaintext: a well formed SeedQR is refused",
                 kiss_seed_from_plaintext(dg, 48, so, sizeof so) != 0);
        }
        static const char NUM[] =
            "000000000000000000000000000000000000000000000003";
        chkb("plaintext: the abandon vector as digits refused",
             kiss_seed_from_plaintext(NUM, 48, so, sizeof so) != 0);

        chkb("plaintext: junk word refused",
             kiss_seed_from_plaintext("abandon abandon abandon abandon abandon abandon "
                                 "abandon abandon abandon abandon abandon zzzzzz",
                                 95, so, sizeof so) != 0);
        chkb("plaintext: eleven words refused",
             kiss_seed_from_plaintext(MN, strlen(MN) - 6, so, sizeof so) != 0);
        chkb("plaintext: tiny out buffer refused",
             kiss_seed_from_plaintext(GOOD, strlen(GOOD), so, 10) != 0);

        // degenerate entropy out of an envelope: all must be refused
        uint8_t ent[32];
        memset(ent, 0x00, 32);
        chkb("plaintext: all-zero 16B entropy refused",
             kiss_seed_from_plaintext((const char *)ent, 16, so, sizeof so) != 0);
        chkb("plaintext: all-zero 32B entropy refused",
             kiss_seed_from_plaintext((const char *)ent, 32, so, sizeof so) != 0);
        memset(ent, 0xFF, 32);
        chkb("plaintext: all-ones entropy refused",
             kiss_seed_from_plaintext((const char *)ent, 32, so, sizeof so) != 0);
        memset(ent, 0xAA, 16);
        chkb("plaintext: single repeated byte refused",
             kiss_seed_from_plaintext((const char *)ent, 16, so, sizeof so) != 0);
        memset(ent, 0x00, 16); ent[7] = 0x01;      // one bit in 128
        chkb("plaintext: near-empty entropy refused",
             kiss_seed_from_plaintext((const char *)ent, 16, so, sizeof so) != 0);

        // random junk at every shape: bytes, printable, digit strings at the
        // lengths the numeric shape used to occupy. Success is only legal with
        // a validating mnemonic (random 16/32-byte lengths ARE real entropy and
        // legitimately pass).
        for (int i = 0; i < 2000; i++) {
            char jb[301];
            size_t n = 1 + rnd() % 300;
            int mode = (int)(rnd() % 3);
            for (size_t j = 0; j < n; j++) {
                uint32_t r = rnd();
                jb[j] = mode == 0 ? (char)r
                      : mode == 1 ? (char)(0x20 + r % 95)
                                  : (char)('0' + r % 10);
            }
            if (mode == 2 && rnd() % 2) n = rnd() % 2 ? 48 : 96;
            memset(so, 0x5A, sizeof so);
            int rc = kiss_seed_from_plaintext(jb, n, so, sizeof so);
            if (rc == 0 && kiss_seed_validate(so) != 0) {
                printf("FAIL: plaintext junk accepted without a valid mnemonic (iter %d)\n", i);
                fails++;
            }
            if (rc != 0 && so[0] != 0) {
                printf("FAIL: plaintext rejection left bytes in out (iter %d)\n", i);
                fails++;
            }
        }
    }
    printf("PASS: backup plaintext: junk never yields an invalid mnemonic, rejections wipe\n");

    // ---- 6. kiss_address_validate: a stranger's address string ----
    // Reaches bech32/bech32m decode, base58check decode and the SP prefix
    // check. Junk must be INVALID; one flipped character in a real address
    // must never keep a network verdict (both encodings checksum a single
    // substitution to death).
    {
        uint8_t seed[BIP39_SEED_LEN_512]; size_t sl = 0;
        struct ext_key master, k;
        bip39_mnemonic_to_seed(
            "abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon abandon abandon about", NULL,
            seed, sizeof seed, &sl);
        bip32_key_from_seed(seed, sizeof seed, BIP32_VER_MAIN_PRIVATE, 0, &master);
        const uint32_t p[5] = {H + 84, H, H, 0, 0};
        bip32_key_from_parent_path(&master, p, 5, BIP32_FLAG_KEY_PRIVATE, &k);

        char *a_bc = NULL, *a_tb = NULL, *a_p2pkh = NULL, *a_p2pkh_t = NULL;
        chkb("addr fixtures derive",
             wally_bip32_key_to_addr_segwit(&k, "bc", 0, &a_bc) == WALLY_OK &&
             wally_bip32_key_to_addr_segwit(&k, "tb", 0, &a_tb) == WALLY_OK &&
             wally_bip32_key_to_address(&k, WALLY_ADDRESS_TYPE_P2PKH,
                 WALLY_ADDRESS_VERSION_P2PKH_MAINNET, &a_p2pkh) == WALLY_OK &&
             wally_bip32_key_to_address(&k, WALLY_ADDRESS_TYPE_P2PKH,
                 WALLY_ADDRESS_VERSION_P2PKH_TESTNET, &a_p2pkh_t) == WALLY_OK);

        chkb("addr: mainnet bech32 is CURRENT (session is mainnet)",
             kiss_address_validate(a_bc) == WADDR_CURRENT_NETWORK);
        chkb("addr: testnet bech32 is WRONG",
             kiss_address_validate(a_tb) == WADDR_WRONG_NETWORK);
        chkb("addr: mainnet p2pkh is CURRENT",
             kiss_address_validate(a_p2pkh) == WADDR_CURRENT_NETWORK);
        chkb("addr: testnet p2pkh is WRONG",
             kiss_address_validate(a_p2pkh_t) == WADDR_WRONG_NETWORK);
        chkb("addr: NULL is INVALID", kiss_address_validate(NULL) == WADDR_INVALID);
        chkb("addr: empty is INVALID", kiss_address_validate("") == WADDR_INVALID);

        static const char B32C[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
        static const char B58C[] =
            "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
        const char *fix[4] = {a_bc, a_tb, a_p2pkh, a_p2pkh_t};
        for (int f = 0; f < 4; f++) {
            const char *cs = f < 2 ? B32C : B58C;
            size_t csn = strlen(cs), an = strlen(fix[f]);
            for (int i = 0; i < 1000; i++) {
                char m[128];
                memcpy(m, fix[f], an + 1);
                size_t pos = rnd() % an;
                char c;
                do { c = cs[rnd() % csn]; } while (c == m[pos]);
                m[pos] = c;
                if (kiss_address_validate(m) != WADDR_INVALID) {
                    printf("FAIL: flipped addr kept a verdict (fix %d iter %d: %s)\n",
                           f, i, m);
                    fails++;
                }
            }
        }

        // random printable junk, with the real prefixes spliced on sometimes
        // so the deeper decode paths run instead of dying on the first char
        static const char *pre[] = {"bc1q", "bc1p", "tb1q", "1", "3", "sp1q", "tsp1q"};
        for (int i = 0; i < 2000; i++) {
            char jb[160];
            size_t n = 1 + rnd() % 150;
            for (size_t j = 0; j < n; j++) jb[j] = (char)(0x21 + rnd() % 94);
            if (rnd() % 2) {
                const char *pf = pre[rnd() % 7];
                size_t pl = strlen(pf);
                memcpy(jb, pf, pl < n ? pl : n);
            }
            jb[n] = 0;
            if (kiss_address_validate(jb) != WADDR_INVALID) {
                printf("FAIL: junk addr got a verdict (iter %d: %s)\n", i, jb);
                fails++;
            }
        }
        wally_free_string(a_bc); wally_free_string(a_tb);
        wally_free_string(a_p2pkh); wally_free_string(a_p2pkh_t);
    }
    printf("PASS: addresses: junk and single flips always INVALID, fixtures verdict right\n");

    // ---- 7. sd_seed_open under ASAN: random shapes ----
    // test_sdseed.c already walks every single-byte flip, truncation and lying
    // length deterministically. This adds what only a fuzzer under ASAN buys:
    // random multi-bit damage and random well-shaped blobs, with the one
    // invariant that matters -- success means EXACTLY the sealed mnemonic.
    {
        static const char *MN =
            "abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon abandon abandon about";
        uint8_t key[32], blob[SDSEED_MAX_BLOB];
        size_t bl = 0;
        char so[WSEED_SD_MAX_PLAIN];
        for (int i = 0; i < 32; i++) key[i] = (uint8_t)(0x42 + i);

        chkb("sd: seal", sd_seed_seal(key, MN, blob, sizeof blob, &bl) == 0 && bl);
        chkb("sd: roundtrip", sd_seed_open(key, blob, bl, so, sizeof so) == 0 &&
                              strcmp(so, MN) == 0);

        static uint8_t dmg[SDSEED_MAX_BLOB];
        for (int i = 0; i < 4000; i++) {
            memcpy(dmg, blob, bl);
            int flips = 1 + rnd() % 16;
            for (int f = 0; f < flips; f++)
                dmg[rnd() % bl] ^= (uint8_t)(1u << (rnd() % 8));
            int rc = sd_seed_open(key, dmg, bl, so, sizeof so);
            // an even number of flips can land on one bit and cancel out, so
            // success is legal -- but only with the true mnemonic
            if (rc == 0 && strcmp(so, MN) != 0) {
                printf("FAIL: damaged sd blob opened to a DIFFERENT mnemonic (iter %d)\n", i);
                fails++;
            }
            if (rc != 0 && so[0] != 0) {
                printf("FAIL: sd rejection left bytes in out (iter %d)\n", i);
                fails++;
            }
        }

        // a wrong key differing by one random bit must never open it
        for (int i = 0; i < 500; i++) {
            uint8_t wk[32];
            memcpy(wk, key, 32);
            wk[rnd() % 32] ^= (uint8_t)(1u << (rnd() % 8));
            if (sd_seed_open(wk, blob, bl, so, sizeof so) == 0) {
                printf("FAIL: near-miss key opened the blob (iter %d)\n", i);
                fails++;
            }
        }

        // random bytes behind a real magic, at header-consistent lengths
        for (int i = 0; i < 2000; i++) {
            uint32_t ct = (uint32_t)(16 * (1 + rnd() % 17));
            size_t n = SDSEED_HDR_LEN + ct + SDSEED_TAG_LEN;
            memcpy(dmg, SDSEED_MAGIC, SDSEED_MAGIC_LEN);
            for (size_t j = SDSEED_MAGIC_LEN; j < n; j++) dmg[j] = (uint8_t)rnd();
            dmg[24] = (uint8_t)ct; dmg[25] = (uint8_t)(ct >> 8);
            dmg[26] = (uint8_t)(ct >> 16); dmg[27] = (uint8_t)(ct >> 24);
            if (sd_seed_open(key, dmg, n, so, sizeof so) == 0) {
                printf("FAIL: random sd blob opened (iter %d)\n", i);
                fails++;
            }
        }
    }
    printf("PASS: sd blobs: random damage never yields a different mnemonic\n");

    // ---- 8. KEF envelopes under ASAN ----
    // test_kef.c walks every single-byte flip and truncation
    // deterministically at the default work factor. Here: random multi-bit
    // damage and random shapes, on a low-iteration envelope so thousands of
    // PBKDF2 runs stay affordable, plus the router disjointness property.
    {
        static const uint8_t KP[16] = { 0xa5, 1, 2, 3, 4, 5, 6, 7,
                                        8, 9, 10, 11, 12, 13, 14, 0xff };
        uint8_t key[32], iv[KEF_IV_LEN], ct[16], tag16[16];
        static uint8_t env[KEF_MAX_ENV], dmg[KEF_MAX_ENV], back[KEF_MAX_ENV];
        char sw[WSEED_MAX_MNEMONIC];
        size_t blen = 0;

        // hand-build a valid envelope at 10001 iterations (the floor's raw
        // form) with the same primitives open uses
        for (size_t i = 0; i < KEF_IV_LEN; i++) iv[i] = (uint8_t)(0x60 + i);
        chkb("kef: fixture key derives",
             wally_pbkdf2_hmac_sha256((const uint8_t *)"fz", 2,
                                      (const uint8_t *)"id", 2, 0, 10001,
                                      key, 32) == WALLY_OK);
        chkb("kef: fixture gcm runs",
             kiss_kef_test_gcm(key, iv, KP, sizeof KP, ct, tag16) == 0);
        size_t elen = kef_emit_header(env, sizeof env, (const uint8_t *)"id",
                                      2, KEF_VERSION_AES_GCM, 10001);
        memcpy(env + elen, iv, KEF_IV_LEN);
        memcpy(env + elen + KEF_IV_LEN, ct, sizeof KP);
        memcpy(env + elen + KEF_IV_LEN + sizeof KP, tag16, KEF_TAG_LEN);
        elen += KEF_IV_LEN + sizeof KP + KEF_TAG_LEN;
        chkb("kef: fixture opens",
             kiss_kef_open("fz", 2, env, elen, back, sizeof back, &blen) == 0
                 && blen == sizeof KP && memcmp(back, KP, sizeof KP) == 0);

        // random multi-bit damage: success only with the true plaintext,
        // rejection only with a zeroed output buffer
        for (int i = 0; i < 800; i++) {
            memcpy(dmg, env, elen);
            int flips = 1 + rnd() % 16;
            for (int f = 0; f < flips; f++)
                dmg[rnd() % elen] ^= (uint8_t)(1u << (rnd() % 8));
            int rc = kiss_kef_open("fz", 2, dmg, elen, back, sizeof back,
                                   &blen);
            if (rc == 0 && (blen != sizeof KP
                            || memcmp(back, KP, sizeof KP) != 0)) {
                printf("FAIL: damaged kef opened to foreign bytes (iter %d)\n",
                       i);
                fails++;
            }
            if (rc != 0) {
                for (size_t j = 0; j < sizeof back; j++)
                    if (back[j]) {
                        printf("FAIL: kef rejection left bytes (iter %d)\n",
                               i);
                        fails++;
                        break;
                    }
            }
        }
        printf("PASS: 800 damaged kef envelopes never a foreign plaintext\n");

        // random buffers through parse and sniff; whenever the sniff claims
        // one, the plaintext reader must not (and neither may crash)
        int both = 0, sniffed = 0;
        for (int i = 0; i < 6000; i++) {
            size_t n = 1 + rnd() % KEF_MAX_ENV;
            for (size_t j = 0; j < n; j += 4) {
                uint32_t r = rnd();
                memcpy(dmg + j, &r, (n - j) < 4 ? (n - j) : 4);
            }
            if (i & 1) {
                // half the runs: a well-formed header so sniff hits happen
                size_t h = kef_emit_header(dmg, n, (const uint8_t *)"FZ", 2,
                                           KEF_VERSION_AES_GCM, 10);
                if (!h || n < h + 17) continue;
            }
            kef_env_t e;
            kef_parse(dmg, n, &e);
            if (kef_sniff(dmg, n)) {
                sniffed++;
                if (kiss_seed_from_plaintext((const char *)dmg, n, sw,
                                      sizeof sw) == 0) both++;
            }
        }
        chkb("kef: sniff fired on the crafted half", sniffed > 1000);
        chkb("kef: no input claimed by both sniff and seed parser",
             both == 0);
    }
    printf("PASS: kef random shapes: no crash, no false claim\n");

    kiss_session_close();
    printf(fails ? "\n%d FUZZ FAIL\n" : "\nALL FUZZ PASS\n", fails);
    return fails ? 1 : 0;
}
