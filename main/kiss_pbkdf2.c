// See kiss_pbkdf2.h for why this exists and where it came from.
#include "kiss_pbkdf2.h"
#include "kiss_wipe.h"

#include <string.h>

#include "wally_crypto.h"

static uint32_t s_compressions;

uint32_t kiss_pbkdf2_hw_compressions(void) { return s_compressions; }
void     kiss_pbkdf2_hw_reset_compressions(void) { s_compressions = 0; }

static int sw_derive(const uint8_t *pass, size_t pass_len,
                     const uint8_t *salt, size_t salt_len,
                     uint32_t iters, uint8_t out[32])
{
    return wally_pbkdf2_hmac_sha256(pass, pass_len, salt, salt_len, 0,
                                    iters, out, 32) == WALLY_OK ? 0 : -1;
}

#ifdef ESP_PLATFORM

#include <hal/sha_types.h>
#include <sha/sha_core.h>

#include "pq_hw_sha.h"   // components/slhdsa: the held bracket, and only that

#define SHA256_BLOCK   64
#define SHA256_DIGEST  32

// Iterations per hold. The crypto lock is shared with AES, so a bracket that
// is never let go stalls NVS, flash encryption and anything else that needs
// either block for as long as the derivation runs -- and this one runs for
// most of a second. At ~816 cycles a compression on a 400 MHz part, 1024
// iterations is two thousand compressions, about 4 ms of held time, and the
// ~2165 cycle reacquire is under a tenth of a percent on top.
//
// The boundary is safe here and nowhere else: between two iterations the whole
// state is the two midstates and the accumulator, all in this function's
// frame, and the peripheral holds nothing worth keeping. pq_hw_sha_begin()
// reasserts the mode after every reacquire, which is not optional -- the reset
// clobbers SHA_MODE_REG along with H_MEM, and an engine left in SHA-1 produces
// a wrong key rather than an error.
#define CHUNK_ITERS 1024

// Both esp_sha_ calls take uint32_t*, so these must stay word aligned.
typedef union { uint8_t b[SHA256_DIGEST]; uint32_t w[SHA256_DIGEST / 4]; } state_t;
typedef union { uint8_t b[SHA256_BLOCK];  uint32_t w[SHA256_BLOCK  / 4]; } block_t;

// Every message the iteration loop hashes is a 64 byte ipad/opad block plus a
// 32 byte digest: 96 bytes, 768 bits. The padding is laid down once, because
// the loop only ever rewrites bytes 0..31 -- esp_sha_read_digest_state writes
// exactly eight words and leaves the tail alone.
static void pad96(block_t *blk)
{
    memset(blk->b, 0, sizeof blk->b);
    blk->b[SHA256_DIGEST]   = 0x80;
    blk->b[SHA256_BLOCK - 2] = 0x03;   // 768 == 0x0300
}

