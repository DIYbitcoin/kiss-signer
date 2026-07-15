// Step 7: the user's own seed — BIP39 helpers (libwally) + persistence.
// Device: NVS blob (plaintext until step 8 enables flash encryption).
// Desktop tests: a plain file, so /tmp/kisstest exercises identical logic.
#include "wallet_seed.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <wally_bip39.h>
#include <wally_core.h>

#ifdef ESP_PLATFORM
#include "nvs.h"
#else
#define SEED_FILE "/tmp/kiss_seed.txt"
#endif

// ---- storage backends ----
static int storage_read(char *out, size_t out_len)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READONLY, &h) != ESP_OK)
        return -1;
    size_t len = out_len;
    int rc = nvs_get_str(h, "words", out, &len) == ESP_OK ? 0 : -1;
    nvs_close(h);
    return rc;
#else
    FILE *f = fopen(SEED_FILE, "r");
    if (!f)
        return -1;
    size_t n = fread(out, 1, out_len - 1, f);
    fclose(f);
    if (n == 0)
        return -1;
    out[n] = 0;
    return 0;
#endif
}

static int storage_write(const char *words)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READWRITE, &h) != ESP_OK)
        return -1;
    int rc = nvs_set_str(h, "words", words) == ESP_OK &&
             nvs_commit(h) == ESP_OK ? 0 : -1;
    nvs_close(h);
    return rc;
#else
    FILE *f = fopen(SEED_FILE, "w");
    if (!f)
        return -1;
    int rc = fputs(words, f) >= 0 ? 0 : -1;
    fclose(f);
    return rc;
#endif
}

static int storage_erase(void)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READWRITE, &h) != ESP_OK)
        return -1;
    esp_err_t e = nvs_erase_key(h, "words");
    int rc = (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND) &&
             nvs_commit(h) == ESP_OK ? 0 : -1;
    nvs_close(h);
    return rc;
#else
    remove(SEED_FILE);                 // absent is fine: wiping twice is a no-op
    return 0;
#endif
}

// ---- staged (not-yet-committed) mnemonic ----
static char s_pending[WSEED_MAX_MNEMONIC];
static bool s_has_pending;

int wallet_seed_stage(const char *mnemonic)
{
    if (wallet_seed_validate(mnemonic) != 0)
        return -1;
    snprintf(s_pending, sizeof s_pending, "%s", mnemonic);
    s_has_pending = true;
    return 0;
}

int wallet_seed_commit(void)
{
    if (!s_has_pending)
        return -1;
    int rc = storage_write(s_pending);
    wally_bzero(s_pending, sizeof s_pending);
    s_has_pending = false;
    return rc;
}

void wallet_seed_discard(void)
{
    wally_bzero(s_pending, sizeof s_pending);
    s_has_pending = false;
}

// ---- API ----
int wallet_seed_exists(void)
{
    if (s_has_pending)
        return 1;
    char tmp[WSEED_MAX_MNEMONIC];
    int rc = storage_read(tmp, sizeof tmp) == 0 ? 1 : 0;
    wally_bzero(tmp, sizeof tmp);
    return rc;
}

int wallet_seed_validate(const char *mnemonic)
{
    if (!mnemonic || !mnemonic[0])
        return -1;
    return bip39_mnemonic_validate(NULL, mnemonic) == WALLY_OK ? 0 : -1;
}

int wallet_seed_store(const char *mnemonic)
{
    if (wallet_seed_validate(mnemonic) != 0)
        return -1;
    return storage_write(mnemonic);
}

int wallet_seed_load(char *out, size_t out_len)
{
    if (!out || out_len < 2)
        return -1;
    if (s_has_pending) {               // staged setup: derive before it's flashed
        snprintf(out, out_len, "%s", s_pending);
        return 0;
    }
    return storage_read(out, out_len);
}

int wallet_seed_wipe(void)
{
    return storage_erase();
}

int wallet_seed_from_entropy(const uint8_t *entropy, size_t len,
                             char *out, size_t out_len)
{
    if (!entropy || !out || (len != 16 && len != 32))
        return -1;
    char *words = NULL;
    if (bip39_mnemonic_from_bytes(NULL, entropy, len, &words) != WALLY_OK || !words)
        return -1;
    int rc = -1;
    if (strlen(words) + 1 <= out_len) {
        strcpy(out, words);
        rc = 0;
    }
    wally_free_string(words);
    return rc;
}

// ---- wordlist access ----
int wallet_seed_word(int index, const char **out)
{
    if (!out || index < 0 || index >= BIP39_WORDLIST_LEN)
        return -1;
    struct words *wl = NULL;
    if (bip39_get_wordlist(NULL, &wl) != WALLY_OK)
        return -1;
    char *w = NULL;
    if (bip39_get_word(wl, (size_t)index, &w) != WALLY_OK || !w)
        return -1;
    // libwally allocates a copy; the wordlist itself is static. Keep a tiny
    // ring of copies so callers get stable pointers without managing frees.
    static char ring[8][12];
    static int ri;
    snprintf(ring[ri], sizeof ring[ri], "%s", w);
    wally_free_string(w);
    *out = ring[ri];
    ri = (ri + 1) & 7;
    return 0;
}

// ---- backup verification: word-by-word compare (see wallet_seed.h) ----
static const char *skip_spaces(const char *p) { while (*p == ' ') p++; return p; }

int wallet_seed_diff_word(const char *typed, const char *stored)
{
    const char *a = skip_spaces(typed), *b = skip_spaces(stored);
    for (int idx = 0;; idx++) {
        // word boundaries in each string
        const char *ae = a; while (*ae && *ae != ' ') ae++;
        const char *be = b; while (*be && *be != ' ') be++;
        size_t al = (size_t)(ae - a), bl = (size_t)(be - b);
        if (al == 0 && bl == 0) return -1;            // both ended together: match
        if (al != bl || strncmp(a, b, al) != 0) return idx;   // word differs (or count differs)
        a = skip_spaces(ae);
        b = skip_spaces(be);
    }
}

int wallet_seed_suggest(const char *prefix, const char *out[], int n)
{
    // matches get their own stable rows — wallet_seed_word's ring would be
    // overwritten while we keep scanning past a match
    static char sug[8][12];
    if (!prefix || !out || n <= 0)
        return 0;
    if (n > 8) n = 8;
    size_t plen = strlen(prefix);
    int found = 0;
    for (int i = 0; i < BIP39_WORDLIST_LEN && found < n; i++) {
        const char *w = NULL;
        if (wallet_seed_word(i, &w) != 0)
            break;
        if (strncmp(w, prefix, plen) == 0) {
            snprintf(sug[found], sizeof sug[found], "%s", w);
            out[found] = sug[found];
            found++;
        }
    }
    return found;
}
