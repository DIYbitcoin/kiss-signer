// The RLE decoder on its own, with no reference to any image, so the unit
// tests can link it without dragging in 2.7 MB of baked art. kiss_art.c
// holds the tables and the boot-time unpack that uses this.
#include "kiss_art_rle.h"

#include <string.h>

// Deliberately a mirror of lv_rle_decompress rather than a call to it: reusing
// LVGL's would mean turning on LV_USE_RLE, which drags in the bin decoder's
// compressed path and the config changes kiss_art.h explains we are avoiding.
// This function is the whole cost of not doing that.
//
// Stricter than LVGL's in one way that matters. LVGL tolerates a final block
// that overruns the output and silently truncates it; here any overrun stops
// and reports what was written, so the caller's "did I get raw_len back" check
// catches a corrupt blob instead of rendering half an image over whatever the
// buffer happened to hold.
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
