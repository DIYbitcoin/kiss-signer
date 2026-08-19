// Step 7: the user's own seed — BIP39 helpers (libwally) + persistence.
// Device: NVS blob (plaintext until step 8 enables flash encryption).
// Desktop tests: a plain file, so /tmp/kisstest exercises identical logic.
#include "kiss_seed.h"
#include "kiss_seed_sd.h"
#include "platform_sd.h"
#include "kiss_usage.h"
#include "kiss_payee.h"
#include "kiss_backup.h"   // the paper check dies with the wallet it was about
#include "kiss_duress.h"   // and so does the stroke that opened it

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
#include "kiss_simpath.h"
// Under KISS_SIM_TMP (kiss_simpath.h), which is /tmp unless a run sets it. The
// accessors keep every call site the shape it already had, including the ones
// that name two of these in one expression.
KISS_SIM_PATH_FN(seed_file_path, "kiss_seed.txt")
KISS_SIM_PATH_FN(mode_file_path, "kiss_seed_mode.txt")
KISS_SIM_PATH_FN(seed_tmp_path,  "kiss_seed.txt.tmp")
KISS_SIM_PATH_FN(mode_tmp_path,  "kiss_seed_mode.txt.tmp")
KISS_SIM_PATH_FN(entq_file_path, "kiss_seed_entq.txt")
#define SEED_FILE seed_file_path()
#define MODE_FILE mode_file_path()
#define SEED_TMP  seed_tmp_path()
#define MODE_TMP  mode_tmp_path()
#define ENTQ_FILE entq_file_path()
static unsigned s_seed_test_fail;
void kiss_seed_test_fail_next(unsigned flags) { s_seed_test_fail = flags; }
static int seed_test_fail(unsigned flag)
{
    if (!(s_seed_test_fail & flag)) return 0;
    s_seed_test_fail &= ~flag;
    return 1;
}
#endif

// ---- storage backends ----
//
// KEEP holds a sealed blob (kiss_seed_sd.h, NVS domain). The tag is the
// point: flash encryption is XTS and carries none, so without it an altered or
// half-written seed decrypts to garbage that the device cannot tell apart from
// a hardware fault. The ciphertext around it buys no secrecy here -- the key
// lives in the same partition -- and no screen may claim it does.
//
// Three outcomes, not two. A seed that fails its tag is NOT an empty slot: a
// device that confuses the two offers to make a new wallet over a seed it just
// refused to load, which is the worst possible reading of a bad byte.
#define KEEP_OK      0
#define KEEP_ABSENT  (-1)
#define KEEP_BAD     (-2)

// Set when the last read found the pre-tag plaintext format, so the caller can
// migrate it. Read paths must stay side effect free: they run inside
// verify-after-write, and a partition erase in there would be a disaster.
static int s_keep_legacy;

static int keep_unseal(const uint8_t *blob, size_t blob_len,
                       char *out, size_t out_len)
{
    uint8_t key[32];
    if (sd_seed_domain_key(SDSEED_DOM_NVS, key) != 0)
        return KEEP_BAD;                // the key is gone; the words are not readable
    int rc = sd_seed_open_in(SDSEED_DOM_NVS, key, blob, blob_len, out, out_len)
           == 0 ? KEEP_OK : KEEP_BAD;
    wally_bzero(key, sizeof key);
    return rc;
}

static int storage_read_keep(char *out, size_t out_len)
{
    s_keep_legacy = 0;
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    esp_err_t orc = nvs_open("kiss", NVS_READONLY, &h);
    if (orc != ESP_OK)
        // A missing namespace is an empty slot; anything else (a corrupt
        // area) is damage and must never read as absent, or setup would
        // offer a fresh wallet over it.
        return orc == ESP_ERR_NVS_NOT_FOUND ? KEEP_ABSENT : KEEP_BAD;
    uint8_t blob[SDSEED_MAX_BLOB];
    size_t blen = sizeof blob;
    esp_err_t err = nvs_get_blob(h, "wblob", blob, &blen);
    if (err == ESP_OK) {
        nvs_close(h);
        int rc = keep_unseal(blob, blen, out, out_len);
        wally_bzero(blob, sizeof blob);
        return rc;
    }
    wally_bzero(blob, sizeof blob);
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return KEEP_BAD;                // a real read failure, not an empty slot
    }
    // Pre-tag devices. The words are still theirs; kiss_seed_load migrates.
    size_t len = out_len;
    esp_err_t nrc = nvs_get_str(h, "words", out, &len);
    // nvs_get_str answers ESP_ERR_NVS_INVALID_LENGTH to an entry longer than
    // out_len. That is damage, not absence: a bad string must never read as
    // an empty slot, or setup would offer a fresh wallet over it.
    int rc = nrc == ESP_OK ? KEEP_OK
           : nrc == ESP_ERR_NVS_NOT_FOUND ? KEEP_ABSENT : KEEP_BAD;
    nvs_close(h);
    if (rc == KEEP_OK && kiss_seed_validate(out) != 0) {
        wally_bzero(out, out_len);
        return KEEP_BAD;
    }
    if (rc == KEEP_OK) s_keep_legacy = 1;
    return rc;
