#include "keypath.h"
#include "byte_buffer.h"
#include "cbor_decoder.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// Keypath registry type (tag 304)
registry_type_t KEYPATH_TYPE = {.name = "crypto-keypath",
                                .tag = CRYPTO_KEYPATH_TAG};

// Create and destroy Keypath
keypath_data_t *keypath_new(path_component_t *components,
                            size_t component_count,
                            const uint8_t *source_fingerprint, int depth) {
  keypath_data_t *keypath = safe_malloc(sizeof(keypath_data_t));
  if (!keypath)
    return NULL;

  keypath->component_count = component_count;
  keypath->depth = depth;

  // Copy components
  if (component_count > 0 && components) {
    keypath->components =
        safe_malloc(component_count * sizeof(path_component_t));
    if (!keypath->components) {
      free(keypath);
      return NULL;
    }
    memcpy(keypath->components, components,
           component_count * sizeof(path_component_t));
  } else {
    keypath->components = NULL;
  }

  // Copy source fingerprint (4 bytes)
  if (source_fingerprint) {
    keypath->source_fingerprint = safe_malloc(4);
    if (!keypath->source_fingerprint) {
      free(keypath->components);
      free(keypath);
      return NULL;
    }
    memcpy(keypath->source_fingerprint, source_fingerprint, 4);
  } else {
    keypath->source_fingerprint = NULL;
  }

  return keypath;
}

void keypath_free(keypath_data_t *keypath) {
  if (!keypath)
    return;
  free(keypath->components);
  free(keypath->source_fingerprint);
  free(keypath);
}

// Parse Keypath from CBOR data item
registry_item_t *keypath_from_data_item(cbor_value_t *data_item) {
  if (!data_item)
    return NULL;

  // Keypath is a map
  if (cbor_value_get_type(data_item) != CBOR_TYPE_MAP)
    return NULL;

  // Get components array (key 1)
  cbor_value_t *components_val = get_map_value(data_item, 1);
  if (!components_val ||
      cbor_value_get_type(components_val) != CBOR_TYPE_ARRAY) {
    return NULL;
  }

  size_t array_size = cbor_value_get_array_size(components_val);
  if (array_size % 2 != 0)
    return NULL; // Must be pairs of (index/wildcard, hardened)

  size_t component_count = array_size / 2;
  path_component_t *components = NULL;

  if (component_count > 0) {
    components = safe_malloc(component_count * sizeof(path_component_t));
    if (!components)
      return NULL;

    for (size_t i = 0; i < component_count; i++) {
      cbor_value_t *index_val =
          cbor_value_get_array_item(components_val, i * 2);
      cbor_value_t *hardened_val =
          cbor_value_get_array_item(components_val, i * 2 + 1);

      if (!hardened_val ||
          cbor_value_get_type(hardened_val) != CBOR_TYPE_BOOL) {
        free(components);
        return NULL;
      }

      bool hardened = cbor_value_get_bool(hardened_val);

      // Check if wildcard (empty array) or index (unsigned int)
      if (cbor_value_get_type(index_val) == CBOR_TYPE_ARRAY) {
        // Wildcard component
        components[i].wildcard = true;
        components[i].index = 0;
        components[i].hardened = hardened;
      } else if (cbor_value_get_type(index_val) == CBOR_TYPE_UNSIGNED_INT) {
        // Regular index
        components[i].wildcard = false;
        components[i].index = (uint32_t)cbor_value_get_uint(index_val);
        components[i].hardened = hardened;
      } else {
        free(components);
        return NULL;
      }
    }
  }

  // Get source_fingerprint (key 2, optional)
  uint8_t *source_fingerprint = NULL;
  cbor_value_t *fingerprint_val = get_map_value(data_item, 2);
  if (fingerprint_val &&
      cbor_value_get_type(fingerprint_val) == CBOR_TYPE_UNSIGNED_INT) {
    uint32_t fp_int = (uint32_t)cbor_value_get_uint(fingerprint_val);
    source_fingerprint = safe_malloc(4);
    if (source_fingerprint) {
      // Big-endian encoding
      source_fingerprint[0] = (fp_int >> 24) & 0xFF;
      source_fingerprint[1] = (fp_int >> 16) & 0xFF;
      source_fingerprint[2] = (fp_int >> 8) & 0xFF;
      source_fingerprint[3] = fp_int & 0xFF;
    }
  }

  // Get depth (key 3, optional)
  int depth = -1;
  cbor_value_t *depth_val = get_map_value(data_item, 3);
  if (depth_val && cbor_value_get_type(depth_val) == CBOR_TYPE_UNSIGNED_INT) {
    depth = (int)cbor_value_get_uint(depth_val);
  }

  keypath_data_t *keypath =
      keypath_new(components, component_count, source_fingerprint, depth);
  free(components);
  free(source_fingerprint);

  if (!keypath)
    return NULL;

  return keypath_to_registry_item(keypath);
}

