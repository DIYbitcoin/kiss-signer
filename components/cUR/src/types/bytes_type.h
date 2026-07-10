#ifndef URTYPES_BYTES_TYPE_H
#define URTYPES_BYTES_TYPE_H

#include "registry.h"
#include <stddef.h>
#include <stdint.h>

// Bytes type structure
typedef struct {
  uint8_t *data;
  size_t len;
} bytes_data_t;

// Bytes registry type
extern registry_type_t BYTES_TYPE;

// Create and destroy bytes
bytes_data_t *bytes_new(const uint8_t *data, size_t len);
void bytes_free(bytes_data_t *bytes);

// Registry item interface for Bytes
registry_item_t *bytes_to_registry_item(bytes_data_t *bytes);
bytes_data_t *bytes_from_registry_item(registry_item_t *item);

// CBOR conversion functions
cbor_value_t *bytes_to_data_item(registry_item_t *item);
registry_item_t *bytes_from_data_item(cbor_value_t *data_item);

// Convenience functions
uint8_t *bytes_to_cbor(bytes_data_t *bytes, size_t *out_len);
bytes_data_t *bytes_from_cbor(const uint8_t *cbor_data, size_t len);

// Accessors
const uint8_t *bytes_get_data(bytes_data_t *bytes, size_t *out_len);

#endif // URTYPES_BYTES_TYPE_H