#else
    FILE *f = fopen(SEED_FILE, "rb");
    if (!f)
        return KEEP_ABSENT;
    uint8_t blob[SDSEED_MAX_BLOB];
    size_t n = fread(blob, 1, sizeof blob, f);
    fclose(f);
    if (n == 0)
        return KEEP_ABSENT;
    // The magic discriminates the two formats on host, where both live in one
    // file. A sealed blob always starts with it; a mnemonic never can.
    if (n > SDSEED_MAGIC_LEN &&
        memcmp(blob, SDSEED_MAGIC, SDSEED_MAGIC_LEN) == 0) {
        int rc = keep_unseal(blob, n, out, out_len);
        wally_bzero(blob, sizeof blob);
        return rc;
    }
    if (n >= out_len) {
        wally_bzero(blob, sizeof blob);
        return KEEP_BAD;
    }
    memcpy(out, blob, n);
    out[n] = 0;
    wally_bzero(blob, sizeof blob);
    // The magic chose this branch, so damage that lands in the first eight
    // bytes arrives here looking like the pre-tag format. Only something that
    // is actually a mnemonic may be treated as one; anything else is damage,
    // and saying so is the difference between a refusal and garbage words.
    if (kiss_seed_validate(out) != 0) {
        wally_bzero(out, out_len);
        return KEEP_BAD;
    }
    s_keep_legacy = 1;
    return KEEP_OK;
#endif
}

static int storage_mode_read_checked(int *out_mode);
static int storage_mode_read(void);
static int storage_mode_write(int mode);

static int keep_seal(const char *words, uint8_t *blob, size_t *blen)
{
    uint8_t key[32];
    if (sd_seed_domain_key(SDSEED_DOM_NVS, key) != 0)
        return -1;
    int rc = sd_seed_seal_in(SDSEED_DOM_NVS, key, words, blob, SDSEED_MAX_BLOB,
                             blen);
    wally_bzero(key, sizeof key);
    return rc;
}

// KEEP mode's words and mode flag are one NVS commit on-device. A power loss
// must never leave a newly persisted seed paired with AMNESIC UI state.
static int storage_write_keep(const char *words)
{
    int old_mode = storage_mode_read();
    uint8_t blob[SDSEED_MAX_BLOB];
    size_t blen = 0;
    if (keep_seal(words, blob, &blen) != 0) {
        wally_bzero(blob, sizeof blob);
        return WSEED_ERR_SD_IO;
    }
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READWRITE, &h) != ESP_OK) {
        wally_bzero(blob, sizeof blob);
        return WSEED_ERR_SD_IO;
    }
    int rc;
    if (old_mode == WSEED_MODE_SD) {
        // NVS commit is not a multi-key transaction. Persist and verify the
        // destination words while SD remains authoritative, then flip mode.
        rc = nvs_set_blob(h, "wblob", blob, blen) == ESP_OK &&
             nvs_commit(h) == ESP_OK ? WSEED_OK : WSEED_ERR_SD_IO;
        if (rc == WSEED_OK) {
            char verify[WSEED_MAX_MNEMONIC];
            rc = storage_read_keep(verify, sizeof verify) == KEEP_OK &&
                 strcmp(verify, words) == 0 ? WSEED_OK : WSEED_ERR_VERIFY;
            wally_bzero(verify, sizeof verify);
        }
        if (rc == WSEED_OK &&
            (nvs_set_u8(h, "smode", WSEED_MODE_KEEP) != ESP_OK ||
             nvs_commit(h) != ESP_OK)) {
            esp_err_t er = nvs_erase_key(h, "wblob");
            rc = (er == ESP_OK || er == ESP_ERR_NVS_NOT_FOUND) &&
                 nvs_commit(h) == ESP_OK
               ? WSEED_ERR_SD_IO : WSEED_ERR_ROLLBACK;
        }
    } else {
        rc = nvs_set_blob(h, "wblob", blob, blen) == ESP_OK &&
             nvs_set_u8(h, "smode", WSEED_MODE_KEEP) == ESP_OK &&
             nvs_commit(h) == ESP_OK ? WSEED_OK : WSEED_ERR_SD_IO;
    }
    nvs_close(h);
