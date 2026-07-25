// KISS Wallet crypto layer — step 1: libwally in the build + test vectors.
// No hand-rolled crypto: everything below is libwally calls.
#include "wallet_crypto.h"
#include "wallet_sp.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wally_core.h>
#include <wally_bip39.h>
#include <wally_bip32.h>
#include <wally_address.h>
#include <wally_crypto.h>

#include "wallet_seed.h"

// Standard BIP39 test vector — ONLY the boot selftest uses it now; the live
// wallet derives from the seed the user stored (wallet_seed.c). Steps 3-6
// shipped on this as "the dev seed"; step 7 retired it from the hot path.
#ifndef KISS_RELEASE   // release builds carry NO dev seed material at all
static const char *DEV_MNEMONIC =
    "abandon abandon abandon abandon abandon abandon "
    "abandon abandon abandon abandon abandon about";

// Master fingerprint of DEV_MNEMONIC with an empty passphrase.
static const uint8_t EXPECTED_FP[4] = {0x73, 0xC5, 0xDA, 0x0A};
#endif

static int fingerprint_of(const char *passphrase, uint8_t out[4])
{
    if (wally_init(0) != WALLY_OK)
        return 1;

    char words[WSEED_MAX_MNEMONIC];
    if (wallet_seed_load(words, sizeof words) != 0)
        return 2;                          // no seed on this device yet

    // single exit below: seed/master are wiped on EVERY path, not just success
    uint8_t seed[BIP39_SEED_LEN_512];
    size_t seed_len = 0;
    struct ext_key master = {0};
    uint8_t fp[BIP32_KEY_FINGERPRINT_LEN];
    int rc = 0;

    int mrc = bip39_mnemonic_to_seed(words, passphrase, seed, sizeof(seed), &seed_len);
    wally_bzero(words, sizeof words);
    if (mrc != WALLY_OK || seed_len != sizeof(seed))
        rc = 3;
    else if (bip32_key_from_seed(seed, sizeof(seed), BIP32_VER_MAIN_PRIVATE, 0, &master) != WALLY_OK)
        rc = 4;
    else if (bip32_key_get_fingerprint(&master, fp, sizeof(fp)) != WALLY_OK)
        rc = 5;

    if (rc == 0 && out)
        memcpy(out, fp, 4);

    wally_bzero(seed, sizeof(seed));
    wally_bzero(&master, sizeof(master));
    return rc;
}

int wallet_entropy_mix(const uint8_t a[32], const uint8_t b[32], uint8_t out[32])
{
    if (!a || !b || !out)
        return -1;
    uint8_t cat[64];
    memcpy(cat, a, 32);
    memcpy(cat + 32, b, 32);
    int rc = wally_sha256(cat, sizeof cat, out, 32) == WALLY_OK ? 0 : -1;
    wally_bzero(cat, sizeof cat);
    return rc;
}

int wallet_fingerprint(const char *passphrase, uint8_t out_fingerprint[4])
{
    if (passphrase && !passphrase[0])
        passphrase = NULL;                 // empty = no passphrase (BIP39)
    return fingerprint_of(passphrase, out_fingerprint);
}

// ---- step 4: wallet session (master key in RAM between unlock and lock) ----
static struct ext_key s_master;
static bool s_session;

int wallet_session_open(const char *passphrase)
{
    if (passphrase && !passphrase[0])
        passphrase = NULL;
    if (wally_init(0) != WALLY_OK)
        return 1;
    wallet_session_close();                // never derive over a stale master
    char words[WSEED_MAX_MNEMONIC];
    if (wallet_seed_load(words, sizeof words) != 0)
        return 2;                          // no seed stored: wizard first
    uint8_t seed[BIP39_SEED_LEN_512];
    size_t seed_len = 0;
    int mrc = bip39_mnemonic_to_seed(words, passphrase, seed, sizeof(seed), &seed_len);
    wally_bzero(words, sizeof words);
    int rc = 0;
    if (mrc != WALLY_OK || seed_len != sizeof(seed))
        rc = 3;
    else if (bip32_key_from_seed(seed, sizeof(seed), BIP32_VER_MAIN_PRIVATE, 0, &s_master) != WALLY_OK)
        rc = 4;
    wally_bzero(seed, sizeof(seed));       // wiped on every path, incl. BIP39 failure
    if (rc != 0)
        wally_bzero(&s_master, sizeof(s_master));   // no partial key on failure
    s_session = (rc == 0);
    return rc;
}

void wallet_session_close(void)
{
    wally_bzero(&s_master, sizeof(s_master));
    s_session = false;
}

const struct ext_key *wallet_session_master(void)
{
    return s_session ? &s_master : NULL;
}

// ---- network. The master key is network-free; testnet only changes the coin
// type (84h/1h), the address hrp (tb) and the tpub serialization, so it can be
// flipped any time without re-opening the session.
static bool s_testnet;
void wallet_set_network(int testnet) { s_testnet = testnet != 0; }
int wallet_testnet(void) { return s_testnet; }

