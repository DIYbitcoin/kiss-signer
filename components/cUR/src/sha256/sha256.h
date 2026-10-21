/*********************************************************************
 * Filename:   sha256.h
 * Author:     Brad Conte (brad AT bradconte.com)
 * Copyright:
 * Disclaimer: This code is presented "as is" without any guarantees.
 * Details:    Defines the API for the corresponding SHA1 implementation.
 *********************************************************************/

#ifndef SHA256_H
#define SHA256_H

/*************************** HEADER FILES ***************************/
#include <stddef.h>

/****************************** MACROS ******************************/
#define SHA256_BLOCK_SIZE 32 // SHA256 outputs a 32 byte digest

/**************************** DATA TYPES ****************************/
// Namespace the types too: FatFs defines WORD as a 16-bit integer.
typedef unsigned char UR_SHA256_BYTE; // 8-bit byte
typedef unsigned int UR_SHA256_WORD;  // 32-bit word, change to "long" for 16-bit machines

typedef struct {
  UR_SHA256_BYTE data[64];
  UR_SHA256_WORD datalen;
  unsigned long long bitlen;
  UR_SHA256_WORD state[8];
} CRYAL_SHA256_CTX;

/*********************** FUNCTION DECLARATIONS **********************/
// Namespaced to avoid link-time collisions with platform SDKs that
// export unprefixed sha256_init / sha256_update / sha256_final symbols
// with different signatures (e.g. Kendryte K210 SDK, mbedTLS).
void ur_bundled_sha256_init(CRYAL_SHA256_CTX *ctx);
void ur_bundled_sha256_update(CRYAL_SHA256_CTX *ctx, const UR_SHA256_BYTE data[],
                              size_t len);
void ur_bundled_sha256_final(CRYAL_SHA256_CTX *ctx, UR_SHA256_BYTE hash[]);

#endif // SHA256_H
