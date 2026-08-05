// Step 7: the user's own seed — BIP39 helpers (libwally) + persistence.
// Device: NVS blob (plaintext until step 8 enables flash encryption).
// Desktop tests: a plain file, so /tmp/kisstest exercises identical logic.
#include "wallet_seed.h"
#include "wallet_seed_sd.h"
#include "platform_sd.h"
#include "wallet_usage.h"
#include "wallet_backup.h"   // the paper check dies with the wallet it was about
#include "wallet_duress.h"   // and so does the stroke that opened it

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <wally_bip39.h>
#include <wally_core.h>

#ifdef ESP_PLATFORM
#include "esp_efuse.h"
#include "nvs.h"
#include "nvs_flash.h"
#else
#include <errno.h>
#include <unistd.h>
#define SEED_FILE "/tmp/kiss_seed.txt"
#define MODE_FILE "/tmp/kiss_seed_mode.txt"
#define SEED_TMP  "/tmp/kiss_seed.txt.tmp"
#define MODE_TMP  "/tmp/kiss_seed_mode.txt.tmp"
static unsigned s_seed_test_fail;
void wallet_seed_test_fail_next(unsigned flags) { s_seed_test_fail = flags; }
static int seed_test_fail(unsigned flag)
{
    if (!(s_seed_test_fail & flag)) return 0;
    s_seed_test_fail &= ~flag;
    return 1;
}
#endif

// ---- storage backends ----
static int storage_read_keep(char *out, size_t out_len)
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
static int storage_mode_read(void);
static int storage_mode_write(int mode);

// KEEP mode's words and mode flag are one NVS commit on-device. A power loss
// must never leave a newly persisted seed paired with AMNESIC UI state.
static int storage_write_keep(const char *words)
{
    int old_mode = storage_mode_read();
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READWRITE, &h) != ESP_OK)
        return WSEED_ERR_SD_IO;
    int rc;
    if (old_mode == WSEED_MODE_SD) {
        // NVS commit is not a multi-key transaction. Persist and verify the
        // destination words while SD remains authoritative, then flip mode.
        rc = nvs_set_str(h, "words", words) == ESP_OK &&
             nvs_commit(h) == ESP_OK ? WSEED_OK : WSEED_ERR_SD_IO;
        if (rc == WSEED_OK) {
            char verify[WSEED_MAX_MNEMONIC];
            rc = storage_read_keep(verify, sizeof verify) == 0 &&
                 strcmp(verify, words) == 0 ? WSEED_OK : WSEED_ERR_VERIFY;
            wally_bzero(verify, sizeof verify);
        }
        if (rc == WSEED_OK &&
            (nvs_set_u8(h, "smode", WSEED_MODE_KEEP) != ESP_OK ||
             nvs_commit(h) != ESP_OK)) {
            esp_err_t er = nvs_erase_key(h, "words");
            rc = (er == ESP_OK || er == ESP_ERR_NVS_NOT_FOUND) &&
                 nvs_commit(h) == ESP_OK
               ? WSEED_ERR_SD_IO : WSEED_ERR_ROLLBACK;
        }
    } else {
        rc = nvs_set_str(h, "words", words) == ESP_OK &&
             nvs_set_u8(h, "smode", WSEED_MODE_KEEP) == ESP_OK &&
             nvs_commit(h) == ESP_OK ? WSEED_OK : WSEED_ERR_SD_IO;
    }
    nvs_close(h);
