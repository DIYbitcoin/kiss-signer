// sim/test_ecash.c: Cashu ecash keys (NUT-13 P2PK derivation).
//
// The vectors are NUT-13's own, from cashubtc/nuts tests/13-tests.md,
// "P2PK Derivation": five public keys from one published mnemonic.
#include <stdio.h>
#include <string.h>

#include <wally_bip32.h>
#include <wally_bip39.h>
#include <wally_core.h>

#include "kiss_crypto.h"
#include "kiss_ecash.h"
#include "kiss_sp.h"

static int ec_fails;

static void ecchk(const char *name, int ok)
{
    if (ok) printf("PASS: ecash: %s\n", name);
    else { printf("FAIL: ecash: %s\n", name); ec_fails++; }
}

static void to_hex(const uint8_t *b, size_t n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = hx[b[i] >> 4];
        out[2 * i + 1] = hx[b[i] & 15];
    }
    out[2 * n] = 0;
}

static const char *const NUT13_MNEMONIC =
    "half depart obvious quality work element tank gorilla view sugar "
    "picture humble";

static const char *const NUT13_P2PK[5] = {
    "021693d45f4fdf610ae641fedb0944fb460fbb8264f21c19d2626c3da755fcbbcb",
    "0395461ab678058c0ed6aa39f38dda490eaa163e9ad27070b23ec3d06b41e07535",
    "02a05e4e593a633e9b4405f01c9632c8afde24cb613017a1aee56fd76291ad26d1",
    "033addea25c3873b93d67d536c61c9d9c993f6efd8b9dfa657951b66b5001e51dd",
    "03c964bdf42fc82b6c574615746eeca37527a24f1fdfc1b34a732c53843b5744a5",
};

static void test_nut13_vectors(void)
{
    uint8_t seed[BIP39_SEED_LEN_512];
    size_t written = 0;
    struct ext_key master, acct, watch;
    int ok = bip39_mnemonic_to_seed(NUT13_MNEMONIC, NULL, seed, sizeof seed,
                                    &written) == WALLY_OK &&
             bip32_key_from_seed(seed, sizeof seed, BIP32_VER_MAIN_PRIVATE, 0,
                                 &master) == WALLY_OK;
    ecchk("vector seed and master", ok);
    if (!ok)
        return;
    ecchk("account rc", kiss_ecash_account(&master, &acct) == 0);

    // The watch only side: the public half of the same account, as a wallet
    // holding only the export would have it.
    char *xpub = NULL;
    ok = bip32_key_to_base58(&acct, BIP32_FLAG_KEY_PUBLIC, &xpub) == WALLY_OK &&
         bip32_key_from_base58(xpub, &watch) == WALLY_OK;
    ecchk("account xpub round trip", ok);
    if (xpub)
        wally_free_string(xpub);

    for (uint32_t i = 0; i < 5; i++) {
        uint8_t pub[33];
        char hex[67], name[64];
        int rc = kiss_ecash_pubkey(&acct, i, pub);
        to_hex(pub, 33, hex);
        snprintf(name, sizeof name, "NUT-13 P2PK key %u", (unsigned)i);
        ecchk(name, rc == 0 && strcmp(hex, NUT13_P2PK[i]) == 0);
        rc = ok ? kiss_ecash_pubkey(&watch, i, pub) : 1;
        to_hex(pub, 33, hex);
        snprintf(name, sizeof name, "NUT-13 P2PK key %u from the xpub",
                 (unsigned)i);
        ecchk(name, rc == 0 && strcmp(hex, NUT13_P2PK[i]) == 0);
    }

    uint8_t pub[33];
    ecchk("hardened counter refused",
          kiss_ecash_pubkey(&acct, BIP32_INITIAL_HARDENED_CHILD, pub) != 0);
    ecchk("no account refused", kiss_ecash_pubkey(NULL, 0, pub) != 0);
    ecchk("no master refused", kiss_ecash_account(NULL, &acct) != 0);

    wally_bzero(seed, sizeof seed);
    wally_bzero(&master, sizeof master);
    wally_bzero(&acct, sizeof acct);
}

