// See kiss_usage.h. Device backend = NVS namespace "kissu" (separate from the
// "kiss" namespace so a usage wipe can erase-all without touching settings or
// the seed). Host backend = a small RAM table (the desktop tests and the sim
// run in one process, so persistence-across-boot is a device-only concern).
#include "kiss_usage.h"
#include "kiss_seed.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// key = "<fp8hex><net><type>", e.g. "ec5a459501" -> 10 chars (NVS limit 15)
static void usage_key(const uint8_t fp[4], int testnet, int script, char out[16])
{
    snprintf(out, 16, "%02x%02x%02x%02x%d%d",
             fp[0], fp[1], fp[2], fp[3], testnet ? 1 : 0, script);
}

#define UMAX 32
struct usage_row { char key[16]; uint32_t v; };
static struct usage_row s_session[UMAX];
static int s_session_n;

static int tab_high(const struct usage_row *tab, int n, const char *key)
{
    for (int i = 0; i < n; i++)
        if (strcmp(tab[i].key, key) == 0)
            return (int)tab[i].v;
    return -1;
}

static void tab_mark(struct usage_row *tab, int *n, const char *key, uint32_t idx)
{
    for (int i = 0; i < *n; i++)
        if (strcmp(tab[i].key, key) == 0) {
            if (idx > tab[i].v) tab[i].v = idx;
            return;
        }
    if (*n < UMAX) {
        snprintf(tab[*n].key, sizeof tab[*n].key, "%s", key);
        tab[*n].v = idx;
        (*n)++;
    }
}

#ifdef ESP_PLATFORM
#include "nvs.h"

static int persistent_high(const char *key)
{
    nvs_handle_t h;
    if (nvs_open("kissu", NVS_READONLY, &h) != ESP_OK)
        return -1;
    uint32_t v = 0;
    int rc = nvs_get_u32(h, key, &v) == ESP_OK ? (int)v : -1;
    nvs_close(h);
    return rc;
}

static void persistent_mark(const char *key, uint32_t idx)
{
    nvs_handle_t h;
    if (nvs_open("kissu", NVS_READWRITE, &h) != ESP_OK)
        return;
    uint32_t cur = 0;
    bool have = nvs_get_u32(h, key, &cur) == ESP_OK;
    if (!have || idx > cur) {           // monotonic: never lower the high-water mark
        nvs_set_u32(h, key, idx);
        nvs_commit(h);
    }
    nvs_close(h);
}

static void persistent_wipe(void)
{
    nvs_handle_t h;
    if (nvs_open("kissu", NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}

#else   // host (sim + desktop tests): RAM table

static struct usage_row s_persistent[UMAX];
static int s_persistent_n;

static int persistent_high(const char *key)
{
    return tab_high(s_persistent, s_persistent_n, key);
}

static void persistent_mark(const char *key, uint32_t idx)
{
    tab_mark(s_persistent, &s_persistent_n, key, idx);
}

static void persistent_wipe(void)
{
    memset(s_persistent, 0, sizeof s_persistent);
    s_persistent_n = 0;
}

#endif

// The key is built from the master fingerprint, so writing one to plaintext
// NVS records WHICH wallet was used. On a device with a passphrase that is the
// whole game: a decoy holds its own fingerprint, and a second one in the same
// namespace proves a second wallet exists, then serves as an offline oracle to
// grind passphrases against. That defeats what kiss_duress.h is for.
//
// So persistence needs encrypted flash under it, not merely a non AMNESIC mode.
// On the beta lane this costs the cross boot memory of the receive high water
// mark; within a session the table below still carries it. A convenience is the
// right thing to lose here.
//
// Read and write move together. Gating only the write would leave the reader
// asking NVS for a record nothing writes any more, so the mark would appear to
// vanish the instant it was made — a silently dead feature rather than a
// deliberately session scoped one.
static bool may_persist(void)
{
    return kiss_seed_mode() != WSEED_MODE_AMNESIC && kiss_seed_flash_encrypted();
}

int kiss_usage_high(const uint8_t fp[4], int testnet, int script)
{
    char key[16];
    usage_key(fp, testnet, script, key);
    if (!may_persist())
        return tab_high(s_session, s_session_n, key);
    int v = persistent_high(key);
    if (v >= 0) tab_mark(s_session, &s_session_n, key, (uint32_t)v);
    return v;
}

void kiss_usage_mark(const uint8_t fp[4], int testnet, int script, uint32_t idx)
{
    char key[16];
    usage_key(fp, testnet, script, key);
    tab_mark(s_session, &s_session_n, key, idx);
    if (may_persist())
        persistent_mark(key, idx);
}

// Second door onto the same NVS keys: the mode change paths in kiss_seed.c
// flush the whole session table at once. Gating only kiss_usage_mark would
// leave every fingerprint to land here instead.
void kiss_usage_persist_session(void)
{
    if (!may_persist())
        return;
    for (int i = 0; i < s_session_n; i++)
        persistent_mark(s_session[i].key, s_session[i].v);
}

void kiss_usage_forget_session(void)
{
    memset(s_session, 0, sizeof s_session);
    s_session_n = 0;
}

void kiss_usage_wipe(void)
{
    persistent_wipe();
    kiss_usage_forget_session();
}
