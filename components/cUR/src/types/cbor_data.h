#ifndef URTYPES_CBOR_DATA_H
#define URTYPES_CBOR_DATA_H

#include "byte_buffer.h"
#include <stdbool.h>
#include <stdint.h>

// CBOR value types (subset used by Bitcoin UR types)
typedef enum {
  CBOR_TYPE_UNSIGNED_INT,
  CBOR_TYPE_BYTES,
  CBOR_TYPE_STRING,
  CBOR_TYPE_ARRAY,
  CBOR_TYPE_MAP,
  CBOR_TYPE_TAG,
  CBOR_TYPE_BOOL
} cbor_type_t;

// Forward declarations
typedef struct cbor_value cbor_value_t;

// CBOR value structure
struct cbor_value {
  cbor_type_t type;
  union {
    uint64_t uint_val;
    struct {
      uint8_t *data;
      size_t len;
    } bytes_val;
    char *string_val;
    struct {
      cbor_value_t **items;
      size_t count;
    } array_val;
    struct {
      cbor_value_t **keys;
      cbor_value_t **values;
      size_t count;
    } map_val;
    struct {
      uint64_t tag;
      cbor_value_t *content;
    } tag_val;
    bool bool_val;
  } value;
};

// CBOR value creation functions
cbor_value_t *cbor_value_new_unsigned_int(uint64_t val);
cbor_value_t *cbor_value_new_bytes(const uint8_t *data, size_t len);
cbor_value_t *cbor_value_new_string(const char *str);
cbor_value_t *cbor_value_new_array(void);
cbor_value_t *cbor_value_new_map(void);
cbor_value_t *cbor_value_new_tag(uint64_t tag, cbor_value_t *content);
cbor_value_t *cbor_value_new_bool(bool val);

// CBOR value manipulation
bool cbor_array_append(cbor_value_t *array, cbor_value_t *item);
bool cbor_map_set(cbor_value_t *map, cbor_value_t *key, cbor_value_t *value);
cbor_value_t *cbor_map_get(cbor_value_t *map, cbor_value_t *key);
cbor_value_t *cbor_map_get_int(cbor_value_t *map, int64_t key);

// CBOR value accessors
cbor_type_t cbor_value_get_type(cbor_value_t *val);
uint64_t cbor_value_get_uint(cbor_value_t *val);
const uint8_t *cbor_value_get_bytes(cbor_value_t *val, size_t *out_len);
const char *cbor_value_get_string(cbor_value_t *val);
size_t cbor_value_get_array_size(cbor_value_t *val);
cbor_value_t *cbor_value_get_array_item(cbor_value_t *val, size_t index);
bool cbor_value_get_bool(cbor_value_t *val);
uint64_t cbor_value_get_tag(cbor_value_t *val);
cbor_value_t *cbor_value_get_tag_content(cbor_value_t *val);

// CBOR value cleanup
void cbor_value_free(cbor_value_t *val);

// Data item (tagged CBOR value) - convenience type
typedef cbor_value_t cbor_data_item_t;

#define cbor_data_item_new(tag, content) cbor_value_new_tag(tag, content)
#define cbor_data_item_free(item) cbor_value_free(item)
#define cbor_data_item_get_tag(item) cbor_value_get_tag(item)
#define cbor_data_item_get_content(item) cbor_value_get_tag_content(item)

#endif // URTYPES_CBOR_DATA_H
