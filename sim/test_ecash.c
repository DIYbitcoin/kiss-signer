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

int test_ecash(void);
int test_ecash(void)
{
    ec_fails = 0;
    test_nut13_vectors();
    test_session_export();
    printf("ecash: %d failure(s)\n", ec_fails);
    return ec_fails;
}
