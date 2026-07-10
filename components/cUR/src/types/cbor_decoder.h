#ifndef URTYPES_CBOR_DECODER_H
#define URTYPES_CBOR_DECODER_H

#include "cbor_data.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// CBOR decoder (prefixed to avoid conflicts with bc-ur)
typedef struct {
  const uint8_t *data;
  size_t len;
  size_t offset;
} urtypes_cbor_decoder_t;

// Create and destroy decoder
urtypes_cbor_decoder_t *urtypes_cbor_decoder_new(const uint8_t *data,
                                                 size_t len);
void urtypes_cbor_decoder_free(urtypes_cbor_decoder_t *decoder);

// Decode CBOR value
cbor_value_t *urtypes_cbor_decoder_decode(urtypes_cbor_decoder_t *decoder);

// Wrapper macros
#define cbor_decoder_t urtypes_cbor_decoder_t
#define cbor_decoder_new urtypes_cbor_decoder_new
#define cbor_decoder_free urtypes_cbor_decoder_free
#define cbor_decoder_decode urtypes_cbor_decoder_decode

// Convenience function to decode bytes to a value
cbor_value_t *cbor_decode(const uint8_t *data, size_t len);

#endif // URTYPES_CBOR_DECODER_H
