#include "hd_key.h"
#include "byte_buffer.h"
#include "cbor_decoder.h"
#include <stdio.h>
#include <string.h>

// HDKey registry type (tag 303)
registry_type_t HDKEY_TYPE = {.name = "crypto-hdkey", .tag = CRYPTO_HDKEY_TAG};

// Create and destroy HDKey
hd_key_data_t *hd_key_new(void) {
  hd_key_data_t *hd_key = safe_malloc(sizeof(hd_key_data_t));
  if (!hd_key)
    return NULL;

  hd_key->master = false;
  hd_key->key = NULL;
  hd_key->key_len = 0;
  hd_key->chain_code = NULL;
  hd_key->private_key = NULL;
  hd_key->private_key_len = 0;
  hd_key->origin = NULL;
  hd_key->children = NULL;
  hd_key->parent_fingerprint = NULL;

  return hd_key;
}

void hd_key_free(hd_key_data_t *hd_key) {
  if (!hd_key)
    return;

  free(hd_key->key);
  free(hd_key->chain_code);
  free(hd_key->private_key);
  free(hd_key->parent_fingerprint);

  if (hd_key->origin)
    keypath_free(hd_key->origin);
  if (hd_key->children)
    keypath_free(hd_key->children);

  free(hd_key);
}

// Parse HDKey from CBOR data item
registry_item_t *hd_key_from_data_item(cbor_value_t *data_item) {
  if (!data_item)
    return NULL;

  // HDKey is a map
  if (cbor_value_get_type(data_item) != CBOR_TYPE_MAP)
    return NULL;

  hd_key_data_t *hd_key = hd_key_new();
  if (!hd_key)
    return NULL;

  // Check if master key (key 1)
  cbor_value_t *master_val = get_map_value(data_item, 1);
  if (master_val && cbor_value_get_type(master_val) == CBOR_TYPE_BOOL) {
    hd_key->master = cbor_value_get_bool(master_val);
  }

  // Get private key (key 2, optional)
  cbor_value_t *priv_val = get_map_value(data_item, 2);
  if (priv_val && cbor_value_get_type(priv_val) == CBOR_TYPE_BYTES) {
    const uint8_t *priv_data =
        cbor_value_get_bytes(priv_val, &hd_key->private_key_len);
    if (priv_data && hd_key->private_key_len > 0) {
      hd_key->private_key = safe_malloc(hd_key->private_key_len);
      if (hd_key->private_key) {
        memcpy(hd_key->private_key, priv_data, hd_key->private_key_len);
      }
    }
  }

  // Get key (key 3, required)
  cbor_value_t *key_val = get_map_value(data_item, 3);
  if (!key_val || cbor_value_get_type(key_val) != CBOR_TYPE_BYTES) {
    hd_key_free(hd_key);
    return NULL;
  }
  const uint8_t *key_data = cbor_value_get_bytes(key_val, &hd_key->key_len);
  if (!key_data || hd_key->key_len == 0) {
    hd_key_free(hd_key);
    return NULL;
  }
  hd_key->key = safe_malloc(hd_key->key_len);
  if (!hd_key->key) {
    hd_key_free(hd_key);
    return NULL;
  }
  memcpy(hd_key->key, key_data, hd_key->key_len);

  // Get chain code (key 4, optional)
  cbor_value_t *chain_val = get_map_value(data_item, 4);
  if (chain_val && cbor_value_get_type(chain_val) == CBOR_TYPE_BYTES) {
    size_t chain_len;
    const uint8_t *chain_data = cbor_value_get_bytes(chain_val, &chain_len);
    if (chain_data && chain_len == 32) {
      hd_key->chain_code = safe_malloc(32);
      if (hd_key->chain_code) {
        memcpy(hd_key->chain_code, chain_data, 32);
      }
    }
  }

  // Skip use_info (key 5) - not needed for descriptors

  // Get origin (key 6, optional)
  cbor_value_t *origin_val = get_map_value(data_item, 6);
  if (origin_val) {
    // Unwrap tag if present (tag 304 for crypto-keypath)
    cbor_value_t *origin_content = origin_val;
    if (cbor_value_get_type(origin_val) == CBOR_TYPE_TAG) {
      origin_content = cbor_value_get_tag_content(origin_val);
    }
    registry_item_t *origin_item = keypath_from_data_item(origin_content);
    if (origin_item) {
      hd_key->origin = keypath_from_registry_item(origin_item);
      free(origin_item); // Transfer ownership
    }
  }

  // Get children (key 7, optional)
  cbor_value_t *children_val = get_map_value(data_item, 7);
  if (children_val) {
    // Unwrap tag if present (tag 304 for crypto-keypath)
    cbor_value_t *children_content = children_val;
    if (cbor_value_get_type(children_val) == CBOR_TYPE_TAG) {
      children_content = cbor_value_get_tag_content(children_val);
    }
    registry_item_t *children_item = keypath_from_data_item(children_content);
    if (children_item) {
      hd_key->children = keypath_from_registry_item(children_item);
      free(children_item); // Transfer ownership
    }
  }

  // Get parent fingerprint (key 8, optional)
  cbor_value_t *pfp_val = get_map_value(data_item, 8);
  if (pfp_val && cbor_value_get_type(pfp_val) == CBOR_TYPE_UNSIGNED_INT) {
    uint32_t fp_int = (uint32_t)cbor_value_get_uint(pfp_val);
    hd_key->parent_fingerprint = safe_malloc(4);
    if (hd_key->parent_fingerprint) {
      // Big-endian encoding
      hd_key->parent_fingerprint[0] = (fp_int >> 24) & 0xFF;
      hd_key->parent_fingerprint[1] = (fp_int >> 16) & 0xFF;
      hd_key->parent_fingerprint[2] = (fp_int >> 8) & 0xFF;
      hd_key->parent_fingerprint[3] = fp_int & 0xFF;
    }
  }

  // Skip name (key 9) and note (key 10) - not needed for descriptors

  return hd_key_to_registry_item(hd_key);
}

