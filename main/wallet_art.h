// Baked art, compressed in flash and unpacked into PSRAM once at boot.
//
// The art was 52% of the app binary and 1.65x the size of everything that
// executes -- four 800x480 RGB565 backdrops at exactly 768,000 bytes each,
// plus the sprites and the logo, all sitting uncompressed in .rodata while the
// partition sat 94% full. tools/bake_art.py turns every pixel map into RLE and
// empties the descriptor that pointed at it; art_unpack_all puts the pixels
// back at boot, in PSRAM, and fills the descriptor in.
//
// After that call LVGL sees ordinary uncompressed images and nothing else in
// the firmware knows this happened. That is the point of doing it here rather
// than through LVGL's own compressed-image path, which would have needed
// LV_BIN_DECODER_RAM_LOAD, LVGL moved off its 128 KB builtin pool (a 768 KB
// decompress cannot allocate from it at all) and the image cache switched on
// (it is off, so every redraw would decompress again and the animated menu
// would do that per frame). Three memory-behaviour changes across the whole UI
// to arrive at the same place this gets to with one call and no LVGL config.
//
// Flash is the scarce resource, not PSRAM: the board carries far more PSRAM
// than the ~4 MB this takes, and the two DPI framebuffers already live there.
#pragma once
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// One baked image. Generated into the art .c files by tools/bake_art.py.
typedef struct {
    lv_image_dsc_t *dsc;      // the descriptor to fill in; .data starts NULL
    const uint8_t  *rle;      // compressed pixels, in flash
    uint32_t        rle_len;
    uint32_t        raw_len;  // decompressed size, and an exact requirement
    uint8_t         blk;      // RLE block size in bytes, chosen per image
} art_entry_t;

// LVGL's RLE format (managed_components/lvgl__lvgl/src/libs/rle/lv_rle.c): a
// control byte with the high bit set copies N blocks straight through, with it
// clear repeats the next block N times, N at most 127 either way. Returns the
// number of bytes written, which the caller must check equals what it asked
// for -- a short read means the input was truncated, not that it should be
// used. Never writes past out_len.
uint32_t art_rle_decompress(const uint8_t *in, uint32_t in_len,
                            uint8_t *out, uint32_t out_len, uint8_t blk);

// Unpack every baked image. Call once, before the first screen is built.
// Returns the number that FAILED, so 0 is success.
//
// A failure is left as a descriptor with .data still NULL. That renders as
// nothing rather than as a crash, which is the right way round: the signer's
// live widgets sit on top of the home backdrop and keep working without it, so
// a board short of PSRAM loses its decoy's looks and not its wallet.
int art_unpack_all(void);

#ifdef __cplusplus
}
#endif
