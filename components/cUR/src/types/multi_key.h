#ifndef URTYPES_MULTI_KEY_H
#define URTYPES_MULTI_KEY_H

#include "hd_key.h"
#include "registry.h"
#include <stddef.h>
#include <stdint.h>

// MultiKey type structure (no tag, embedded in other types)
typedef struct {
  uint32_t threshold;      // M in M-of-N multisig
  hd_key_data_t **hd_keys; // Array of HD keys
  size_t hd_key_count;
  size_t hd_key_capacity; // >= hd_key_count; grows in doubling steps
} multi_key_data_t;

// Create and destroy MultiKey
multi_key_data_t *multi_key_new(uint32_t threshold);
void multi_key_free(multi_key_data_t *multi_key);

// CBOR conversion functions
multi_key_data_t *multi_key_from_data_item(cbor_value_t *data_item);
cbor_value_t *multi_key_to_data_item(multi_key_data_t *multi_key);

// Add keys to MultiKey
bool multi_key_add_hd_key(multi_key_data_t *multi_key, hd_key_data_t *hd_key);

#endif // URTYPES_MULTI_KEY_H
