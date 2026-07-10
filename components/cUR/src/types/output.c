#include "output.h"
#include "../sha256/sha256_compat.h"
#include "byte_buffer.h"
#include "cbor_decoder.h"
#include "cbor_encoder.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// Output registry type (tag 308)
registry_type_t OUTPUT_TYPE = {.name = "crypto-output",
                               .tag = CRYPTO_OUTPUT_TAG};

// Script expression mapping (tag -> expression string)
static const script_expression_t SCRIPT_EXPRESSIONS[] = {
    {SCRIPT_EXPR_ADDR, "addr"},
    {SCRIPT_EXPR_SH, "sh"},
    {SCRIPT_EXPR_WSH, "wsh"},
    {SCRIPT_EXPR_PK, "pk"},
    {SCRIPT_EXPR_PKH, "pkh"},
    {SCRIPT_EXPR_WPKH, "wpkh"},
    {SCRIPT_EXPR_COMBO, "combo"},
    {SCRIPT_EXPR_MULTI, "multi"},
    {SCRIPT_EXPR_SORTEDMULTI, "sortedmulti"},
    {SCRIPT_EXPR_RAW, "raw"},
    {SCRIPT_EXPR_TR, "tr"},
    {SCRIPT_EXPR_COSIGNER, "cosigner"},
    {0, NULL} // Sentinel
};

const script_expression_t *get_script_expression_by_tag(uint64_t tag) {
  for (size_t i = 0; SCRIPT_EXPRESSIONS[i].expression != NULL; i++) {
    if (SCRIPT_EXPRESSIONS[i].tag == tag) {
      return &SCRIPT_EXPRESSIONS[i];
    }
  }
  return NULL;
}

// Create and destroy Output
output_data_t *output_new(void) {
  output_data_t *output = safe_malloc(sizeof(output_data_t));
  if (!output)
    return NULL;

  output->script_expressions = NULL;
  output->script_expression_count = 0;
  output->key_type = KEY_TYPE_HD;
  output->crypto_key.hd_key = NULL;

  return output;
}

void output_free(output_data_t *output) {
  if (!output)
    return;

  free(output->script_expressions);

  // Free the appropriate key type
  switch (output->key_type) {
  case KEY_TYPE_HD:
    hd_key_free(output->crypto_key.hd_key);
    break;
  case KEY_TYPE_MULTI:
    multi_key_free(output->crypto_key.multi_key);
    break;
  }

  free(output);
}

// Parse Output from CBOR data item
registry_item_t *output_from_data_item(cbor_value_t *data_item) {
  if (!data_item)
    return NULL;

  output_data_t *output = output_new();
  if (!output)
    return NULL;

  // Parse script expressions by unwrapping tags
  cbor_value_t *tmp_item = data_item;
  script_expression_t **expressions = NULL;
  size_t expr_count = 0;

  // Collect script expression tags by unwrapping nested tags
  while (tmp_item && cbor_value_get_type(tmp_item) == CBOR_TYPE_TAG) {
    uint64_t tag = cbor_value_get_tag(tmp_item);
    const script_expression_t *expr = get_script_expression_by_tag(tag);

    if (expr) {
      // Add this expression to the list
      expr_count++;
      script_expression_t **new_exprs =
          safe_realloc(expressions, expr_count * sizeof(script_expression_t *));
      if (!new_exprs) {
        free(expressions);
        output_free(output);
        return NULL;
      }
      expressions = new_exprs;
      expressions[expr_count - 1] = (script_expression_t *)expr;

      // Unwrap to the next level
      tmp_item = cbor_value_get_tag_content(tmp_item);
    } else {
      // This tag is not a script expression, must be a key type tag
      break;
    }
  }

  output->script_expressions = expressions;
  output->script_expression_count = expr_count;

  // Determine if this is a multisig output
  bool is_multi = false;
  if (expr_count > 0) {
    const script_expression_t *last_expr = expressions[expr_count - 1];
    if (strcmp(last_expr->expression, "multi") == 0 ||
        strcmp(last_expr->expression, "sortedmulti") == 0) {
      is_multi = true;
    }
  }

  // Get the innermost item (the actual key data)
  cbor_value_t *key_item = tmp_item;
  uint64_t key_tag = cbor_value_get_tag(key_item);

  // Parse the key based on type
  if (is_multi) {
    // Parse as MultiKey (map with threshold and keys)
    multi_key_data_t *multi_key = multi_key_from_data_item(key_item);
    if (!multi_key) {
      output_free(output);
      return NULL;
    }
    output->key_type = KEY_TYPE_MULTI;
    output->crypto_key.multi_key = multi_key;
  } else if (key_tag == CRYPTO_HDKEY_TAG) {
    // Parse as HDKey - unwrap the tag to get the map content
    cbor_value_t *key_content = cbor_value_get_tag_content(key_item);
    registry_item_t *item = hd_key_from_data_item(key_content);
    if (!item) {
      output_free(output);
      return NULL;
    }
    hd_key_data_t *hd_key = hd_key_from_registry_item(item);
    if (!hd_key) {
      free(item);
      output_free(output);
      return NULL;
    }
    item->data = NULL; // Transfer ownership
    free(item);
    output->key_type = KEY_TYPE_HD;
    output->crypto_key.hd_key = hd_key;
  } else {
    // Unknown key type
    output_free(output);
    return NULL;
  }

  return output_to_registry_item(output);
}

