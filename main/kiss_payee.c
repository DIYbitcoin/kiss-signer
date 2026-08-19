// See kiss_payee.h. Device backend = NVS namespace "kissp" (its own, so a payee
// wipe cannot touch settings, the seed or the receive guard). Host backend = a
// RAM table, same as kiss_usage: the desktop tests and the sim run in one
// process, so persistence across boot is a device only concern.
#include "kiss_payee.h"
#include "kiss_crypto.h"
#include "kiss_seed.h"

#include <stdio.h>
#include <string.h>

#include <wally_bip32.h>
#include <wally_core.h>
#include <wally_crypto.h>

// The id is a keyed digest of the destination, never the destination. The salt
// comes from the master key itself, so:
//
//   - flash holds no address. A reader who dumps it learns how many distinct
//     payees a wallet has, and nothing about who they are.
//   - it is not a dictionary either. Without the master key an attacker cannot
//     hash a candidate address and look it up, which a bare sha256(addr) would
//     have let anyone do against the whole UTXO set.
//   - two wallets on one device produce unrelated ids for the same payee, so
//     the namespace cannot link a decoy to the wallet behind the passphrase.
//
// 56 bits, because the key is the id in hex and NVS caps a key at 15 chars. A
// collision would say "paid before" about a destination that was not, at odds
// no owner reaches: a wallet with a thousand payees is one chance in 10^11.
#define PAYEE_ID_HEX 14

static int payee_id(const char *dest, char out[PAYEE_ID_HEX + 1])
{
    const struct ext_key *m = kiss_session_master();
    if (!m || !dest || !*dest)
        return -1;
    uint8_t salt[32], id[32];
    // Domain separated, so this digest can never collide with another use of
    // the same key material (the silent payment scan key derives from it too).
    uint8_t pre[13 + 32 + 33];
    memcpy(pre, "kiss/payee/1", 12);
    pre[12] = 0;
    memcpy(pre + 13, m->chain_code, 32);
    memcpy(pre + 13 + 32, m->priv_key, 33);
    int rc = wally_sha256(pre, sizeof pre, salt, sizeof salt) == WALLY_OK ? 0 : -1;
    wally_bzero(pre, sizeof pre);
    if (rc != 0)
        return -1;

    // salt || dest, not HMAC: the salt is a full 32 bytes of secret and the
    // input is a bounded, non attacker chosen shape, so length extension buys
    // nothing here and the device already has this one primitive.
    size_t dlen = strlen(dest);
    uint8_t buf[32 + 128];
    if (dlen > sizeof buf - 32)
        dlen = sizeof buf - 32;
    memcpy(buf, salt, 32);
    memcpy(buf + 32, dest, dlen);
    rc = wally_sha256(buf, 32 + dlen, id, sizeof id) == WALLY_OK ? 0 : -1;
    wally_bzero(salt, sizeof salt);
    wally_bzero(buf, sizeof buf);
    if (rc != 0)
        return -1;

    for (int i = 0; i < PAYEE_ID_HEX / 2; i++)
        snprintf(out + i * 2, 3, "%02x", id[i]);
    out[PAYEE_ID_HEX] = 0;
    wally_bzero(id, sizeof id);
    return 0;
}

// How many distinct payees a wallet remembers within one session. Well past a
// personal spending pattern, and the table is the only copy on the beta lane.
// Full is FULL: dropping the oldest would make the mark vanish from the payee
// paid most regularly, which is precisely the one it exists for.
#define PMAX 64
struct payee_row { char id[PAYEE_ID_HEX + 1]; };
static struct payee_row s_session[PMAX];
static int s_session_n;

static bool tab_seen(const struct payee_row *tab, int n, const char *id)
{
    for (int i = 0; i < n; i++)
        if (strcmp(tab[i].id, id) == 0)
            return true;
    return false;
}

static void tab_mark(struct payee_row *tab, int *n, const char *id)
{
    if (tab_seen(tab, *n, id) || *n >= PMAX)
        return;
    snprintf(tab[*n].id, sizeof tab[*n].id, "%s", id);
    (*n)++;
}

#ifdef ESP_PLATFORM
#include "nvs.h"

static bool persistent_seen(const char *id)
{
    nvs_handle_t h;
    if (nvs_open("kissp", NVS_READONLY, &h) != ESP_OK)
        return false;
    uint8_t v = 0;
    bool got = nvs_get_u8(h, id, &v) == ESP_OK;
    nvs_close(h);
    return got;
}

static void persistent_mark(const char *id)
{
    nvs_handle_t h;
    if (nvs_open("kissp", NVS_READWRITE, &h) != ESP_OK)
        return;
    uint8_t v = 0;
    if (nvs_get_u8(h, id, &v) != ESP_OK) {          // present is the whole value
        nvs_set_u8(h, id, 1);
        nvs_commit(h);
    }
    nvs_close(h);
}

static void persistent_wipe(void)
{
    nvs_handle_t h;
    if (nvs_open("kissp", NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}

#else   // host (sim + desktop tests): RAM table

static struct payee_row s_persistent[PMAX];
static int s_persistent_n;

static bool persistent_seen(const char *id)
{
    return tab_seen(s_persistent, s_persistent_n, id);
}

static void persistent_mark(const char *id)
{
    tab_mark(s_persistent, &s_persistent_n, id);
}

static void persistent_wipe(void)
{
    memset(s_persistent, 0, sizeof s_persistent);
    s_persistent_n = 0;
}

#endif

// The same gate kiss_usage.c argues at length, and this table wants it more.
// A receive high water mark leaks that a wallet exists; a payee table leaks how
// many counterparties it has, and the ids are an offline oracle for anyone who
// later learns the master key. Salting keeps addresses out of flash either way,
// but plaintext NVS is not where a list of payment relationships belongs.
static bool may_persist(void)
{
    return kiss_seed_mode() != WSEED_MODE_AMNESIC && kiss_seed_flash_encrypted();
}

bool kiss_payee_seen(const char *dest)
{
    char id[PAYEE_ID_HEX + 1];
    if (payee_id(dest, id) != 0)
        return false;
    if (tab_seen(s_session, s_session_n, id))
        return true;
    if (!may_persist())
        return false;
    bool got = persistent_seen(id);
    if (got) tab_mark(s_session, &s_session_n, id);
    return got;
}

void kiss_payee_mark(const char *dest)
{
    char id[PAYEE_ID_HEX + 1];
    if (payee_id(dest, id) != 0)
        return;
    tab_mark(s_session, &s_session_n, id);
    if (may_persist())
        persistent_mark(id);
}

// Second door onto the same keys: the mode change paths in kiss_seed.c flush
// the whole session table at once, the way they do for kiss_usage.
void kiss_payee_persist_session(void)
{
    if (!may_persist())
        return;
    for (int i = 0; i < s_session_n; i++)
        persistent_mark(s_session[i].id);
}

void kiss_payee_forget_session(void)
{
    memset(s_session, 0, sizeof s_session);
    s_session_n = 0;
}

void kiss_payee_wipe(void)
{
    persistent_wipe();
    kiss_payee_forget_session();
}
