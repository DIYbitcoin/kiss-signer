#include "bip39.h"
#include "byte_buffer.h"
#include "cbor_decoder.h"
#include <string.h>

// BIP39 registry type (tag 301)
registry_type_t BIP39_TYPE = {.name = "crypto-bip39", .tag = CRYPTO_BIP39_TAG};

// Create and destroy BIP39
bip39_data_t *bip39_new(char **words, size_t word_count, const char *lang) {
  if (!words || word_count == 0)
    return NULL;

  bip39_data_t *bip39 = safe_malloc(sizeof(bip39_data_t));
  if (!bip39)
    return NULL;

  // Copy words array
  bip39->words = safe_malloc(word_count * sizeof(char *));
  if (!bip39->words) {
    free(bip39);
    return NULL;
  }

  for (size_t i = 0; i < word_count; i++) {
    bip39->words[i] = safe_strdup(words[i]);
    if (!bip39->words[i]) {
      // Free previously allocated words
      for (size_t j = 0; j < i; j++) {
        free(bip39->words[j]);
      }
      free(bip39->words);
      free(bip39);
      return NULL;
    }
  }

  bip39->word_count = word_count;
  bip39->lang = lang ? safe_strdup(lang) : NULL;

  return bip39;
}

void bip39_free(bip39_data_t *bip39) {
  if (!bip39)
    return;

  if (bip39->words) {
    for (size_t i = 0; i < bip39->word_count; i++) {
      free(bip39->words[i]);
    }
    free(bip39->words);
  }

  free(bip39->lang);
  free(bip39);
}

// CBOR conversion functions
registry_item_t *bip39_from_data_item(cbor_value_t *data_item) {
  if (!data_item)
    return NULL;

  // Expect plain map, NOT tagged (matches Python implementation)
  if (cbor_value_get_type(data_item) != CBOR_TYPE_MAP)
    return NULL;

  // Get words array (key 1)
  cbor_value_t *words_array = get_map_value(data_item, 1);
  if (!words_array || cbor_value_get_type(words_array) != CBOR_TYPE_ARRAY)
    return NULL;

  size_t word_count = cbor_value_get_array_size(words_array);
  if (word_count == 0)
    return NULL;

  char **words = safe_malloc(word_count * sizeof(char *));
  if (!words)
    return NULL;

  for (size_t i = 0; i < word_count; i++) {
    cbor_value_t *word_val = cbor_value_get_array_item(words_array, i);
    if (!word_val || cbor_value_get_type(word_val) != CBOR_TYPE_STRING) {
      for (size_t j = 0; j < i; j++) {
        free(words[j]);
      }
      free(words);
      return NULL;
    }

    const char *word_str = cbor_value_get_string(word_val);
    words[i] = safe_strdup(word_str);
    if (!words[i]) {
      for (size_t j = 0; j < i; j++) {
        free(words[j]);
      }
      free(words);
      return NULL;
    }
  }

  // Get lang (key 2) if present
  char *lang = NULL;
  cbor_value_t *lang_val = get_map_value(data_item, 2);
  if (lang_val && cbor_value_get_type(lang_val) == CBOR_TYPE_STRING) {
    const char *lang_str = cbor_value_get_string(lang_val);
    lang = safe_strdup(lang_str);
  }

  // Build the bip39 directly instead of going through bip39_new, which
  // would strdup every word a second time.
  bip39_data_t *bip39 = safe_malloc(sizeof(bip39_data_t));
  if (!bip39) {
    for (size_t i = 0; i < word_count; i++)
      free(words[i]);
    free(words);
    free(lang);
    return NULL;
  }
  bip39->words = words;
  bip39->word_count = word_count;
  bip39->lang = lang;

  return bip39_to_registry_item(bip39);
}

// Registry item interface for BIP39
registry_item_t *bip39_to_registry_item(bip39_data_t *bip39) {
  // to_data_item is NULL — BIP39 is decode-only.
  return registry_item_new(&BIP39_TYPE, bip39, NULL, bip39_from_data_item);
}

bip39_data_t *bip39_from_registry_item(registry_item_t *item) {
  if (!item || item->type != &BIP39_TYPE)
    return NULL;
  return (bip39_data_t *)item->data;
}

// Convenience functions
bip39_data_t *bip39_from_cbor(const uint8_t *cbor_data, size_t len) {
  return (bip39_data_t *)registry_item_unwrap_from_cbor(cbor_data, len,
                                                        bip39_from_data_item);
}

// Accessors
char **bip39_get_words(bip39_data_t *bip39, size_t *out_count) {
  if (!bip39 || !out_count)
    return NULL;

  *out_count = bip39->word_count;
  return bip39->words;
}