// Registry item interface
registry_item_t *output_to_registry_item(output_data_t *output) {
  // to_data_item is NULL — output takes a different path for encode
  // (output_to_cbor calls output_to_data_item directly, not through
  // the registry vtable).
  return registry_item_new(&OUTPUT_TYPE, output, NULL, output_from_data_item);
}

output_data_t *output_from_registry_item(registry_item_t *item) {
  if (!item || item->type != &OUTPUT_TYPE)
    return NULL;
  return (output_data_t *)item->data;
}

output_data_t *output_from_cbor(const uint8_t *cbor_data, size_t len) {
  return (output_data_t *)registry_item_unwrap_from_cbor(cbor_data, len,
                                                         output_from_data_item);
}

cbor_value_t *output_to_data_item(output_data_t *output) {
  if (!output)
    return NULL;

  cbor_value_t *content = NULL;
  if (output->key_type == KEY_TYPE_HD) {
    cbor_value_t *m = hd_key_to_data_item(output->crypto_key.hd_key);
    if (!m)
      return NULL;
    content = cbor_value_new_tag(CRYPTO_HDKEY_TAG, m);
  } else {
    content = multi_key_to_data_item(output->crypto_key.multi_key);
  }
  if (!content)
    return NULL;

  // Wrap with script expression tags (innermost to outermost)
  for (int i = (int)output->script_expression_count - 1; i >= 0; i--)
    content = cbor_value_new_tag(output->script_expressions[i]->tag, content);

  return content;
}

uint8_t *output_to_cbor(output_data_t *output, size_t *out_len) {
  if (!output || !out_len)
    return NULL;

  cbor_value_t *item = output_to_data_item(output);
  if (!item)
    return NULL;

  uint8_t *data = cbor_encode(item, out_len);
  cbor_value_free(item);
  return data;
}

// Base58check decode
static const int8_t BASE58_MAP[128] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,  7,
    8,  -1, -1, -1, -1, -1, -1, -1, 9,  10, 11, 12, 13, 14, 15, 16, -1, 17, 18,
    19, 20, 21, -1, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, -1, -1, -1, -1,
    -1, -1, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, -1, 44, 45, 46, 47, 48,
    49, 50, 51, 52, 53, 54, 55, 56, 57, -1, -1, -1, -1, -1};

