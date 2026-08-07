// Host tests for the SD firmware update logic. The two things that can decide
// wrongly without any hardware being involved are version ordering and reading
// an image descriptor off a card, so those are what this covers.
//
// The one that matters most: "0.1.0-beta10" is NEWER than "0.1.0-beta7". A
// string compare says the opposite, and getting it backwards offers a
// downgrade as an upgrade on a device that holds money.
// Build: sim/build_test.sh -> /tmp/kisstest
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "wallet_fw.h"
#include "platform_sd.h"

static int ffails;
static void ok(const char *n, int c)
{
    if (c) printf("PASS: %s\n", n);
    else { printf("FAIL: %s\n", n); ffails++; }
}

static void cmp_is(const char *a, const char *b, int want)
{
    int got = wallet_fw_version_cmp(a, b);
    int norm = got < 0 ? -1 : got > 0 ? 1 : 0;
    char nm[96];
    snprintf(nm, sizeof nm, "cmp(%s, %s) %s", a, b,
             want < 0 ? "older" : want > 0 ? "newer" : "same");
    if (norm != want)
        printf("  (got %d, want %d)\n", norm, want);
    ok(nm, norm == want);
    // Antisymmetry, every time: a comparator that says both are newer than the
    // other passes half a test suite and sorts nothing.
    int rev = wallet_fw_version_cmp(b, a);
    int rnorm = rev < 0 ? -1 : rev > 0 ? 1 : 0;
    if (rnorm != -want) {
        printf("FAIL: cmp(%s, %s) not antisymmetric (%d vs %d)\n", a, b, norm, rnorm);
        ffails++;
    }
}

// A minimal but real ESP app image head: 32 bytes of image + segment header,
// then the descriptor. Only the fields wallet_fw reads are filled.
#define DESC_OFF 32
static void mk_image_head(uint8_t *buf, size_t len, uint32_t magic,
                          const char *ver, const char *proj)
{
    memset(buf, 0, len);
    buf[0] = 0xE9;                       // ESP image magic, for realism
    buf[DESC_OFF + 0] = (uint8_t)(magic);
    buf[DESC_OFF + 1] = (uint8_t)(magic >> 8);
    buf[DESC_OFF + 2] = (uint8_t)(magic >> 16);
    buf[DESC_OFF + 3] = (uint8_t)(magic >> 24);
    if (ver)  memcpy(buf + DESC_OFF + 16, ver, strlen(ver));
    if (proj) memcpy(buf + DESC_OFF + 48, proj, strlen(proj));
}

static void wipe_card(void)
{
    // platform_sd's sim base. Remove only what these tests create.
    static const char *const junk[] = {
        "fw-new.bin", "fw-old.bin", "fw-same.bin", "aaa-notimage.bin",
        "huge.bin", "short.bin", NULL
    };
    for (int i = 0; junk[i]; i++) {
        char p[128];
        snprintf(p, sizeof p, "/tmp/simsd/%s", junk[i]);
        remove(p);
    }
}

static int put(const char *name, const uint8_t *buf, size_t len)
{
    char p[128];
    snprintf(p, sizeof p, "/tmp/simsd/%s", name);
    FILE *f = fopen(p, "wb");
    if (!f) return -1;
    size_t w = fwrite(buf, 1, len, f);
    return (fclose(f) == 0 && w == len) ? 0 : -1;
}