cbor_value_t *hd_key_to_data_item(hd_key_data_t *hd_key) {
  if (!hd_key)
    return NULL;

  cbor_value_t *map = cbor_value_new_map();
  if (!map)
    return NULL;

  if (hd_key->master)
    cbor_map_set(map, cbor_value_new_unsigned_int(1),
                 cbor_value_new_bool(true));

  if (hd_key->private_key && hd_key->private_key_len > 0)
    cbor_map_set(
        map, cbor_value_new_unsigned_int(2),
        cbor_value_new_bytes(hd_key->private_key, hd_key->private_key_len));

  if (hd_key->key && hd_key->key_len > 0)
    cbor_map_set(map, cbor_value_new_unsigned_int(3),
                 cbor_value_new_bytes(hd_key->key, hd_key->key_len));

  if (hd_key->chain_code)
    cbor_map_set(map, cbor_value_new_unsigned_int(4),
                 cbor_value_new_bytes(hd_key->chain_code, 32));

  if (hd_key->origin) {
    cbor_value_t *m = keypath_to_data_item(hd_key->origin);
    if (m)
      cbor_map_set(map, cbor_value_new_unsigned_int(6),
                   cbor_value_new_tag(CRYPTO_KEYPATH_TAG, m));
  }

  if (hd_key->children) {
    cbor_value_t *m = keypath_to_data_item(hd_key->children);
    if (m)
      cbor_map_set(map, cbor_value_new_unsigned_int(7),
                   cbor_value_new_tag(CRYPTO_KEYPATH_TAG, m));
  }

  if (hd_key->parent_fingerprint) {
    uint32_t fp = ((uint32_t)hd_key->parent_fingerprint[0] << 24) |
                  ((uint32_t)hd_key->parent_fingerprint[1] << 16) |
                  ((uint32_t)hd_key->parent_fingerprint[2] << 8) |
                  ((uint32_t)hd_key->parent_fingerprint[3]);
    cbor_map_set(map, cbor_value_new_unsigned_int(8),
                 cbor_value_new_unsigned_int(fp));
  }

  return map;
}

// Registry item interface
registry_item_t *hd_key_to_registry_item(hd_key_data_t *hd_key) {
  return registry_item_new(&HDKEY_TYPE, hd_key, NULL, hd_key_from_data_item);
}

hd_key_data_t *hd_key_from_registry_item(registry_item_t *item) {
  if (!item || item->type != &HDKEY_TYPE)
    return NULL;
  return (hd_key_data_t *)item->data;
}