#else
    // Build the host destination in a temp first. It is promoted in the order
    // appropriate to its source, with rollback on every injected failure.
    FILE *f = fopen(SEED_TMP, "w");
    if (!f)
        return WSEED_ERR_SD_IO;
    int rc = fputs(words, f) >= 0 ? WSEED_OK : WSEED_ERR_SD_IO;
    if (rc == WSEED_OK && fflush(f) != 0) rc = WSEED_ERR_SD_IO;
    if (rc == WSEED_OK && fsync(fileno(f)) != 0) rc = WSEED_ERR_SD_IO;
    if (fclose(f) != 0) rc = WSEED_ERR_SD_IO;
    if (rc != WSEED_OK) {
        (void)remove(SEED_TMP);
        return rc;
    }

    if (old_mode == WSEED_MODE_SD) {
        // Destination first. Card remains authoritative until KEEP publishes.
        if (rename(SEED_TMP, SEED_FILE) != 0)
            return WSEED_ERR_SD_IO;
        if (storage_mode_write(WSEED_MODE_KEEP) != 0) {
            if (seed_test_fail(WSEED_TEST_FAIL_SEED_REMOVE) ||
                (remove(SEED_FILE) != 0 && errno != ENOENT)) {
                // The rollback could not remove the verified KEEP copy. Retry
                // the publish now that one-shot faults are consumed. CLEANUP
                // always means the destination is active, regardless of path.
                if (storage_mode_write(WSEED_MODE_KEEP) == 0)
                    return WSEED_ERR_CLEANUP;
                if (remove(SEED_FILE) != 0 && errno != ENOENT)
                    return WSEED_ERR_ROLLBACK;
            }
            return WSEED_ERR_SD_IO;             // rolled back; SD still active
        }
    } else {
        // AMNESIC/setup: mode first, then promote. If promotion fails, restore
        // the previous promise before returning.
        if (storage_mode_write(WSEED_MODE_KEEP) != 0) {
            (void)remove(SEED_TMP);
            return WSEED_ERR_SD_IO;
        }
        if (rename(SEED_TMP, SEED_FILE) != 0) {
            if (storage_mode_write(old_mode) != 0) return WSEED_ERR_ROLLBACK;
            (void)remove(SEED_TMP);
            return WSEED_ERR_SD_IO;
        }
    }
#endif
    if (rc != WSEED_OK)
        return rc;

    char verify[WSEED_MAX_MNEMONIC];
    int mode = -1;
    rc = storage_read_keep(verify, sizeof verify) == 0 &&
         strcmp(verify, words) == 0 &&
         storage_mode_read_checked(&mode) == 0 &&
         mode == WSEED_MODE_KEEP ? WSEED_OK : WSEED_ERR_VERIFY;
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

// Residue scrub, after one KEEP wallet has replaced another.
//
// storage_write_keep persists the new mnemonic with nvs_set_str, which is a
// LOGICAL overwrite for exactly the reason spelled out above nvs_erase_key: NVS
// is log structured, so the mnemonic that was just replaced stays on its page,
// readable to anyone who dumps the chip. ERASE THIS WALLET takes the sector
// path because of that. START A NEW WALLET did not, so the wallet an owner
// deliberately walked away from outlived the act of walking away, on the one
// storage mode that is the default.
//
// Deliberately runs AFTER the replacement has committed and verified, not
// before. fp_tap_cb's whole invariant is "a failed commit leaves the old wallet
// untouched"; erasing first would trade a residue bug for a device that can end
// up holding nothing.
//
// Returns 0 on success, or if the erase never ran -- the new wallet is live and
// correct either way, and residue is a hardening miss, not a commit failure.
// -1 ONLY when the partition was erased and the words could not be put back,
// which is the one case where the caller must not report success.
static int storage_scrub_keep(const char *words)
{
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

    esp_err_t err = nvs_flash_deinit();
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_INITIALIZED)
        return 0;                       // nothing erased; the new wallet stands
    if (nvs_flash_erase() != ESP_OK) {
        (void)nvs_flash_init();
        return 0;
    }
    if (nvs_flash_init() != ESP_OK)
        return -1;                      // erased, and NVS will not come back

    // Past this point the words exist only in the caller's buffer. Retry once:
    // a fresh partition has every reason to accept a write, and the cost of
    // giving up here is a device with no wallet on it.
    int rc = -1;
    for (int attempt = 0; attempt < 2 && rc != 0; attempt++) {
        if (nvs_open("kiss", NVS_READWRITE, &h) != ESP_OK)
            continue;
        rc = 0;
        for (size_t i = 0; i < N_KEEP; i++)
            if (have[i] && nvs_set_u8(h, KEEP_KEYS[i], keep[i]) != ESP_OK)
                rc = -1;
        if (nvs_set_str(h, "words", words) != ESP_OK ||
            nvs_set_u8(h, "smode", WSEED_MODE_KEEP) != ESP_OK ||
            nvs_commit(h) != ESP_OK)
            rc = -1;
        nvs_close(h);
    }
    if (rc != 0)
        return -1;
#else
    // Host has no log-structured store to leave residue in: storage_write_keep
    // renames over the file. Do the same work anyway so the desktop tests walk
    // this path and assert the contract (right words, mode still KEEP) that the
    // device branch has to keep.
    if (remove(SEED_FILE) != 0 && errno != ENOENT)
        return 0;
    FILE *f = fopen(SEED_FILE, "w");
    if (!f)
        return -1;
    int rc = fputs(words, f) >= 0 ? 0 : -1;
    if (rc == 0 && fflush(f) != 0) rc = -1;
    if (rc == 0 && fsync(fileno(f)) != 0) rc = -1;
    if (fclose(f) != 0) rc = -1;
    if (rc != 0)
        return -1;
    if (storage_mode_write(WSEED_MODE_KEEP) != 0)
        return -1;
