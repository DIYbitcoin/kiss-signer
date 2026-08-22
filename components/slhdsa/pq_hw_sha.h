// SHA-256 for SLH-DSA, on the chip's accelerator, held.
//
// Every hash based signature is SHA-256 and almost nothing else: one
// SLH-DSA-SHA2-128s signature is tens of thousands of compressions over inputs
// of a few dozen bytes each. The P4 has an accelerator for it, and the obvious
// way to use it does not pay -- acquiring the peripheral takes the crypto lock,
// enables a bus clock and resets the block, measured on an S3 at ~2165 cycles
// against a held compression of ~816. A design that acquires per hash throws
// most of the benefit away and some of it goes backwards.
//
// So the two concerns are separate. pq_hw_sha_begin()/end() take the peripheral
// once for a whole operation and nest; sha2_256_compress() then does nothing per
// hash but move 32 words in and 8 out.
//
// This works because the P4 sets SOC_SHA_SUPPORT_RESUME, so SHA_H_MEM is
// writable and an arbitrary midstate can be loaded -- which SLH-DSA needs on
// every hash, since they all start from a cached pk_seed midstate. And because
// slhdsa-c keeps its chaining state and message block adjacent and already in
// big endian order, its context maps straight onto SHA_H_MEM + SHA_M_MEM with no
// marshalling at all: neither sha_ll_write_digest() nor sha_ll_fill_text_block()
// byteswaps on this target. The 24 rev8_be32() calls the software compression
// performs disappear along with the compression.
//
// Ported from odudex's work on Blockstream Research's hash_based_signatures
// branch of Jade -- DmitriiKJ/Jade#1, "held-hardware accelerated SHA-256" --
// which measured 48583 ms -> 10971 ms for one SLH-DSA-SHA2-128s signature on an
// ESP32-S3 at 240 MHz. The idea below is that PR's; what is new here is the P4,
// which runs at 400 MHz and whose HAL does not byteswap either half of the
// mapping, and the direction, which is verifying rather than signing.
#ifndef PQ_HW_SHA_H
#define PQ_HW_SHA_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Take the peripheral for a whole operation. Nests: only the outermost pair
// acquires and releases, so a caller cannot break an inner one by bracketing
// around it. Every entry point in this component brackets itself, so no caller
// has to remember.
void pq_hw_sha_begin(void);
void pq_hw_sha_end(void);

// Hand the peripheral back without unwinding the bracket, and take it again.
//
// This is not an optimisation, it is the only way a callback is survivable. The
// crypto lock is a mutex and it is NOT reentrant, and it is shared with AES --
// so anything reached from inside a held bracket that hashes or decrypts, the
// UI, NVS on the encrypted lane, an OTA write, deadlocks the task with no panic,
// no backtrace and no watchdog to explain it. Suspend across the callback.
void pq_hw_sha_suspend(void);
void pq_hw_sha_resume(void);

// True while an outer bracket is open AND the peripheral is actually in hand.
// The hardware path is guarded on this, because driving an unclocked SHA engine
// does not fail quietly: the digest reads back as all zeroes and ESP-IDF's own
// fault injection check calls abort(). A missed bracket has to cost speed, not
// a panic, on a device that is holding somebody's keys.
bool pq_hw_sha_held_and_active(void);

// Force the portable C path even inside a bracket. The point is not speed, it
// is that the two paths can be run over the same input and compared: a
// signature that is byte identical either way is the only evidence the register
// mapping above is right. sim/test_pq.c does exactly that, and so does the
// on-device selftest, where the hardware path is the only one that exists.
//
// Only call this OUTSIDE a bracket -- it is read by begin(), so flipping it
// mid-operation would leave the hold depth unbalanced.
void pq_hw_sha_force_sw(bool on);

// Compressions since the peripheral was last handed back. The counter behind
// the periodic yield, exposed because a benchmark wants to divide cost per
// operation into cost per compression.
uint32_t pq_hw_sha_compressions(void);
void pq_hw_sha_reset_compressions(void);

// slhdsa-c's portable compression, under the name UPSTREAM.md explains.
void sha2_256_compress_sw(void *v);

#ifdef __cplusplus
}
#endif

#endif // PQ_HW_SHA_H