static int s_script;   // WSCRIPT_NATIVE / _NESTED / _LEGACY
void wallet_set_script(int script)
{
    s_script = (script == WSCRIPT_NESTED || script == WSCRIPT_LEGACY) ? script : WSCRIPT_NATIVE;
}
int wallet_script(void) { return s_script; }

// BIP44/49/84 purpose for the active script type.
static uint32_t script_purpose(void)
{
    return s_script == WSCRIPT_LEGACY ? 44 : s_script == WSCRIPT_NESTED ? 49 : 84;
}

static int account_key(struct ext_key *out)    // m/<purpose>h/<coin>h/0h
{
    const uint32_t path[3] = {
        BIP32_INITIAL_HARDENED_CHILD + script_purpose(),
        BIP32_INITIAL_HARDENED_CHILD + (s_testnet ? 1 : 0),
        BIP32_INITIAL_HARDENED_CHILD + 0,
    };
    return bip32_key_from_parent_path(&s_master, path, 3, BIP32_FLAG_KEY_PRIVATE, out) == WALLY_OK ? 0 : 1;
}

int wallet_session_address(int change, uint32_t index, char *out, size_t out_len)
{
    if (!s_session)
        return 1;
    struct ext_key acct, child;
    if (account_key(&acct))
        return 2;
    const uint32_t path[2] = {(uint32_t)change, index};
    int rc = bip32_key_from_parent_path(&acct, path, 2, BIP32_FLAG_KEY_PUBLIC, &child) == WALLY_OK ? 0 : 3;
    if (rc == 0) {
        char *addr = NULL;
        int ok;
        if (s_script == WSCRIPT_NATIVE) {
            ok = wally_bip32_key_to_addr_segwit(&child, s_testnet ? "tb" : "bc", 0, &addr) == WALLY_OK;
        } else {
            uint32_t flags = s_script == WSCRIPT_LEGACY ? WALLY_ADDRESS_TYPE_P2PKH
                                                        : WALLY_ADDRESS_TYPE_P2SH_P2WPKH;
            uint32_t ver = s_script == WSCRIPT_LEGACY ? (s_testnet ? 0x6F : 0x00)
                                                      : (s_testnet ? 0xC4 : 0x05);
            ok = wally_bip32_key_to_address(&child, flags, ver, &addr) == WALLY_OK;
        }
        if (ok && addr) {
            if (strlen(addr) + 1 <= out_len)
                strcpy(out, addr);
            else
                rc = 4;
            wally_free_string(addr);
        } else {
            rc = 5;
        }
    }
    wally_bzero(&acct, sizeof(acct));
    wally_bzero(&child, sizeof(child));
    return rc;
}

// BIP352 silent-payment receive address (sp1/tsp1) for this wallet + network.
// Static and reusable by design - no index. Derived entirely on-device.
int wallet_session_sp_address(char *out, size_t out_len)
{
    if (!s_session)
        return 1;
    uint8_t scan[33], spend[33];
    if (sp_receive_keys(&s_master, s_testnet, scan, spend) != 0)
        return 2;
    return sp_address_encode(scan, spend, s_testnet, out, out_len) == 0 ? 0 : 3;
}

// Scan-key export: sp([fp/352h/coinh/0h]spscan1...) so a scanner (Sparrow/
// Frigate) can detect payments to this wallet's silent-payment address. Carries
// the scan PRIVATE key (never the spend key), so the holder can see but never
// spend. The origin path is the SP account, mirroring how the wpkh descriptor
// shows [fp/84h/coinh/0h].
int wallet_session_sp_scan_export(char *out, size_t out_len)
{
    if (!s_session)
        return 1;
    uint8_t scan_priv[32], spend_pub[33];
    int rc = sp_scan_export_keys(&s_master, s_testnet, scan_priv, spend_pub) == 0 ? 0 : 2;
    uint8_t fp[BIP32_KEY_FINGERPRINT_LEN];
    if (rc == 0 && bip32_key_get_fingerprint(&s_master, fp, sizeof(fp)) != WALLY_OK)
        rc = 3;
    if (rc == 0) {
        char key[128];
        if (sp_scan_encode(scan_priv, spend_pub, s_testnet, key, sizeof key) != 0) {
            rc = 4;
        } else {
            int n = snprintf(out, out_len, "sp([%02x%02x%02x%02x/352h/%dh/0h]%s)",
                             fp[0], fp[1], fp[2], fp[3], s_testnet ? 1 : 0, key);
            if (n < 0 || (size_t)n >= out_len)
                rc = 5;
        }
        wally_bzero(key, sizeof key);
    }
    wally_bzero(scan_priv, sizeof scan_priv);
    return rc;
}