#else
    // Build the host destination in a temp first. It is promoted in the order
    // appropriate to its source, with rollback on every injected failure.
    FILE *f = fopen(SEED_TMP, "wb");
    if (!f) {
        wally_bzero(blob, sizeof blob);
        return WSEED_ERR_SD_IO;
    }
    int rc = fwrite(blob, 1, blen, f) == blen ? WSEED_OK : WSEED_ERR_SD_IO;
    if (rc == WSEED_OK && fflush(f) != 0) rc = WSEED_ERR_SD_IO;
    if (rc == WSEED_OK && fsync(fileno(f)) != 0) rc = WSEED_ERR_SD_IO;
    if (fclose(f) != 0) rc = WSEED_ERR_SD_IO;
    if (rc != WSEED_OK) {
        (void)remove(SEED_TMP);
        wally_bzero(blob, sizeof blob);
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
    wally_bzero(blob, sizeof blob);
    if (rc != WSEED_OK)
        return rc;

    // Verify through the full public read path, tag included, the way SD
    // already does. A blob that cannot be opened must never be published as a
    // wallet, whatever the write layer reported.
    char verify[WSEED_MAX_MNEMONIC];
    int mode = -1;
    rc = storage_read_keep(verify, sizeof verify) == KEEP_OK &&
         strcmp(verify, words) == 0 &&
         storage_mode_read_checked(&mode) == 0 &&
         mode == WSEED_MODE_KEEP ? WSEED_OK : WSEED_ERR_VERIFY;
#ifndef ESP_PLATFORM
    // After the write, deliberately: the point of this failure is that the new
    // blob is already committed over the old one when it fires.
    if (seed_test_fail(WSEED_TEST_FAIL_VERIFY))
        rc = WSEED_ERR_VERIFY;
#endif
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
    // The partition erase above already took the flash key on device. Say so
    // explicitly so the host build ends in the same state and the tests can
    // see the seed become unreadable rather than merely absent.
    if (sd_seed_forget_flash_key() != 0)
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
#ifndef ESP_PLATFORM
    // Erased, and the words did not go back: the exact shape of the -1 below,
    // reproduced rather than described, so the caller's handling of it is what
    // the test actually exercises.
    if (seed_test_fail(WSEED_TEST_FAIL_SCRUB)) {
        (void)remove(SEED_FILE);
        return -1;
    }
#endif
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
    //
    // Seal before the handle is opened, not inside the loop. The erase took
    // the device key with it, so this mints a fresh one through its own NVS
    // handle, and doing that under a handle we already hold is the kind of
    // nesting that differs between IDF versions.
    int rc = -1;
    uint8_t blob[SDSEED_MAX_BLOB];
    size_t blen = 0;
    for (int attempt = 0; attempt < 2 && rc != 0; attempt++) {
        if (keep_seal(words, blob, &blen) != 0)
            continue;
        if (nvs_open("kiss", NVS_READWRITE, &h) != ESP_OK)
            continue;
        rc = 0;
        for (size_t i = 0; i < N_KEEP; i++)
            if (have[i] && nvs_set_u8(h, KEEP_KEYS[i], keep[i]) != ESP_OK)
                rc = -1;
        if (nvs_set_blob(h, "wblob", blob, blen) != ESP_OK ||
            nvs_set_u8(h, "smode", WSEED_MODE_KEEP) != ESP_OK ||
            nvs_commit(h) != ESP_OK)
            rc = -1;
        nvs_close(h);
    }
    wally_bzero(blob, sizeof blob);
    if (rc != 0)
        return -1;
#else
    // Host has no log-structured store to leave residue in: storage_write_keep
    // renames over the file. Do the same work anyway so the desktop tests walk
    // this path and assert the contract (right words, mode still KEEP) that the
    // device branch has to keep.
    if (remove(SEED_FILE) != 0 && errno != ENOENT)
        return 0;
    uint8_t blob[SDSEED_MAX_BLOB];
    size_t blen = 0;
    if (keep_seal(words, blob, &blen) != 0) {
        wally_bzero(blob, sizeof blob);
        return -1;
    }
    FILE *f = fopen(SEED_FILE, "wb");
    if (!f) {
        wally_bzero(blob, sizeof blob);
        return -1;
    }
    int rc = fwrite(blob, 1, blen, f) == blen ? 0 : -1;
    if (rc == 0 && fflush(f) != 0) rc = -1;
    if (rc == 0 && fsync(fileno(f)) != 0) rc = -1;
    if (fclose(f) != 0) rc = -1;
    wally_bzero(blob, sizeof blob);
    if (rc != 0)
        return -1;
    if (storage_mode_write(WSEED_MODE_KEEP) != 0)
        return -1;
#endif
    char verify[WSEED_MAX_MNEMONIC];
    int mode = -1;
    int ok = storage_read_keep(verify, sizeof verify) == KEEP_OK &&
             strcmp(verify, words) == 0 &&
             storage_mode_read_checked(&mode) == 0 &&
             mode == WSEED_MODE_KEEP ? 0 : -1;
    wally_bzero(verify, sizeof verify);
    return ok;
}

