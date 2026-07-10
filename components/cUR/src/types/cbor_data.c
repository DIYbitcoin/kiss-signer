#include "cbor_data.h"
#include <stdlib.h>
#include <string.h>

// CBOR value creation functions
cbor_value_t *cbor_value_new_unsigned_int(uint64_t val) {
  cbor_value_t *v = safe_malloc(sizeof(cbor_value_t));
  if (!v)
    return NULL;

  v->type = CBOR_TYPE_UNSIGNED_INT;
  v->value.uint_val = val;
  return v;
}

cbor_value_t *cbor_value_new_bytes(const uint8_t *data, size_t len) {
  cbor_value_t *v = safe_malloc(sizeof(cbor_value_t));
  if (!v)
    return NULL;

  v->type = CBOR_TYPE_BYTES;

  if (len > 0 && data != NULL) {
    v->value.bytes_val.data = safe_malloc(len);
    if (!v->value.bytes_val.data) {
      free(v);
      return NULL;
    }
    memcpy(v->value.bytes_val.data, data, len);
    v->value.bytes_val.len = len;
  } else {
    v->value.bytes_val.data = NULL;
    v->value.bytes_val.len = 0;
  }

  return v;
}

cbor_value_t *cbor_value_new_string(const char *str) {
  if (!str)
    return NULL;

  cbor_value_t *v = safe_malloc(sizeof(cbor_value_t));
  if (!v)
    return NULL;

  v->type = CBOR_TYPE_STRING;
  v->value.string_val = safe_strdup(str);
  if (!v->value.string_val) {
    free(v);
    return NULL;
  }

  return v;
}

cbor_value_t *cbor_value_new_array(void) {
  cbor_value_t *v = safe_malloc(sizeof(cbor_value_t));
  if (!v)
    return NULL;

  v->type = CBOR_TYPE_ARRAY;
  v->value.array_val.items = NULL;
  v->value.array_val.count = 0;
  return v;
}

cbor_value_t *cbor_value_new_map(void) {
  cbor_value_t *v = safe_malloc(sizeof(cbor_value_t));
  if (!v)
    return NULL;

  v->type = CBOR_TYPE_MAP;
  v->value.map_val.keys = NULL;
  v->value.map_val.values = NULL;
  v->value.map_val.count = 0;
  return v;
}

cbor_value_t *cbor_value_new_tag(uint64_t tag, cbor_value_t *content) {
  if (!content)
    return NULL;

  cbor_value_t *v = safe_malloc(sizeof(cbor_value_t));
  if (!v)
    return NULL;

  v->type = CBOR_TYPE_TAG;
  v->value.tag_val.tag = tag;
  v->value.tag_val.content = content;
  return v;
}

cbor_value_t *cbor_value_new_bool(bool val) {
  cbor_value_t *v = safe_malloc(sizeof(cbor_value_t));
  if (!v)
    return NULL;

  v->type = CBOR_TYPE_BOOL;
  v->value.bool_val = val;
  return v;
}

// CBOR value manipulation
bool cbor_array_append(cbor_value_t *array, cbor_value_t *item) {
  if (!array || array->type != CBOR_TYPE_ARRAY || !item)
    return false;

  size_t new_count = array->value.array_val.count + 1;
  cbor_value_t **new_items = safe_realloc(array->value.array_val.items,
                                          new_count * sizeof(cbor_value_t *));
  if (!new_items)
    return false;

  new_items[array->value.array_val.count] = item;
  array->value.array_val.items = new_items;
  array->value.array_val.count = new_count;
  return true;
}

bool cbor_map_set(cbor_value_t *map, cbor_value_t *key, cbor_value_t *value) {
  if (!map || map->type != CBOR_TYPE_MAP || !key || !value)
    return false;

  // Check if key already exists (only unsigned int keys used in Bitcoin URs)
  for (size_t i = 0; i < map->value.map_val.count; i++) {
    if (map->value.map_val.keys[i]->type == CBOR_TYPE_UNSIGNED_INT &&
        key->type == CBOR_TYPE_UNSIGNED_INT &&
        map->value.map_val.keys[i]->value.uint_val == key->value.uint_val) {
      // Update existing value
      cbor_value_free(map->value.map_val.values[i]);
      map->value.map_val.values[i] = value;
      cbor_value_free(key);
      return true;
    }
  }

  // Add new key-value pair. Commit each realloc to the struct immediately
  // so a later failure never leaves the struct pointing at a freed block.
  size_t new_count = map->value.map_val.count + 1;
  cbor_value_t **new_keys =
      safe_realloc(map->value.map_val.keys, new_count * sizeof(cbor_value_t *));
  if (!new_keys)
    return false;
  map->value.map_val.keys = new_keys;

  cbor_value_t **new_values = safe_realloc(map->value.map_val.values,
                                           new_count * sizeof(cbor_value_t *));
  if (!new_values)
    return false;
  map->value.map_val.values = new_values;

  new_keys[map->value.map_val.count] = key;
  new_values[map->value.map_val.count] = value;
  map->value.map_val.count = new_count;
  return true;
}

