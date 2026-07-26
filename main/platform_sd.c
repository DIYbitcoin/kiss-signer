// SD card platform seam — see platform_sd.h. v1 policy: the SD card carries
// PUBLIC data only (PSBTs, descriptors); never secrets.
#include "platform_sd.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#ifdef SIMULATOR

#include <sys/stat.h>
#define SD_BASE "/tmp/simsd"
int platform_sd_mount(void) { mkdir(SD_BASE, 0777); return 0; }
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
    if (s_card)
        return 0;
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
    if (s_card) {                              // already mounted: is it still there?
        if (sdmmc_get_status(s_card) == ESP_OK)
            return 1;
        platform_sd_unmount();                 // card was pulled — drop the stale mount
        return 0;
    }
    return platform_sd_mount() == 0 ? 1 : 0;   // no card known: try to catch an insert
}

#endif

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

int platform_sd_list_psbt(char names[][SD_NAME_LEN], int max)
{
    DIR *d = opendir(SD_BASE);
    if (!d)
        return -1;
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        size_t l = strlen(nm);
        if (nm[0] == '.')                     // macOS AppleDouble junk on real cards
            continue;
        if (l < 6 || l >= SD_NAME_LEN || strcasecmp(nm + l - 5, ".psbt") != 0)
            continue;
        snprintf(names[n++], SD_NAME_LEN, "%s", nm);
    }
    closedir(d);
    qsort(names, (size_t)n, SD_NAME_LEN, name_cmp);
    return n;
}

static void full_path(char *dst, size_t dstsz, const char *name)
{
    snprintf(dst, dstsz, "%s/%s", SD_BASE, name);
}

int platform_sd_read(const char *name, uint8_t *buf, size_t max, size_t *len)
{
    char p[SD_NAME_LEN + 16];
    full_path(p, sizeof p, name);
    FILE *f = fopen(p, "rb");
    if (!f)
        return -1;
    *len = fread(buf, 1, max, f);
    int full = !feof(f);                      // file bigger than our buffer = reject
    fclose(f);
    return (*len == 0 || full) ? -2 : 0;
}

int platform_sd_write(const char *name, const uint8_t *buf, size_t len)
{
    char p[SD_NAME_LEN + 16];
    full_path(p, sizeof p, name);
    FILE *f = fopen(p, "wb");
    if (!f)
        return -1;
    size_t wr = fwrite(buf, 1, len, f);
    int rc = (fclose(f) == 0 && wr == len) ? 0 : -2;
    return rc;
}