static uint8_t *base58check_decode(const char *str, size_t *out_len) {
  if (!str || !out_len)
    return NULL;
  size_t str_len = strlen(str);
  if (str_len == 0)
    return NULL;

  size_t zeros = 0;
  while (zeros < str_len && str[zeros] == '1')
    zeros++;

  size_t max_size = str_len * 733 / 1000 + 1;
  uint8_t *buf = safe_malloc(max_size);
  if (!buf)
    return NULL;
  memset(buf, 0, max_size);

  size_t buf_len = 0;
  for (size_t i = 0; i < str_len; i++) {
    uint8_t ch = (uint8_t)str[i];
    if (ch >= 128 || BASE58_MAP[ch] < 0) {
      free(buf);
      return NULL;
    }
    uint32_t carry = (uint32_t)BASE58_MAP[ch];
    for (size_t j = 0; j < buf_len || carry; j++) {
      if (j < buf_len)
        carry += (uint32_t)buf[j] * 58;
      buf[j] = carry & 0xFF;
      carry >>= 8;
      if (j >= buf_len)
        buf_len = j + 1;
    }
  }

  size_t result_len = zeros + buf_len;
  uint8_t *result = safe_malloc(result_len);
  if (!result) {
    free(buf);
    return NULL;
  }
  memset(result, 0, zeros);
  for (size_t i = 0; i < buf_len; i++)
    result[zeros + i] = buf[buf_len - 1 - i];
  free(buf);

  if (result_len < 5) {
    free(result);
    return NULL;
  }

  size_t payload_len = result_len - 4;
  uint8_t hash1[32], hash2[32];
  ur_sha256(result, payload_len, hash1);
  ur_sha256(hash1, 32, hash2);
  if (memcmp(hash2, result + payload_len, 4) != 0) {
    free(result);
    return NULL;
  }

  *out_len = payload_len;
  return result;
}

static const script_expression_t *
get_script_expression_by_name(const char *name, size_t len) {
  for (size_t i = 0; SCRIPT_EXPRESSIONS[i].expression != NULL; i++)
    if (strlen(SCRIPT_EXPRESSIONS[i].expression) == len &&
        strncmp(SCRIPT_EXPRESSIONS[i].expression, name, len) == 0)
      return &SCRIPT_EXPRESSIONS[i];
  return NULL;
}