// Generate BIP32 extended key (xpub/xprv format)
char *hd_key_bip32_key(hd_key_data_t *hd_key, bool include_derivation_path) {
  if (!hd_key || !hd_key->key)
    return NULL;

  // Build the 78-byte BIP32 key data
  uint8_t key_data[78];
  memset(key_data, 0, 78);

  // Detect network from BIP44 coin type in origin path
  // BIP44 path: m/purpose'/coin_type'/account'
  // coin_type: 0' = mainnet, 1' = testnet
  bool is_testnet = false;
  if (hd_key->origin && hd_key->origin->component_count >= 2) {
    // Check the second component (coin_type)
    path_component_t *coin_type = &hd_key->origin->components[1];
    if (coin_type->hardened && coin_type->index == 1) {
      is_testnet = true;
    }
  }

  // Version bytes (4 bytes)
  uint8_t version[4];
  if (is_testnet) {
    // tpub (testnet)
    version[0] = 0x04;
    version[1] = 0x35;
    version[2] = 0x87;
    version[3] = 0xCF;
  } else {
    // xpub (mainnet) - default
    version[0] = 0x04;
    version[1] = 0x88;
    version[2] = 0xB2;
    version[3] = 0x1E;
  }
  memcpy(key_data, version, 4);

  // Depth (1 byte)
  uint8_t depth = 0;
  if (!hd_key->master && hd_key->origin) {
    depth = hd_key->origin->depth >= 0
                ? (uint8_t)hd_key->origin->depth
                : (uint8_t)hd_key->origin->component_count;
  }
  key_data[4] = depth;

  // Parent fingerprint (4 bytes)
  uint8_t parent_fp[4] = {0, 0, 0, 0};
  bool source_is_parent = false;

  if (!hd_key->master) {
    if (hd_key->parent_fingerprint) {
      memcpy(parent_fp, hd_key->parent_fingerprint, 4);
    } else if (hd_key->origin && hd_key->origin->source_fingerprint &&
               hd_key->origin->component_count == 1) {
      memcpy(parent_fp, hd_key->origin->source_fingerprint, 4);
      source_is_parent = true;
    }
  }
  memcpy(key_data + 5, parent_fp, 4);

  // Child index (4 bytes, big-endian)
  uint32_t index = 0;
  if (!hd_key->master && hd_key->origin &&
      hd_key->origin->component_count > 0) {
    path_component_t *last =
        &hd_key->origin->components[hd_key->origin->component_count - 1];
    index = last->index;
    if (last->hardened) {
      index |= 0x80000000;
    }
  }
  key_data[9] = (index >> 24) & 0xFF;
  key_data[10] = (index >> 16) & 0xFF;
  key_data[11] = (index >> 8) & 0xFF;
  key_data[12] = index & 0xFF;

  // Chain code (32 bytes)
  if (hd_key->chain_code) {
    memcpy(key_data + 13, hd_key->chain_code, 32);
  }

  // Key data (33 bytes)
  if (hd_key->key_len == 32) {
    // Private key: prefix with 0x00
    key_data[45] = 0x00;
    memcpy(key_data + 46, hd_key->key, 32);
  } else if (hd_key->key_len == 33) {
    // Public key: use as-is
    memcpy(key_data + 45, hd_key->key, 33);
  } else {
    return NULL; // Invalid key length
  }

  // Encode with base58check
  char *xpub = base58check_encode(key_data, 78);
  if (!xpub)
    return NULL;

  if (!include_derivation_path) {
    return xpub;
  }

  // Build full descriptor string with derivation paths
  char *derivation = NULL;
  if (hd_key->origin && hd_key->origin->source_fingerprint &&
      hd_key->origin->component_count > 0 && !source_is_parent) {
    // Format: [fingerprint/path]
    char fp_hex[9];
    sprintf(fp_hex, "%02x%02x%02x%02x", hd_key->origin->source_fingerprint[0],
            hd_key->origin->source_fingerprint[1],
            hd_key->origin->source_fingerprint[2],
            hd_key->origin->source_fingerprint[3]);

    char *path = keypath_to_string(hd_key->origin);
    derivation = safe_malloc(strlen(fp_hex) + strlen(path) + 4); // [fp/path]
    if (derivation) {
      sprintf(derivation, "[%s/%s]", fp_hex, path);
    }
    free(path);
  }

  char *child_derivation = NULL;
  if (hd_key->children && hd_key->children->component_count > 0) {
    // Format: /path
    char *child_path = keypath_to_string(hd_key->children);
    child_derivation = safe_malloc(strlen(child_path) + 2); // /path
    if (child_derivation) {
      sprintf(child_derivation, "/%s", child_path);
    }
    free(child_path);
  }

  // Concatenate: derivation + xpub + child_derivation
  size_t total_len = strlen(xpub) + 1;
  if (derivation)
    total_len += strlen(derivation);
  if (child_derivation)
    total_len += strlen(child_derivation);

  char *result = safe_malloc(total_len);
  if (!result) {
    free(xpub);
    free(derivation);
    free(child_derivation);
    return NULL;
  }

  result[0] = '\0';
  if (derivation)
    strcat(result, derivation);
  strcat(result, xpub);
  if (child_derivation)
    strcat(result, child_derivation);

  free(xpub);
  free(derivation);
  free(child_derivation);

  return result;
}

char *hd_key_descriptor_key(hd_key_data_t *hd_key) {
  return hd_key_bip32_key(hd_key, true);
}