#endif
    char verify[WSEED_MAX_MNEMONIC];
    int mode = -1;
    int ok = storage_read_keep(verify, sizeof verify) == 0 &&
             strcmp(verify, words) == 0 &&
             storage_mode_read_checked(&mode) == 0 &&
             mode == WSEED_MODE_KEEP ? 0 : -1;
    wally_bzero(verify, sizeof verify);
    return ok;
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
        (m != WSEED_MODE_KEEP && m != WSEED_MODE_AMNESIC &&
         m != WSEED_MODE_SD))
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
        (m != WSEED_MODE_KEEP && m != WSEED_MODE_AMNESIC &&
         m != WSEED_MODE_SD))
        return -1;
    *out_mode = m;
    return 0;
#endif
}

static int storage_mode_read(void)
{
    int mode = WSEED_MODE_INVALID;
    // A genuinely fresh store is reported as KEEP by read_checked. Corrupt or
    // unreadable metadata is a separate blocked state, never AMNESIC/fresh.
    return storage_mode_read_checked(&mode) == 0
         ? mode : WSEED_MODE_INVALID;
}

static int storage_mode_write(int mode)
{
    if (mode != WSEED_MODE_KEEP && mode != WSEED_MODE_AMNESIC &&
        mode != WSEED_MODE_SD)
        return -1;
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READWRITE, &h) != ESP_OK)
        return -1;
    int rc = nvs_set_u8(h, "smode", (uint8_t)mode) == ESP_OK &&
             nvs_commit(h) == ESP_OK ? 0 : -1;
    nvs_close(h);
#else
    if (seed_test_fail(WSEED_TEST_FAIL_MODE_WRITE))
        return -1;
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

// SD is available whenever the card hardware is, which on this board is always.
// The seed is written to the card as an authenticated, device-key-sealed blob:
// a stolen card alone is inert (its key lives in this device's flash) and the
// device alone holds no card ciphertext. That split does not need flash
// encryption. Encryption is a separate, stronger layer that also protects the
// device key at rest, closing the device+card-together gap -- and FLASH mode,
// which stores the seed in the clear, is offered on the same firmware anyway.
// So there is no "SD unsupported" state to gate on; the predicate that used to
// express one is gone.

int wallet_seed_flash_encrypted(void)
{
#ifdef ESP_PLATFORM
    return esp_efuse_is_flash_encryption_enabled() ? 1 : 0;
#else
    return 0;
#endif
}

// SD is deliberately a separate backend instead of teaching platform_sd about
// keys. The platform sees only authenticated ciphertext; key access, BIP39
// validation and plaintext zeroization remain in the seed layer.
static int storage_read_sd(char *out, size_t out_len)
{
    if (!out || out_len < 2) return WSEED_ERR_INVALID;
    wally_bzero(out, out_len);
    if (platform_sd_mount() != 0) return WSEED_ERR_SD_MISSING;

    uint8_t blob[SDSEED_MAX_BLOB], key[32];
    size_t blob_len = 0;
    int rr = platform_sd_read(SDSEED_FILENAME, blob, sizeof blob, &blob_len);
    if (rr != 0) {
        wally_bzero(blob, sizeof blob);
        return rr == -3 ? WSEED_ERR_SD_IO : WSEED_ERR_SD_CORRUPT;
    }
    if (sd_seed_device_key(key) != 0) {
        wally_bzero(blob, sizeof blob);
        wally_bzero(key, sizeof key);
        return WSEED_ERR_SD_IO;
    }
    int rc = sd_seed_open(key, blob, blob_len, out, out_len) == 0 &&
             wallet_seed_validate(out) == 0 ? WSEED_OK : WSEED_ERR_SD_CORRUPT;
    wally_bzero(blob, sizeof blob);
    wally_bzero(key, sizeof key);
    if (rc != WSEED_OK) wally_bzero(out, out_len);
    return rc;
}