cbor_value_t *cbor_map_get(cbor_value_t *map, cbor_value_t *key) {
  if (!map || map->type != CBOR_TYPE_MAP || !key)
    return NULL;

  // Only unsigned int keys used in Bitcoin URs
  if (key->type != CBOR_TYPE_UNSIGNED_INT)
    return NULL;

  for (size_t i = 0; i < map->value.map_val.count; i++) {
    if (map->value.map_val.keys[i]->type == CBOR_TYPE_UNSIGNED_INT &&
        map->value.map_val.keys[i]->value.uint_val == key->value.uint_val) {
      return map->value.map_val.values[i];
    }
  }

  return NULL;
}

cbor_value_t *cbor_map_get_int(cbor_value_t *map, int64_t key) {
  if (key < 0)
    return NULL; // Negative keys not used

  cbor_value_t *key_val = cbor_value_new_unsigned_int((uint64_t)key);
  if (!key_val)
    return NULL;

  cbor_value_t *result = cbor_map_get(map, key_val);
  cbor_value_free(key_val);
  return result;
}

// CBOR value accessors
cbor_type_t cbor_value_get_type(cbor_value_t *val) {
  if (!val)
    return CBOR_TYPE_UNSIGNED_INT; // Safe default
  return val->type;
}

uint64_t cbor_value_get_uint(cbor_value_t *val) {
  if (!val || val->type != CBOR_TYPE_UNSIGNED_INT)
    return 0;
  return val->value.uint_val;
}

const uint8_t *cbor_value_get_bytes(cbor_value_t *val, size_t *out_len) {
  if (!val || val->type != CBOR_TYPE_BYTES || !out_len)
    return NULL;
  *out_len = val->value.bytes_val.len;
  return val->value.bytes_val.data;
}

const char *cbor_value_get_string(cbor_value_t *val) {
  if (!val || val->type != CBOR_TYPE_STRING)
    return NULL;
  return val->value.string_val;
}

size_t cbor_value_get_array_size(cbor_value_t *val) {
  if (!val || val->type != CBOR_TYPE_ARRAY)
    return 0;
  return val->value.array_val.count;
}

cbor_value_t *cbor_value_get_array_item(cbor_value_t *val, size_t index) {
  if (!val || val->type != CBOR_TYPE_ARRAY ||
      index >= val->value.array_val.count) {
    return NULL;
  }
  return val->value.array_val.items[index];
}

bool cbor_value_get_bool(cbor_value_t *val) {
  if (!val || val->type != CBOR_TYPE_BOOL)
    return false;
  return val->value.bool_val;
}

uint64_t cbor_value_get_tag(cbor_value_t *val) {
  if (!val || val->type != CBOR_TYPE_TAG)
    return 0;
  return val->value.tag_val.tag;
}

cbor_value_t *cbor_value_get_tag_content(cbor_value_t *val) {
  if (!val || val->type != CBOR_TYPE_TAG)
    return NULL;
  return val->value.tag_val.content;
}

// CBOR value cleanup
void cbor_value_free(cbor_value_t *val) {
  if (!val)
    return;

  switch (val->type) {
  case CBOR_TYPE_BYTES:
    free(val->value.bytes_val.data);
    break;
  case CBOR_TYPE_STRING:
    free(val->value.string_val);
    break;
  case CBOR_TYPE_ARRAY:
    for (size_t i = 0; i < val->value.array_val.count; i++) {
      cbor_value_free(val->value.array_val.items[i]);
    }
    free(val->value.array_val.items);
    break;
  case CBOR_TYPE_MAP:
    for (size_t i = 0; i < val->value.map_val.count; i++) {
      cbor_value_free(val->value.map_val.keys[i]);
      cbor_value_free(val->value.map_val.values[i]);
    }
    free(val->value.map_val.keys);
    free(val->value.map_val.values);
    break;
  case CBOR_TYPE_TAG:
    cbor_value_free(val->value.tag_val.content);
    break;
  default:
    break;
  }

  free(val);
}
