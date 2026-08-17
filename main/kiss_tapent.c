// tap entropy: see kiss_tapent.h
#include "kiss_tapent.h"

#include <string.h>

#include "wally_core.h"
#include "wally_crypto.h"

static uint8_t  s_chain[32];
static unsigned s_count;
static uint64_t s_last_us;
static int      s_started;

void kiss_tapent_reset(void)
{
    wally_bzero(s_chain, sizeof s_chain);
    s_count = 0;
    s_last_us = 0;
    s_started = 0;
}

int kiss_tapent_tap(uint64_t us, uint32_t cycles, int16_t x, int16_t y)
{
    if (s_count >= WTAP_TARGET)
        return 0;
    // The first tap has no predecessor, so it cannot be too close to one.
    // Afterwards the window is measured from the last ACCEPTED tap: measuring
    // from the last OFFERED one would let a fast drag ratchet the window
    // forward and admit the artifacts it exists to drop.
    if (s_started && us - s_last_us < WTAP_DEBOUNCE_US)
        return 0;
    s_started = 1;
    s_last_us = us;

    // 16 bytes: cycles || us || x || y, little-endian. The cycle counter carries
    // the entropy; the microsecond stamp is the value an auditor recognises and
    // costs two words; the coordinates are a bonus and are NOT counted toward
    // the budget, because people tap the same spot and that correlation is real.
    uint8_t rec[16];
    rec[0] = (uint8_t)(cycles      ); rec[1] = (uint8_t)(cycles >>  8);
    rec[2] = (uint8_t)(cycles >> 16); rec[3] = (uint8_t)(cycles >> 24);
    for (int i = 0; i < 8; i++) rec[4 + i] = (uint8_t)(us >> (8 * i));
    rec[12] = (uint8_t)((uint16_t)x     ); rec[13] = (uint8_t)((uint16_t)x >> 8);
    rec[14] = (uint8_t)((uint16_t)y     ); rec[15] = (uint8_t)((uint16_t)y >> 8);

    uint8_t cat[48];
    memcpy(cat, s_chain, 32);
    memcpy(cat + 32, rec, sizeof rec);
    int rc = wally_sha256(cat, sizeof cat, s_chain, 32);
    wally_bzero(cat, sizeof cat);
    wally_bzero(rec, sizeof rec);
    if (rc != WALLY_OK)
        return 0;

    s_count++;
    return 1;
}

unsigned kiss_tapent_count(void)
{
    return s_count;
}

int kiss_tapent_take(uint8_t out[32])
{
    if (!out || s_count < WTAP_TARGET)
        return -1;
    memcpy(out, s_chain, 32);
    return 0;
}

void kiss_tapent_peek(uint8_t out[32])
{
    if (!out)
        return;
    memcpy(out, s_chain, 32);
}