static int storage_write_sd(const char *words, bool *sidecar_cleanup)
{
    if (!words || wallet_seed_validate(words) != 0) return WSEED_ERR_INVALID;
    if (sidecar_cleanup) *sidecar_cleanup = false;
    if (platform_sd_mount() != 0) return WSEED_ERR_SD_MISSING;

    uint8_t blob[SDSEED_MAX_BLOB], key[32];
    size_t blob_len = 0;
    int rc = WSEED_ERR_SD_IO;
    if (sd_seed_device_key(key) != 0) goto out;
    if (sd_seed_seal(key, words, blob, sizeof blob, &blob_len) != 0) goto out;
    int wr = platform_sd_write_atomic(SDSEED_FILENAME, blob, blob_len);
    if (wr < 0) goto out;
    if (sidecar_cleanup && wr == PLATFORM_SD_ATOMIC_CLEANUP)
        *sidecar_cleanup = true;

    // Verify through the full public read path: file I/O, MAC, decrypt, BIP39.
    char verify[WSEED_MAX_MNEMONIC];
    int vr = storage_read_sd(verify, sizeof verify);
    rc = vr != WSEED_OK ? vr
       : strcmp(verify, words) == 0 ? WSEED_OK : WSEED_ERR_VERIFY;
    wally_bzero(verify, sizeof verify);
out:
    wally_bzero(blob, sizeof blob);
    wally_bzero(key, sizeof key);
    return rc;
}

static int storage_delete_sd(void)
{
    if (platform_sd_mount() != 0) return WSEED_ERR_SD_MISSING;
    return platform_sd_delete(SDSEED_FILENAME) == 0
         ? WSEED_OK : WSEED_ERR_SD_IO;
}

// Publish SD only after its sealed file has been written and verified. On the
// device this ONE encrypted-NVS transaction removes words and changes smode
// while retaining dkey. A whole-partition erase here would introduce a power
// cut window in which the card survives but its only key does not.
static int storage_publish_sd(void)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READWRITE, &h) != ESP_OK)
        return WSEED_ERR_SD_IO;
    // NVS set/erase calls are individually durable; commit is not a
    // transaction. Publish SD mode first (its card is already verified), then
    // clean the now-old words. A cut between them leaves two copies, never zero.
    int rc = nvs_set_u8(h, "smode", WSEED_MODE_SD) == ESP_OK &&
             nvs_commit(h) == ESP_OK ? WSEED_OK : WSEED_ERR_SD_IO;
    if (rc == WSEED_OK) {
        esp_err_t er = nvs_erase_key(h, "words");
        if (!((er == ESP_OK || er == ESP_ERR_NVS_NOT_FOUND) &&
              nvs_commit(h) == ESP_OK))
            rc = WSEED_ERR_CLEANUP;
    }
    nvs_close(h);
#else
    // Host state uses two files rather than NVS. Publish the verified SD mode
    // first; a power cut before deleting the emulated flash seed leaves two
    // copies, never zero, and SD remains the authoritative one.
    int rc = storage_mode_write(WSEED_MODE_SD) == 0
           ? WSEED_OK : WSEED_ERR_SD_IO;
    if (rc == WSEED_OK &&
        (seed_test_fail(WSEED_TEST_FAIL_SEED_REMOVE) ||
         (remove(SEED_FILE) != 0 && errno != ENOENT)))
        rc = WSEED_ERR_CLEANUP;
    (void)remove(SEED_TMP);
#endif
    int verify = -1;
    int published = storage_mode_read_checked(&verify) == 0 &&
                    verify == WSEED_MODE_SD;
    // Any error after SD became durable is a cleanup/uncertain-state result,
    // never permission to delete the now-authoritative card.
    if (published && rc != WSEED_OK) return WSEED_ERR_CLEANUP;
    if (!published && rc == WSEED_OK) return WSEED_ERR_VERIFY;
    return rc;
}

// ---- staged (not-yet-committed) mnemonic ----
// In KEEP mode this is the setup safety net: staged in RAM, written to flash
// only once the whole ritual finishes. In AMNESIC mode it is the wallet — it
// is never written anywhere, and wallet_seed_forget() (called from
// wallet_session_close) is what ends the session.
// A setup candidate must not overwrite the currently loaded AMNESIC wallet:
// cancelling Replace Wallet has to reveal the old session unchanged.
static char s_pending[WSEED_MAX_MNEMONIC];
static bool s_has_pending;
static char s_active_ram[WSEED_MAX_MNEMONIC];
static bool s_has_active_ram;

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
                   : mode == WSEED_MODE_SD      ? WSEED_MODE_SD
                                                : WSEED_MODE_KEEP;
}

