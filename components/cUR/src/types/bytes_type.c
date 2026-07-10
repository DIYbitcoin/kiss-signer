#include "bytes_type.h"
#include "byte_buffer.h"
#include "cbor_decoder.h"
#include "cbor_encoder.h"
#include <string.h>

// Bytes registry type (no tag)
registry_type_t BYTES_TYPE = {.name = "bytes", .tag = 0};

// Create and destroy bytes
bytes_data_t *bytes_new(const uint8_t *data, size_t len) {
  bytes_data_t *bytes = safe_malloc(sizeof(bytes_data_t));
  if (!bytes)
    return NULL;

  if (len > 0 && data != NULL) {
    bytes->data = safe_malloc(len);
    if (!bytes->data) {
      free(bytes);
      return NULL;
    }
    memcpy(bytes->data, data, len);
    bytes->len = len;
  } else {
    // Handle empty bytes case
    bytes->data = NULL;
    bytes->len = 0;
  }

  return bytes;
}

void bytes_free(bytes_data_t *bytes) {
  if (!bytes)
    return;

  free(bytes->data);
  free(bytes);
}

// CBOR conversion functions
cbor_value_t *bytes_to_data_item(registry_item_t *item) {
  if (!item || !item->data)
    return NULL;

  bytes_data_t *bytes = (bytes_data_t *)item->data;
  return cbor_value_new_bytes(bytes->data, bytes->len);
}

registry_item_t *bytes_from_data_item(cbor_value_t *data_item) {
  if (!data_item)
    return NULL;

  // Handle both plain bytes and tagged bytes
  cbor_value_t *bytes_val = data_item;
  if (cbor_value_get_type(data_item) == CBOR_TYPE_TAG) {
    bytes_val = cbor_value_get_tag_content(data_item);
  }

  cbor_type_t type = cbor_value_get_type(bytes_val);

  // Accept both byte strings (type 2) and text strings (type 3)
  if (type != CBOR_TYPE_BYTES && type != CBOR_TYPE_STRING)
    return NULL;

  size_t len;
  const uint8_t *data;

  if (type == CBOR_TYPE_STRING) {
    // Handle text string (type 3)
    const char *str = cbor_value_get_string(bytes_val);
    if (!str) {
      // Empty string case
      len = 0;
      data = NULL;
    } else {
      len = strlen(str);
      data = (const uint8_t *)str;
    }
  } else {
    // Handle byte string (type 2)
    data = cbor_value_get_bytes(bytes_val, &len);
  }

  // Allow NULL data if len is 0 (empty bytes)
  if (!data && len > 0)
    return NULL;

  bytes_data_t *bytes = bytes_new(data, len);
  if (!bytes)
    return NULL;

  return bytes_to_registry_item(bytes);
}

// Registry item interface for Bytes
registry_item_t *bytes_to_registry_item(bytes_data_t *bytes) {
  return registry_item_new(&BYTES_TYPE, bytes, bytes_to_data_item,
                           bytes_from_data_item);
}

bytes_data_t *bytes_from_registry_item(registry_item_t *item) {
  if (!item || item->type != &BYTES_TYPE)
    return NULL;
  return (bytes_data_t *)item->data;
}

// Convenience functions
uint8_t *bytes_to_cbor(bytes_data_t *bytes, size_t *out_len) {
  if (!bytes || !out_len)
    return NULL;
  // Stack-allocated wrapper — registry_item_to_cbor only reads the item
  // through the vtable, never retains it.
  registry_item_t item = {.type = &BYTES_TYPE,
                          .data = bytes,
                          .to_data_item = bytes_to_data_item,
                          .from_data_item = NULL,
                          .free_item = NULL};
  return registry_item_to_cbor(&item, out_len);
}

bytes_data_t *bytes_from_cbor(const uint8_t *cbor_data, size_t len) {
  return (bytes_data_t *)registry_item_unwrap_from_cbor(cbor_data, len,
                                                        bytes_from_data_item);
}

// Accessors
const uint8_t *bytes_get_data(bytes_data_t *bytes, size_t *out_len) {
  if (!bytes || !out_len)
    return NULL;

  *out_len = bytes->len;
  return bytes->data;
}
