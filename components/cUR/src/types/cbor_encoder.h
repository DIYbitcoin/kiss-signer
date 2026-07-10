#ifndef URTYPES_CBOR_ENCODER_H
#define URTYPES_CBOR_ENCODER_H

#include "byte_buffer.h"
#include "cbor_data.h"
#include <stddef.h>
#include <stdint.h>

// CBOR encoder (prefixed to avoid conflicts with bc-ur)
typedef struct {
  byte_buffer_t *buffer;
} urtypes_cbor_encoder_t;

// Create and destroy encoder
urtypes_cbor_encoder_t *urtypes_cbor_encoder_new(void);
void urtypes_cbor_encoder_free(urtypes_cbor_encoder_t *encoder);

// Encode CBOR value
bool urtypes_cbor_encoder_encode(urtypes_cbor_encoder_t *encoder,
                                 cbor_value_t *value);

// Get encoded data
uint8_t *urtypes_cbor_encoder_get_data(urtypes_cbor_encoder_t *encoder,
                                       size_t *out_len);

// Wrapper macros
#define cbor_encoder_t urtypes_cbor_encoder_t
#define cbor_encoder_new urtypes_cbor_encoder_new
#define cbor_encoder_free urtypes_cbor_encoder_free
#define cbor_encoder_encode urtypes_cbor_encoder_encode
#define cbor_encoder_get_data urtypes_cbor_encoder_get_data

// Convenience function to encode a value to bytes
uint8_t *cbor_encode(cbor_value_t *value, size_t *out_len);

#endif // URTYPES_CBOR_ENCODER_H