int wallet_seed_set_mode(int mode)
{
    if (mode != WSEED_MODE_KEEP && mode != WSEED_MODE_AMNESIC &&
        mode != WSEED_MODE_SD)
        return WSEED_ERR_INVALID;
    if (mode == WSEED_MODE_SD)
        return WSEED_ERR_NO_SEED;        // use stage+commit or move_to: never label
                                         // an absent/unverified card as the wallet
    if (mode == WSEED_MODE_KEEP && storage_mode_read() == WSEED_MODE_SD)
        return wallet_seed_move_to(WSEED_MODE_KEEP);
    // Turning amnesic ON has to take the stored seed with it, otherwise the
    // screen would claim "nothing saved" while flash still held the words.
    int rc = mode == WSEED_MODE_AMNESIC
           ? storage_erase(WSEED_MODE_AMNESIC)
           : storage_mode_write(WSEED_MODE_KEEP);
    if (rc == 0) {
        s_pending_mode = -1;           // only publish a verified mode change
        if (mode == WSEED_MODE_AMNESIC) {
            wally_bzero(s_active_ram, sizeof s_active_ram);
            s_has_active_ram = false;
            wallet_usage_wipe();
            wallet_backup_forget();    // amnesic keeps no metadata either
        }
    }
    return rc;
}

int wallet_seed_stage(const char *mnemonic)
{
    if (wallet_seed_validate(mnemonic) != 0)
        return -1;
    // The per-boot AMNESIC load has no staged storage choice. It is the active
    // wallet, not a replacement candidate. Setup always stages a mode first.
    if (storage_mode_read() == WSEED_MODE_AMNESIC && s_pending_mode < 0) {
        snprintf(s_active_ram, sizeof s_active_ram, "%s", mnemonic);
        s_has_active_ram = true;
        return WSEED_OK;
    }
    snprintf(s_pending, sizeof s_pending, "%s", mnemonic);
    s_has_pending = true;
    return WSEED_OK;
}

// The ONE moment flash changes. Everything the wizard collected (the words and
// the storage mode) lands here together, or not at all.
static void clear_staged(void)
{
    wally_bzero(s_pending, sizeof s_pending);
    s_has_pending = false;
    s_pending_mode = -1;
}

static void clear_active_ram(void)
{
    wally_bzero(s_active_ram, sizeof s_active_ram);
    s_has_active_ram = false;
}

