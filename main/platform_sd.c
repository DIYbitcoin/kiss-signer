// SD card platform seam — see platform_sd.h. PSBTs use the simple operations;
// the sealed seed uses atomic replace/delete so a pulled card cannot truncate
// the only durable destination during a storage-mode migration.
#include "platform_sd.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef ESP_PLATFORM

#define SD_BASE "/tmp/simsd"
static int s_test_present = 1;
static unsigned s_test_fail;

void platform_sd_test_set_present(int present) { s_test_present = present != 0; }
void platform_sd_test_fail_next(unsigned flags) { s_test_fail = flags; }

static int test_fail(unsigned flag)
{
    if (!(s_test_fail & flag)) return 0;
    s_test_fail &= ~flag;
    return 1;
}

int platform_sd_mount(void)
{
    if (!s_test_present) return -2;
    if (mkdir(SD_BASE, 0777) != 0 && errno != EEXIST) return -1;
    struct stat st;
    return stat(SD_BASE, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
}
void platform_sd_unmount(void) {}
int platform_sd_probe(void) { return platform_sd_mount() == 0 ? 1 : 0; }

#else

#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#define SD_BASE "/sdcard"

static sdmmc_card_t *s_card;
static sd_pwr_ctrl_handle_t s_pwr;   // LDO stays claimed across mounts

int platform_sd_mount(void)
{
    if (s_card) {
        if (sdmmc_get_status(s_card) == ESP_OK)
            return 0;
        // A card can be pulled between the home-screen probe and a secret
        // operation. Drop that stale VFS/card object here too, so RETRY after
        // reinsertion performs a real mount instead of trusting s_card.
        esp_vfs_fat_sdcard_unmount(SD_BASE, s_card);
        s_card = NULL;
    }
    if (!s_pwr) {
        sd_pwr_ctrl_ldo_config_t lc = { .ldo_chan_id = 4 };   // card VDD on this board
        if (sd_pwr_ctrl_new_on_chip_ldo(&lc, &s_pwr) != ESP_OK)
            return -1;
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = s_pwr;
    const sdmmc_slot_config_t slot = {   // slot 0 pins come from IO MUX
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 4,
        .flags = 0,
    };
    const esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    if (esp_vfs_fat_sdmmc_mount(SD_BASE, &host, &slot, &mc, &s_card) != ESP_OK) {
        s_card = NULL;
        return -2;
    }
    return 0;
}

void platform_sd_unmount(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(SD_BASE, s_card);
        s_card = NULL;
    }
}

int platform_sd_probe(void)
{
    // mount() validates an existing mount and remounts after a pull/reinsert.
    return platform_sd_mount() == 0 ? 1 : 0;
}

#endif

static int name_ok(const char *name)
{
    if (!name || !name[0] || strlen(name) >= SD_NAME_LEN) return 0;
    if (name[0] == '.' || strstr(name, "..")) return 0;
    return strchr(name, '/') == NULL && strchr(name, '\\') == NULL;
}

static int name_is_signed(const char *name)
{
    size_t n = strlen(name);
    return n >= 12 && strcasecmp(name + n - 12, "-signed.psbt") == 0;
}

static int name_cmp(const void *a, const void *b)
{
    const char *na = a, *nb = b;
    int sa = name_is_signed(na), sb = name_is_signed(nb);
    if (sa != sb) return sa - sb;       // work still to sign first; signed files below
    return strcasecmp(na, nb);          // stable, obvious A-Z order within each group
}

// Does `cand` name the "-signed.psbt" sibling of `src`? Compared against the
// source's own length so "payment-011.psbt" is not claimed by
// "payment-01-signed.psbt", and case insensitively because FAT preserves case
// but does not respect it.
static int is_sibling_of(const char *signed_name, size_t base, const char *src)
{
    return strlen(src) == base + 5 &&
           strncasecmp(src, signed_name, base) == 0 &&
           strcasecmp(src + base, ".psbt") == 0;
}

// One pass over the directory. See platform_sd.h for why it must be the
// directory and not the caller's array.
static int signed_pass(char names[][SD_NAME_LEN], uint8_t *mark, int n,
                       char *victim, size_t victim_len)
{
    DIR *d = opendir(SD_BASE);
    if (!d)
        return -1;
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        if (nm[0] == '.' || strlen(nm) >= SD_NAME_LEN || !name_is_signed(nm))
            continue;
        size_t base = strlen(nm) - 12;      // strlen("-signed.psbt")
        found++;
        if (victim) {                       // caller wants one to delete
            snprintf(victim, victim_len, "%s", nm);
            break;                          // close the dir BEFORE unlinking
        }
        if (!mark || base == 0)             // a bare "-signed.psbt" owns nothing
            continue;
        for (int i = 0; i < n; i++)
            if (is_sibling_of(nm, base, names[i]))
                mark[i] = 1;
    }
    closedir(d);
    return found;
}