// The export through the session, on the dev seed: refused while locked,
// and when open its xpub re-derives the same keys as the master does.
static void test_session_export(void)
{
    char out[160];
    kiss_session_close();
    ecchk("export refused while locked",
          kiss_session_ecash_export(out, sizeof out) != 0 && out[0] == 0);

    kiss_set_network(KISS_NET_TESTNET);   // must not change the export
    ecchk("session open", kiss_session_open(NULL) == 0);
    int rc = kiss_session_ecash_export(out, sizeof out);
    ecchk("export rc", rc == 0);
    printf("ecash export: %s\n", out);
    ecchk("export origin is the dev seed's account",
          strncmp(out, "[73c5da0a/129373h/10h/0h/0h]xpub", 32) == 0);
    char small[20];
    ecchk("export refused when it does not fit",
          kiss_session_ecash_export(small, sizeof small) != 0 &&
              small[0] == 0);

    char again[160];
    kiss_set_network(KISS_NET_MAIN);
    ecchk("export is the same on mainnet",
          kiss_session_ecash_export(again, sizeof again) == 0 &&
              strcmp(out, again) == 0);

    struct ext_key watch, acct;
    const char *x = strchr(out, ']');
    int ok = x && bip32_key_from_base58(x + 1, &watch) == WALLY_OK &&
             kiss_ecash_account(kiss_session_master(), &acct) == 0;
    ecchk("export parses as an xpub", ok);
    if (ok) {
        uint8_t a[33], b[33];
        ecchk("export key 7 matches the seed's key 7",
              kiss_ecash_pubkey(&watch, 7, a) == 0 &&
                  kiss_ecash_pubkey(&acct, 7, b) == 0 &&
                  memcmp(a, b, 33) == 0);
        wally_bzero(&acct, sizeof acct);
    }
    kiss_session_close();
}

static int from_hex(const char *h, uint8_t *out, size_t n)
{
    if (strlen(h) != 2 * n)
        return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(h + 2 * i, "%2x", &v) != 1)
            return 0;
        out[i] = (uint8_t)v;
    }
    return 1;
}

// NUT-00 tests/00-tests.md: hash_to_curve and blinded messages.
static void test_nut00_vectors(void)
{
    static const char *const htc[3][2] = {
        {"0000000000000000000000000000000000000000000000000000000000000000",
         "024cce997d3b518f739663b757deaec95bcd9473c30a14ac2fd04023a739d1a725"},
        {"0000000000000000000000000000000000000000000000000000000000000001",
         "022e7158e11c9506f1aa4248bf531298daa7febd6194f003edcd9b93ade6253acf"},
        {"0000000000000000000000000000000000000000000000000000000000000002",
         "026cdbe15362df59cd1dd3c9c11de8aedac2106eca69236ecd9fbe117af897be4f"},
    };
    for (int i = 0; i < 3; i++) {
        uint8_t x[32], y[33];
        char hex[67], name[48];
        int ok = from_hex(htc[i][0], x, 32) &&
                 kiss_ecash_hash_to_curve(x, 32, y) == 0;
        to_hex(y, 33, hex);
        snprintf(name, sizeof name, "NUT-00 hash_to_curve %d", i + 1);
        ecchk(name, ok && strcmp(hex, htc[i][1]) == 0);
    }
    static const char *const bl[2][3] = {
        {"d341ee4871f1f889041e63cf0d3823c713eea6aff01e80f1719f08f9e5be98f6",
         "99fce58439fc37412ab3468b73db0569322588f62fb3a49182d67e23d877824a",
         "033b1a9737a40cc3fd9b6af4b723632b76a67a36782596304612a6c2bfb5197e6d"},
        {"f1aaf16c2239746f369572c0784d9dd3d032d952c2d992175873fb58fae31a60",
         "f78476ea7cc9ade20f9e05e58a804cf19533f03ea805ece5fee88c8e2874ba50",
         "029bdf2d716ee366eddf599ba252786c1033f47e230248a4612a5670ab931f1763"},
    };
    for (int i = 0; i < 2; i++) {
        uint8_t x[32], r[32], b[33];
        char hex[67], name[48];
        int ok = from_hex(bl[i][0], x, 32) && from_hex(bl[i][1], r, 32) &&
                 kiss_ecash_blind(x, 32, r, b) == 0;
        to_hex(b, 33, hex);
        snprintf(name, sizeof name, "NUT-00 blinded message %d", i + 1);
        ecchk(name, ok && strcmp(hex, bl[i][2]) == 0);
    }
    uint8_t x[1] = {0}, zero[32] = {0}, b[33];
    ecchk("blinding factor zero refused", kiss_ecash_blind(x, 1, zero, b) != 0);
    uint8_t n_order[32];
    from_hex("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141",
             n_order, 32);
    ecchk("blinding factor n refused", kiss_ecash_blind(x, 1, n_order, b) != 0);
}