int wallet_seed_commit(void)
{
    // Legacy AMNESIC load code stages+commits; the load is already the active
    // RAM wallet and committing it is intentionally a no-op.
    if (!s_has_pending)
        return storage_mode_read() == WSEED_MODE_AMNESIC && s_has_active_ram
             ? WSEED_OK : WSEED_ERR_NO_SEED;
    int mode = wallet_seed_mode();
    int prior_mode = storage_mode_read();
    if (prior_mode == WSEED_MODE_INVALID)
        return WSEED_ERR_VERIFY;
    if (mode == WSEED_MODE_AMNESIC) {
        if (storage_erase(WSEED_MODE_AMNESIC) != 0)
            return WSEED_ERR_SD_IO;
        int key_cleanup = sd_seed_forget_device_key();
        int card_cleanup = storage_delete_sd();
        wallet_usage_forget_session();   // replacement is a different wallet
        snprintf(s_active_ram, sizeof s_active_ram, "%s", s_pending);
        s_has_active_ram = true;
        clear_staged();
        // Either deleting the card or destroying its key invalidates an old SD
        // wallet. Only report cleanup if both halves might still survive.
        return prior_mode == WSEED_MODE_SD && key_cleanup != 0 &&
               card_cleanup != WSEED_OK
             ? WSEED_ERR_CLEANUP : WSEED_OK;
    }
    if (mode == WSEED_MODE_SD) {
        bool sidecar_cleanup = false;
        int rc = storage_write_sd(s_pending, &sidecar_cleanup);
        if (rc != WSEED_OK)
            return rc;
        // Replacing one SD wallet with another already committed when the
        // authenticated atomic card replacement verified; mode/key are unchanged.
        if (prior_mode == WSEED_MODE_SD) {
            clear_staged();
            return sidecar_cleanup ? WSEED_ERR_CLEANUP : WSEED_OK;
        }
        rc = storage_publish_sd();
        if (rc == WSEED_ERR_CLEANUP) {
            clear_staged();
            if (prior_mode == WSEED_MODE_AMNESIC) {
                wallet_usage_persist_session();
                clear_active_ram();
            }
            return rc;
        }
        if (rc != WSEED_OK) {
            int cleanup = storage_delete_sd();
            if (cleanup != WSEED_OK) return WSEED_ERR_ROLLBACK;
            return rc;
        }
        clear_staged();
        if (prior_mode == WSEED_MODE_AMNESIC) {
            wallet_usage_persist_session();
            clear_active_ram();
        }
        return sidecar_cleanup ? WSEED_ERR_CLEANUP : WSEED_OK;
    }
    // Ask BEFORE the write whether there was a wallet here at all, because
    // after it the answer is always yes. prior_mode is not enough on its own:
    // storage_mode_read reports a factory-fresh store as KEEP (it says so), so
    // gating the scrub on the mode alone would erase the partition during
    // first-time setup, when there is nothing to scrub and the only wallet on
    // the device is the one being written.
    bool had_prior_words = false;
    if (prior_mode == WSEED_MODE_KEEP) {
        char prev[WSEED_MAX_MNEMONIC];
        had_prior_words = storage_read_keep(prev, sizeof prev) == 0;
        wally_bzero(prev, sizeof prev);
    }
    int rc = storage_write_keep(s_pending);
    if (rc != WSEED_OK && rc != WSEED_ERR_CLEANUP)
        return rc;   // still staged, but the caller decides: the setup login
                     // discards it rather than hold an unsaved mnemonic in RAM
    int result = rc;
    if (had_prior_words) {
        // One KEEP wallet just replaced another, so the previous mnemonic is
        // sitting on an NVS page that was only logically overwritten. Scrub it.
        //
        // Only this transition. From SD, storage_write_keep above is mid
        // handover and erasing would take dkey with it; from AMNESIC there was
        // never anything in flash to scrub.
        //
        // A scrub that could not erase reports success on purpose: the wallet
        // is committed and verified either way, and returning anything nonzero
        // here would send fp_tap_cb into wallet_seed_discard() on a wallet that
        // is already durable.
        if (storage_scrub_keep(s_pending) != 0)
            return WSEED_ERR_SD_IO;
        // On device the erase above was the whole NVS partition, so it already
        // took the receive-index history and the paper-check marks with it. Say
        // it explicitly so the host build ends up in the SAME state: otherwise
        // the simulator would show a replacement wallet wearing the replaced
        // one's green "paper checked", which is the exact lie this row exists
        // to avoid. A replacement is a different wallet, as the AMNESIC branch
        // above already says.
        wallet_usage_wipe();
        wallet_backup_forget();
        // The unlock stroke went with it too, and for the same reason: "greal"
        // is deliberately outside KEEP_KEYS so the partition erase takes it.
        // The host build keeps it in a static, so without this line the
        // simulator shows a brand new wallet still opening on the replaced
        // wallet's decoy gesture -- and the DURESS row would name it.
        wallet_duress_forget();
    }
    if (prior_mode == WSEED_MODE_SD) {
        int card_cleanup = storage_delete_sd();
        int key_cleanup = sd_seed_forget_device_key();
        if (card_cleanup != WSEED_OK || key_cleanup != 0)
            result = WSEED_ERR_CLEANUP;
    }
    clear_staged();
    if (prior_mode == WSEED_MODE_AMNESIC) {
        wallet_usage_persist_session();
        clear_active_ram();
    }
    return result;
}

// Backing out of setup, at any step, for any reason. Nothing was written yet,
// so this only has to drop what is held in RAM: the words AND the storage-mode
// answer, which reverts wallet_seed_mode() to whatever flash still says.
void wallet_seed_discard(void)
{
    clear_staged();
}

