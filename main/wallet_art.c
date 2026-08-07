// See wallet_art.h for why the art is compressed and why the unpack lives here
// rather than in LVGL's decoder.
#include "wallet_art.h"

#include <stdlib.h>
#include <string.h>

#include "menu_img.h"
#include "gameover_img.h"
#include "wallet_img.h"
#include "sprites.h"
#include "menu_logo.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
static const char *TAG = "art";
#else
#include <stdio.h>
#endif

// Deliberately a mirror of lv_rle_decompress rather than a call to it: reusing
// LVGL's would mean turning on LV_USE_RLE, which drags in the bin decoder's
// compressed path and the config changes wallet_art.h explains we are avoiding.
// This is the whole cost of not doing that.
//
// Stricter than LVGL's in one way that matters. LVGL tolerates a final block
// that overruns the output and silently truncates it; here any overrun stops
// and reports what was written, so the caller's "did I get raw_len back" check
// catches a corrupt blob instead of rendering half an image over whatever the
// buffer held before.
uint32_t art_rle_decompress(const uint8_t *in, uint32_t in_len,
                            uint8_t *out, uint32_t out_len, uint8_t blk)
{
    uint32_t rd = 0, wr = 0;

    if (!in || !out || blk == 0) return 0;

    while (rd < in_len) {
        uint8_t ctrl = in[rd++];

        if (ctrl & 0x80) {                      // literal: copy N blocks
            uint32_t n = (uint32_t)blk * (ctrl & 0x7F);
            if (rd + n > in_len || wr + n > out_len) return wr;
            memcpy(out + wr, in + rd, n);
            rd += n;
            wr += n;
        } else {                                // run: repeat one block N times
            uint32_t n = (uint32_t)blk * ctrl;
            if (rd + blk > in_len || wr + n > out_len) return wr;
            if (blk == 1) {
                memset(out + wr, in[rd], ctrl); // the common case for an A8 plane
                wr += ctrl;
            } else {
                for (uint8_t i = 0; i < ctrl; i++) {
                    memcpy(out + wr, in + rd, blk);
                    wr += blk;
                }
            }
            rd += blk;
        }
    }
    return wr;
}

// Every art .c file contributes its own table, so adding or removing an image
// is a bake_art.py run and not an edit here.
static const struct {
    const art_entry_t *e;
    const int         *n;
} TABLES[] = {
    { menu_img_art,     &menu_img_art_n     },
    { gameover_img_art, &gameover_img_art_n },
    { wallet_img_art,   &wallet_img_art_n   },
    { sprites_art,      &sprites_art_n      },
    { menu_logo_art,    &menu_logo_art_n    },
};

static void *art_alloc(uint32_t len)
{
#ifdef ESP_PLATFORM
    // PSRAM explicitly, never internal: 4 MB of internal SRAM does not exist,
    // and a silent fallback to it would starve the DMA buffers instead of
    // failing here where it is reported.
    return heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(len);
#endif
}

int art_unpack_all(void)
{
    int failed = 0, done = 0;
    uint32_t bytes = 0;

    for (size_t t = 0; t < sizeof TABLES / sizeof TABLES[0]; t++) {
        const art_entry_t *tab = TABLES[t].e;
        for (int i = 0; i < *TABLES[t].n; i++) {
            const art_entry_t *a = &tab[i];

            if (!a->dsc || a->dsc->data) continue;      // already unpacked

            uint8_t *buf = art_alloc(a->raw_len);
            if (!buf) { failed++; continue; }

            uint32_t got = art_rle_decompress(a->rle, a->rle_len, buf,
                                              a->raw_len, a->blk);
            if (got != a->raw_len) {
                // Short or long is equally wrong, and a partly filled buffer is
                // worse than no image: free it and leave .data NULL.
                free(buf);
                failed++;
                continue;
            }

            a->dsc->data = buf;
            a->dsc->data_size = a->raw_len;
            bytes += a->raw_len;
            done++;
        }
    }

#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "unpacked %d images, %lu KB PSRAM, %lu KB free",
             done, (unsigned long)(bytes / 1024),
             (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    if (failed)
        ESP_LOGE(TAG, "%d images FAILED to unpack -- art will be missing", failed);
#else
    if (failed)
        fprintf(stderr, "art: %d images failed to unpack\n", failed);
#endif
    return failed;
}