// One-time upgrade from the pre-tag plaintext format, run from
// kiss_seed_load and nowhere else. Every other reader of KEEP is a
// verify-after-write, and side effects in there would fire mid commit.
//
// Deliberately NOT storage_scrub_keep, though the residue argument points that
// way. That function erases the whole partition and restores four preference
// keys; the duress gesture, the paper backup record and the usage counters are
// not among them. Erasing them is right when the owner walks away from a
// wallet and wrong when the only thing changing is the byte format of a wallet
// they are keeping. Losing the stroke that opens the real wallet is a worse
// outcome than the residue below.
//
// So the legacy plaintext stays on its page until a wipe or a new wallet takes
// the sector path. That is the same trade storage_publish_sd already records,
// and it ends for good on the encrypted lane, where the residue is ciphertext.
//
// Failure is never fatal: the words are already in the caller's buffer and
// correct, and the legacy copy is only dropped after the sealed one has been
// read back through the public path. A device that cannot upgrade still opens
// its wallet and tries again next load.
static int storage_migrate_keep(const char *words)
{
    if (!words || kiss_seed_validate(words) != 0)
        return -1;

    uint8_t blob[SDSEED_MAX_BLOB];
    size_t blen = 0;
    if (keep_seal(words, blob, &blen) != 0) {
        wally_bzero(blob, sizeof blob);
        return -1;
    }
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READWRITE, &h) != ESP_OK) {
        wally_bzero(blob, sizeof blob);
        return -1;
    }
    int rc = nvs_set_blob(h, "wblob", blob, blen) == ESP_OK &&
             nvs_commit(h) == ESP_OK ? 0 : -1;
    nvs_close(h);
#else
    FILE *f = fopen(SEED_TMP, "wb");
    if (!f) {
        wally_bzero(blob, sizeof blob);
        return -1;
    }
    int rc = fwrite(blob, 1, blen, f) == blen ? 0 : -1;
    if (rc == 0 && fflush(f) != 0) rc = -1;
    if (rc == 0 && fsync(fileno(f)) != 0) rc = -1;
    if (fclose(f) != 0) rc = -1;
    // Host has no second key to read first, so the rename IS the switch: the
    // file holds one format or the other and never both.
    if (rc == 0 && rename(SEED_TMP, SEED_FILE) != 0) rc = -1;
    if (rc != 0) (void)remove(SEED_TMP);
#endif
    wally_bzero(blob, sizeof blob);
    if (rc != 0)
        return -1;

    // storage_read_keep prefers the sealed copy, so this reads what was just
    // written, tag and all. Only a clean readback may retire the plaintext.
    char verify[WSEED_MAX_MNEMONIC];
    rc = storage_read_keep(verify, sizeof verify) == KEEP_OK &&
         strcmp(verify, words) == 0 ? 0 : -1;
    wally_bzero(verify, sizeof verify);
    if (rc != 0)
        return -1;

#ifdef ESP_PLATFORM
    if (nvs_open("kiss", NVS_READWRITE, &h) != ESP_OK)
        return -1;
    esp_err_t er = nvs_erase_key(h, "words");
    rc = (er == ESP_OK || er == ESP_ERR_NVS_NOT_FOUND) &&
         nvs_commit(h) == ESP_OK ? 0 : -1;
    nvs_close(h);