// ---- API ----
int wallet_seed_exists(void)
{
    if (s_has_pending)
        return 1;
    int mode = storage_mode_read();
    // A configured SD wallet still exists when its card is in the user's
    // pocket. Returning false here would send boot into first-time setup and
    // make "missing second factor" look like "no wallet".
    if (mode == WSEED_MODE_SD)
        return 1;
    if (mode == WSEED_MODE_INVALID)
        return 1;                       // blocked metadata, never first-time setup
    if (mode == WSEED_MODE_AMNESIC)
        return s_has_active_ram ? 1 : 0;
    if (mode != WSEED_MODE_KEEP)
        return 0;
    char tmp[WSEED_MAX_MNEMONIC];
    int rc = storage_read_keep(tmp, sizeof tmp) == 0 ? 1 : 0;
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
        return WSEED_ERR_INVALID;
    wally_bzero(out, out_len);
    if (s_has_pending) {               // staged setup: derive before it's flashed
        snprintf(out, out_len, "%s", s_pending);
        return WSEED_OK;
    }
    int mode = storage_mode_read();
    if (mode == WSEED_MODE_AMNESIC) {
        if (!s_has_active_ram)
            return WSEED_ERR_NO_SEED;
        snprintf(out, out_len, "%s", s_active_ram);
        return WSEED_OK;
    }
    if (mode == WSEED_MODE_SD)
        return storage_read_sd(out, out_len);
    if (mode == WSEED_MODE_INVALID)
        return WSEED_ERR_VERIFY;
    if (mode != WSEED_MODE_KEEP)
        return WSEED_ERR_NO_SEED;
    return storage_read_keep(out, out_len) == 0
         ? WSEED_OK : WSEED_ERR_NO_SEED;
}

int wallet_seed_wipe(void)
{
    clear_staged();
    clear_active_ram();
    wallet_usage_wipe();
    wallet_backup_forget();          // the paper check was about THAT wallet

    // Best effort on the card, but absence cannot block a wipe: destroying the
    // device key below permanently invalidates a card that is elsewhere.
    if (platform_sd_mount() == 0)
        (void)platform_sd_delete(SDSEED_FILENAME);

    // Device: whole-partition erase physically removes words AND dkey. Host:
    // storage_erase handles seed/mode and the explicit helper removes its key
    // file. Calling the helper on device too verifies the post-erase state.
    if (storage_erase(WSEED_MODE_KEEP) != 0)
        return WSEED_ERR_SD_IO;
    if (sd_seed_forget_device_key() != 0)
        return WSEED_ERR_SD_IO;
    // A leftover sealed file is cryptographically erased with its only key.
    // Do not tell Settings that the wallet survived a successful key wipe.
    return WSEED_OK;
}

void wallet_seed_forget(void)
{
    // During Replace Wallet, closing a failed candidate session must not erase
    // the pre-existing AMNESIC wallet. The candidate is discarded separately.
    if (storage_mode_read() == WSEED_MODE_AMNESIC && !s_has_pending)
        clear_active_ram();
    wallet_usage_forget_session();
}