int platform_sd_list_signed(char names[][SD_NAME_LEN], int max, int *total)
{
    DIR *d = opendir(SD_BASE);
    if (!d)
        return -1;
    int n = 0, all = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        if (nm[0] == '.' || strlen(nm) >= SD_NAME_LEN || !name_is_signed(nm))
            continue;
        all++;
        int at = 0;
        while (at < n && strcasecmp(names[at], nm) <= 0)
            at++;
        if (at >= max)
            continue;
        for (int i = (n < max ? n : max - 1); i > at; i--)
            memcpy(names[i], names[i - 1], SD_NAME_LEN);
        snprintf(names[at], SD_NAME_LEN, "%s", nm);
        if (n < max)
            n++;
    }
    closedir(d);
    if (total)
        *total = all;
    return n;
}

int platform_sd_signed_scan(char names[][SD_NAME_LEN], uint8_t *mark,
                            int n, int del)
{
    if (mark)
        memset(mark, 0, (size_t)(n > 0 ? n : 0));
    if (!del)
        return signed_pass(names, mark, n, NULL, 0);

    // Deleting: take ONE victim per pass and close the directory before
    // unlinking it. FATFS makes no promise about f_readdir after f_unlink in the
    // same directory, and a delete-in-place loop can silently skip files --
    // which would leave the REMOVE pill up after a hold that looked like it
    // worked. Cards hold tens of files and this runs once per hold, so the
    // extra opendir per file costs nothing anyone can feel.
    int removed = 0;
    for (;;) {
        char victim[SD_NAME_LEN];
        victim[0] = 0;
        int rc = signed_pass(NULL, NULL, 0, victim, sizeof victim);
        if (rc < 0)
            return removed ? removed : -1;
        if (!victim[0])
            return removed;                 // nothing left to take
        if (platform_sd_delete(victim) != 0)
            return removed;                 // stop on the first refusal; the
        removed++;                          // pill stays up with what survived
    }
}

int platform_sd_list_psbt(char names[][SD_NAME_LEN], int max, int *total)
{
    DIR *d = opendir(SD_BASE);
    if (!d)
        return -1;
    int n = 0, all = 0;
    struct dirent *e;
    // Read the WHOLE directory and keep the `max` names that sort first. The
    // old loop stopped reading at max, which capped the list in FAT directory
    // order -- effectively creation order -- so which files a full card showed
    // had nothing to do with the order on screen, and the files that vanished
    // gave no sign they existed. On a real device test that read as "the card
    // has two files" when it had nine. Insertion keeps memory bounded at the
    // caller's array, and max is small, so N*max is nothing.
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        size_t l = strlen(nm);
        if (nm[0] == '.')                     // macOS AppleDouble junk on real cards
            continue;
        if (l < 6 || l >= SD_NAME_LEN || strcasecmp(nm + l - 5, ".psbt") != 0)
            continue;
        all++;
        int at = 0;
        while (at < n && name_cmp(names[at], nm) <= 0)
            at++;
        if (at >= max)                        // sorts past the window: not kept,
            continue;                         // but still counted in `all`
        for (int i = (n < max ? n : max - 1); i > at; i--)
            memcpy(names[i], names[i - 1], SD_NAME_LEN);
        snprintf(names[at], SD_NAME_LEN, "%s", nm);
        if (n < max)
            n++;
    }
    closedir(d);
    if (total)
        *total = all;
    return n;
}

static void full_path(char *dst, size_t dstsz, const char *name)
{
    snprintf(dst, dstsz, "%s/%s", SD_BASE, name);
}

static void side_path(char *dst, size_t dstsz, const char *name, const char *suffix)
{
    snprintf(dst, dstsz, "%s/%s%s", SD_BASE, name, suffix);
}

// Finish or roll back an interrupted atomic replace. A target that exists is
// authoritative. If it does not, .tmp can only coexist with .bak after the
// fully-written temp was verified and the old target was moved aside, so the
// temp is the committed candidate. A lone .bak is the previous good file.
static int recover_atomic(const char *name)
{
    char target[SD_NAME_LEN + 24], tmp[SD_NAME_LEN + 24], bak[SD_NAME_LEN + 24];
    full_path(target, sizeof target, name);
    side_path(tmp, sizeof tmp, name, ".tmp");
    side_path(bak, sizeof bak, name, ".bak");
    if (access(target, F_OK) == 0) {
        int rc = 0;
        if (remove(tmp) != 0 && errno != ENOENT) rc = PLATFORM_SD_ATOMIC_CLEANUP;
        if (remove(bak) != 0 && errno != ENOENT) rc = PLATFORM_SD_ATOMIC_CLEANUP;
        return rc;
    }
    int have_tmp = access(tmp, F_OK) == 0;
    int have_bak = access(bak, F_OK) == 0;
    if (have_tmp && have_bak) {
        if (rename(tmp, target) != 0) return -2;
        return remove(bak) == 0 || errno == ENOENT
             ? 0 : PLATFORM_SD_ATOMIC_CLEANUP;
    }
    if (have_bak) return rename(bak, target) == 0 ? 0 : -2;
    // A lone temp may be a power-cut partial first write; never promote it.
    if (have_tmp && remove(tmp) != 0 && errno != ENOENT)
        return PLATFORM_SD_ATOMIC_CLEANUP;
    return 0;
}

