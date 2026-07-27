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
#include "nvs_flash.h"
#else
#include <errno.h>
#define SEED_FILE "/tmp/kiss_seed.txt"
#define MODE_FILE "/tmp/kiss_seed_mode.txt"
#define SEED_TMP  "/tmp/kiss_seed.txt.tmp"
#define MODE_TMP  "/tmp/kiss_seed_mode.txt.tmp"
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

static int storage_mode_read_checked(int *out_mode);
static int storage_mode_write(int mode);

// KEEP mode's words and mode flag are one NVS commit on-device. A power loss
// must never leave a newly persisted seed paired with AMNESIC UI state.
static int storage_write_keep(const char *words)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READWRITE, &h) != ESP_OK)
        return -1;
    int rc = nvs_set_str(h, "words", words) == ESP_OK &&
             nvs_set_u8(h, "smode", WSEED_MODE_KEEP) == ESP_OK &&
             nvs_commit(h) == ESP_OK ? 0 : -1;
    nvs_close(h);
#else
    // Set KEEP first: if writing the seed then fails, no new secret has been
    // persisted under an AMNESIC label. Temp+rename avoids truncating an
    // existing wallet on a short write.
    if (storage_mode_write(WSEED_MODE_KEEP) != 0)
        return -1;
    FILE *f = fopen(SEED_TMP, "w");
    if (!f)
        return -1;
    int rc = fputs(words, f) >= 0 ? 0 : -1;
    if (fclose(f) != 0) rc = -1;
    if (rc == 0 && rename(SEED_TMP, SEED_FILE) != 0) rc = -1;
    if (rc != 0) remove(SEED_TMP);
#endif
    if (rc != 0)
        return -1;

    char verify[WSEED_MAX_MNEMONIC];
    int mode = -1;
    rc = storage_read(verify, sizeof verify) == 0 &&
         strcmp(verify, words) == 0 &&
         storage_mode_read_checked(&mode) == 0 &&
         mode == WSEED_MODE_KEEP ? 0 : -1;
    wally_bzero(verify, sizeof verify);
    return rc;
}

#ifdef ESP_PLATFORM
// Non-secret settings that must survive an erase. Named here because erasing
// is a WHOLE-PARTITION operation: everything else in NVS goes with it, and
// dumping someone back into English is a rotten way to end a wipe.
static const char *const KEEP_KEYS[] = { "testnet", "script", "accent", "lang" };
#define N_KEEP (sizeof KEEP_KEYS / sizeof KEEP_KEYS[0])
#endif

// nvs_erase_key is a LOGICAL delete. NVS is log-structured, so the old entry
// stays on its page, readable to anyone who dumps the chip, until a compaction
// that may never come. On the encrypted-release lane that residue is
// ciphertext and harmless; on a plaintext board it is the seed.
//
// This one function sits behind BOTH "ERASE THIS WALLET" and amnesic mode, and
// amnesic mode's whole promise is that a device which gets searched holds no
// wallet bytes at all. A logical delete does not deliver that, so erase the
// flash sectors themselves and put the preferences back afterwards.
static int storage_erase(int mode_after)
{
    if (mode_after != WSEED_MODE_KEEP && mode_after != WSEED_MODE_AMNESIC)
        return -1;
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    uint8_t keep[N_KEEP];
    bool have[N_KEEP];

    for (size_t i = 0; i < N_KEEP; i++)
        have[i] = false;
    if (nvs_open("kiss", NVS_READONLY, &h) == ESP_OK) {
        for (size_t i = 0; i < N_KEEP; i++)
            have[i] = nvs_get_u8(h, KEEP_KEYS[i], &keep[i]) == ESP_OK;
        nvs_close(h);
    }

    // deinit explicitly: every handle above is closed, and erasing a partition
    // that is still initialized is not portable across IDF versions
    esp_err_t err = nvs_flash_deinit();
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_INITIALIZED)
        return -1;
    if (nvs_flash_erase() != ESP_OK) {
        (void)nvs_flash_init();             // leave NVS usable if erase failed
        return -1;
    }
    if (nvs_flash_init() != ESP_OK)
        return -1;

    if (nvs_open("kiss", NVS_READWRITE, &h) != ESP_OK)
        return -1;                         // blank partition, which is safe
    int rc = 0;
    for (size_t i = 0; i < N_KEEP; i++)
        if (have[i] && nvs_set_u8(h, KEEP_KEYS[i], keep[i]) != ESP_OK)
            rc = -1;
    if (rc == 0 &&
        nvs_set_u8(h, "smode", (uint8_t)mode_after) != ESP_OK)
        rc = -1;
    if (rc == 0 && nvs_commit(h) != ESP_OK)
        rc = -1;
    nvs_close(h);
#else
    int rc = 0;
    if (remove(SEED_FILE) != 0 && errno != ENOENT)
        rc = -1;
    if (remove(SEED_TMP) != 0 && errno != ENOENT)
        rc = -1;
    if (rc == 0)
        rc = storage_mode_write(mode_after);
#endif
    if (rc != 0)
        return -1;
    int verify = -1;
    return storage_mode_read_checked(&verify) == 0 && verify == mode_after
         ? 0 : -1;
}

// ---- storage mode (see wallet_seed.h) ----
static int storage_mode_read_checked(int *out_mode)
{
    if (!out_mode)
        return -1;
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    esp_err_t err = nvs_open("kiss", NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out_mode = WSEED_MODE_KEEP;
        return 0;
    }
    if (err != ESP_OK)
        return -1;
    uint8_t m = 0xff;
    err = nvs_get_u8(h, "smode", &m);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out_mode = WSEED_MODE_KEEP;
        return 0;
    }
    if (err != ESP_OK ||
        (m != WSEED_MODE_KEEP && m != WSEED_MODE_AMNESIC))
        return -1;
    *out_mode = (int)m;
    return 0;