#endif
    return rc;
}

// ---- storage mode (see kiss_seed.h) ----
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

// ---- entropy quality note (see kiss_seed.h) ----
// One flag for the DEVICE, not per wallet, and that is the whole reason it is
// safe to store in the clear. The dice made one master seed; every passphrase
// wallet descends from it, so this says nothing about how many wallets exist
// and cannot become the oracle kiss_usage's fingerprint keys were.
//
// Written on every path that creates a seed, including the clean ones, so a
// flagged attempt that was abandoned and redone honestly does not leave its
// verdict behind. A whole partition erase (wipe, replace, amnesic) takes it,
// which is correct: the verdict belongs to the seed that is gone.
void kiss_seed_set_entropy_note(int v)
{
    if (v < 0 || v > 255) return;
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READWRITE, &h) != ESP_OK)
        return;
    if (nvs_set_u8(h, "entq", (uint8_t)v) == ESP_OK)
        nvs_commit(h);
    nvs_close(h);
#else
    FILE *f = fopen(ENTQ_FILE, "w");
    if (!f) return;
    fprintf(f, "%d", v);
    fclose(f);
#endif
}

int kiss_seed_entropy_note(void)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READONLY, &h) != ESP_OK)
        return 0;
    uint8_t v = 0;
    int rc = nvs_get_u8(h, "entq", &v) == ESP_OK ? (int)v : 0;
    nvs_close(h);
    return rc;
#else
    FILE *f = fopen(ENTQ_FILE, "r");
    if (!f) return 0;
    int v = 0;
    if (fscanf(f, "%d", &v) != 1) v = 0;
    fclose(f);
    return v;
#endif
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

// Now load bearing beyond the storage note: it gates whether a wallet
// fingerprint may be written to NVS at all (kiss_usage.c, kiss_backup.c).
// Both answers therefore need host coverage — the beta lane where the record
// must NOT be written, and the encrypted lane where it is allowed and must
// still behave. Same seam convention as kiss_seed_test_fail_next above.
#ifndef ESP_PLATFORM
static int s_test_flash_enc;
void kiss_seed_test_set_flash_encrypted(int on) { s_test_flash_enc = on ? 1 : 0; }
#endif

int kiss_seed_flash_encrypted(void)
{
#ifdef ESP_PLATFORM
    return esp_efuse_is_flash_encryption_enabled() ? 1 : 0;
#else
    return s_test_flash_enc;
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
             kiss_seed_validate(out) == 0 ? WSEED_OK : WSEED_ERR_SD_CORRUPT;
    wally_bzero(blob, sizeof blob);
    wally_bzero(key, sizeof key);
    if (rc != WSEED_OK) wally_bzero(out, out_len);
    return rc;
}

static int storage_write_sd(const char *words, bool *sidecar_cleanup)
{
    if (!words || kiss_seed_validate(words) != 0) return WSEED_ERR_INVALID;
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

// Take the internal copy away once the card is authoritative.
//
// This erased "words" and nothing else for as long as "words" WAS the internal
// copy. The tag migration made "wblob" the representation storage_read_keep
// actually reads, and this was not moved with it: after a move to SD the device
// still held the whole sealed seed and the nkey that opens it, while the screen
// said the internal copy was gone. The card stopped being a second factor at
// that point -- a device seized without it opened the wallet on its own.
//
// nkey goes FIRST, and it is the part that does the work. nvs_erase_key is a
// logical delete (see the note above storage_erase), so the blob stays on its
// page until a compaction that may never come; killing the key first means a
// power cut anywhere after it leaves dead ciphertext rather than live. dkey is
// deliberately left alone -- it is the only thing that can read the card this
// wallet now lives on, which is also why a whole-partition erase is wrong here.
static int storage_drop_keep_copy(void)
{
#ifdef ESP_PLATFORM
    if (sd_seed_forget_flash_key() != 0)
        return WSEED_ERR_CLEANUP;
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READWRITE, &h) != ESP_OK)
        return WSEED_ERR_CLEANUP;
    int rc = WSEED_OK;
    // "words" is the pre-tag representation and may still be here on a device
    // that never opened its wallet after the migration; "wblob" is today's.
    // Absent is success for both: this is a promise about what is left, not
    // about what was found.
    static const char *const GONE[] = { "wblob", "words" };
    for (size_t i = 0; i < sizeof GONE / sizeof GONE[0]; i++) {
        esp_err_t er = nvs_erase_key(h, GONE[i]);
        if (er != ESP_OK && er != ESP_ERR_NVS_NOT_FOUND)
            rc = WSEED_ERR_CLEANUP;
    }
    if (rc == WSEED_OK && nvs_commit(h) != ESP_OK)
        rc = WSEED_ERR_CLEANUP;
    nvs_close(h);
    return rc;
