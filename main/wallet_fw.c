// See wallet_fw.h. Two gates: the signature esp_ota checks before the slot is
// made bootable, and the boot the new firmware has to survive before the old
// one is released.
#include "wallet_fw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// The sim links this without the IDF build's compile definitions.
#ifndef KISS_VERSION_STR
#define KISS_VERSION_STR "0.0.0-sim"
#endif

#ifdef ESP_PLATFORM
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#endif

// The app descriptor sits after the image header (24) and the first segment
// header (8). Its fields are read by offset rather than by including
// esp_app_desc.h, because this has to parse an image the sim can hand it on a
// host where that header does not exist -- and because the thing being read is
// a file off a card, not a struct this build produced.
#define DESC_OFF        32
#define DESC_MAGIC      0xABCD5432u
#define DESC_VER_OFF    (DESC_OFF + 16)
#define DESC_PROJ_OFF   (DESC_OFF + 48)
#define DESC_FIELD_LEN  32

#define FW_CHUNK 4096

// ---- version comparison ----------------------------------------------------

// One numeric field, stopping at the first non digit. Returns the value and
// leaves *s on the terminator.
static unsigned num_field(const char **s)
{
    unsigned v = 0;
    while (**s >= '0' && **s <= '9') {
        // Saturate rather than wrap. A hostile or corrupt version string of
        // forty nines must not roll over into a small number and read as older.
        if (v < 100000u) v = v * 10u + (unsigned)(**s - '0');
        (*s)++;
    }
    return v;
}

// Compare the prerelease tails ("beta7" against "beta10", "" against "beta1").
// Semver's rule, which is the one that matters here: a release with NO tail is
// newer than the same numbers with one, because 0.1.0 ships after 0.1.0-beta7.
static int tail_cmp(const char *a, const char *b)
{
    int ea = (*a == 0), eb = (*b == 0);
    if (ea || eb)
        return ea && eb ? 0 : (ea ? 1 : -1);

    // Alphabetic run first, so "beta" and "rc" order by name.
    const char *pa = a, *pb = b;
    while (*pa && !(*pa >= '0' && *pa <= '9')) pa++;
    while (*pb && !(*pb >= '0' && *pb <= '9')) pb++;
    size_t la = (size_t)(pa - a), lb = (size_t)(pb - b);
    size_t l = la < lb ? la : lb;
    int c = strncasecmp(a, b, l);
    if (c) return c < 0 ? -1 : 1;
    if (la != lb) return la < lb ? -1 : 1;

    // Then the number, NUMERICALLY. Strcmp would put beta10 before beta7 and
    // offer a downgrade as an upgrade, which is the whole reason this function
    // is separate and tested.
    unsigned na = num_field(&pa), nb = num_field(&pb);
    if (na != nb) return na < nb ? -1 : 1;
    return 0;
}