cbor_value_t *keypath_to_data_item(keypath_data_t *keypath) {
  if (!keypath)
    return NULL;

  cbor_value_t *map = cbor_value_new_map();
  if (!map)
    return NULL;

  if (keypath->component_count > 0) {
    cbor_value_t *arr = cbor_value_new_array();
    if (!arr) {
      cbor_value_free(map);
      return NULL;
    }
    for (size_t i = 0; i < keypath->component_count; i++) {
      cbor_array_append(
          arr, keypath->components[i].wildcard
                   ? cbor_value_new_array()
                   : cbor_value_new_unsigned_int(keypath->components[i].index));
      cbor_array_append(arr,
                        cbor_value_new_bool(keypath->components[i].hardened));
    }
    cbor_map_set(map, cbor_value_new_unsigned_int(1), arr);
  }

  if (keypath->source_fingerprint) {
    uint32_t fp = ((uint32_t)keypath->source_fingerprint[0] << 24) |
                  ((uint32_t)keypath->source_fingerprint[1] << 16) |
                  ((uint32_t)keypath->source_fingerprint[2] << 8) |
                  ((uint32_t)keypath->source_fingerprint[3]);
    cbor_map_set(map, cbor_value_new_unsigned_int(2),
                 cbor_value_new_unsigned_int(fp));
  }

  if (keypath->depth >= 0)
    cbor_map_set(map, cbor_value_new_unsigned_int(3),
                 cbor_value_new_unsigned_int((uint64_t)keypath->depth));

  return map;
}

// Registry item interface
registry_item_t *keypath_to_registry_item(keypath_data_t *keypath) {
  return registry_item_new(&KEYPATH_TYPE, keypath, NULL,
                           keypath_from_data_item);
}

keypath_data_t *keypath_from_registry_item(registry_item_t *item) {
  if (!item || item->type != &KEYPATH_TYPE)
    return NULL;
  return (keypath_data_t *)item->data;
}

// Generate path string like "44'/0'/0'" or "1/0/*"
char *keypath_to_string(keypath_data_t *keypath) {
  if (!keypath || keypath->component_count == 0) {
    return safe_strdup("");
  }

  // Calculate required buffer size
  size_t buf_size = 1; // For null terminator
  for (size_t i = 0; i < keypath->component_count; i++) {
    if (i > 0)
      buf_size++; // For '/'
    if (keypath->components[i].wildcard) {
      buf_size += 1; // '*'
    } else {
      buf_size += 10; // Max digits for uint32
    }
    if (keypath->components[i].hardened) {
      buf_size += 1; // For "'"
    }
  }

  char *str = safe_malloc(buf_size);
  if (!str)
    return NULL;

  char *ptr = str;
  for (size_t i = 0; i < keypath->component_count; i++) {
    if (i > 0) {
      *ptr++ = '/';
    }

    if (keypath->components[i].wildcard) {
      *ptr++ = '*';
    } else {
      int written = sprintf(ptr, "%" PRIu32, keypath->components[i].index);
      ptr += written;
    }

    if (keypath->components[i].hardened) {
      *ptr++ = '\'';
    }
  }
  *ptr = '\0';

  return str;
}
