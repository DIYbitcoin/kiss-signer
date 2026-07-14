// See wallet_usage.h. Device backend = NVS namespace "kissu" (separate from the
// "kiss" namespace so a usage wipe can erase-all without touching settings or
// the seed). Host backend = a small RAM table (the desktop tests and the sim
// run in one process, so persistence-across-boot is a device-only concern).
#include "wallet_usage.h"

#include <stdio.h>
#include <string.h>

// key = "<fp8hex><net><type>", e.g. "ec5a459501" -> 10 chars (NVS limit 15)
static void usage_key(const uint8_t fp[4], int testnet, int script, char out[16])
{
    snprintf(out, 16, "%02x%02x%02x%02x%d%d",
             fp[0], fp[1], fp[2], fp[3], testnet ? 1 : 0, script);
}

#ifdef ESP_PLATFORM
#include "nvs.h"

int wallet_usage_high(const uint8_t fp[4], int testnet, int script)
{
    char key[16];
    usage_key(fp, testnet, script, key);
    nvs_handle_t h;
    if (nvs_open("kissu", NVS_READONLY, &h) != ESP_OK)
        return -1;
    uint32_t v = 0;
    int rc = nvs_get_u32(h, key, &v) == ESP_OK ? (int)v : -1;
    nvs_close(h);
    return rc;
}

void wallet_usage_mark(const uint8_t fp[4], int testnet, int script, uint32_t idx)
{
    char key[16];
    usage_key(fp, testnet, script, key);
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

void wallet_usage_wipe(void)
{
    nvs_handle_t h;
    if (nvs_open("kissu", NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}

#else   // host (sim + desktop tests): RAM table

#define UMAX 32
static struct { char key[16]; uint32_t v; } s_tab[UMAX];
static int s_n;

int wallet_usage_high(const uint8_t fp[4], int testnet, int script)
{
    char key[16];
    usage_key(fp, testnet, script, key);
    for (int i = 0; i < s_n; i++)
        if (strcmp(s_tab[i].key, key) == 0)
            return (int)s_tab[i].v;
    return -1;
}

void wallet_usage_mark(const uint8_t fp[4], int testnet, int script, uint32_t idx)
{
    char key[16];
    usage_key(fp, testnet, script, key);
    for (int i = 0; i < s_n; i++)
        if (strcmp(s_tab[i].key, key) == 0) {
            if (idx > s_tab[i].v) s_tab[i].v = idx;
            return;
        }
    if (s_n < UMAX) {
        snprintf(s_tab[s_n].key, sizeof s_tab[s_n].key, "%s", key);
        s_tab[s_n].v = idx;
        s_n++;
    }
}

void wallet_usage_wipe(void) { s_n = 0; }

#endif