int wallet_fw_version_cmp(const char *a, const char *b)
{
    if (!a) a = "";
    if (!b) b = "";
    for (int i = 0; i < 3; i++) {
        unsigned va = num_field(&a), vb = num_field(&b);
        if (va != vb) return va < vb ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    // Whatever is left is the prerelease tail. Skip the separator so "-beta7"
    // and "beta7" compare the same; a build metadata "+..." is not ordered by
    // semver, so it is simply not read.
    while (*a == '-' || *a == '.') a++;
    while (*b == '-' || *b == '.') b++;
    return tail_cmp(a, b);
}

// ---- image descriptor ------------------------------------------------------

static void copy_field(char *dst, size_t dn, const uint8_t *src)
{
    size_t n = 0;
    while (n < DESC_FIELD_LEN && n + 1 < dn && src[n])
        { dst[n] = (char)src[n]; n++; }
    dst[n] = 0;
}

int wallet_fw_desc_parse(const uint8_t *hdr, size_t len,
                         char *ver, size_t vn, char *proj, size_t pn)
{
    if (ver && vn) ver[0] = 0;
    if (proj && pn) proj[0] = 0;
    if (!hdr || len < WFW_DESC_MIN)
        return WFW_ERR_UNREADABLE;
    // Little endian read by byte: the file came off a card, so it is bytes, and
    // casting it to a uint32_t* would be an unaligned load on the one target
    // that traps them.
    uint32_t magic = (uint32_t)hdr[DESC_OFF] | ((uint32_t)hdr[DESC_OFF + 1] << 8) |
                     ((uint32_t)hdr[DESC_OFF + 2] << 16) | ((uint32_t)hdr[DESC_OFF + 3] << 24);
    if (magic != DESC_MAGIC)
        return WFW_ERR_UNREADABLE;
    if (ver && vn) copy_field(ver, vn, hdr + DESC_VER_OFF);
    if (proj && pn) copy_field(proj, pn, hdr + DESC_PROJ_OFF);
    // A descriptor with the right magic and an empty version is still not
    // something to compare against, so it is refused here rather than treated
    // as version "" and silently ordered below everything.
    if (ver && vn && !ver[0])
        return WFW_ERR_UNREADABLE;
    return 0;
}

// ---- running firmware ------------------------------------------------------

const char *wallet_fw_running_version(void)
{
#ifdef ESP_PLATFORM
    const esp_app_desc_t *d = esp_app_get_description();
    return d && d->version[0] ? d->version : KISS_VERSION_STR;
#else
    return KISS_VERSION_STR;
#endif
}

#ifndef ESP_PLATFORM
// Sim seams. See wallet_fw.h. They default to the honest desktop answer, so a
// test that forgets to set them gets "cannot be checked" rather than a pretend
// install that proves nothing.
static int s_test_avail = WFW_ERR_UNSIGNED;
static int s_test_install = WFW_ERR_UNSIGNED;
static int s_test_steps;
void wallet_fw_test_set_available(int rc) { s_test_avail = rc; }
void wallet_fw_test_set_install(int rc, int steps)
{
    s_test_install = rc;
    s_test_steps = steps;
}
#endif

int wallet_fw_available(void)
{
    // The public key an update is checked against lives in the running app's
    // own signature block, so a build that was never signed has nothing to
    // check against. Refusing here is the difference between "this build cannot
    // update" and "this build installs anything it is handed".
#if defined(ESP_PLATFORM) && defined(CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT)
    return WFW_OK;
#elif defined(ESP_PLATFORM) && defined(CONFIG_SECURE_BOOT)
    return WFW_OK;
#else
    return s_test_avail;
#endif
}

static size_t slot_size(void)
{
#ifdef ESP_PLATFORM
    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    return p ? p->size : 0;
#else
    return 0x7F0000;      // partitions.csv, so the sim can exercise TOO_BIG
#endif
}

// ---- scan ------------------------------------------------------------------

int wallet_fw_scan(wfw_image_t *out)
{
    if (!out) return WFW_ERR_UNREADABLE;
    memset(out, 0, sizeof *out);
    out->slot = slot_size();

    if (platform_sd_probe() != 1)
        return out->status = WFW_ERR_NO_CARD;

    char names[8][SD_NAME_LEN];
    int n = platform_sd_list_firmware(names, 8, NULL);
    if (n <= 0)
        return out->status = WFW_ERR_NO_FILE;

    // First readable app image in sort order wins. Reading the descriptor is
    // what picks it, not the name: a card holding a photo called firmware.bin
    // and the real image called z.bin has to land on the real image.
    for (int i = 0; i < n; i++) {
        size_t len = 0;
        platform_sd_file *f = platform_sd_open(names[i], &len);
        if (!f) continue;
        uint8_t hdr[WFW_DESC_MIN];
        size_t got = 0;
        int rc = platform_sd_read_chunk(f, hdr, sizeof hdr, &got);
        platform_sd_close(f);
        if (rc != 0 || got < sizeof hdr) continue;

        char ver[WFW_VER_LEN], proj[WFW_VER_LEN];
        if (wallet_fw_desc_parse(hdr, got, ver, sizeof ver, proj, sizeof proj) != 0)
            continue;

        snprintf(out->name, sizeof out->name, "%s", names[i]);
        snprintf(out->version, sizeof out->version, "%s", ver);
        snprintf(out->project, sizeof out->project, "%s", proj);
        out->size = len;
        out->cmp = wallet_fw_version_cmp(ver, wallet_fw_running_version());

        // Order matters. Too big is a fact about this device and outranks what
        // the version says; same and older are offers the screen presents
        // differently, not failures to parse.
        if (out->slot && len > out->slot) out->status = WFW_ERR_TOO_BIG;
        else if (out->cmp == 0)           out->status = WFW_ERR_SAME;
        else if (out->cmp < 0)            out->status = WFW_ERR_OLDER;
        else                              out->status = WFW_OK;
        return out->status;
    }

    // Files were there, none of them an app image.
    snprintf(out->name, sizeof out->name, "%s", names[0]);
    return out->status = WFW_ERR_UNREADABLE;
}

// ---- install ---------------------------------------------------------------

int wallet_fw_install(const wfw_image_t *img, wfw_progress_fn cb, void *ud)
{
    if (!img || !img->name[0]) return WFW_ERR_UNREADABLE;
    int avail = wallet_fw_available();
    if (avail != WFW_OK) return avail;

#ifndef ESP_PLATFORM
    // No flash to write and no key to check with, so the outcome is whatever
    // the test asked for. The progress ticks are real calls: the writing screen
    // redraws from them, and that redraw is what the screen walk measures.
    for (int i = 1; cb && i <= s_test_steps; i++)
        cb((int)((long)i * 100 / s_test_steps), ud);
    return s_test_install;
#else
    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    if (!dst) return WFW_ERR_WRITE;
    if (img->size == 0 || img->size > dst->size) return WFW_ERR_TOO_BIG;

    size_t len = 0;
    platform_sd_file *f = platform_sd_open(img->name, &len);
    if (!f) return WFW_ERR_CARD_GONE;
    // The card is re-read here rather than trusted from the scan: it may have
    // been swapped between the screen that offered the image and the hold that
    // accepted it, and the size the erase is sized against has to be the size
    // that is about to be written.
    if (len != img->size) { platform_sd_close(f); return WFW_ERR_CARD_GONE; }

    esp_ota_handle_t h = 0;
    if (esp_ota_begin(dst, len, &h) != ESP_OK) {
        platform_sd_close(f);
        return WFW_ERR_WRITE;
    }

    uint8_t *buf = malloc(FW_CHUNK);
    if (!buf) { esp_ota_abort(h); platform_sd_close(f); return WFW_ERR_WRITE; }

    size_t done = 0;
    int last_pct = -1, rc = WFW_OK;
    for (;;) {
        size_t got = 0;
        if (platform_sd_read_chunk(f, buf, FW_CHUNK, &got) != 0) {
            rc = WFW_ERR_CARD_GONE;
            break;
        }
        if (got == 0)
            break;
        if (esp_ota_write(h, buf, got) != ESP_OK) {
            rc = WFW_ERR_WRITE;
            break;
        }
        done += got;
        if (cb) {
            int pct = (int)((done * 100) / len);
            if (pct != last_pct) { last_pct = pct; cb(pct, ud); }
        }
    }
    free(buf);
    platform_sd_close(f);

    // A short read is not an error at the seam, so it is caught here: fewer
    // bytes than the file claimed means the card went away mid write, and a
    // truncated image must never reach esp_ota_end, which would judge it only
    // by its signature over whatever arrived.
    if (rc == WFW_OK && done != len)
        rc = WFW_ERR_CARD_GONE;

    if (rc != WFW_OK) {
        esp_ota_abort(h);
        return rc;
    }

    // The gate. esp_ota_end verifies the image against the key in the running
    // app's signature block; ESP_ERR_OTA_VALIDATE_FAILED is a real refusal, not
    // an IO problem, and the screen says so in those words.
    esp_err_t err = esp_ota_end(h);
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) return WFW_ERR_REJECTED;
    if (err != ESP_OK) return WFW_ERR_WRITE;

    if (esp_ota_set_boot_partition(dst) != ESP_OK) return WFW_ERR_WRITE;
    return WFW_OK;
#endif
}

// ---- rollback --------------------------------------------------------------

void wallet_fw_mark_valid(void)
{
#ifdef ESP_PLATFORM
    // Only a slot actually on trial gets marked. Calling this unconditionally
    // would be harmless today, but reading the state is what makes the intent
    // legible: this confirms an update, it is not a ritual every boot performs.
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY)
        esp_ota_mark_app_valid_cancel_rollback();
#endif
}