int wallet_seed_move_to(int mode)
{
    if (mode != WSEED_MODE_KEEP && mode != WSEED_MODE_AMNESIC &&
        mode != WSEED_MODE_SD)
        return WSEED_ERR_INVALID;

    if (s_has_pending)
        return WSEED_ERR_INVALID;       // setup owns the staged candidate
    int source = storage_mode_read();
    if (source == mode)
        return WSEED_OK;
    if (source != WSEED_MODE_KEEP && source != WSEED_MODE_AMNESIC &&
        source != WSEED_MODE_SD)
        return WSEED_ERR_INVALID;

    char words[WSEED_MAX_MNEMONIC], verify[WSEED_MAX_MNEMONIC];
    int rc = wallet_seed_load(words, sizeof words);
    if (rc != WSEED_OK) {
        wally_bzero(words, sizeof words);
        return rc;
    }
    if (wallet_seed_validate(words) != 0) {
        wally_bzero(words, sizeof words);
        return source == WSEED_MODE_SD ? WSEED_ERR_SD_CORRUPT
                                      : WSEED_ERR_VERIFY;
    }

    if (mode == WSEED_MODE_KEEP) {
        // Destination NVS writes words+KEEP in one commit. Only after a
        // byte-for-byte readback may the SD source and its device key go.
        rc = storage_write_keep(words);
        if (rc != WSEED_OK) {
            // CLEANUP can mean KEEP committed despite a reported metadata
            // failure. Inspect and verify before deciding which source owns it.
            if (rc != WSEED_ERR_CLEANUP ||
                storage_mode_read() != WSEED_MODE_KEEP ||
                storage_read_keep(verify, sizeof verify) != 0 ||
                strcmp(verify, words) != 0)
                goto out;
            wally_bzero(verify, sizeof verify);
        }
        int committed_with_cleanup = rc == WSEED_ERR_CLEANUP;
        rc = storage_read_keep(verify, sizeof verify) == 0 &&
             strcmp(verify, words) == 0 ? WSEED_OK : WSEED_ERR_VERIFY;
        wally_bzero(verify, sizeof verify);
        if (rc != WSEED_OK) goto out;

        if (source == WSEED_MODE_SD) {
            int cleanup = storage_delete_sd();
            // Whether or not the card can be cleaned, forgetting its key makes
            // a leftover sealed file useless while KEEP remains verified.
            if (sd_seed_forget_device_key() != 0 ||
                cleanup != WSEED_OK)
                rc = WSEED_ERR_CLEANUP;
        }
        if (source == WSEED_MODE_AMNESIC) {
            wallet_usage_persist_session();
            clear_active_ram();
        }
        if (committed_with_cleanup) rc = WSEED_ERR_CLEANUP;
        goto out;
    }

    if (mode == WSEED_MODE_SD) {
        // Sealed destination first, including decrypt+MAC+BIP39+byte compare.
        bool sidecar_cleanup = false;
        rc = storage_write_sd(words, &sidecar_cleanup);
        if (rc != WSEED_OK) goto out;
        rc = storage_publish_sd();
        if (rc == WSEED_ERR_CLEANUP) {
            // SD is already the durable mode. Keep its verified card and clear
            // an AMNESIC source; the caller must report the leftover old copy.
            if (source == WSEED_MODE_AMNESIC) {
                wallet_usage_persist_session();
                clear_active_ram();
            }
            s_pending_mode = -1;
            goto out;
        }
        if (rc != WSEED_OK) {
            // KEEP/AMNESIC is still authoritative. Remove the uncommitted card
            // copy if possible; failure means two copies, never zero.
            int cleanup = storage_delete_sd();
            if (cleanup != WSEED_OK) rc = WSEED_ERR_ROLLBACK;
            goto out;
        }
        if (source == WSEED_MODE_AMNESIC) {
            wallet_usage_persist_session();
            clear_active_ram();
        }
        s_pending_mode = -1;
        if (sidecar_cleanup) rc = WSEED_ERR_CLEANUP;
        goto out;
    }

    // Destination AMNESIC is the validated RAM copy. Publish it before erasing
    // persistence, then physically erase NVS (including dkey). If the source
    // was SD, delete its now-unreadable ciphertext afterwards.
    snprintf(s_active_ram, sizeof s_active_ram, "%s", words);
    s_has_active_ram = true;
    s_pending_mode = -1;
    if (storage_erase(WSEED_MODE_AMNESIC) != 0) {
        clear_active_ram();
        rc = WSEED_ERR_SD_IO;
        goto out;
    }
    if (sd_seed_forget_device_key() != 0) {
        rc = WSEED_ERR_CLEANUP;
        goto out;
    }
    if (source == WSEED_MODE_SD) {
        int cleanup = storage_delete_sd();
        if (cleanup != WSEED_OK && cleanup != WSEED_ERR_SD_MISSING)
            rc = WSEED_ERR_CLEANUP;
        else
            rc = WSEED_OK;
    } else {
        // Remove any stale card artifact from an earlier cleanup failure; a
        // missing card is fine because its key has just been destroyed.
        int cleanup = storage_delete_sd();
        rc = cleanup == WSEED_ERR_SD_IO ? WSEED_ERR_CLEANUP : WSEED_OK;
    }
out:
    wally_bzero(words, sizeof words);
    wally_bzero(verify, sizeof verify);
    return rc;
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

    // CompactSeedQR: raw entropy, no encoding at all. Every other seed route in
    // this device passes its entropy through a health check first (camera floor
    // and novelty, dice histogram and period); this one is 16 or 32 bytes off a
    // QR and straight into a wallet, so a printed square of 32 zero bytes used
    // to become a real, funded-if-you-fund-it wallet with nothing said. It is
    // the owner's own QR, so this is a footgun rather than an attack -- but the
    // check is two lines and the failure is total.
    //
    // Deliberately narrow: only the degenerate cases that cannot be an accident
    // of a real generator. A byte pattern this weak is a mistake or a joke, and
    // a real 128/256 bits of entropy has no chance of tripping it.
    if (len == 16 || len == 32) {
        const uint8_t *e = (const uint8_t *)data;
        unsigned same = 0, bits = 0;
        for (size_t i = 0; i < len; i++) {
            if (e[i] == e[0]) same++;
            for (uint8_t b = e[i]; b; b &= (uint8_t)(b - 1)) bits++;
        }
        // all one byte value (00.., ff.., aa..), or fewer than a tenth of the
        // bits set either way -- the shapes a hand-drawn or blank QR produces.
        if (same == len || bits < len || bits > len * 8 - len)
            goto fail;
        if (wallet_seed_from_entropy(e, len, out, out_len) != 0)
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
