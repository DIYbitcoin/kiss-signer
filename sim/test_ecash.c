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

// The signer's own signature, from the NUT-13 vector seed's key 0, verifies
// under that key's published public key. A public account cannot sign.
static void test_sign(void)
{
    uint8_t seed[BIP39_SEED_LEN_512], h[32], sig[64], xo[32];
    size_t written = 0;
    struct ext_key master, acct, watch;
    char *xpub = NULL;
    int ok = bip39_mnemonic_to_seed(NUT13_MNEMONIC, NULL, seed, sizeof seed,
                                    &written) == WALLY_OK &&
             bip32_key_from_seed(seed, sizeof seed, BIP32_VER_MAIN_PRIVATE, 0,
                                 &master) == WALLY_OK &&
             kiss_ecash_account(&master, &acct) == 0 &&
             bip32_key_to_base58(&acct, BIP32_FLAG_KEY_PUBLIC, &xpub) == WALLY_OK &&
             bip32_key_from_base58(xpub, &watch) == WALLY_OK;
    if (xpub)
        wally_free_string(xpub);
    ecchk("sign setup", ok);
    if (!ok)
        return;
    memset(h, 0x5a, sizeof h);
    ecchk("sign rc", kiss_ecash_sign(&acct, 0, h, sig) == 0);
    ecchk("signature verifies under NUT-13 key 0",
          from_hex(NUT13_P2PK[0] + 2, xo, 32) && sp_schnorr_verify(xo, h, sig) == 0);
    ecchk("signature fails under NUT-13 key 1",
          from_hex(NUT13_P2PK[1] + 2, xo, 32) && sp_schnorr_verify(xo, h, sig) != 0);
    ecchk("public account cannot sign", kiss_ecash_sign(&watch, 0, h, sig) != 0);
    ecchk("hardened counter cannot sign",
          kiss_ecash_sign(&acct, BIP32_INITIAL_HARDENED_CHILD, h, sig) != 0);
    wally_bzero(seed, sizeof seed);
    wally_bzero(&master, sizeof master);
    wally_bzero(&acct, sizeof acct);
}

int test_ecash(void);
int test_ecash(void)
{
    ec_fails = 0;
    test_nut13_vectors();
    test_nut00_vectors();
    test_nut11_sigall_vectors();
    test_sign();
    test_session_export();
    printf("ecash: %d failure(s)\n", ec_fails);
    return ec_fails;
}