int platform_sd_read(const char *name, uint8_t *buf, size_t max, size_t *len)
{
    if (!name_ok(name) || !buf || !len || max == 0) return -3;
#ifndef ESP_PLATFORM
    if (test_fail(PLATFORM_SD_TEST_FAIL_READ)) return -3;
#endif
    (void)recover_atomic(name);
    char p[SD_NAME_LEN + 16];
    full_path(p, sizeof p, name);
    FILE *f = fopen(p, "rb");
    if (!f)
        return -1;
    *len = fread(buf, 1, max, f);
    int io = ferror(f);
    int full = !feof(f);                      // file bigger than our buffer = reject
    if (fclose(f) != 0) io = 1;
    return io ? -3 : (*len == 0 || full) ? -2 : 0;
}

int platform_sd_write(const char *name, const uint8_t *buf, size_t len)
{
    if (!name_ok(name) || (!buf && len)) return -3;
    char p[SD_NAME_LEN + 16];
    full_path(p, sizeof p, name);
    FILE *f = fopen(p, "wb");
    if (!f)
        return -1;
    size_t wr = fwrite(buf, 1, len, f);
    int rc = (fclose(f) == 0 && wr == len) ? 0 : -2;
    return rc;
}

static int file_matches(const char *path, const uint8_t *buf, size_t len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t chunk[128];
    size_t off = 0;
    int ok = 1;
    while (off < len) {
        size_t want = len - off < sizeof chunk ? len - off : sizeof chunk;
        size_t n = fread(chunk, 1, want, f);
        if (n != want || memcmp(chunk, buf + off, want) != 0) {
            ok = 0;
            break;
        }
        off += want;
    }
    if (ok && fgetc(f) != EOF) ok = 0;
    if (ferror(f) || fclose(f) != 0) ok = 0;
    memset(chunk, 0, sizeof chunk);
    return ok;
}

int platform_sd_write_atomic(const char *name, const uint8_t *buf, size_t len)
{
    if (!name_ok(name) || !buf || len == 0) return -3;
#ifndef ESP_PLATFORM
    if (test_fail(PLATFORM_SD_TEST_FAIL_WRITE)) return -2;
#endif
    if (recover_atomic(name) < 0) return -2;

    char target[SD_NAME_LEN + 24], tmp[SD_NAME_LEN + 24], bak[SD_NAME_LEN + 24];
    full_path(target, sizeof target, name);
    side_path(tmp, sizeof tmp, name, ".tmp");
    side_path(bak, sizeof bak, name, ".bak");
    (void)remove(tmp);

    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    size_t wr = fwrite(buf, 1, len, f);
    int rc = wr == len && fflush(f) == 0 ? 0 : -2;
    if (rc == 0 && fsync(fileno(f)) != 0) rc = -2;
    if (fclose(f) != 0) rc = -2;
    if (rc != 0 || !file_matches(tmp, buf, len)) {
        (void)remove(tmp);
        return -2;
    }

#ifndef ESP_PLATFORM
    if (test_fail(PLATFORM_SD_TEST_FAIL_RENAME)) {
        (void)remove(tmp);
        return -2;
    }
#endif
    (void)remove(bak);
    int had_target = access(target, F_OK) == 0;
    if (had_target && rename(target, bak) != 0) {
        (void)remove(tmp);
        return -2;
    }
    if (rename(tmp, target) != 0) {
        if (had_target) (void)rename(bak, target);
        (void)remove(tmp);
        return -2;
    }
    if (had_target) {
#ifndef ESP_PLATFORM
        if (test_fail(PLATFORM_SD_TEST_FAIL_BAK_DELETE))
            return PLATFORM_SD_ATOMIC_CLEANUP;
#endif
        if (remove(bak) != 0 && errno != ENOENT)
            return PLATFORM_SD_ATOMIC_CLEANUP;
    }
    return 0;
}

int platform_sd_delete(const char *name)
{
    if (!name_ok(name)) return -3;
#ifndef ESP_PLATFORM
    if (test_fail(PLATFORM_SD_TEST_FAIL_DELETE)) return -2;
#endif
    char target[SD_NAME_LEN + 24], tmp[SD_NAME_LEN + 24], bak[SD_NAME_LEN + 24];
    full_path(target, sizeof target, name);
    side_path(tmp, sizeof tmp, name, ".tmp");
    side_path(bak, sizeof bak, name, ".bak");
    int rc = 0;
    if (remove(target) != 0 && errno != ENOENT) rc = -2;
    if (remove(tmp) != 0 && errno != ENOENT) rc = -2;
    if (remove(bak) != 0 && errno != ENOENT) rc = -2;
    return rc;
}