#else
    // The host keeps the same two acts in the same order, so the tests walk
    // this path rather than a shortcut: the emulated flash key, then the
    // emulated blob. Deleting only the seed file is what let this ship.
    if (sd_seed_forget_flash_key() != 0)
        return WSEED_ERR_CLEANUP;
    if (seed_test_fail(WSEED_TEST_FAIL_SEED_REMOVE) ||
        (remove(SEED_FILE) != 0 && errno != ENOENT))
        return WSEED_ERR_CLEANUP;
    return WSEED_OK;
#endif
}

// Publish SD only after its sealed file has been written and verified. Mode
// first (its card is already verified), then the internal copy: NVS set/erase
// calls are individually durable and commit is not a transaction, so a cut
// between them leaves two copies, never zero.
static int storage_publish_sd(void)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READWRITE, &h) != ESP_OK)
        return WSEED_ERR_SD_IO;
    int rc = nvs_set_u8(h, "smode", WSEED_MODE_SD) == ESP_OK &&
             nvs_commit(h) == ESP_OK ? WSEED_OK : WSEED_ERR_SD_IO;
    nvs_close(h);
    if (rc == WSEED_OK)
        rc = storage_drop_keep_copy();
#else
    // Host state uses files rather than NVS. Publish the verified SD mode
    // first; a power cut before dropping the emulated copy leaves two copies,
    // never zero, and SD remains the authoritative one.
    int rc = storage_mode_write(WSEED_MODE_SD) == 0
           ? WSEED_OK : WSEED_ERR_SD_IO;
    if (rc == WSEED_OK)
        rc = storage_drop_keep_copy();
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
// is never written anywhere, and kiss_seed_forget() (called from
// kiss_session_close) is what ends the session.
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
int kiss_seed_mode(void)
{
    return s_pending_mode >= 0 ? s_pending_mode : storage_mode_read();
}

void kiss_seed_stage_mode(int mode)
{
    s_pending_mode = mode == WSEED_MODE_AMNESIC ? WSEED_MODE_AMNESIC
                   : mode == WSEED_MODE_SD      ? WSEED_MODE_SD
                                                : WSEED_MODE_KEEP;
}

int kiss_seed_set_mode(int mode)
{
    if (mode != WSEED_MODE_KEEP && mode != WSEED_MODE_AMNESIC &&
        mode != WSEED_MODE_SD)
        return WSEED_ERR_INVALID;
    if (mode == WSEED_MODE_SD)
        return WSEED_ERR_NO_SEED;        // use stage+commit or move_to: never label
                                         // an absent/unverified card as the wallet
    if (mode == WSEED_MODE_KEEP && storage_mode_read() == WSEED_MODE_SD)
        return kiss_seed_move_to(WSEED_MODE_KEEP);
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
            kiss_usage_wipe();
            kiss_payee_wipe();
            kiss_backup_forget();    // amnesic keeps no metadata either
        }
    }
    return rc;
}

