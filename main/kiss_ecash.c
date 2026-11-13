// Cashu ecash keys. See kiss_ecash.h.
#include "kiss_ecash.h"

#include <stdio.h>
#include <string.h>

#include <wally_bip32.h>
#include <wally_core.h>

#include "kiss_crypto.h"

int kiss_ecash_account(const struct ext_key *master, struct ext_key *out)
{
    if (!master || !out)
        return 1;
    const uint32_t path[4] = {
        BIP32_INITIAL_HARDENED_CHILD + KISS_ECASH_PURPOSE,
        BIP32_INITIAL_HARDENED_CHILD + KISS_ECASH_ACCOUNT,
        BIP32_INITIAL_HARDENED_CHILD + 0,
        BIP32_INITIAL_HARDENED_CHILD + 0,
    };
    if (bip32_key_from_parent_path(master, path, 4, BIP32_FLAG_KEY_PRIVATE,
                                   out) != WALLY_OK) {
        wally_bzero(out, sizeof *out);
        return 2;
    }
    return 0;
}

int kiss_ecash_pubkey(const struct ext_key *account, uint32_t counter,
                      uint8_t pub33[33])
{
    // A hardened counter is not NUT-13's key, and from a public account it
    // could not be derived at all; refuse it rather than hand back a key no
    // watch only wallet would ever lock to.
    if (!account || !pub33 || counter >= BIP32_INITIAL_HARDENED_CHILD)
        return 1;
    struct ext_key child;
    int rc = bip32_key_from_parent(account, counter, BIP32_FLAG_KEY_PUBLIC,
                                   &child) == WALLY_OK ? 0 : 2;
    if (rc == 0)
        memcpy(pub33, child.pub_key, 33);
    wally_bzero(&child, sizeof child);
    return rc;
}

int kiss_session_ecash_export(char *out, size_t out_len)
{
    if (!out || out_len == 0)
        return 1;
    out[0] = 0;
    const struct ext_key *master = kiss_session_master();
    if (!master)
        return 2;   // locked, or the keys are on a card
    struct ext_key acct;
    if (kiss_ecash_account(master, &acct))
        return 3;
    uint8_t fp[BIP32_KEY_FINGERPRINT_LEN];
    int rc = bip32_key_get_fingerprint((struct ext_key *)master, fp,
                                       sizeof fp) == WALLY_OK ? 0 : 4;
    char *xpub = NULL;
    if (rc == 0) {
        acct.version = BIP32_VER_MAIN_PRIVATE;   // network free: always xpub
        if (bip32_key_to_base58(&acct, BIP32_FLAG_KEY_PUBLIC, &xpub) != WALLY_OK)
            rc = 5;
    }
    if (rc == 0) {
        int n = snprintf(out, out_len, "[%02x%02x%02x%02x/%uh/%uh/0h/0h]%s",
                         fp[0], fp[1], fp[2], fp[3],
                         KISS_ECASH_PURPOSE, KISS_ECASH_ACCOUNT, xpub);
        if (n < 0 || (size_t)n >= out_len) {
            out[0] = 0;
            rc = 6;
        }
    }
    if (xpub)
        wally_free_string(xpub);
    wally_bzero(&acct, sizeof acct);
    return rc;
}
