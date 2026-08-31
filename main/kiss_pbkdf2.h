// PBKDF2-HMAC-SHA256, on the chip's SHA accelerator.
//
// KEF derives its key with 100,000 iterations. That is 200,000 SHA-256
// compressions and it has always run in portable C inside libwally, on the UI
// task, from the OK key of the password keyboard -- so the owner watches a
// screen that has stopped answering and is told nothing about why. It looks
// like one line of code, which is why it sat there: nothing about
// wally_pbkdf2_hmac_sha256(...) says two hundred thousand of anything.
//
// The accelerator was already here. components/slhdsa/pq_hw_sha.c holds it
// across a whole SLH-DSA verification instead of acquiring per hash, because
// acquiring costs ~2165 cycles against a held compression of ~816 and a design
// that pays it per hash goes backwards. Nothing but SLH-DSA used it. This is
// the same bracket around the other hot hash on the device.
//
// The register mapping this leans on is not new either, and it is the part
// that would fail silently: the digest state written back to SHA_H_MEM, the
// message written to SHA_M_MEM, and neither half of the HAL byteswapping on
// this target. That mapping is what verifies every firmware update, so it has
// been on real hardware since the PQ signature landed.
//
// Ported from the same author's work Kern 0.0.18 shipped (main/core/pbkdf2.c),
// which measured 8.07 s -> 0.66 s at 100,000 iterations on an ESP32-P4.
#ifndef KISS_PBKDF2_H
#define KISS_PBKDF2_H

#include <stddef.h>
#include <stdint.h>

// 32 bytes out, always: that is what wally_pbkdf2_hmac_sha256 produces and
// the only shape KEF asks for. 0 on success, -1 on bad arguments.
//
// MUST NOT be called with the SHA/AES crypto lock already held. It takes that
// lock itself and the lock is not recursive, so a second take from the same
// task hangs with no panic and no backtrace. Nothing that hashes, encrypts or
// touches NVS on the encrypted lane may sit between this call and its caller.
//
// Falls back to libwally's portable C whenever the accelerated path is not
// available or does not agree with a known answer vector, so a wrong register
// mapping costs speed and never a wrong key. On the desktop there is no
// peripheral and this IS libwally.
int kiss_pbkdf2_sha256(const uint8_t *pass, size_t pass_len,
                       const uint8_t *salt, size_t salt_len,
                       uint32_t iters, uint8_t out[32]);

// Compressions the accelerated path has performed since the last reset, so a
// bench can divide a measured cost by the work actually done. 0 on the
// desktop and 0 whenever the fallback ran.
uint32_t kiss_pbkdf2_hw_compressions(void);
void     kiss_pbkdf2_hw_reset_compressions(void);

#endif // KISS_PBKDF2_H
