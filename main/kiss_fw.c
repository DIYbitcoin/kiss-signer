// See kiss_fw.h. Two gates: the signature esp_ota checks before the slot is
// made bootable, and the boot the new firmware has to survive before the old
// one is released.
#include "kiss_fw.h"
#include "kiss_pqsig.h"

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
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_secure_boot.h"   // signature blocks of the RUNNING app
static const char *TAG = "kissfw";
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

int kiss_fw_version_cmp(const char *a, const char *b)
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

int kiss_fw_desc_parse(const uint8_t *hdr, size_t len,
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

const char *kiss_fw_running_version(void)
{
#ifdef ESP_PLATFORM
    const esp_app_desc_t *d = esp_app_get_description();
    return d && d->version[0] ? d->version : KISS_VERSION_STR;
#else
    return KISS_VERSION_STR;
#endif
}

#ifndef ESP_PLATFORM
// Sim seams. See kiss_fw.h. They default to the honest desktop answer, so a
// test that forgets to set them gets "cannot be checked" rather than a pretend
// install that proves nothing.
static int s_test_avail = WFW_ERR_UNSIGNED;
static int s_test_install = WFW_ERR_UNSIGNED;
static int s_test_steps;
void kiss_fw_test_set_available(int rc) { s_test_avail = rc; }
void kiss_fw_test_set_install(int rc, int steps)
{
    s_test_install = rc;
    s_test_steps = steps;
}
#endif

int kiss_fw_available(void)
{
    // The public key an update is checked against lives in the running app's
    // own signature block, so a build that was never signed has nothing to
    // check against. Refusing here is the difference between "this build cannot
    // update" and "this build installs anything it is handed".
#if defined(ESP_PLATFORM) && defined(CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT)
    // Without secure boot the trust anchor is not an eFuse: IDF reads the
    // public key digests out of the RUNNING app's own appended signature
    // block. So the Kconfig symbol only promises that verification will be
    // ATTEMPTED, and says nothing about whether it can succeed. An app that
    // was built with this on but never signed carries no key, and every
    // incoming image then fails with "no signatures found for the running
    // app" -- after the idle slot has been erased and 7 MB written, and the
    // screen blames the card and the signer's key rather than this board.
    //
    // Nothing unsafe installs either way; esp_ota_set_boot_partition is never
    // reached. The damage is that the device is permanently unable to update
    // and says so in the wrong words, and on the flash encryption release
    // recipe there is no serial reflash left to recover with.
    //
    // The header promises this function answers at runtime. Ask at runtime.
    esp_image_sig_public_key_digests_t digests = {0};
    if (esp_secure_boot_get_signature_blocks_for_running_app(true, &digests)
            != ESP_OK || digests.num_digests == 0)
        return WFW_ERR_UNSIGNED;
    // And the second key, by the same argument. A build with no post quantum
    // public key compiled in, or one whose SHA accelerator fails its own
    // selftest, cannot judge the trailer -- so it says the image cannot be
    // checked instead of writing 4 MB and then refusing every image it is ever
    // handed. kiss_pqsig_available() answers both questions.
    if (!kiss_pqsig_available()) return WFW_ERR_UNSIGNED;
    return WFW_OK;
#elif defined(ESP_PLATFORM) && defined(CONFIG_SECURE_BOOT)
    // Anchored in eFuse, not in the app's own block, so the running app's
    // block is the wrong thing to test here. The post quantum half is anchored
    // in this image either way.
    if (!kiss_pqsig_available()) return WFW_ERR_UNSIGNED;
    return WFW_OK;
#elif defined(ESP_PLATFORM)
    // A device build with neither signing option configured, which is the
    // shipped release lane today. s_test_avail is a HOST seam and does not
    // exist here, so this used to be a compile error the desktop gates could
    // never see: every one of them builds the #else.
    //
    // WFW_ERR_UNSIGNED is also the honest answer, and the one SIGNING.md
    // already promises: a build with no key of its own cannot judge an image,
    // so it says "cannot be checked" rather than installing what it is handed.
    return WFW_ERR_UNSIGNED;
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

int kiss_fw_scan(wfw_image_t *out)
{
    if (!out) return WFW_ERR_UNREADABLE;
    memset(out, 0, sizeof *out);
    out->slot = slot_size();

    if (platform_sd_probe() != 1)
        return out->status = WFW_ERR_NO_CARD;

    // Static rather than automatic: 24 names is 1.5KB, which is a third of some
    // of the task stacks this can be called from, and there is exactly one scan
    // in flight at a time.
    static char names[WFW_SCAN_MAX][SD_NAME_LEN];
    int all = 0;
    int n = platform_sd_list_firmware(names, WFW_SCAN_MAX, &all);
    if (n <= 0)
        return out->status = WFW_ERR_NO_FILE;
    // The `total` the lister has always computed and this call has always
    // thrown away, which is what made the window invisible. The comment below
    // claimed every readable image was examined; past the window it was not,
    // and nothing on the screen or in the struct said which case the owner was
    // looking at.
    out->examined = n;
    out->on_card  = all;

    // EVERY readable app image IN THE WINDOW is examined and the NEWEST wins. Reading the
    // descriptor is what picks it, not the name: a card holding a photo called
    // firmware.bin and the real image called z.bin has to land on the real
    // image.
    //
    // This used to return on the first name that parsed, which quietly made
    // sort order the decision. Two harms, one of them deliberate: an owner who
    // keeps last month's release on the card gets offered last month's
    // release, and anyone who can write to the card can drop a GENUINE,
    // correctly signed older build under a name that sorts first and have the
    // device present it as the update. The signature check cannot see that --
    // the image really is ours -- so choosing correctly here is the only place
    // it gets caught. Sort order survives as the tie break, so two images
    // claiming the same version always resolve the same way.
    //
    // Newest wins even when it does not fit: an image too big for the slot is
    // reported as too big, rather than silently falling back to an older one
    // that does fit. A downgrade the owner did not ask for is worse than a
    // refusal they can read.
    //
    // The window is the residual of that same attack and it is why out->on_card
    // exists. Choosing correctly among the names that were opened is no defence
    // if the real image was never one of them: enough files sorting ahead of it
    // pushed it out of an eight name window entirely, and the device offered
    // whatever was left with nothing on the screen to say a choice had been
    // narrowed. WFW_SCAN_MAX is far past any card an owner builds by hand now,
    // and where it is not, the screen says how many were looked at out of how
    // many are there.
    int best = -1;
    size_t blen = 0;
    char bver[WFW_VER_LEN] = {0}, bproj[WFW_VER_LEN] = {0};
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
        if (kiss_fw_desc_parse(hdr, got, ver, sizeof ver, proj, sizeof proj) != 0)
            continue;

        if (best >= 0 && kiss_fw_version_cmp(ver, bver) <= 0)
            continue;

        best = i;
        blen = len;
        snprintf(bver,  sizeof bver,  "%s", ver);
        snprintf(bproj, sizeof bproj, "%s", proj);
    }

    // Files were there, none of them an app image.
    if (best < 0) {
        snprintf(out->name, sizeof out->name, "%.*s",
                 (int)(sizeof out->name - 1), names[0]);
        return out->status = WFW_ERR_UNREADABLE;
    }

    // A card can carry a name far longer than the row that shows it, and
    // cutting it is the right answer: the descriptor already decided this is
    // the image. The precision says that out loud, because the device compiler
    // treats a bare %s that might not fit as an error.
    snprintf(out->name, sizeof out->name, "%.*s",
             (int)(sizeof out->name - 1), names[best]);
    snprintf(out->version, sizeof out->version, "%s", bver);
    snprintf(out->project, sizeof out->project, "%s", bproj);
    out->size = blen;
    out->cmp = kiss_fw_version_cmp(bver, kiss_fw_running_version());

    // Order matters. Too big is a fact about this device and outranks what the
    // version says; same and older are offers the screen presents differently,
    // not failures to parse.
    //
    // What the slot has to hold is the IMAGE, and the file on the card is the
    // image plus its signature trailer. out->size stays the file size, because
    // that is the number an owner can check against the card; the comparison
    // takes the trailer off. A file no bigger than a trailer has no image under
    // it at all, whatever its descriptor claimed.
    const size_t iblen = blen > KISS_PQSIG_TRAILER_LEN
                       ? blen - KISS_PQSIG_TRAILER_LEN : 0;
    if (iblen == 0)                     out->status = WFW_ERR_UNREADABLE;
    else if (out->slot && iblen > out->slot) out->status = WFW_ERR_TOO_BIG;
    else if (out->cmp == 0)            out->status = WFW_ERR_SAME;
    else if (out->cmp < 0)             out->status = WFW_ERR_OLDER;
    else                               out->status = WFW_OK;
    return out->status;
}

// ---- install ---------------------------------------------------------------

#ifdef ESP_PLATFORM
// What the splitter hands the flash. Everything reaching here is image; the
// trailer never does, so the slot ends up holding the exact bytes espsecure
// signed and esp_ota_end judges the same image it always did.
static int fw_ota_sink(const uint8_t *d, size_t n, void *ud)
{
    return esp_ota_write(*(esp_ota_handle_t *)ud, d, n) == ESP_OK ? 0 : -1;
}
#endif


int kiss_fw_install(const wfw_image_t *img, wfw_progress_fn cb, void *ud)
{
    if (!img || !img->name[0]) return WFW_ERR_UNREADABLE;
    int avail = kiss_fw_available();
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
    // img->size is what the CARD holds. The image is that minus the signature
    // trailer, and the slot only ever receives the image -- so that is the
    // number the slot is measured against and the number esp_ota_begin is told.
    if (img->size <= KISS_PQSIG_TRAILER_LEN) return WFW_ERR_UNREADABLE;
    const size_t image_len = img->size - KISS_PQSIG_TRAILER_LEN;
    if (image_len > dst->size) return WFW_ERR_TOO_BIG;

    size_t len = 0;
    platform_sd_file *f = platform_sd_open(img->name, &len);
    if (!f) return WFW_ERR_CARD_GONE;
    // The card is re-read here rather than trusted from the scan: it may have
    // been swapped between the screen that offered the image and the hold that
    // accepted it, and the size the erase is sized against has to be the size
    // that is about to be written.
    if (len != img->size) { platform_sd_close(f); return WFW_ERR_CARD_GONE; }

    uint8_t *buf = malloc(FW_CHUNK);
    // The splitter holds 8 KB of possible trailer plus a hash context, which is
    // a third of some of the task stacks this is called from. Heap, and freed on
    // every path out -- including the ones that abort the write.
    kiss_pqsig_stream_t *ps = malloc(sizeof *ps);
    if (!buf || !ps) {
        free(buf); free(ps); platform_sd_close(f); return WFW_ERR_WRITE;
    }
    kiss_pqsig_stream_init(ps);

    // The version the owner approved has to be the version about to be written,
    // and matching byte lengths does not say that. A card swapped during the
    // hold -- or an SD emulator that serves different content on the second
    // open -- can hand back a DIFFERENT image of the same size, correctly
    // signed, an older release with a fixed bug in it. The signature check
    // downstream cannot object, because that image really is ours.
    //
    // So the descriptor is re-read from the handle that is about to be written
    // and compared to what the screen showed. Before esp_ota_begin, so a card
    // that changed under us costs nothing: the slot is not erased and the
    // firmware in it is still there. platform_sd cannot seek, so the first
    // chunk is kept and written below rather than read twice.
    size_t first = 0;
    if (platform_sd_read_chunk(f, buf, FW_CHUNK, &first) != 0 ||
        first < WFW_DESC_MIN) {
        free(buf); free(ps); platform_sd_close(f); return WFW_ERR_CARD_GONE;
    }
    {
        char ver[WFW_VER_LEN], proj[WFW_VER_LEN];
        if (kiss_fw_desc_parse(buf, first, ver, sizeof ver,
                                 proj, sizeof proj) != 0 ||
            strcmp(ver, img->version) != 0 || strcmp(proj, img->project) != 0) {
            free(buf); free(ps); platform_sd_close(f); return WFW_ERR_CARD_GONE;
        }
    }

    esp_ota_handle_t h = 0;
    if (esp_ota_begin(dst, image_len, &h) != ESP_OK) {
        free(buf); free(ps);
        platform_sd_close(f);
        return WFW_ERR_WRITE;
    }

    // Every byte off the card now goes through the splitter rather than straight
    // into the slot. It releases everything more than a trailer's length from
    // the end and hashes exactly what it releases, so esp_ota_write is handed
    // the image and nothing else and the digest can only cover bytes that
    // reached flash. The first chunk goes through it too: the descriptor above
    // was read FROM that chunk, not instead of it.
    size_t read_total = 0, got = first;
    int last_pct = -1, rc = WFW_OK;
    for (;;) {
        if (got == 0)
            break;
        if (kiss_pqsig_stream_feed(ps, buf, got, fw_ota_sink, &h) != 0) {
            rc = WFW_ERR_WRITE;
            break;
        }
        read_total += got;
        if (cb) {
            int pct = (int)((read_total * 100) / len);
            if (pct != last_pct) { last_pct = pct; cb(pct, ud); }
        }
        got = 0;
        if (platform_sd_read_chunk(f, buf, FW_CHUNK, &got) != 0) {
            rc = WFW_ERR_CARD_GONE;
            break;
        }
    }
    free(buf);
    platform_sd_close(f);

    // A short read is not an error at the seam, so it is caught here: fewer
    // bytes than the file claimed means the card went away mid write, and a
    // truncated image must never reach esp_ota_end, which would judge it only
    // by its signature over whatever arrived.
    if (rc == WFW_OK && read_total != len)
        rc = WFW_ERR_CARD_GONE;

    uint8_t digest[32];
    const uint8_t *trailer = NULL;
    size_t tlen = 0;
    if (rc == WFW_OK &&
        kiss_pqsig_stream_end(ps, digest, &trailer, &tlen) != KISS_PQSIG_OK)
        rc = WFW_ERR_PQ_REJECTED;

    if (rc != WFW_OK) {
        esp_ota_abort(h);
        free(ps);
        return rc;
    }

    // Two gates, and an image has to pass BOTH. The post quantum one goes
    // FIRST, and the order is not a preference -- it is the difference between
    // a refusal that reads true and one that lies.
    //
    // Found on a bench: an image from before the trailer existed was refused
    // with "the signature did not check out". It is a genuine release, signed
    // with the release key, and that message sends its owner hunting for a
    // corrupt download. What actually happened is that the splitter held back
    // its last 8192 bytes -- which on a file with no trailer are the END OF THE
    // IMAGE, ECDSA signature block included -- so esp_ota_end was handed a
    // truncated image and said the only thing it could.
    //
    // Holding those bytes back is not optional and cannot be conditional: the
    // card does not seek, so nothing can know whether a trailer is there until
    // the file has gone past. What CAN change is which gate speaks first. Ask
    // the question that is actually wrong with the file, and the ECDSA check
    // only ever runs on an image that really did keep its own last 8 KB.
    const int pq = kiss_pqsig_check(digest, trailer, tlen);
    free(ps);                      // trailer pointed into it
    if (pq != KISS_PQSIG_OK) { esp_ota_abort(h); return WFW_ERR_PQ_REJECTED; }

    // Now the first lock. esp_ota_end verifies the image against the key in the
    // running app's signature block; ESP_ERR_OTA_VALIDATE_FAILED is a real
    // refusal, not an IO problem, and the screen says so in those words.
    esp_err_t err = esp_ota_end(h);
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) return WFW_ERR_REJECTED;
    if (err != ESP_OK) return WFW_ERR_WRITE;

    if (esp_ota_set_boot_partition(dst) != ESP_OK) return WFW_ERR_WRITE;
    return WFW_OK;
#endif
}

// ---- rollback --------------------------------------------------------------

bool kiss_fw_confirm_ok(bool sign_ok, bool touch_ok, bool storage_ok)
{
    return sign_ok && touch_ok && storage_ok;
}

void kiss_fw_mark_valid(void)
{
#ifdef ESP_PLATFORM
    // Only a slot actually on trial gets marked. Calling this unconditionally
    // would be harmless today, but reading the state is what makes the intent
    // legible: this confirms an update, it is not a ritual every boot performs.
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        // The result was dropped here. It is the difference between "this image
        // is now permanent" and "the bootloader will revert it on the next
        // reboot", and a device is entitled to say which of those happened in
        // its log rather than leaving it to be inferred from behaviour weeks
        // later.
        esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
        if (e == ESP_OK)
            ESP_LOGI(TAG, "slot confirmed: rollback cancelled");
        else
            ESP_LOGE(TAG, "could not confirm this slot (%s); the next reboot "
                          "returns the previous firmware", esp_err_to_name(e));
    }
#endif
}
