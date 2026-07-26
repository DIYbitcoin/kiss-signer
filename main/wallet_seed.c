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
#define MODE_FILE "/tmp/kiss_seed_mode.txt"
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

// ---- storage mode (see wallet_seed.h) ----
static int storage_mode_read(void)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READONLY, &h) != ESP_OK)
        return WSEED_MODE_KEEP;
    uint8_t m = WSEED_MODE_KEEP;
    nvs_get_u8(h, "smode", &m);
    nvs_close(h);
    return m == WSEED_MODE_AMNESIC ? WSEED_MODE_AMNESIC : WSEED_MODE_KEEP;
#else
    FILE *f = fopen(MODE_FILE, "r");
    if (!f)
        return WSEED_MODE_KEEP;
    int m = WSEED_MODE_KEEP;
    if (fscanf(f, "%d", &m) != 1) m = WSEED_MODE_KEEP;
    fclose(f);
    return m == WSEED_MODE_AMNESIC ? WSEED_MODE_AMNESIC : WSEED_MODE_KEEP;
#endif
}

static void storage_mode_write(int mode)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_u8(h, "smode", (uint8_t)mode);
    nvs_commit(h);
    nvs_close(h);
#else
    FILE *f = fopen(MODE_FILE, "w");
    if (!f)
        return;
    fprintf(f, "%d", mode);
    fclose(f);
#endif
}

// ---- staged (not-yet-committed) mnemonic ----
// In KEEP mode this is the setup safety net: staged in RAM, written to flash
// only once the whole ritual finishes. In AMNESIC mode it is the wallet — it
// is never written anywhere, and wallet_seed_forget() (called from
// wallet_session_close) is what ends the session.
static char s_pending[WSEED_MAX_MNEMONIC];
static bool s_has_pending;

int wallet_seed_mode(void) { return storage_mode_read(); }

void wallet_seed_set_mode(int mode)
{
    mode = mode == WSEED_MODE_AMNESIC ? WSEED_MODE_AMNESIC : WSEED_MODE_KEEP;
    // Turning amnesic ON has to take the stored seed with it, otherwise the
    // screen would claim "nothing saved" while flash still held the words.
    if (mode == WSEED_MODE_AMNESIC)
        storage_erase();
    storage_mode_write(mode);
}

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
    // Amnesic: "committing" means keeping it in RAM and nowhere else. The
    // staged copy stays so the session can derive from it until the lock.
    if (storage_mode_read() == WSEED_MODE_AMNESIC)
        return 0;
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
    wallet_seed_discard();          // an amnesic seed only ever lives here
    return storage_erase();
}

void wallet_seed_forget(void)
{
    if (storage_mode_read() == WSEED_MODE_AMNESIC)
        wallet_seed_discard();
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

// ---- QR seed import (see wallet_seed.h) ----
static bool all_digits(const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] < '0' || p[i] > '9') return false;
    return n > 0;
}

// 48 or 96 ASCII digits, four per wordlist index. Rebuilding the words from
// indices means a damaged QR shows up as a checksum failure below, not as a
// silently different wallet.
static int from_numeric_seedqr(const char *p, size_t n, char *out, size_t out_len)
{
    size_t words_n = n / 4;
    size_t o = 0;
    for (size_t w = 0; w < words_n; w++) {
        int idx = 0;
        for (int d = 0; d < 4; d++) idx = idx * 10 + (p[w * 4 + d] - '0');
        const char *word = NULL;
        if (wallet_seed_word(idx, &word) != 0 || !word)   // 2048+ lands here
            return -1;
        int need = snprintf(out + o, out_len - o, "%s%s", w ? " " : "", word);
        if (need < 0 || (size_t)need >= out_len - o)
            return -1;
        o += (size_t)need;
    }
    return 0;
}

int wallet_seed_from_qr(const char *data, size_t len, char *out, size_t out_len)
{
    if (out && out_len) out[0] = 0;      // never leave a stale value behind
    if (!data || !out || out_len < 2 || len == 0)
        return -1;

    // Numeric SeedQR first: it is the only all-ASCII-digit form, and its
    // lengths (48/96) cannot be mistaken for a CompactSeedQR (16/32).
    if ((len == 48 || len == 96) && all_digits(data, len)) {
        if (from_numeric_seedqr(data, len, out, out_len) != 0)
            goto fail;
        if (wallet_seed_validate(out) != 0)
            goto fail;
        return 0;
    }

    // CompactSeedQR: raw entropy, no encoding at all.
    if (len == 16 || len == 32) {
        if (wallet_seed_from_entropy((const uint8_t *)data, len, out, out_len) != 0)
            goto fail;
        return 0;                        // built from entropy: the checksum is ours
    }

    // Plain text mnemonic. Trimmed, so a trailing newline from a text QR does
    // not turn into a failed wordlist lookup.
    {
        const char *b = data, *e = data + len;
        while (b < e && (*b == ' ' || *b == '\n' || *b == '\r' || *b == '\t')) b++;
        while (e > b && (e[-1] == ' ' || e[-1] == '\n' || e[-1] == '\r' ||
                         e[-1] == '\t' || e[-1] == 0)) e--;
        size_t n = (size_t)(e - b);
        if (n == 0 || n + 1 > out_len)
            goto fail;
        memcpy(out, b, n);
        out[n] = 0;
        if (wallet_seed_validate(out) == 0)
            return 0;
    }
fail:
    wally_bzero(out, out_len);
    return -1;
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