int kiss_seed_stage(const char *mnemonic)
{
    if (kiss_seed_validate(mnemonic) != 0)
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

int kiss_seed_commit(void)
{
    // Legacy AMNESIC load code stages+commits; the load is already the active
    // RAM wallet and committing it is intentionally a no-op.
    if (!s_has_pending)
        return storage_mode_read() == WSEED_MODE_AMNESIC && s_has_active_ram
             ? WSEED_OK : WSEED_ERR_NO_SEED;
    int mode = kiss_seed_mode();
    int prior_mode = storage_mode_read();
    if (prior_mode == WSEED_MODE_INVALID)
        return WSEED_ERR_VERIFY;
    if (mode == WSEED_MODE_AMNESIC) {
        if (storage_erase(WSEED_MODE_AMNESIC) != 0)
            return WSEED_ERR_SD_IO;
        int key_cleanup = sd_seed_forget_device_key();
        int card_cleanup = storage_delete_sd();
        kiss_usage_forget_session();   // replacement is a different wallet
        kiss_payee_forget_session();
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
                kiss_usage_persist_session();
                kiss_payee_persist_session();
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
            kiss_usage_persist_session();
            kiss_payee_persist_session();
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
        // A prior seed that fails its tag is still residue on the page, so it
        // still has to be scrubbed. Only an empty slot means nothing to erase.
        had_prior_words = storage_read_keep(prev, sizeof prev) != KEEP_ABSENT;
        wally_bzero(prev, sizeof prev);
    }
    int rc = storage_write_keep(s_pending);
    if (rc != WSEED_OK && rc != WSEED_ERR_CLEANUP) {
        // VERIFY on a REPLACEMENT is the same shape as a failed scrub and was
        // missed. storage_write_keep sets "wblob" and commits BEFORE it reads
        // back, so by the time the readback fails the previous wallet's blob
        // has already been overwritten -- the old words are unreadable and the
        // new ones exist only in staging. Answering that by discarding is a
        // total loss, on exactly the shape RECOVER was added for.
        //
        // Only VERIFY, and only with a prior wallet. An earlier failure never
        // reached the write, so the old wallet is intact and staging is safe to
        // drop; and with no prior wallet nothing was destroyed.
        if (rc == WSEED_ERR_VERIFY && had_prior_words)
            return WSEED_ERR_RECOVER;
        return rc;   // still staged, but the caller decides: the setup login
                     // discards it rather than hold an unsaved mnemonic in RAM
    }
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
        // here would send fp_tap_cb into kiss_seed_discard() on a wallet that
        // is already durable.
        // -1 from the scrub is its one narrow meaning: the partition WAS erased
        // and the mnemonic could not be put back. Reported as SD_IO, which the
        // setup login answers by discarding the staging -- and at that instant
        // the staged copy is the only one left anywhere. The distinct code is
        // what lets the caller keep it.
        if (storage_scrub_keep(s_pending) != 0)
            return WSEED_ERR_RECOVER;
        // On device the erase above was the whole NVS partition, so it already
        // took the receive-index history and the paper-check marks with it. Say
        // it explicitly so the host build ends up in the SAME state: otherwise
        // the simulator would show a replacement wallet wearing the replaced
        // one's green "paper checked", which is the exact lie this row exists
        // to avoid. A replacement is a different wallet, as the AMNESIC branch
        // above already says.
        kiss_usage_wipe();
        kiss_payee_wipe();
        kiss_backup_forget();
        // The unlock stroke went with it too, and for the same reason: "greal"
        // is deliberately outside KEEP_KEYS so the partition erase takes it.
        // The host build keeps it in a static, so without this line the
        // simulator shows a brand new wallet still opening on the replaced
        // wallet's decoy gesture -- and the DURESS row would name it.
        kiss_duress_forget();
    }
    if (prior_mode == WSEED_MODE_SD) {
        int card_cleanup = storage_delete_sd();
        int key_cleanup = sd_seed_forget_device_key();
        if (card_cleanup != WSEED_OK || key_cleanup != 0)
            result = WSEED_ERR_CLEANUP;
    }
    clear_staged();
    if (prior_mode == WSEED_MODE_AMNESIC) {
        kiss_usage_persist_session();
        kiss_payee_persist_session();
        clear_active_ram();
    }
    return result;
}

// Backing out of setup, at any step, for any reason. Nothing was written yet,
// so this only has to drop what is held in RAM: the words AND the storage-mode
// answer, which reverts kiss_seed_mode() to whatever flash still says.
void kiss_seed_discard(void)
{
    clear_staged();
}

// ---- API ----
int kiss_seed_exists(void)
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
    // Same reasoning as the SD case above: a seed whose tag fails is a wallet
    // this device cannot currently open, not an empty device. Answering 0 here
    // would drop the owner into first-time setup on top of it.
    char tmp[WSEED_MAX_MNEMONIC];
    int rc = storage_read_keep(tmp, sizeof tmp) != KEEP_ABSENT ? 1 : 0;
    wally_bzero(tmp, sizeof tmp);
    return rc;
}

int kiss_seed_validate(const char *mnemonic)
{
    if (!mnemonic || !mnemonic[0])
        return -1;
    return bip39_mnemonic_validate(NULL, mnemonic) == WALLY_OK ? 0 : -1;
}

