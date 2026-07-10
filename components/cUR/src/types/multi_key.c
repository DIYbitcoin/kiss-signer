#include "multi_key.h"
#include "byte_buffer.h"
#include "cbor_decoder.h"
#include <string.h>

// Create and destroy MultiKey
multi_key_data_t *multi_key_new(uint32_t threshold) {
  multi_key_data_t *multi_key = safe_malloc(sizeof(multi_key_data_t));
  if (!multi_key)
    return NULL;

  multi_key->threshold = threshold;
  multi_key->hd_keys = NULL;
  multi_key->hd_key_count = 0;
  multi_key->hd_key_capacity = 0;

  return multi_key;
}

void multi_key_free(multi_key_data_t *multi_key) {
  if (!multi_key)
    return;

  if (multi_key->hd_keys) {
    for (size_t i = 0; i < multi_key->hd_key_count; i++) {
      hd_key_free(multi_key->hd_keys[i]);
    }
    free(multi_key->hd_keys);
  }

  free(multi_key);
}

bool multi_key_add_hd_key(multi_key_data_t *multi_key, hd_key_data_t *hd_key) {
  if (!multi_key || !hd_key)
    return false;

  // Grow capacity in doubling steps (4, 8, 16, ...) so N-of-N multisigs
  // don't trigger N reallocations in a row.
  if (multi_key->hd_key_count >= multi_key->hd_key_capacity) {
    size_t new_capacity =
        multi_key->hd_key_capacity == 0 ? 4 : multi_key->hd_key_capacity * 2;
    hd_key_data_t **new_keys = safe_realloc(
        multi_key->hd_keys, new_capacity * sizeof(hd_key_data_t *));
    if (!new_keys)
      return false;
    multi_key->hd_keys = new_keys;
    multi_key->hd_key_capacity = new_capacity;
  }

  multi_key->hd_keys[multi_key->hd_key_count++] = hd_key;
  return true;
}

cbor_value_t *multi_key_to_data_item(multi_key_data_t *multi_key) {
  if (!multi_key)
    return NULL;

  cbor_value_t *map = cbor_value_new_map();
  if (!map)
    return NULL;

  cbor_map_set(map, cbor_value_new_unsigned_int(1),
               cbor_value_new_unsigned_int(multi_key->threshold));

  cbor_value_t *arr = cbor_value_new_array();
  if (!arr) {
    cbor_value_free(map);
    return NULL;
  }
  for (size_t i = 0; i < multi_key->hd_key_count; i++) {
    cbor_value_t *m = hd_key_to_data_item(multi_key->hd_keys[i]);
    if (m)
      cbor_array_append(arr, cbor_value_new_tag(CRYPTO_HDKEY_TAG, m));
  }
  cbor_map_set(map, cbor_value_new_unsigned_int(2), arr);

  return map;
}

// Parse MultiKey from CBOR data item
multi_key_data_t *multi_key_from_data_item(cbor_value_t *data_item) {
  if (!data_item || cbor_value_get_type(data_item) != CBOR_TYPE_MAP) {
    return NULL;
  }

  // Get threshold (key 1, required)
  cbor_value_t *threshold_val = get_map_value(data_item, 1);
  if (!threshold_val ||
      cbor_value_get_type(threshold_val) != CBOR_TYPE_UNSIGNED_INT) {
    return NULL;
  }
  uint32_t threshold = (uint32_t)cbor_value_get_uint(threshold_val);

  multi_key_data_t *multi_key = multi_key_new(threshold);
  if (!multi_key)
    return NULL;

  // Get keys array (key 2, required)
  cbor_value_t *keys_val = get_map_value(data_item, 2);
  if (!keys_val || cbor_value_get_type(keys_val) != CBOR_TYPE_ARRAY) {
    multi_key_free(multi_key);
    return NULL;
  }

  size_t key_count = cbor_value_get_array_size(keys_val);
  for (size_t i = 0; i < key_count; i++) {
    cbor_value_t *key_item = cbor_value_get_array_item(keys_val, i);
    if (!key_item)
      continue;

    // Check if this is a tagged item
    if (cbor_value_get_type(key_item) != CBOR_TYPE_TAG)
      continue;

    uint64_t tag = cbor_value_get_tag(key_item);
    cbor_value_t *key_content = cbor_value_get_tag_content(key_item);

    if (tag == CRYPTO_HDKEY_TAG) {
      // HDKey - unwrap the tag to get the map content
      registry_item_t *item = hd_key_from_data_item(key_content);
      if (item) {
        hd_key_data_t *hd_key = hd_key_from_registry_item(item);
        if (hd_key) {
          item->data = NULL; // Transfer ownership
          multi_key_add_hd_key(multi_key, hd_key);
        }
        free(item);
      }
    }
    // Other key types (e.g., EC keys) are ignored
  }

  return multi_key;
}