// NUT-11 tests/11-test.md, "SIG_ALL Test Vectors": the swap and melt
// messages' hashes, and the spec's own witness signatures verifying under
// the locking key -- which pins the scheme as BIP340 over SHA256(msg).
static const char SWAP_SECRET[] =
    "[\"P2PK\",{\"nonce\":\"c7f280eb55c1e8564e03db06973e94bc9b666d9e1ca42ad278408fe625950303\","
    "\"data\":\"030d8acedfe072c9fa449a1efe0817157403fbec460d8e79f957966056e5dd76c1\","
    "\"tags\":[[\"sigflag\",\"SIG_ALL\"]]}]";
static const char MELT_SECRET[] =
    "[\"P2PK\",{\"nonce\":\"bbf9edf441d17097e39f5095a3313ba24d3055ab8a32f758ff41c10d45c4f3de\","
    "\"data\":\"029116d32e7da635c8feeb9f1f4559eb3d9b42d400f9d22a64834d89cde0eb6835\","
    "\"tags\":[[\"sigflag\",\"SIG_ALL\"]]}]";
static const char VEC_B[] =
    "038ec853d65ae1b79b5cdbc2774150b2cb288d6d26e12958a16fb33c32d9a86c39";

static void test_nut11_sigall_vectors(void)
{
    uint8_t h[32], sig[64], xo[32];
    char hex[65];

    kiss_ecash_in_t in = {SWAP_SECRET,
        "02c97ee3d1db41cf0a3ddb601724be8711a032950811bf326f8219c50c4808d3cd"};
    kiss_ecash_out_t out = {2, VEC_B};
    int rc = kiss_ecash_sigall_hash(&in, 1, &out, 1, NULL, h);
    to_hex(h, 32, hex);
    ecchk("NUT-11 SIG_ALL swap message hash", rc == 0 &&
          !strcmp(hex, "de7f9e3ca0fcc5ed3258fcf83dbf1be7fa78a5ed6da7bf2aa60d61e9dc6eb09a"));
    ecchk("NUT-11 SIG_ALL swap witness verifies",
          from_hex("ce017ca25b1b97df2f72e4b49f69ac26a240ce14b3690a8fe619d41ccc42d3c1"
                   "282e073f85acd36dc50011638906f35b56615f24e4d03e8effe8257f6a808538", sig, 64) &&
          from_hex("0d8acedfe072c9fa449a1efe0817157403fbec460d8e79f957966056e5dd76c1", xo, 32) &&
          sp_schnorr_verify(xo, h, sig) == 0);

    kiss_ecash_in_t min = {MELT_SECRET,
        "02a9d461ff36448469dccf828fa143833ae71c689886ac51b62c8d61ddaa10028b"};
    kiss_ecash_out_t mout = {0, VEC_B};
    rc = kiss_ecash_sigall_hash(&min, 1, &mout, 1,
                                "cF8911fzT88aEi1d-6boZZkq5lYxbUSVs-HbJxK0", h);
    to_hex(h, 32, hex);
    ecchk("NUT-11 SIG_ALL melt message hash", rc == 0 &&
          !strcmp(hex, "9efa1067cc7dc870f4074f695115829c3cd817a6866c3b84e9814adf3c3cf262"));
    ecchk("NUT-11 SIG_ALL melt witness verifies",
          from_hex("478224fbe715e34f78cb33451db6fcf8ab948afb8bd04ff1a952c92e562ac0f7"
                   "c1cb5e61809410635be0aa94d0448f7f7959bd5762cc3802b0a00ff58b2da747", sig, 64) &&
          from_hex("9116d32e7da635c8feeb9f1f4559eb3d9b42d400f9d22a64834d89cde0eb6835", xo, 32) &&
          sp_schnorr_verify(xo, h, sig) == 0);

    // The strings are hashed as the mint will see them, so anything the
    // mint would hash differently is refused rather than repaired.
    kiss_ecash_in_t bad = in;
    bad.c_hex = "02C97EE3D1DB41CF0A3DDB601724BE8711A032950811BF326F8219C50C4808D3CD";
    ecchk("uppercase C refused", kiss_ecash_sigall_hash(&bad, 1, &out, 1, NULL, h) != 0);
    kiss_ecash_out_t badout = {2, "048ec853d65ae1b79b5cdbc2774150b2cb288d6d26e12958a16fb33c32d9a86c39"};
    ecchk("non compressed B_ refused", kiss_ecash_sigall_hash(&in, 1, &badout, 1, NULL, h) != 0);
    bad = in;
    bad.secret = "[\"P2PK\",\n{}]";
    ecchk("control character in a secret refused",
          kiss_ecash_sigall_hash(&bad, 1, &out, 1, NULL, h) != 0);
    ecchk("no inputs refused", kiss_ecash_sigall_hash(&in, 0, &out, 1, NULL, h) != 0);
    ecchk("empty quote id refused", kiss_ecash_sigall_hash(&in, 1, &out, 1, "", h) != 0);
}