static int hex_val(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

static bool append_decimal_digit_u32(uint32_t *value, uint32_t digit) {
  if (!value || digit > 9)
    return false;
  if (*value > (UINT32_MAX - digit) / 10)
    return false;
  *value = (*value * 10) + digit;
  return true;
}

static bool parse_keypath_components(const char *path, size_t len,
                                     path_component_t **out_components,
                                     size_t *out_count) {
  if (!path || len == 0 || !out_components || !out_count)
    return false;

  size_t max_count = 1;
  for (size_t i = 0; i < len; i++)
    if (path[i] == '/')
      max_count++;

  path_component_t *comp = safe_malloc(max_count * sizeof(path_component_t));
  if (!comp)
    return false;

  size_t idx = 0;
  const char *p = path, *end = path + len;
  while (p < end && idx < max_count) {
    if (*p == '/') {
      p++;
      continue;
    }

    path_component_t *c = &comp[idx];
    c->hardened = false;
    if (*p == '*') {
      c->wildcard = true;
      c->index = 0;
      p++;
    } else if (isdigit((unsigned char)*p)) {
      c->wildcard = false;
      c->index = 0;
      while (p < end && isdigit((unsigned char)*p)) {
        uint32_t digit = (uint32_t)(*p - '0');
        if (!append_decimal_digit_u32(&c->index, digit)) {
          free(comp);
          return false;
        }
        p++;
      }
    } else {
      free(comp);
      return false;
    }
    if (p < end && (*p == '\'' || *p == 'h')) {
      c->hardened = true;
      p++;
    }
    idx++;
  }

  *out_components = comp;
  *out_count = idx;
  return true;
}

// Parse "[fingerprint/path]xpub/children" into hd_key_data_t
static hd_key_data_t *parse_hd_key_from_string(const char *str, size_t len) {
  if (!str || len == 0)
    return NULL;

  hd_key_data_t *hd_key = hd_key_new();
  if (!hd_key)
    return NULL;

  const char *p = str, *end = str + len;

  // Parse optional [fingerprint/path] origin
  if (*p == '[') {
    p++;
    const char *bracket_end = memchr(p, ']', end - p);
    if (!bracket_end || bracket_end - p < 8) {
      hd_key_free(hd_key);
      return NULL;
    }

    uint8_t fp[4];
    for (int i = 0; i < 4; i++) {
      int hi = hex_val(p[i * 2]), lo = hex_val(p[i * 2 + 1]);
      if (hi < 0 || lo < 0) {
        hd_key_free(hd_key);
        return NULL;
      }
      fp[i] = (uint8_t)((hi << 4) | lo);
    }
    p += 8;

    path_component_t *origin_comp = NULL;
    size_t origin_count = 0;
    if (p < bracket_end && *p == '/') {
      p++;
      if (bracket_end - p > 0 &&
          !parse_keypath_components(p, bracket_end - p, &origin_comp,
                                    &origin_count)) {
        hd_key_free(hd_key);
        return NULL;
      }
    }
    hd_key->origin =
        keypath_new(origin_comp, origin_count, fp, (int)origin_count);
    free(origin_comp);
    if (!hd_key->origin) {
      hd_key_free(hd_key);
      return NULL;
    }
    p = bracket_end + 1;
  }

  // Find xpub end (base58 doesn't contain '/')
  const char *xpub_start = p;
  const char *slash = memchr(p, '/', end - p);
  const char *xpub_end = slash ? slash : end;

  // Decode xpub/tpub
  size_t xpub_len = xpub_end - xpub_start;
  char *xpub_str = safe_malloc(xpub_len + 1);
  if (!xpub_str) {
    hd_key_free(hd_key);
    return NULL;
  }
  memcpy(xpub_str, xpub_start, xpub_len);
  xpub_str[xpub_len] = '\0';

  size_t dec_len = 0;
  uint8_t *dec = base58check_decode(xpub_str, &dec_len);
  free(xpub_str);
  if (!dec || dec_len != 78) {
    free(dec);
    hd_key_free(hd_key);
    return NULL;
  }

  // BIP32: [0-3]version [4]depth [5-8]parent_fp [9-12]child_idx
  //        [13-44]chain_code [45-77]key
  if (dec[5] || dec[6] || dec[7] || dec[8]) {
    hd_key->parent_fingerprint = safe_malloc(4);
    if (!hd_key->parent_fingerprint) {
      free(dec);
      hd_key_free(hd_key);
      return NULL;
    }
    memcpy(hd_key->parent_fingerprint, dec + 5, 4);
  }
  hd_key->chain_code = safe_malloc(32);
  hd_key->key_len = 33;
  hd_key->key = safe_malloc(33);
  if (!hd_key->chain_code || !hd_key->key) {
    free(dec);
    hd_key_free(hd_key);
    return NULL;
  }
  memcpy(hd_key->chain_code, dec + 13, 32);
  memcpy(hd_key->key, dec + 45, 33);
  if (hd_key->origin)
    hd_key->origin->depth = (int)dec[4];
  free(dec);

  // Parse optional children path
  if (xpub_end < end && *xpub_end == '/') {
    size_t ch_len = end - (xpub_end + 1);
    path_component_t *ch_comp = NULL;
    size_t ch_count = 0;
    if (ch_len == 0 ||
        !parse_keypath_components(xpub_end + 1, ch_len, &ch_comp, &ch_count)) {
      hd_key_free(hd_key);
      return NULL;
    }
    hd_key->children = keypath_new(ch_comp, ch_count, NULL, -1);
    free(ch_comp);
    if (!hd_key->children) {
      hd_key_free(hd_key);
      return NULL;
    }
  }

  return hd_key;
}

output_data_t *output_from_descriptor_string(const char *descriptor) {
  if (!descriptor)
    return NULL;

  // Strip checksum if present
  size_t desc_len = strlen(descriptor);
  const char *hash = strchr(descriptor, '#');
  if (hash)
    desc_len = hash - descriptor;

  output_data_t *output = output_new();
  if (!output)
    return NULL;

  const char *p = descriptor, *end = descriptor + desc_len;

  // Collect script expression prefixes
  script_expression_t **expressions = NULL;
  size_t expr_count = 0;
  while (p < end) {
    const char *paren = memchr(p, '(', end - p);
    if (!paren || paren == p)
      break;
    const script_expression_t *expr =
        get_script_expression_by_name(p, paren - p);
    if (!expr)
      break;
    expr_count++;
    script_expression_t **new_exprs =
        safe_realloc(expressions, expr_count * sizeof(script_expression_t *));
    if (!new_exprs) {
      free(expressions);
      output_free(output);
      return NULL;
    }
    expressions = new_exprs;
    expressions[expr_count - 1] = (script_expression_t *)expr;
    p = paren + 1;
  }
  output->script_expressions = expressions;
  output->script_expression_count = expr_count;

  // Strip trailing ')'
  const char *content_end = end;
  while (content_end > p && *(content_end - 1) == ')')
    content_end--;

  bool is_multi =
      expr_count > 0 &&
      (strcmp(expressions[expr_count - 1]->expression, "multi") == 0 ||
       strcmp(expressions[expr_count - 1]->expression, "sortedmulti") == 0);

  if (is_multi) {
    uint32_t threshold = 0;
    size_t threshold_digits = 0;
    while (p < content_end && isdigit((unsigned char)*p)) {
      uint32_t digit = (uint32_t)(*p - '0');
      if (!append_decimal_digit_u32(&threshold, digit)) {
        output_free(output);
        return NULL;
      }
      threshold_digits++;
      p++;
    }
    if (threshold_digits == 0) {
      output_free(output);
      return NULL;
    }
    if (threshold == 0) {
      output_free(output);
      return NULL;
    }
    if (p < content_end && *p == ',')
      p++;

    multi_key_data_t *mk = multi_key_new(threshold);
    if (!mk) {
      output_free(output);
      return NULL;
    }

    while (p < content_end) {
      const char *comma = memchr(p, ',', content_end - p);
      const char *key_end = comma ? comma : content_end;
      hd_key_data_t *hk = parse_hd_key_from_string(p, key_end - p);
      if (!hk || !multi_key_add_hd_key(mk, hk)) {
        hd_key_free(hk);
        multi_key_free(mk);
        output_free(output);
        return NULL;
      }
      p = comma ? comma + 1 : content_end;
    }
    if (mk->hd_key_count == 0 || threshold > mk->hd_key_count) {
      multi_key_free(mk);
      output_free(output);
      return NULL;
    }
    output->key_type = KEY_TYPE_MULTI;
    output->crypto_key.multi_key = mk;
  } else {
    hd_key_data_t *hk = parse_hd_key_from_string(p, content_end - p);
    if (!hk) {
      output_free(output);
      return NULL;
    }
    output->key_type = KEY_TYPE_HD;
    output->crypto_key.hd_key = hk;
  }

  return output;
}

// Descriptor checksum algorithm (polymod)
static uint64_t polymod(uint64_t c, int val) {
  uint8_t c0 = (c >> 35) & 0xFF;
  c = ((c & 0x7FFFFFFFF) << 5) ^ val;
  if (c0 & 1)
    c ^= 0xF5DEE51989ULL;
  if (c0 & 2)
    c ^= 0xA9FDCA3312ULL;
  if (c0 & 4)
    c ^= 0x1BAB10E32DULL;
  if (c0 & 8)
    c ^= 0x3706B1677AULL;
  if (c0 & 16)
    c ^= 0x644D626FFDULL;
  return c;
}

static const char INPUT_CHARSET[] = "0123456789()[],'/"
                                    "*abcdefgh@:$%{}IJKLMNOPQRSTUVWXYZ&+-.;<=>?"
                                    "!^_|~ijklmnopqrstuvwxyzABCDEFGH`#\"\\ ";
static const char CHECKSUM_CHARSET[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

static char *descriptor_checksum(const char *descriptor) {
  if (!descriptor)
    return NULL;

  uint64_t c = 1;
  int cls = 0;
  int clscount = 0;

  for (const char *ptr = descriptor; *ptr; ptr++) {
    const char *pos_ptr = strchr(INPUT_CHARSET, *ptr);
    if (!pos_ptr)
      return NULL; // Invalid character

    int pos = pos_ptr - INPUT_CHARSET;
    c = polymod(c, pos & 31);
    cls = cls * 3 + (pos >> 5);
    clscount++;

    if (clscount == 3) {
      c = polymod(c, cls);
      cls = 0;
      clscount = 0;
    }
  }

  if (clscount > 0) {
    c = polymod(c, cls);
  }

  for (int i = 0; i < 8; i++) {
    c = polymod(c, 0);
  }
  c ^= 1;

  char *checksum = safe_malloc(9);
  if (!checksum)
    return NULL;

  for (int i = 0; i < 8; i++) {
    checksum[i] = CHECKSUM_CHARSET[(c >> (5 * (7 - i))) & 31];
  }
  checksum[8] = '\0';

  return checksum;
}

// Generate output descriptor string
char *output_descriptor(output_data_t *output, bool include_checksum) {
  if (!output)
    return NULL;

  byte_buffer_t *buf = byte_buffer_new();
  if (!buf)
    return NULL;

  // Write script expressions (opening)
  for (size_t i = 0; i < output->script_expression_count; i++) {
    const char *expr = output->script_expressions[i]->expression;
    byte_buffer_append(buf, (const uint8_t *)expr, strlen(expr));
    byte_buffer_append_byte(buf, '(');
  }

  // Write threshold for multisig
  if (output->key_type == KEY_TYPE_MULTI) {
    char threshold_str[16];
    sprintf(threshold_str, "%" PRIu32 ",",
            output->crypto_key.multi_key->threshold);
    byte_buffer_append(buf, (const uint8_t *)threshold_str,
                       strlen(threshold_str));
  }

  // Write keys
  if (output->key_type == KEY_TYPE_HD) {
    char *key_str = hd_key_descriptor_key(output->crypto_key.hd_key);
    if (key_str) {
      byte_buffer_append(buf, (const uint8_t *)key_str, strlen(key_str));
      free(key_str);
    }
  } else if (output->key_type == KEY_TYPE_MULTI) {
    multi_key_data_t *mk = output->crypto_key.multi_key;

    // Write HD keys
    for (size_t i = 0; i < mk->hd_key_count; i++) {
      char *key_str = hd_key_descriptor_key(mk->hd_keys[i]);
      if (key_str) {
        byte_buffer_append(buf, (const uint8_t *)key_str, strlen(key_str));
        free(key_str);
      }
      if (i < mk->hd_key_count - 1) {
        byte_buffer_append_byte(buf, ',');
      }
    }
  }

  // Write script expressions (closing)
  for (size_t i = 0; i < output->script_expression_count; i++) {
    byte_buffer_append_byte(buf, ')');
  }

  byte_buffer_append_byte(buf, '\0');

  char *descriptor = safe_strdup((char *)byte_buffer_get_data(buf));
  byte_buffer_free(buf);

  if (!descriptor)
    return NULL;

  if (include_checksum) {
    char *checksum = descriptor_checksum(descriptor);
    if (checksum) {
      size_t desc_len = strlen(descriptor);
      char *result =
          safe_malloc(desc_len + 1 + 8 + 1); // descriptor + # + checksum + null
      if (result) {
        sprintf(result, "%s#%s", descriptor, checksum);
        free(descriptor);
        free(checksum);
        return result;
      }
      free(checksum);
    }
  }

  return descriptor;
}

// Helper function to extract first output descriptor from Account CBOR
char *output_descriptor_from_cbor_account(const uint8_t *account_cbor,
                                          size_t len) {
  if (!account_cbor || len == 0)
    return NULL;

  // Decode CBOR
  cbor_value_t *cbor_val = cbor_decode(account_cbor, len);
  if (!cbor_val)
    return NULL;

  // Account CBOR is a plain map (not wrapped in a tag)
  // Map structure: { 1: master_fingerprint, 2: [outputs...] }
  if (cbor_value_get_type(cbor_val) != CBOR_TYPE_MAP) {
    cbor_value_free(cbor_val);
    return NULL;
  }

  cbor_value_t *map = cbor_val;

  // Get map key 2 (output_descriptors array)
  cbor_value_t *outputs_array = cbor_map_get_int(map, 2);
  if (!outputs_array || cbor_value_get_type(outputs_array) != CBOR_TYPE_ARRAY) {
    cbor_value_free(cbor_val);
    return NULL;
  }

  // Get array size and check it has at least one element
  size_t array_size = cbor_value_get_array_size(outputs_array);
  if (array_size == 0) {
    cbor_value_free(cbor_val);
    return NULL;
  }

  // Get first output from array
  cbor_value_t *first_output = cbor_value_get_array_item(outputs_array, 0);
  if (!first_output) {
    cbor_value_free(cbor_val);
    return NULL;
  }

  // Parse as Output
  registry_item_t *output_item = output_from_data_item(first_output);
  if (!output_item) {
    cbor_value_free(cbor_val);
    return NULL;
  }

  output_data_t *output = output_from_registry_item(output_item);
  if (!output) {
    free(output_item);
    cbor_value_free(cbor_val);
    return NULL;
  }

  // Generate descriptor string (with checksum)
  char *descriptor = output_descriptor(output, true);

  // Cleanup
  output_item->data = NULL; // Prevent double free
  free(output_item);
  cbor_value_free(cbor_val);
  output_free(output);

  return descriptor;
}
