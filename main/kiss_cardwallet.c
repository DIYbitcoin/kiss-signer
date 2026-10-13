// The wallet on a Satochip card. See kiss_cardwallet.h.
#include "kiss_cardwallet.h"

#include <string.h>

#include <wally_core.h>
#include <wally_crypto.h>

#include "kiss_satochip.h"
#include "kiss_wipe.h"

#define H(x) (BIP32_INITIAL_HARDENED_CHILD + (x))

static const uint32_t PURPOSES[3] = { 44, 49, 84 };

static kiss_satocard s_card;
static bool s_connected;
static bool s_active;
static uint8_t s_fp[4];
static struct ext_key s_acct[3][2];     // [purpose][coin]
static bool s_acct_ok[3][2];

static int purpose_index(uint32_t purpose)
{
    for (int i = 0; i < 3; i++)
        if (PURPOSES[i] == purpose)
            return i;
    return -1;
}

void kiss_cardwallet_close(void)
{
    kiss_satocard_close(&s_card);
    s_connected = false;
    s_active = false;
    kiss_wipe(s_fp, sizeof s_fp);
    kiss_wipe(s_acct, sizeof s_acct);
    memset(s_acct_ok, 0, sizeof s_acct_ok);
}

int kiss_cardwallet_connect(kiss_sc_xfer_fn xfer, void *ctx, kiss_sc_rng_fn rng,
                            int *pin_tries)
{
    kiss_cardwallet_close();
    kiss_satocard_init(&s_card, SC_APPLET_SATOCHIP, xfer, ctx, rng);
    int rc = kiss_satocard_open(&s_card);
    if (rc != SC_OK) {
        kiss_cardwallet_close();
        return rc;
    }
    if (!s_card.seeded) {
        kiss_cardwallet_close();
        return SC_ERR_SETUP;          // a Satochip with no wallet on it yet
    }
    if (pin_tries)
        *pin_tries = s_card.pin_tries;
    s_connected = true;
    return SC_OK;
}

// hash160 of a compressed public key, the first 4 bytes of which are a
// BIP32 fingerprint.
static void hash160(const uint8_t pub33[33], uint8_t out[20])
{
    (void)wally_hash160(pub33, 33, out, 20);
}

static int read_keys(void (*progress)(int, int))
{
    const int total = 13;
    int done = 0;
    uint8_t pub[33], cc[32], h[20], parent160[20];

    int rc = kiss_satochip_pubkey(&s_card, NULL, 0, pub, cc);
    if (rc != SC_OK)
        return rc;
    hash160(pub, h);
    memcpy(s_fp, h, 4);
    if (progress) progress(++done, total);

    for (int p = 0; p < 3; p++) {
        for (int coin = 0; coin < 2; coin++) {
            uint32_t path[3] = { H(PURPOSES[p]), H((uint32_t)coin), H(0) };
            rc = kiss_satochip_pubkey(&s_card, path, 2, pub, cc);
            if (rc != SC_OK)
                return rc;
            hash160(pub, parent160);
            if (progress) progress(++done, total);
            rc = kiss_satochip_pubkey(&s_card, path, 3, pub, cc);
            if (rc != SC_OK)
                return rc;
            hash160(pub, h);
            if (bip32_key_init(coin ? BIP32_VER_TEST_PUBLIC : BIP32_VER_MAIN_PUBLIC,
                               3, H(0), cc, 32, pub, 33, NULL, 0, h, 20,
                               parent160, 20, &s_acct[p][coin]) != WALLY_OK)
                return SC_ERR_PROTOCOL;
            s_acct_ok[p][coin] = true;
            if (progress) progress(++done, total);
        }
    }
    return SC_OK;
}

int kiss_cardwallet_unlock(const char *pin, size_t pin_len, int *tries_left,
                           void (*progress)(int done, int total))
{
    if (!s_connected)
        return SC_ERR_ORDER;
    int rc = kiss_satocard_verify_pin(&s_card, pin, pin_len, tries_left);
    if (rc == SC_ERR_PIN)
        return rc;                     // the card is still open for another try
    if (rc == SC_OK)
        rc = kiss_satocard_genuine(&s_card);
    if (rc == SC_OK)
        rc = read_keys(progress);
    if (rc != SC_OK) {
        kiss_cardwallet_close();
        return rc;
    }
    s_active = true;
    return SC_OK;
}

bool kiss_cardwallet_active(void)
{
    return s_active;
}

bool kiss_cardwallet_needs_2fa(void)
{
    return s_connected && s_card.needs_2fa;
}

int kiss_cardwallet_fingerprint(uint8_t out[4])
{
    if (!s_active || !out)
        return 1;
    memcpy(out, s_fp, 4);
    return 0;
}

const struct ext_key *kiss_cardwallet_account(uint32_t purpose, bool testnet)
{
    int p = purpose_index(purpose);
    if (!s_active || p < 0 || !s_acct_ok[p][testnet ? 1 : 0])
        return NULL;
    return &s_acct[p][testnet ? 1 : 0];
}

int kiss_cardwallet_sign(const uint32_t *path, int depth, const uint8_t hash32[32],
                         uint8_t *der, size_t *der_len)
{
    *der_len = 0;
    if (!s_active)
        return SC_ERR_ORDER;
    return kiss_satochip_sign(&s_card, path, depth, hash32, der, der_len);
}
