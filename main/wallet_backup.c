// See wallet_backup.h. Device backend = NVS namespace "kissb" (its own, so a
// backup fact never has to be untangled from settings, the seed or the usage
// history). Host backend = a small RAM table, exactly as wallet_usage.c does:
// the desktop tests and the sim run in one process, so persistence-across-boot
// is a device-only concern.
#include "wallet_backup.h"
#include "wallet_seed.h"

#include <stdio.h>
#include <string.h>

// key = "<fp8hex>", 8 of the 15 chars NVS allows. Same shape as usage_key in
// wallet_usage.c, minus the network/script suffix: which chain you are on does
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

static void persistent_wipe(void)
{
    memset(s_persistent, 0, sizeof s_persistent);
    s_persistent_n = 0;
}

#endif

bool wallet_backup_checked(const uint8_t fp[4])
{
    char key[16];
    backup_key(fp, key);
    if (wallet_seed_mode() == WSEED_MODE_AMNESIC)
        return tab_has(s_session, s_session_n, key);
    return persistent_has(key);
}

void wallet_backup_mark(const uint8_t fp[4])
{
    char key[16];
    backup_key(fp, key);
    tab_add(s_session, &s_session_n, key);
    if (wallet_seed_mode() != WSEED_MODE_AMNESIC)
        persistent_add(key);
}

void wallet_backup_forget(void)
{
    persistent_wipe();
    memset(s_session, 0, sizeof s_session);
    s_session_n = 0;
}
