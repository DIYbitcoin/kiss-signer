// See kiss_art.h for why the art is compressed and why the unpack lives here
// rather than in LVGL's decoder.
#include "kiss_art.h"

#include <stdlib.h>

#include "menu_img.h"
#include "gameover_img.h"
#include "kiss_img.h"
#include "sprites.h"
#include "menu_logo.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
static const char *TAG = "art";
#else
#include <stdio.h>
#endif

// art_rle_decompress lives in kiss_art_rle.c so the unit tests can link the
// decoder without every baked image coming with it.

// Every art .c file contributes its own table, so adding or removing an image
// is a bake_art.py run and not an edit here.
static const struct {
    const art_entry_t *e;
    const int         *n;
} TABLES[] = {
    { menu_img_art,     &menu_img_art_n     },
    { gameover_img_art, &gameover_img_art_n },
    { kiss_img_art,   &kiss_img_art_n   },
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
    // On the host a failure is fatal, and that is the point: nothing else
    // checks the baked blobs. The device tolerates a missing image because a
    // board short of PSRAM should still be a wallet, but on the host the only
    // way to fail is a blob that does not decompress to its declared length --
    // which is a corrupt or mis-baked asset, and a gate that shrugged at it
    // would let one ship. Every sim build runs this, so the screen walk is
    // also a 41 image integrity check.
    if (failed) {
        fprintf(stderr, "art: %d of %d images failed to unpack -- corrupt or "
                        "mis-baked asset, run tools/bake_art.py\n",
                failed, failed + done);
        abort();
    }
#endif
    return failed;
}