int wallet_session_descriptor(char *out, size_t out_len)
{
    if (!s_session)
        return 1;
    struct ext_key acct;
    if (account_key(&acct))
        return 2;
    uint8_t fp[BIP32_KEY_FINGERPRINT_LEN];
    int rc = bip32_key_get_fingerprint(&s_master, fp, sizeof(fp)) == WALLY_OK ? 0 : 3;
    if (rc == 0) {
        char *xpub = NULL;
        if (s_testnet)
            acct.version = BIP32_VER_TEST_PRIVATE;   // serializes as tpub below
        if (bip32_key_to_base58(&acct, BIP32_FLAG_KEY_PUBLIC, &xpub) == WALLY_OK) {
            // descriptor wrapper matches the script type; xpub/tpub carries the
            // account key (Sparrow derives the address style from the wrapper)
            const char *pre = s_script == WSCRIPT_LEGACY ? "pkh("
                            : s_script == WSCRIPT_NESTED ? "sh(wpkh(" : "wpkh(";
            const char *post = s_script == WSCRIPT_NESTED ? "))" : ")";
            int n = snprintf(out, out_len, "%s[%02x%02x%02x%02x/%uh/%dh/0h]%s/<0;1>/*%s",
                             pre, fp[0], fp[1], fp[2], fp[3],
                             (unsigned)script_purpose(), s_testnet ? 1 : 0, xpub, post);
            if (n < 0 || (size_t)n >= out_len)
                rc = 4;
            wally_free_string(xpub);
        } else {
            rc = 5;
        }
    }
    wally_bzero(&acct, sizeof(acct));
    return rc;
}

// SLIP-132 version bytes: how BlueWallet (and Electrum-family wallets) tell
// the address type from the extended-key prefix instead of a descriptor.
static uint32_t slip132_version(void)
{
    if (s_script == WSCRIPT_NATIVE) return s_testnet ? 0x045F1CF6 : 0x04B24746; // vpub/zpub
    if (s_script == WSCRIPT_NESTED) return s_testnet ? 0x044A5262 : 0x049D7CB2; // upub/ypub
    return s_testnet ? 0x043587CF : 0x0488B21E;                                 // tpub/xpub
}

int wallet_session_bw_export(char *out, size_t out_len)
{
    if (!s_session)
        return 1;
    struct ext_key acct;
    if (account_key(&acct))
        return 2;
    uint8_t fp[BIP32_KEY_FINGERPRINT_LEN];
    int rc = bip32_key_get_fingerprint(&s_master, fp, sizeof(fp)) == WALLY_OK ? 0 : 3;
    if (rc == 0) {
        uint8_t ser[BIP32_SERIALIZED_LEN];
        if (bip32_key_serialize(&acct, BIP32_FLAG_KEY_PUBLIC, ser, sizeof ser) == WALLY_OK) {
            uint32_t v = slip132_version();
            ser[0] = (uint8_t)(v >> 24); ser[1] = (uint8_t)(v >> 16);
            ser[2] = (uint8_t)(v >> 8);  ser[3] = (uint8_t)v;
            char *b58 = NULL;
            if (wally_base58_from_bytes(ser, sizeof ser, BASE58_FLAG_CHECKSUM, &b58) == WALLY_OK) {
                int n = snprintf(out, out_len, "[%02x%02x%02x%02x/%u'/%d'/0']%s",
                                 fp[0], fp[1], fp[2], fp[3],
                                 (unsigned)script_purpose(), s_testnet ? 1 : 0, b58);
                if (n < 0 || (size_t)n >= out_len)
                    rc = 4;
                wally_free_string(b58);
            } else {
                rc = 5;
            }
            wally_bzero(ser, sizeof ser);
        } else {
            rc = 6;
        }
    }
    wally_bzero(&acct, sizeof(acct));
    return rc;
}

// The boot selftest proves the CRYPTO STACK against the published vector —
// deliberately independent of whatever seed the user stored.
#ifdef KISS_RELEASE
// Release: the dev-mnemonic selftest is compiled out entirely (the string must
// not exist in the binary). wally still needs its one-time init at boot.
int wallet_selftest(uint8_t out_fingerprint[4])
{
    if (out_fingerprint)
        memset(out_fingerprint, 0, 4);
    return wally_init(0) == WALLY_OK ? -1 : 1;   // -1 = "not available"
}
#else
int wallet_selftest(uint8_t out_fingerprint[4])
{
    uint8_t fp[4] = {0};
    if (wally_init(0) != WALLY_OK)
        return 1;
    if (bip39_mnemonic_validate(NULL, DEV_MNEMONIC) != WALLY_OK)
        return 2;
    uint8_t seed[BIP39_SEED_LEN_512];
    size_t seed_len = 0;
    if (bip39_mnemonic_to_seed(DEV_MNEMONIC, NULL, seed, sizeof(seed), &seed_len) != WALLY_OK ||
        seed_len != sizeof(seed))
        return 3;
    struct ext_key master;
    int rc = bip32_key_from_seed(seed, sizeof(seed), BIP32_VER_MAIN_PRIVATE, 0, &master) == WALLY_OK ? 0 : 4;
    if (rc == 0 && bip32_key_get_fingerprint(&master, fp, sizeof fp) != WALLY_OK)
        rc = 5;
    wally_bzero(seed, sizeof seed);
    wally_bzero(&master, sizeof master);
    if (out_fingerprint)
        memcpy(out_fingerprint, fp, 4);
    if (rc != 0)
        return rc;
    return memcmp(fp, EXPECTED_FP, 4) == 0 ? 0 : 6;
}
#endif  // KISS_RELEASE