int test_fw(void)
{
    printf("\n-- firmware update --\n");

    // ---- version ordering ----
    cmp_is("0.1.0", "0.1.0", 0);
    cmp_is("0.2.0", "0.1.9", 1);
    cmp_is("1.0.0", "0.99.99", 1);
    cmp_is("0.1.1", "0.1.0", 1);
    // The numeric tail. This is the case a strcmp gets wrong.
    cmp_is("0.1.0-beta10", "0.1.0-beta7", 1);
    cmp_is("0.1.0-beta7", "0.1.0-beta10", -1);
    cmp_is("0.1.0-beta7", "0.1.0-beta7", 0);
    // A finished release is newer than any prerelease of the same numbers.
    cmp_is("0.1.0", "0.1.0-beta7", 1);
    cmp_is("0.1.0", "0.1.0-rc1", 1);
    // Different tail names order by name, so rc follows beta.
    cmp_is("0.1.0-rc1", "0.1.0-beta9", 1);
    // Numbers outrank the tail entirely.
    cmp_is("0.2.0-beta1", "0.1.0", 1);
    // Absurd input must not wrap into something small and read as older.
    cmp_is("99999999999.0.0", "1.0.0", 1);
    ok("cmp survives null", wallet_fw_version_cmp(NULL, NULL) == 0);

    // ---- descriptor parsing ----
    uint8_t img[WFW_DESC_MIN];
    char ver[WFW_VER_LEN], proj[WFW_VER_LEN];

    mk_image_head(img, sizeof img, 0xABCD5432u, "0.9.9-beta2", "kiss");
    ok("descriptor parses",
       wallet_fw_desc_parse(img, sizeof img, ver, sizeof ver, proj, sizeof proj) == 0 &&
       strcmp(ver, "0.9.9-beta2") == 0 && strcmp(proj, "kiss") == 0);

    mk_image_head(img, sizeof img, 0xDEADBEEFu, "0.9.9", "kiss");
    ok("wrong magic refused",
       wallet_fw_desc_parse(img, sizeof img, ver, sizeof ver, proj, sizeof proj)
           == WFW_ERR_UNREADABLE);

    // Right magic, empty version: refused rather than treated as version "",
    // which would sort below everything and read as a downgrade.
    mk_image_head(img, sizeof img, 0xABCD5432u, NULL, "kiss");
    ok("empty version refused",
       wallet_fw_desc_parse(img, sizeof img, ver, sizeof ver, proj, sizeof proj)
           == WFW_ERR_UNREADABLE);

    mk_image_head(img, sizeof img, 0xABCD5432u, "0.9.9", "kiss");
    ok("short buffer refused",
       wallet_fw_desc_parse(img, 8, ver, sizeof ver, proj, sizeof proj)
           == WFW_ERR_UNREADABLE);

    // A version field with no terminator must not run off the end of its 32
    // bytes into the project name.
    mk_image_head(img, sizeof img, 0xABCD5432u, NULL, "kiss");
    memset(img + DESC_OFF + 16, '9', 32);
    ok("unterminated version stays in its field",
       wallet_fw_desc_parse(img, sizeof img, ver, sizeof ver, proj, sizeof proj) == 0 &&
       strlen(ver) == 31);

    // ---- scan over the sim card ----
    platform_sd_test_set_present(1);
    if (platform_sd_mount() != 0) {
        printf("FAIL: sim card would not mount\n");
        return ++ffails;
    }
    wipe_card();

    wfw_image_t got;
    ok("empty card reports no file", wallet_fw_scan(&got) == WFW_ERR_NO_FILE);

    // A .bin that is not an app image is skipped, not offered.
    uint8_t junk[WFW_DESC_MIN];
    memset(junk, 0x5A, sizeof junk);
    put("aaa-notimage.bin", junk, sizeof junk);
    ok("non image .bin is refused", wallet_fw_scan(&got) == WFW_ERR_UNREADABLE);

    // Sorting puts aaa-notimage.bin first, so finding the real image proves the
    // descriptor picks the file rather than the name.
    static uint8_t big[WFW_DESC_MIN * 4];
    mk_image_head(big, sizeof big, 0xABCD5432u, "99.0.0", "kiss");
    put("fw-new.bin", big, sizeof big);
    int rc = wallet_fw_scan(&got);
    ok("newer image found past a decoy",
       rc == WFW_OK && strcmp(got.name, "fw-new.bin") == 0 &&
       strcmp(got.version, "99.0.0") == 0 && got.cmp > 0);
    ok("size reported", got.size == sizeof big);

    // Older and same are reported as their own outcomes, not as OK and not as
    // unreadable: the screen presents each differently.
    wipe_card();
    mk_image_head(big, sizeof big, 0xABCD5432u, "0.0.1", "kiss");
    put("fw-old.bin", big, sizeof big);
    ok("older image reported older", wallet_fw_scan(&got) == WFW_ERR_OLDER && got.cmp < 0);

    wipe_card();
    mk_image_head(big, sizeof big, 0xABCD5432u, wallet_fw_running_version(), "kiss");
    put("fw-same.bin", big, sizeof big);
    ok("same version reported same", wallet_fw_scan(&got) == WFW_ERR_SAME && got.cmp == 0);

    // Too big outranks the version: it is a fact about this device.
    wipe_card();
    {
        size_t huge = 0x7F0000 + 1;
        uint8_t *hb = calloc(1, huge);
        if (hb) {
            mk_image_head(hb, WFW_DESC_MIN, 0xABCD5432u, "99.0.0", "kiss");
            put("huge.bin", hb, huge);
            free(hb);
            ok("oversized image refused before any write",
               wallet_fw_scan(&got) == WFW_ERR_TOO_BIG && got.cmp > 0);
        } else {
            printf("SKIP: oversized image (no memory)\n");
        }
    }

    // A card that is not there is its own answer, distinct from an empty one.
    wipe_card();
    platform_sd_test_set_present(0);
    ok("absent card reports no card", wallet_fw_scan(&got) == WFW_ERR_NO_CARD);
    platform_sd_test_set_present(1);

    // A read that fails mid file must not look like a clean end of file.
    {
        static uint8_t s[WFW_DESC_MIN * 2];
        mk_image_head(s, sizeof s, 0xABCD5432u, "99.0.0", "kiss");
        put("short.bin", s, sizeof s);
        size_t len = 0;
        platform_sd_file *f = platform_sd_open("short.bin", &len);
        ok("stream open reports size", f != NULL && len == sizeof s);
        if (f) {
            uint8_t b[64];
            size_t n = 0;
            platform_sd_test_fail_next(PLATFORM_SD_TEST_FAIL_READ);
            ok("failing read reports an error", platform_sd_read_chunk(f, b, sizeof b, &n) < 0);
            // Sticky: every later read stays failed, so a caller looping to EOF
            // cannot mistake the yank for the end of the file.
            ok("failure is sticky", platform_sd_read_chunk(f, b, sizeof b, &n) < 0);
            platform_sd_close(f);
        }
    }

    // The sim has no flash and no key, so install must refuse rather than
    // pretend. This is also what a device build without the release key does.
    wipe_card();
    mk_image_head(big, sizeof big, 0xABCD5432u, "99.0.0", "kiss");
    put("fw-new.bin", big, sizeof big);
    wallet_fw_scan(&got);
    ok("install refuses without a verifying build",
       wallet_fw_install(&got, NULL, NULL) == WFW_ERR_UNSIGNED);
    ok("availability agrees", wallet_fw_available() == WFW_ERR_UNSIGNED);

    wipe_card();
    return ffails;
}