// ---- spend requests, end to end through review and sign ----
// Every case goes through kiss_ecash_review and kiss_ecash_sign_reviewed, the
// only public way to a signature, on the dev seed's session. An attack case
// passes only when no signature comes out.

static struct ext_key s_acct;    // the dev seed's ecash account (private)

static void pubhex(uint32_t counter, char out[67])
{
    uint8_t p[33];
    kiss_ecash_pubkey(&s_acct, counter, p);
    to_hex(p, 33, out);
}

// A lock as cashu-ts writes it: no spaces, nonce, data, tags.
static void lock_secret(char *out, size_t cap, const char *nonce,
                        const char *data, const char *tags)
{
    snprintf(out, cap, "[\"P2PK\",{\"nonce\":\"%s\",\"data\":\"%s\",\"tags\":%s}]",
             nonce, data, tags);
}

// The same string as a JSON string body: " and \ escaped, nothing else.
static void json_esc(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 3 < cap; in++) {
        if (*in == '"' || *in == '\\') out[o++] = '\\';
        out[o++] = *in;
    }
    out[o] = 0;
}

static const char C_HEX[] =
    "02c97ee3d1db41cf0a3ddb601724be8711a032950811bf326f8219c50c4808d3cd";
static const char LEAVE_B[] =
    "038ec853d65ae1b79b5cdbc2774150b2cb288d6d26e12958a16fb33c32d9a86c39";