int kiss_seed_store(const char *mnemonic)
{
    if (kiss_seed_validate(mnemonic) != 0)
        return -1;
    return storage_write_keep(mnemonic);
}

int kiss_seed_load(char *out, size_t out_len)
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
    int rc = storage_read_keep(out, out_len);
    if (rc == KEEP_BAD)
        return WSEED_ERR_SD_CORRUPT;    // altered or half written, not missing
    if (rc != KEEP_OK)
        return WSEED_ERR_NO_SEED;
    if (s_keep_legacy)
        (void)storage_migrate_keep(out);
    return WSEED_OK;
}

int kiss_seed_wipe(void)
{
    clear_staged();
    clear_active_ram();
    kiss_usage_wipe();
    kiss_payee_wipe();
    kiss_backup_forget();          // the paper check was about THAT wallet

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

void kiss_seed_forget(void)
{
    // During Replace Wallet, closing a failed candidate session must not erase
    // the pre-existing AMNESIC wallet. The candidate is discarded separately.
    if (storage_mode_read() == WSEED_MODE_AMNESIC && !s_has_pending)
        clear_active_ram();
    kiss_usage_forget_session();
    kiss_payee_forget_session();
}

int kiss_seed_move_to(int mode)
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
    int rc = kiss_seed_load(words, sizeof words);
    if (rc != WSEED_OK) {
        wally_bzero(words, sizeof words);
        return rc;
    }
    if (kiss_seed_validate(words) != 0) {
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
                storage_read_keep(verify, sizeof verify) != KEEP_OK ||
                strcmp(verify, words) != 0)
                goto out;
            wally_bzero(verify, sizeof verify);
        }
        int committed_with_cleanup = rc == WSEED_ERR_CLEANUP;
        rc = storage_read_keep(verify, sizeof verify) == KEEP_OK &&
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
            kiss_usage_persist_session();
            kiss_payee_persist_session();
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
                kiss_usage_persist_session();
                kiss_payee_persist_session();
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
            kiss_usage_persist_session();
            kiss_payee_persist_session();
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

int kiss_seed_from_entropy(const uint8_t *entropy, size_t len,
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

// ---- QR seed import (see kiss_seed.h) ----
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
        if (kiss_seed_word(idx, &word) != 0 || !word)   // 2048+ lands here
            return -1;
        int need = snprintf(out + o, out_len - o, "%s%s", w ? " " : "", word);
        if (need < 0 || (size_t)need >= out_len - o)
            return -1;
        o += (size_t)need;
    }
    return 0;
}

int kiss_seed_from_qr(const char *data, size_t len, char *out, size_t out_len)
{
    if (out && out_len) out[0] = 0;      // never leave a stale value behind
    if (!data || !out || out_len < 2 || len == 0)
        return -1;

    // Numeric SeedQR first: it is the only all-ASCII-digit form, and its
    // lengths (48/96) cannot be mistaken for a CompactSeedQR (16/32).
    if ((len == 48 || len == 96) && all_digits(data, len)) {
        if (from_numeric_seedqr(data, len, out, out_len) != 0)
            goto fail;
        if (kiss_seed_validate(out) != 0)
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
        if (kiss_seed_from_entropy(e, len, out, out_len) != 0)
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
        if (kiss_seed_validate(out) == 0)
            return 0;
    }
fail:
    wally_bzero(out, out_len);
    return -1;
}

// ---- wordlist access ----
int kiss_seed_word(int index, const char **out)
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

// ---- backup verification: word-by-word compare (see kiss_seed.h) ----
static const char *skip_spaces(const char *p) { while (*p == ' ') p++; return p; }

int kiss_seed_diff_word(const char *typed, const char *stored)
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

int kiss_seed_suggest(const char *prefix, const char *out[], int n)
{
    // matches get their own stable rows — kiss_seed_word's ring would be
    // overwritten while we keep scanning past a match
    static char sug[8][12];
    if (!prefix || !out || n <= 0)
        return 0;
    if (n > 8) n = 8;
    size_t plen = strlen(prefix);
    int found = 0;
    for (int i = 0; i < BIP39_WORDLIST_LEN && found < n; i++) {
        const char *w = NULL;
        if (kiss_seed_word(i, &w) != 0)
            break;
        if (strncmp(w, prefix, plen) == 0) {
            snprintf(sug[found], sizeof sug[found], "%s", w);
            out[found] = sug[found];
            found++;
        }
    }
    return found;
}
