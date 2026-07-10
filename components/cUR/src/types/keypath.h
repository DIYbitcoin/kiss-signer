#ifndef URTYPES_KEYPATH_H
#define URTYPES_KEYPATH_H

#include "registry.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Keypath tag number
#define CRYPTO_KEYPATH_TAG 304

// Path component structure
typedef struct {
  uint32_t index; // Path index (ignored if wildcard is true)
  bool hardened;  // Whether this component is hardened
  bool wildcard;  // Whether this is a wildcard component (*)
} path_component_t;

// Keypath type structure
typedef struct {
  path_component_t *components;
  size_t component_count;
  uint8_t *source_fingerprint; // 4 bytes, optional (can be NULL)
  int depth;                   // -1 means not set
} keypath_data_t;

// Keypath registry type
extern registry_type_t KEYPATH_TYPE;

// Create and destroy Keypath
keypath_data_t *keypath_new(path_component_t *components,
                            size_t component_count,
                            const uint8_t *source_fingerprint, int depth);
void keypath_free(keypath_data_t *keypath);

// Registry item interface for Keypath
registry_item_t *keypath_to_registry_item(keypath_data_t *keypath);
keypath_data_t *keypath_from_registry_item(registry_item_t *item);

// CBOR conversion functions
registry_item_t *keypath_from_data_item(cbor_value_t *data_item);
cbor_value_t *keypath_to_data_item(keypath_data_t *keypath);

// Helper to generate path string (e.g., "44'/0'/0'", "1/0/*")
char *keypath_to_string(keypath_data_t *keypath);

#endif // URTYPES_KEYPATH_H
