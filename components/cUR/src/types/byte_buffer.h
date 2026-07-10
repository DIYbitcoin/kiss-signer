#ifndef URTYPES_BYTE_BUFFER_H
#define URTYPES_BYTE_BUFFER_H

#include "../utils.h" // Use root utils for memory management (safe_malloc, safe_realloc, safe_strdup)
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h> // For free()

// Byte buffer utilities
typedef struct {
  uint8_t *data;
  size_t len;
  size_t capacity;
} byte_buffer_t;

byte_buffer_t *byte_buffer_new(void);
byte_buffer_t *byte_buffer_new_with_capacity(size_t capacity);
void byte_buffer_free(byte_buffer_t *buf);
bool byte_buffer_append(byte_buffer_t *buf, const uint8_t *data, size_t len);
uint8_t *byte_buffer_get_data(byte_buffer_t *buf);
size_t byte_buffer_get_len(byte_buffer_t *buf);

// Inlined here so toolchains that don't enable LTO (notably the K210
// MaixPy build) don't pay the call overhead on the CBOR hot path.
static inline bool byte_buffer_append_byte(byte_buffer_t *buf, uint8_t byte) {
  return byte_buffer_append(buf, &byte, 1);
}

// Base58 encoding utilities
char *base58_encode(const uint8_t *data, size_t len);
char *base58check_encode(const uint8_t *data, size_t len);

#endif // URTYPES_BYTE_BUFFER_H
