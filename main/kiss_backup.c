// See kiss_backup.h. Device backend = NVS namespace "kissb" (its own, so a
// backup fact never has to be untangled from settings, the seed or the usage
// history). Host backend = a small RAM table, exactly as kiss_usage.c does:
// the desktop tests and the sim run in one process, so persistence-across-boot
// is a device-only concern.
#include "kiss_backup.h"
#include "kiss_seed.h"

#include <stdio.h>
#include <string.h>

// key = "<fp8hex>", 8 of the 15 chars NVS allows. Same shape as usage_key in
// kiss_usage.c, minus the network/script suffix: which chain you are on does
// not change whether the paper is right.
static void backup_key(const uint8_t fp[4], char out[16])
{
    snprintf(out, 16, "%02x%02x%02x%02x", fp[0], fp[1], fp[2], fp[3]);
}

#define BMAX 8                      // one device, a handful of passphrase wallets
static char s_session[BMAX][16];
static int  s_session_n;

static bool tab_has(char tab[][16], int n, const char *key)
{
    for (int i = 0; i < n; i++)
        if (strcmp(tab[i], key) == 0)
            return true;
    return false;
}

static void tab_add(char tab[][16], int *n, const char *key)
{
    if (tab_has(tab, *n, key) || *n >= BMAX)
        return;
    snprintf(tab[*n], 16, "%s", key);
    (*n)++;
}

#ifdef ESP_PLATFORM
#include "nvs.h"

static bool persistent_has(const char *key)
{
    nvs_handle_t h;
    if (nvs_open("kissb", NVS_READONLY, &h) != ESP_OK)
        return false;
    uint8_t v = 0;
    bool ok = nvs_get_u8(h, key, &v) == ESP_OK && v != 0;
    nvs_close(h);
    return ok;
}

static void persistent_add(const char *key)
{
    nvs_handle_t h;
    if (nvs_open("kissb", NVS_READWRITE, &h) != ESP_OK)
        return;
    if (nvs_set_u8(h, key, 1) == ESP_OK)
        nvs_commit(h);
    nvs_close(h);
}

static void persistent_wipe(void)
{
    nvs_handle_t h;
    if (nvs_open("kissb", NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}

static int persistent_get(const char *key)
{
    nvs_handle_t h;
    if (nvs_open("kissb", NVS_READONLY, &h) != ESP_OK)
        return 0;
    uint8_t v = 0;
    if (nvs_get_u8(h, key, &v) != ESP_OK) v = 0;
    nvs_close(h);
    return v;
}

static void persistent_set(const char *key, int v)
{
    nvs_handle_t h;
    if (nvs_open("kissb", NVS_READWRITE, &h) != ESP_OK)
        return;
    if (nvs_set_u8(h, key, (uint8_t)v) == ESP_OK)
        nvs_commit(h);
    nvs_close(h);
}

#else   // host (sim + desktop tests): RAM table

static char s_persistent[BMAX][16];
static int  s_persistent_n;

static bool persistent_has(const char *key)
{
    return tab_has(s_persistent, s_persistent_n, key);
}

static void persistent_add(const char *key)
{
    tab_add(s_persistent, &s_persistent_n, key);
}

static char    s_plock_key[BMAX][16];
static uint8_t s_plock_val[BMAX];
static int     s_plock_n;

static void persistent_wipe(void)
{
    memset(s_persistent, 0, sizeof s_persistent);
    s_persistent_n = 0;
    memset(s_plock_key, 0, sizeof s_plock_key);
    memset(s_plock_val, 0, sizeof s_plock_val);
    s_plock_n = 0;
}

static int persistent_get(const char *key)
{
    for (int i = 0; i < s_plock_n; i++)
        if (strcmp(s_plock_key[i], key) == 0)
            return s_plock_val[i];
    return 0;
}

static void persistent_set(const char *key, int v)
{
    for (int i = 0; i < s_plock_n; i++)
        if (strcmp(s_plock_key[i], key) == 0) { s_plock_val[i] = (uint8_t)v; return; }
    if (s_plock_n >= BMAX) return;
    snprintf(s_plock_key[s_plock_n], 16, "%s", key);
    s_plock_val[s_plock_n++] = (uint8_t)v;
}

#endif

// Same reasoning as kiss_usage.c's may_persist, and the same read/write pair:
// the key carries the master fingerprint, so a plaintext record names which
// wallet was backed up. That needs encrypted flash under it, not just a non
// AMNESIC mode. Below the bar the flag is session scoped, exactly as AMNESIC
// already is, so the SETTINGS row still answers correctly for this session.
static bool may_persist(void)
{
    return kiss_seed_mode() != WSEED_MODE_AMNESIC && kiss_seed_flash_encrypted();
}

bool kiss_backup_checked(const uint8_t fp[4])
{
    char key[16];
    backup_key(fp, key);
    if (!may_persist())
        return tab_has(s_session, s_session_n, key);
    return persistent_has(key);
}

void kiss_backup_mark(const uint8_t fp[4])
{
    char key[16];
    backup_key(fp, key);
    tab_add(s_session, &s_session_n, key);
    if (may_persist())
        persistent_add(key);
}

// ---- which secret was locked (see kiss_backup.h) ----
// key = "<fp8hex>L", nine of NVS's fifteen characters, beside the eight
// character key of the paper flag for the same keys.
static char    s_lock_key[BMAX][16];
static uint8_t s_lock_val[BMAX];
static int     s_lock_n;

static void lock_key(const uint8_t fp[4], char out[16])
{
    snprintf(out, 16, "%02x%02x%02x%02xL", fp[0], fp[1], fp[2], fp[3]);
}

static bool lock_mode_records(void)
{
    int m = kiss_seed_mode();
    return m == WSEED_MODE_KEEP || m == WSEED_MODE_SD;
}

int kiss_backup_locked(const uint8_t fp[4])
{
    // AMNESIC keeps none, not even for the session: a signer that holds no
    // seed between sessions has no business remembering what it made for one.
    // Asked as "a mode that keeps a seed", so a mode byte that can not be
    // read answers the same way.
    if (!lock_mode_records())
        return KISS_LOCKED_NONE;
    char key[16];
    lock_key(fp, key);
    if (may_persist())
        return persistent_get(key);
    for (int i = 0; i < s_lock_n; i++)
        if (strcmp(s_lock_key[i], key) == 0)
            return s_lock_val[i];
    return KISS_LOCKED_NONE;
}

void kiss_backup_set_locked(const uint8_t fp[4], int which)
{
    if (which != KISS_LOCKED_SEED && which != KISS_LOCKED_PASS)
        return;
    if (!lock_mode_records())
        return;
    char key[16];
    lock_key(fp, key);
    int at = -1;
    for (int i = 0; i < s_lock_n; i++)
        if (strcmp(s_lock_key[i], key) == 0) at = i;
    if (at < 0 && s_lock_n < BMAX) {
        at = s_lock_n++;
        snprintf(s_lock_key[at], 16, "%s", key);
    }
    if (at >= 0) s_lock_val[at] = (uint8_t)which;
    if (may_persist())
        persistent_set(key, which);
}

void kiss_backup_forget(void)
{
    persistent_wipe();
    memset(s_session, 0, sizeof s_session);
    s_session_n = 0;
    memset(s_lock_key, 0, sizeof s_lock_key);
    memset(s_lock_val, 0, sizeof s_lock_val);
    s_lock_n = 0;
}
