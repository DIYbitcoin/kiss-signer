// See pq_hw_sha.h for why the peripheral is held rather than acquired per hash.
#include "pq_hw_sha.h"

#ifdef ESP_PLATFORM

#include <hal/sha_types.h>
#include <sha/sha_core.h>
#include <soc/soc_caps.h>

#if !SOC_SHA_SUPPORT_RESUME
#error "SLH-DSA on the SHA accelerator needs an engine that resumes from an arbitrary midstate"
#endif

// How many compressions may pass before the peripheral is handed back once.
//
// The crypto lock is shared with AES, so a bracket that is never let go is a
// bracket that stalls flash encryption, NVS and anything else that needs either
// block for as long as the operation runs -- and one SLH-DSA signature runs for
// seconds. Releasing costs the ~2165 cycle re-acquire, so it is worth doing
// rarely: at roughly 816 cycles a compression on a 400 MHz part, 2048 of them is
// about 4 ms of held time and the re-acquire is under 0.2% on top.
//
// The yield is safe at exactly this point and nowhere else. Between two
// compressions the whole state lives in the caller's context in RAM and the
// peripheral holds nothing worth keeping, which is the same property that makes
// the held design work at all.
#define PQ_SHA_YIELD_EVERY 2048

static int s_hold_depth;        // nesting of begin()/end()
static bool s_suspended;        // the hold is open but the peripheral is out
static bool s_force_sw;         // portable C even inside a bracket
static uint32_t s_since_yield;  // compressions since the last hand back
static uint32_t s_compressions; // compressions since the last reset

static void take(void)
{
    esp_sha_acquire_hardware();
    esp_sha_set_mode(SHA2_256);
    s_since_yield = 0;
}

void pq_hw_sha_begin(void)
{
    if (s_force_sw) return;
    if (s_hold_depth++ == 0) take();
}

void pq_hw_sha_end(void)
{
    if (s_force_sw) return;
    if (s_hold_depth > 0 && --s_hold_depth == 0) {
        if (!s_suspended) esp_sha_release_hardware();
        s_suspended = false;
    }
}

void pq_hw_sha_suspend(void)
{
    if (s_hold_depth > 0 && !s_suspended) {
        esp_sha_release_hardware();
        s_suspended = true;
    }
}

void pq_hw_sha_resume(void)
{
    if (s_hold_depth > 0 && s_suspended) {
        take();
        s_suspended = false;
    }
}

bool pq_hw_sha_held_and_active(void) { return s_hold_depth > 0 && !s_suspended; }

void pq_hw_sha_force_sw(bool on) { s_force_sw = on; }

// slhdsa-c keeps its SHA-256 context as
//
//     struct { uint32_t s[8 + 24]; size_t i, len; }
//
// with the chaining state in s[0..7] and the 64 byte message block in s[8..23],
// both already in big endian order -- which is exactly the SHA_H_MEM + SHA_M_MEM
// layout, and neither half of the HAL byteswaps on this target. So the context
// maps onto the peripheral with no marshalling, and the mode was set once in
// take(). All this does per hash is move words.
static void compress_hw(void *v)
{
    uint32_t *sp = (uint32_t *)v;
    esp_sha_write_digest_state(SHA2_256, sp);  // midstate -> SHA_H_MEM
    esp_sha_block(SHA2_256, sp + 8, false);    // block -> SHA_M_MEM, then CONTINUE
    esp_sha_read_digest_state(SHA2_256, sp);   // wait idle, SHA_H_MEM -> state
}

void sha2_256_compress(void *v)
{
    s_compressions++;
    if (!pq_hw_sha_held_and_active()) {
        // No bracket, or one that is currently suspended. Correct either way,
        // just slower -- which is the failure mode a forgotten bracket has to
        // have. See pq_hw_sha.h.
        sha2_256_compress_sw(v);
        return;
    }
    compress_hw(v);
    if (++s_since_yield >= PQ_SHA_YIELD_EVERY) {
        esp_sha_release_hardware();
        take();
    }
}

#else // !ESP_PLATFORM

// Desktop: there is no peripheral, so the brackets are shape only. They are
// still compiled and still called, so the seam the device takes is the seam the
// test suite exercises, and a caller that forgets one fails the same way in both
// places -- which is to say not at all.
static uint32_t s_compressions;

void pq_hw_sha_begin(void) {}
void pq_hw_sha_end(void) {}
void pq_hw_sha_suspend(void) {}
void pq_hw_sha_resume(void) {}
bool pq_hw_sha_held_and_active(void) { return false; }
void pq_hw_sha_force_sw(bool on) { (void)on; }

void sha2_256_compress(void *v)
{
    s_compressions++;
    sha2_256_compress_sw(v);
}

#endif // ESP_PLATFORM

uint32_t pq_hw_sha_compressions(void) { return s_compressions; }
void pq_hw_sha_reset_compressions(void) { s_compressions = 0; }
