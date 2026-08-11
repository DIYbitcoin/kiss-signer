// The baked-art RLE decoder, declared on its own so it can be built and tested
// without LVGL. kiss_art.h has the image side of this and the reasoning for
// why the art is compressed at all.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// LVGL's RLE format (managed_components/lvgl__lvgl/src/libs/rle/lv_rle.c): a
// control byte with the high bit set copies N blocks straight through, with it
// clear repeats the next block N times, N at most 127 either way. `blk` is the
// block size in bytes, chosen per image by tools/bake_art.py.
//
// Returns the number of bytes written, which the caller MUST check against
// what it asked for -- a short return means the input was truncated or the
// output was too small, not that the result is usable. Never writes past
// out_len, and never reads past in_len.
uint32_t art_rle_decompress(const uint8_t *in, uint32_t in_len,
                            uint8_t *out, uint32_t out_len, uint8_t blk);

#ifdef __cplusplus
}
#endif