#else
    errno = 0;
    FILE *f = fopen(MODE_FILE, "r");
    if (!f) {
        if (errno == ENOENT) {
            *out_mode = WSEED_MODE_KEEP;
            return 0;
        }
        return -1;
    }
    int m = -1;
    char extra = 0;
    int fields = fscanf(f, "%d %c", &m, &extra);
    int close_rc = fclose(f);
    if (fields != 1 || close_rc != 0 ||
        (m != WSEED_MODE_KEEP && m != WSEED_MODE_AMNESIC))
        return -1;
    *out_mode = m;
    return 0;
#endif
}

static int storage_mode_read(void)
{
    int mode = WSEED_MODE_AMNESIC;
    // Fail closed: a corrupt/unreadable mode must never make the UI claim a
    // seed is safely persisted. A genuinely fresh store still returns KEEP.
    return storage_mode_read_checked(&mode) == 0
         ? mode : WSEED_MODE_AMNESIC;
}

static int storage_mode_write(int mode)
{
    if (mode != WSEED_MODE_KEEP && mode != WSEED_MODE_AMNESIC)
        return -1;
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READWRITE, &h) != ESP_OK)
        return -1;
    int rc = nvs_set_u8(h, "smode", (uint8_t)mode) == ESP_OK &&
             nvs_commit(h) == ESP_OK ? 0 : -1;
    nvs_close(h);
#else
    FILE *f = fopen(MODE_TMP, "w");
    if (!f)
        return -1;
    int rc = fprintf(f, "%d", mode) > 0 ? 0 : -1;
    if (fclose(f) != 0) rc = -1;
    if (rc == 0 && rename(MODE_TMP, MODE_FILE) != 0) rc = -1;
    if (rc != 0) remove(MODE_TMP);
#endif
    int verify = -1;
    return rc == 0 &&
           storage_mode_read_checked(&verify) == 0 && verify == mode ? 0 : -1;
}

// ---- staged (not-yet-committed) mnemonic ----
// In KEEP mode this is the setup safety net: staged in RAM, written to flash
// only once the whole ritual finishes. In AMNESIC mode it is the wallet — it
// is never written anywhere, and wallet_seed_forget() (called from
// wallet_session_close) is what ends the session.
static char s_pending[WSEED_MAX_MNEMONIC];
static bool s_has_pending;

// Staged storage mode, or -1 for "no choice pending". The setup wizard asks
// KEEP vs NOTHING SAVED on its FIRST screen, before a single word of the new
// wallet exists, so that answer is staged exactly like the mnemonic is.
// Applying it on the tap erased the wallet the user still had: one BACK press
// or a power cut and the words were gone with nothing to replace them.
static int s_pending_mode = -1;

// The staged choice is what the wizard's own screens must reflect, so it wins
// while it exists. Everywhere else there is nothing staged and this is flash.
int wallet_seed_mode(void)
{
    return s_pending_mode >= 0 ? s_pending_mode : storage_mode_read();
}

void wallet_seed_stage_mode(int mode)
{
    s_pending_mode = mode == WSEED_MODE_AMNESIC ? WSEED_MODE_AMNESIC
                                                : WSEED_MODE_KEEP;
}

int wallet_seed_set_mode(int mode)
{
    mode = mode == WSEED_MODE_AMNESIC ? WSEED_MODE_AMNESIC : WSEED_MODE_KEEP;
    // Turning amnesic ON has to take the stored seed with it, otherwise the
    // screen would claim "nothing saved" while flash still held the words.
    int rc = mode == WSEED_MODE_AMNESIC
           ? storage_erase(WSEED_MODE_AMNESIC)
           : storage_mode_write(WSEED_MODE_KEEP);
    if (rc == 0)
        s_pending_mode = -1;           // only publish a verified mode change
    return rc;
}

int wallet_seed_stage(const char *mnemonic)
{
    if (wallet_seed_validate(mnemonic) != 0)
        return -1;
    snprintf(s_pending, sizeof s_pending, "%s", mnemonic);
    s_has_pending = true;
    return 0;
}

// The ONE moment flash changes. Everything the wizard collected (the words and
// the storage mode) lands here together, or not at all.
int wallet_seed_commit(void)
{
    if (!s_has_pending)
        return -1;
    int mode = wallet_seed_mode();
    if (mode == WSEED_MODE_AMNESIC) {
        // "Committing" means keeping it in RAM and nowhere else, and taking
        // any previously stored wallet with it -- otherwise the screen would
        // claim "nothing saved" while flash still held the old words. The
        // staged copy stays so the session can derive from it until the lock.
        if (storage_erase(WSEED_MODE_AMNESIC) != 0)
            return -1;
        s_pending_mode = -1;
        return 0;
    }
    int rc = storage_write_keep(s_pending);
    if (rc != 0)
        return -1;   // still staged, but the caller decides: the setup login
                     // discards it rather than hold an unsaved mnemonic in RAM
    wally_bzero(s_pending, sizeof s_pending);
    s_has_pending = false;
    s_pending_mode = -1;
    return 0;
}

// Backing out of setup, at any step, for any reason. Nothing was written yet,
// so this only has to drop what is held in RAM: the words AND the storage-mode
// answer, which reverts wallet_seed_mode() to whatever flash still says.
void wallet_seed_discard(void)
{
    wally_bzero(s_pending, sizeof s_pending);
    s_has_pending = false;
    s_pending_mode = -1;
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
    return storage_write_keep(mnemonic);
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
    return storage_erase(WSEED_MODE_KEEP);
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