static const char NONCE_A[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char NONCE_B[] =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static const char NONCE_C[] =
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
static const char R_HEX[] =
    "99fce58439fc37412ab3468b73db0569322588f62fb3a49182d67e23d877824a";
static const char SIGALL[] = "[[\"sigflag\",\"SIG_ALL\"]]";

// The B_ of a change output locked to counter with nonce, blinded by r_hex.
static void change_b(uint32_t counter, const char *nonce, const char *r_hex,
                     char out[67])
{
    char ph[67], sec[256];
    uint8_t r[32], b[33];
    pubhex(counter, ph);
    lock_secret(sec, sizeof sec, nonce, ph, SIGALL);
    from_hex(r_hex, r, 32);
    if (kiss_ecash_blind((const uint8_t *)sec, strlen(sec), r, b) == 0)
        to_hex(b, 33, out);
    else
        out[0] = 0;
}

typedef struct {
    uint32_t counter;            // the envelope's counter
    const char *in_secret[3];    // raw secrets, NULL terminated
    const char *outs;            // the outputs array body, raw
    const char *extra;           // more envelope members, or NULL
} reqspec_t;

static void build(char *out, size_t cap, const reqspec_t *q)
{
    char ins[3000] = "";
    for (int i = 0; i < 3 && q->in_secret[i]; i++) {
        char esc[1200], one[1400];
        json_esc(q->in_secret[i], esc, sizeof esc);
        snprintf(one, sizeof one, "%s{\"secret\":\"%s\",\"C\":\"%s\"}",
                 i ? "," : "", esc, C_HEX);
        strncat(ins, one, sizeof ins - strlen(ins) - 1);
    }
    snprintf(out, cap,
             "{\"v\":1,\"mint\":\"https://testnut.cashu.space\",\"unit\":\"sat\","
             "\"counter\":%u,\"inputs\":[%s],\"outputs\":[%s]%s%s}",
             (unsigned)q->counter, ins, q->outs, q->extra ? "," : "",
             q->extra ? q->extra : "");
}

// Review then sign. Returns the first refusal, or OK with *got_sig set when
// a reply came out.
static int run(const char *req, kiss_ecash_summary_t *sum, char *reply,
               size_t cap)
{
    int rc = kiss_ecash_review(req, strlen(req), sum);
    int src = kiss_ecash_sign_reviewed(reply, cap);
    if (rc != KISS_ECASH_OK)
        return src == KISS_ECASH_OK ? -1 : rc;   // -1: a refused review signed
    return src;
}

static void refused(const char *name, const char *req, int want)
{
    kiss_ecash_summary_t sum;
    char reply[256] = "x";
    int rc = run(req, &sum, reply, sizeof reply);
    char nm[128];
    snprintf(nm, sizeof nm, "attack: %s (rc %d)", name, rc);
    ecchk(nm, rc == want && reply[0] == 0);
}

static void test_requests(void)
{
    kiss_set_network(KISS_NET_MAIN);
    if (kiss_session_open(NULL) != 0 ||
        kiss_ecash_account(kiss_session_master(), &s_acct) != 0) {
        ecchk("request session", 0);
        return;
    }
    char k7[67], k9[67], sa[400], sb[400], b8[67], req[KISS_ECASH_REQ_MAX + 64];
    pubhex(7, k7);
    pubhex(9, k9);
    lock_secret(sa, sizeof sa, NONCE_A, k7, SIGALL);
    lock_secret(sb, sizeof sb, NONCE_B, k7, SIGALL);
    change_b(8, NONCE_C, R_HEX, b8);

    char outs[600];
    snprintf(outs, sizeof outs,
             "{\"amount\":16,\"B_\":\"%s\"},{\"amount\":4,\"B_\":\"%s\","
             "\"change\":{\"counter\":8,\"nonce\":\"%s\",\"r\":\"%s\"}}",
             LEAVE_B, b8, NONCE_C, R_HEX);
    reqspec_t good = {7, {sa, sb, NULL}, outs, NULL};

    // ---- the honest request signs, and the signature is the right one ----
    build(req, sizeof req, &good);
    kiss_ecash_summary_t sum;
    char reply[256];
    int rc = run(req, &sum, reply, sizeof reply);
    ecchk("honest request signs", rc == KISS_ECASH_OK);
    ecchk("summary: 16 leaving, 4 back, 2 in, 2 out",
          sum.leaving == 16 && sum.change == 4 && sum.n_in == 2 &&
              sum.n_out == 2 && sum.n_change == 1 && sum.counter == 7 &&
              !strcmp(sum.mint, "https://testnut.cashu.space") &&
              !strcmp(sum.unit, "sat"));
    {
        kiss_ecash_in_t ins[2] = {{sa, C_HEX}, {sb, C_HEX}};
        kiss_ecash_out_t o[2] = {{16, LEAVE_B}, {4, b8}};
        uint8_t h[32], sig[64], xo[32];
        char hh[65], want[256];
        kiss_ecash_sigall_hash(ins, 2, o, 2, NULL, h);
        to_hex(h, 32, hh);
        const char *sp = strstr(reply, "\"sig\":\"");
        ecchk("reply carries the SIG_ALL hash", strstr(reply, hh) != NULL);
        ecchk("reply signature verifies under key 7",
              sp && strlen(sp + 7) >= 130 &&
                  (memcpy(want, sp + 7, 128), want[128] = 0, 1) &&
                  from_hex(want, sig, 64) && from_hex(k7 + 2, xo, 32) &&
                  sp_schnorr_verify(xo, h, sig) == 0);
    }
    ecchk("nothing kept after signing",
          kiss_ecash_sign_reviewed(reply, sizeof reply) == KISS_ECASH_E_NOTHING &&
              reply[0] == 0);
    ecchk("sign without a review refused",
          kiss_ecash_sign_reviewed(reply, sizeof reply) == KISS_ECASH_E_NOTHING);

    // ---- inputs ----
    {
        reqspec_t q = good;
        q.counter = 9;
        build(req, sizeof req, &q);
        refused("counter is not the inputs' key", req, KISS_ECASH_E_NOT_OURS);
    }
    {
        char s9[400];
        lock_secret(s9, sizeof s9, NONCE_B, k9, SIGALL);
        reqspec_t q = {7, {sa, s9, NULL}, outs, NULL};
        build(req, sizeof req, &q);
        refused("an input locked to another key", req, KISS_ECASH_E_NOT_OURS);
    }
    {
        static const char *const bad_tags[] = {
            "[[\"sigflag\",\"SIG_INPUTS\"]]",
            "[]",
            "[[\"sigflag\",\"SIG_ALL\"],[\"locktime\",\"1\"]]",
            "[[\"locktime\",\"1\"],[\"sigflag\",\"SIG_ALL\"]]",
            "[[\"sigflag\",\"SIG_ALL\"],[\"refund\",\"02c97ee3d1db41cf0a3ddb601724be8711a032950811bf326f8219c50c4808d3cd\"]]",
            "[[\"pubkeys\",\"02c97ee3d1db41cf0a3ddb601724be8711a032950811bf326f8219c50c4808d3cd\"],[\"sigflag\",\"SIG_ALL\"]]",
        };
        for (size_t i = 0; i < sizeof bad_tags / sizeof bad_tags[0]; i++) {
            char s[600], nm[64];
            lock_secret(s, sizeof s, NONCE_B, k7, bad_tags[i]);
            reqspec_t q = {7, {sa, s, NULL}, outs, NULL};
            build(req, sizeof req, &q);
            snprintf(nm, sizeof nm, "input lock with other conditions %zu", i);
            refused(nm, req, KISS_ECASH_E_LOCK);
        }
        // no tags at all is SIG_INPUTS
        char s[400];
        snprintf(s, sizeof s, "[\"P2PK\",{\"nonce\":\"%s\",\"data\":\"%s\"}]",
                 NONCE_B, k7);
        reqspec_t q = {7, {sa, s, NULL}, outs, NULL};
        build(req, sizeof req, &q);
        refused("input lock with no tags (SIG_INPUTS)", req, KISS_ECASH_E_LOCK);
        snprintf(s, sizeof s, "[\"HTLC\",{\"nonce\":\"%s\",\"data\":\"%s\",\"tags\":%s}]",
                 NONCE_B, k7, SIGALL);
        build(req, sizeof req, &q);
        refused("an HTLC input", req, KISS_ECASH_E_LOCK);
    }
    {
        // The mint reads the LAST of two "data" keys in most parsers; a
        // reader taking the first would see our key on somebody else's lock.
        char s[600];
        snprintf(s, sizeof s,
                 "[\"P2PK\",{\"nonce\":\"%s\",\"data\":\"%s\",\"data\":\"%s\",\"tags\":%s}]",
                 NONCE_B, k7, k9, SIGALL);
        reqspec_t q = {7, {sa, s, NULL}, outs, NULL};
        build(req, sizeof req, &q);
        refused("a lock with two data keys", req, KISS_ECASH_E_FORMAT);
        // In the nonce, which has no length or charset check of its own, so
        // only the escape ban can refuse it.
        snprintf(s, sizeof s,
                 "[\"P2PK\",{\"nonce\":\"\\u0030%s\",\"data\":\"%s\",\"tags\":%s}]",
                 NONCE_B + 1, k7, SIGALL);
        build(req, sizeof req, &q);
        refused("a lock with a unicode escape", req, KISS_ECASH_E_FORMAT);
        snprintf(s, sizeof s,
                 "[\"P2PK\",{\"nonce\":\"%s\",\"data\":\"%s\",\"tags\":%s,\"x\":1}]",
                 NONCE_B, k7, SIGALL);
        build(req, sizeof req, &q);
        refused("a lock with an unknown key", req, KISS_ECASH_E_FORMAT);
    }
    {
        reqspec_t q = {7, {sa, sa, NULL}, outs, NULL};
        build(req, sizeof req, &q);
        refused("the same input twice", req, KISS_ECASH_E_DUP);
    }

    // ---- change ----
    {
        char o[600], b_other[67];
        change_b(8, NONCE_C,
                 "f78476ea7cc9ade20f9e05e58a804cf19533f03ea805ece5fee88c8e2874ba50",
                 b_other);
        snprintf(o, sizeof o,
                 "{\"amount\":16,\"B_\":\"%s\",\"change\":{\"counter\":8,\"nonce\":\"%s\",\"r\":\"%s\"}}",
                 LEAVE_B, NONCE_C, R_HEX);
        reqspec_t q = {7, {sa, sb, NULL}, o, NULL};
        build(req, sizeof req, &q);
        refused("somebody else's output labelled change", req, KISS_ECASH_E_CHANGE);
        snprintf(o, sizeof o,
                 "{\"amount\":4,\"B_\":\"%s\",\"change\":{\"counter\":8,\"nonce\":\"%s\",\"r\":\"%s\"}}",
                 b_other, NONCE_C, R_HEX);
        build(req, sizeof req, &q);
        refused("change B_ from another blinding factor", req, KISS_ECASH_E_CHANGE);
        char b9[67];
        change_b(9, NONCE_C, R_HEX, b9);
        snprintf(o, sizeof o,
                 "{\"amount\":4,\"B_\":\"%s\",\"change\":{\"counter\":8,\"nonce\":\"%s\",\"r\":\"%s\"}}",
                 b9, NONCE_C, R_HEX);
        build(req, sizeof req, &q);
        refused("change locked to a different counter than claimed", req,
                KISS_ECASH_E_CHANGE);
        // With r = 0, B_ would be Y itself: give exactly that, so only the
        // scalar check can refuse it.
        {
            char ph8[67], sec8[256];
            uint8_t y[33];
            pubhex(8, ph8);
            lock_secret(sec8, sizeof sec8, NONCE_C, ph8, SIGALL);
            kiss_ecash_hash_to_curve((const uint8_t *)sec8, strlen(sec8), y);
            char yh[67];
            to_hex(y, 33, yh);
            snprintf(o, sizeof o,
                     "{\"amount\":4,\"B_\":\"%s\",\"change\":{\"counter\":8,\"nonce\":\"%s\",\"r\":\"%064d\"}}",
                     yh, NONCE_C, 0);
        }
        build(req, sizeof req, &q);
        refused("change with a zero blinding factor", req, KISS_ECASH_E_CHANGE);
        snprintf(o, sizeof o,
                 "{\"amount\":4,\"B_\":\"%s\",\"change\":{\"counter\":2147483648,\"nonce\":\"%s\",\"r\":\"%s\"}}",
                 b8, NONCE_C, R_HEX);
        build(req, sizeof req, &q);
        refused("change at a hardened counter", req, KISS_ECASH_E_FORMAT);

        // change whose secret is an input's own: already spent by this swap
        char b7a[67];
        change_b(7, NONCE_A, R_HEX, b7a);
        snprintf(o, sizeof o,
                 "{\"amount\":4,\"B_\":\"%s\",\"change\":{\"counter\":7,\"nonce\":\"%s\",\"r\":\"%s\"}}",
                 b7a, NONCE_A, R_HEX);
        build(req, sizeof req, &q);
        refused("change reusing an input's secret", req, KISS_ECASH_E_DUP);
        char b8r[67];
        change_b(8, NONCE_C,
                 "f78476ea7cc9ade20f9e05e58a804cf19533f03ea805ece5fee88c8e2874ba50",
                 b8r);
        snprintf(o, sizeof o,
                 "{\"amount\":4,\"B_\":\"%s\",\"change\":{\"counter\":8,\"nonce\":\"%s\",\"r\":\"%s\"}},"
                 "{\"amount\":4,\"B_\":\"%s\",\"change\":{\"counter\":8,\"nonce\":\"%s\",\"r\":\"f78476ea7cc9ade20f9e05e58a804cf19533f03ea805ece5fee88c8e2874ba50\"}}",
                 b8, NONCE_C, R_HEX, b8r, NONCE_C);
        build(req, sizeof req, &q);
        refused("two change outputs with one secret", req, KISS_ECASH_E_DUP);

        // An honest change output with its label left off is counted as
        // leaving: the screen shows more going, never less.
        snprintf(o, sizeof o, "{\"amount\":16,\"B_\":\"%s\"},{\"amount\":4,\"B_\":\"%s\"}",
                 LEAVE_B, b8);
        build(req, sizeof req, &q);
        kiss_ecash_summary_t s2;
        char r2[256];
        ecchk("unlabelled change counts as leaving",
              run(req, &s2, r2, sizeof r2) == KISS_ECASH_OK &&
                  s2.leaving == 20 && s2.change == 0);
    }

    // ---- the envelope ----
    {
        reqspec_t q = good;
        q.extra = "\"fee\":1";
        build(req, sizeof req, &q);
        refused("an unknown request key", req, KISS_ECASH_E_FORMAT);
        q.extra = "\"counter\":9";
        build(req, sizeof req, &q);
        refused("a repeated request key", req, KISS_ECASH_E_FORMAT);
        char o[300];
        snprintf(o, sizeof o, "{\"amount\":16.5,\"B_\":\"%s\"}", LEAVE_B);
        q = good;
        q.outs = o;
        build(req, sizeof req, &q);
        refused("a fractional amount", req, KISS_ECASH_E_FORMAT);
        snprintf(o, sizeof o, "{\"amount\":0,\"B_\":\"%s\"}", LEAVE_B);
        build(req, sizeof req, &q);
        refused("a zero amount", req, KISS_ECASH_E_FORMAT);
        snprintf(o, sizeof o, "{\"amount\":9007199254740993,\"B_\":\"%s\"}", LEAVE_B);
        build(req, sizeof req, &q);
        refused("an amount past 2^53", req, KISS_ECASH_E_FORMAT);
        snprintf(o, sizeof o, "{\"amount\":16,\"B_\":\"%s\"},{\"amount\":16,\"B_\":\"%s\"}",
                 LEAVE_B, LEAVE_B);
        build(req, sizeof req, &q);
        refused("the same output twice", req, KISS_ECASH_E_DUP);
        snprintf(o, sizeof o, "{\"amount\":16,\"B_\":\"%s\"}",
                 "038EC853D65AE1B79B5CDBC2774150B2CB288D6D26E12958A16FB33C32D9A86C39");
        build(req, sizeof req, &q);
        refused("an uppercase B_", req, KISS_ECASH_E_FORMAT);

        build(req, sizeof req, &good);
        char *v = strstr(req, "\"v\":1");
        v[4] = '2';
        refused("version 2", req, KISS_ECASH_E_VERSION);
        build(req, sizeof req, &good);
        strcat(req, "x");
        refused("trailing bytes", req, KISS_ECASH_E_FORMAT);
        char big[KISS_ECASH_REQ_MAX + 2];
        memset(big, ' ', sizeof big - 1);
        big[sizeof big - 1] = 0;
        build(req, sizeof req, &good);
        memcpy(big, req, strlen(req));
        refused("a request over 4096 bytes", big, KISS_ECASH_E_FORMAT);
    }

    // ---- the session ----
    {
        build(req, sizeof req, &good);
        kiss_ecash_summary_t s2;
        char r2[256] = "x";
        ecchk("review on the dev seed", kiss_ecash_review(req, strlen(req), &s2) == KISS_ECASH_OK);
        kiss_session_close();
        ecchk("attack: sign after lock refused",
              kiss_ecash_sign_reviewed(r2, sizeof r2) == KISS_ECASH_E_LOCKED && r2[0] == 0);
        ecchk("review while locked refused",
              kiss_ecash_review(req, strlen(req), &s2) == KISS_ECASH_E_LOCKED);
        kiss_session_open(NULL);
        ecchk("review again", kiss_ecash_review(req, strlen(req), &s2) == KISS_ECASH_OK);
        kiss_session_close();
        kiss_session_open("another passphrase");
        r2[0] = 'x';
        ecchk("attack: keys changed between review and sign",
              kiss_ecash_sign_reviewed(r2, sizeof r2) == KISS_ECASH_E_SESSION && r2[0] == 0);
        kiss_session_close();
    }
    wally_bzero(&s_acct, sizeof s_acct);
}

int test_ecash(void);
int test_ecash(void)
{
    ec_fails = 0;
    test_nut13_vectors();
    test_nut00_vectors();
    test_nut11_sigall_vectors();
    test_requests();
    test_session_export();
    printf("ecash: %d failure(s)\n", ec_fails);
    return ec_fails;
}
