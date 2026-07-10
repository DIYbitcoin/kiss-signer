#ifndef UR_SHA256_COMPAT_H
#define UR_SHA256_COMPAT_H

#include <stddef.h>
#include <stdint.h>

// Pick a SHA256 backend based on a compile-time flag.
//   UR_USE_MBEDTLS_SHA256 -> mbedtls_sha256  (e.g. ESP-IDF)
//   UR_USE_K210_SHA256    -> sha256_hard_calculate (Kendryte K210 SDK)
// Otherwise, fall back to the bundled implementation in sha256/sha256.c.
#if defined(UR_USE_MBEDTLS_SHA256)

#include <mbedtls/sha256.h>

static inline void ur_sha256(const uint8_t *input, size_t len,
                             uint8_t output[32]) {
  mbedtls_sha256(input, len, output, 0);
}

#elif defined(UR_USE_K210_SHA256)

#include <sha256.h>

static inline void ur_sha256(const uint8_t *input, size_t len,
                             uint8_t output[32]) {
  sha256_hard_calculate(input, len, output);
}

#else

#include "sha256/sha256.h"

static inline void ur_sha256(const uint8_t *input, size_t len,
                             uint8_t output[32]) {
  CRYAL_SHA256_CTX ctx;
  ur_bundled_sha256_init(&ctx);
  ur_bundled_sha256_update(&ctx, input, len);
  ur_bundled_sha256_final(&ctx, output);
}

#endif

#endif