// -1 means "could not", never "wrong": every caller falls back to libwally.
static int hw_derive(const uint8_t *pass, size_t pass_len,
                     const uint8_t *salt, size_t salt_len,
                     uint32_t iters, uint8_t out[32])
{
    uint8_t k0[SHA256_BLOCK];
    state_t ipad, opad, accum;
    block_t pad, inner, outer;
    int rc = -1;

    // U_1 first, and from libwally, because everything awkward about PBKDF2
    // lives in it: the variable length salt, the big endian block counter and
    // a message that spans blocks. It is one iteration out of a hundred
    // thousand. What is left for the accelerator is the part that is uniform
    // -- a fixed 96 byte message, twice per iteration, forever.
    //
    // It also has to happen before anything is held. libwally hashes in
    // software so it would not deadlock, but the rule that nothing between
    // begin() and end() may reach another crypto path is only keepable if it
    // has no exceptions.
    if (sw_derive(pass, pass_len, salt, salt_len, 1, accum.b) != 0)
        return -1;
    if (iters <= 1) {
        memcpy(out, accum.b, SHA256_DIGEST);
        kiss_wipe(&accum, sizeof accum);
        return 0;
    }

    memset(k0, 0, sizeof k0);
    if (pass_len > SHA256_BLOCK) {
        if (wally_sha256(pass, pass_len, k0, SHA256_DIGEST) != WALLY_OK)
            goto out;
    } else if (pass_len) {
        memcpy(k0, pass, pass_len);
    }

    // The two midstates the loop reloads instead of recomputing. is_first_block
    // starts the engine from the SHA-256 IV.
    pq_hw_sha_begin();
    if (!pq_hw_sha_held_and_active()) { pq_hw_sha_end(); goto out; }
    for (size_t i = 0; i < SHA256_BLOCK; i++) pad.b[i] = (uint8_t)(k0[i] ^ 0x36);
    esp_sha_block(SHA2_256, pad.w, true);
    esp_sha_read_digest_state(SHA2_256, ipad.w);
    for (size_t i = 0; i < SHA256_BLOCK; i++) pad.b[i] = (uint8_t)(k0[i] ^ 0x5c);
    esp_sha_block(SHA2_256, pad.w, true);
    esp_sha_read_digest_state(SHA2_256, opad.w);
    s_compressions += 2;
    pq_hw_sha_end();

    pad96(&inner);
    pad96(&outer);
    memcpy(inner.b, accum.b, SHA256_DIGEST);

    // U_n = HMAC(K, U_n-1), accumulated by XOR. Two compressions and no copies:
    // each digest is read straight into the message block the next compression
    // consumes, whose padding tail is already there.
    for (uint32_t done = 1; done < iters; ) {
        uint32_t end = done + CHUNK_ITERS;
        if (end > iters || end < done) end = iters;
        s_compressions += (end - done) * 2u;

        pq_hw_sha_begin();
        if (!pq_hw_sha_held_and_active()) { pq_hw_sha_end(); goto out; }
        for (; done < end; done++) {
            esp_sha_write_digest_state(SHA2_256, ipad.w);
            esp_sha_block(SHA2_256, inner.w, false);
            esp_sha_read_digest_state(SHA2_256, outer.w);

            esp_sha_write_digest_state(SHA2_256, opad.w);
            esp_sha_block(SHA2_256, outer.w, false);
            esp_sha_read_digest_state(SHA2_256, inner.w);

            for (size_t j = 0; j < SHA256_DIGEST / 4; j++)
                accum.w[j] ^= inner.w[j];
        }
        pq_hw_sha_end();
    }
    memcpy(out, accum.b, SHA256_DIGEST);
    rc = 0;

out:
    kiss_wipe(k0, sizeof k0);
    kiss_wipe(&pad, sizeof pad);
    kiss_wipe(&ipad, sizeof ipad);
    kiss_wipe(&opad, sizeof opad);
    kiss_wipe(&accum, sizeof accum);
    kiss_wipe(&inner, sizeof inner);
    kiss_wipe(&outer, sizeof outer);
    return rc;
}

// A wrong key here is silent and it is not recoverable by trying again: it
// seals a backup nothing can open, or refuses one that was fine. The register
// mapping is the thing that could be wrong, and it fails by producing plausible
// bytes rather than an error. So one known answer vector goes through THIS code
// path before every real derivation, and a disagreement turns a correctness
// failure into a performance one.
//
// Per call rather than cached: no init order to get right, no thread safety
// question, and at two iterations it is four compressions and two holds
// against a hundred thousand iterations of the real thing.
static int self_check(void)
{
    // PBKDF2-HMAC-SHA256("password", "salt", c=2, dkLen=32), RFC 6070's
    // companion vector for SHA-256, regenerated from python's hashlib.
    static const uint8_t want[32] = {
        0xae,0x4d,0x0c,0x95,0xaf,0x6b,0x46,0xd3,0x2d,0x0a,0xdf,0xf9,
        0x28,0xf0,0x6d,0xd0,0x2a,0x30,0x3f,0x8e,0xf3,0xc2,0x51,0xdf,
        0xd6,0xe2,0xd8,0x5a,0x95,0x47,0x4c,0x43
    };
    uint8_t got[32];
    int ok = hw_derive((const uint8_t *)"password", 8,
                       (const uint8_t *)"salt", 4, 2, got) == 0
             && memcmp(got, want, sizeof want) == 0;
    kiss_wipe(got, sizeof got);
    return ok;
}

#endif // ESP_PLATFORM

int kiss_pbkdf2_sha256(const uint8_t *pass, size_t pass_len,
                       const uint8_t *salt, size_t salt_len,
                       uint32_t iters, uint8_t out[32])
{
    if (!pass || !salt || !out || iters == 0) return -1;
#ifdef ESP_PLATFORM
    if (self_check() && hw_derive(pass, pass_len, salt, salt_len, iters, out) == 0)
        return 0;
#endif
    return sw_derive(pass, pass_len, salt, salt_len, iters, out);
}
