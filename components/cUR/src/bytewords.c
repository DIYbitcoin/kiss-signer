//
// bytewords.c
//
// Copyright © 2025 Krux Contributors
// Licensed under the "BSD-2-Clause Plus Patent License"
//
// Implementation of Bytewords encoding/decoding as specified in:
// https://github.com/BlockchainCommons/Research/blob/master/papers/bcr-2020-012-bytewords.md
//
// This is an independent implementation written using
// foundation-ur-py as a reference for testing and validation.
//
// Note: Optimized for MINIMAL style only (2-char encoding)
//

#include "bytewords.h"
#include "crc32.h"
#include "utils.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// Minimal bytewords table: first+last character pairs for all 256 words.
// Stored uppercase so bytewords_encode can emit directly without a per-byte
// toupper() call. (Decode is unaffected — it reads only lookup_table below.)
// For byte i: first_char = bytewords_minimal[i*2], last_char =
// bytewords_minimal[i*2+1]
static const char bytewords_minimal[] = "AEADAOAXAAAHAMATAYASBKBDBNBTBABS"
                                        "BEBYBGBWBBBZCMCHCSCFCYCWCECACKCT"
                                        "CXCLCPCNDKDADSDIDEDTDRDNDWDPDMDL"
                                        "DYEHEYEOEEECENEMETESFTFRFNFSFMFH"
                                        "FZFPFWFXFYFEFGFLFDGAGEGRGSGTGLGW"
                                        "GDGYGMGUGHGOHFHGHDHKHTHPHHHLHYHE"
                                        "HNHSIDIAIEIHIYIOISINIMJEJZJNJTJL"
                                        "JOJSJPJKJYKPKOKTKSKKKNKGKEKIKBLB"
                                        "LALYLFLSLRLPLNLTLOLDLELULKLGMNMY"
                                        "MHMEMOMUMWMDMTMSMKNLNYNDNSNTNNNE"
                                        "NBOYOEOTOXONOLOSPDPTPKPYPSPMPLPE"
                                        "PFPAPRQDQZRERPRLRORHRDRKRFRYRNRS"
                                        "RTSESASRSSSKSWSTSPSOSGSBSFSNTOTK"
                                        "TITTTDTETYTLTBTSTPTATNUYUOUTUEUR"
                                        "VTVYVOVLVEVWVAVDVSWLWDWMWPWEWYWS"
                                        "WTWNWZWFWKYKYNYLYAYTZSZOZTZCZEZM";

// Optimized lookup table for fast word-to-byte mapping (2-char minimal style)
// Uses first and last character positions to create 2D hash
static const int16_t lookup_table[] = {
    4,   14,  29,  37,  -1,  -1,  73,  -1,  99,  -1,  -1,  128, -1,  -1,  -1,
    177, -1,  -1,  194, 217, -1,  230, -1,  -1,  248, -1,  -1,  20,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  126, 127, -1,  160, -1,  -1,  -1,  -1,  203,
    214, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  53,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  253, 1,   11,  -1,  -1,  -1,  72,  80,  88,  98,  -1,  -1,  137,
    149, 155, -1,  168, 179, 186, -1,  210, -1,  231, 234, -1,  -1,  -1,  0,
    16,  28,  40,  52,  69,  74,  95,  100, 107, 124, 138, 145, 159, 162, 175,
    -1,  181, 193, 211, 222, 228, 237, -1,  -1,  254, -1,  -1,  25,  -1,  -1,
    -1,  -1,  86,  -1,  -1,  -1,  130, -1,  -1,  -1,  176, -1,  188, 204, -1,
    -1,  -1,  243, -1,  -1,  -1,  -1,  18,  -1,  -1,  -1,  70,  -1,  87,  -1,
    -1,  123, 141, -1,  -1,  -1,  -1,  -1,  -1,  202, -1,  -1,  -1,  -1,  -1,
    -1,  -1,  5,   -1,  23,  -1,  49,  63,  84,  92,  101, -1,  -1,  -1,  144,
    -1,  -1,  -1,  -1,  185, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  39,  -1,  -1,  -1,  -1,  -1,  -1,  125, -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  208, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  10,  30,  36,  -1,  -1,  -1,  89,  -1,  115,
    121, 140, 152, -1,  -1,  170, -1,  187, 197, 207, -1,  -1,  244, -1,  245,
    -1,  -1,  -1,  33,  47,  -1,  71,  78,  93,  -1,  111, -1,  -1,  -1,  153,
    166, 174, -1,  183, -1,  213, -1,  227, 233, -1,  247, -1,  6,   -1,  22,
    46,  55,  62,  82,  -1,  106, -1,  -1,  -1,  -1,  -1,  -1,  173, -1,  -1,
    -1,  -1,  -1,  -1,  235, -1,  -1,  255, -1,  12,  35,  43,  54,  60,  -1,
    96,  105, 109, 122, 134, 142, 158, 165, -1,  -1,  190, 205, 218, -1,  -1,
    241, -1,  246, -1,  2,   -1,  -1,  -1,  51,  -1,  85,  -1,  103, 112, 118,
    136, 146, -1,  -1,  -1,  -1,  184, 201, 206, 220, 226, -1,  -1,  -1,  251,
    -1,  -1,  34,  45,  -1,  65,  -1,  91,  -1,  114, 117, 133, -1,  -1,  -1,
    -1,  -1,  182, 200, 216, -1,  -1,  236, -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  42,  -1,  59,  75,  -1,
    -1,  -1,  -1,  132, -1,  -1,  -1,  178, -1,  -1,  195, -1,  223, -1,  -1,
    -1,  -1,  -1,  9,   15,  24,  38,  57,  61,  76,  97,  104, 113, 120, 131,
    151, 156, 167, 172, -1,  191, 196, 215, -1,  232, 239, -1,  -1,  250, 7,
    13,  31,  41,  56,  58,  77,  90,  -1,  110, 119, 135, 150, 157, 163, 169,
    -1,  192, 199, 209, 221, 224, 240, -1,  249, 252, -1,  -1,  -1,  -1,  -1,
    -1,  83,  -1,  -1,  -1,  -1,  139, 147, -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  19,  27,  44,  -1,  66,  79,  -1,  -1,  -1,  -1,  -1,  148,
    -1,  -1,  -1,  -1,  -1,  198, -1,  -1,  229, -1,  -1,  -1,  -1,  3,   -1,
    32,  -1,  -1,  67,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  164, -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  8,   17,  26,  48,  50,  68,
    81,  94,  102, 116, -1,  129, 143, 154, 161, 171, -1,  189, -1,  212, 219,
    225, 238, -1,  -1,  -1,  -1,  21,  -1,  -1,  -1,  64,  -1,  -1,  -1,  108,
    -1,  -1,  -1,  -1,  -1,  -1,  180, -1,  -1,  -1,  -1,  -1,  242, -1,  -1,
    -1};

bool bytewords_encode(const uint8_t *data, size_t data_len, char **encoded) {
  if (!data || !encoded || data_len == 0)
    return false;

  uint32_t crc = crc32_calculate(data, data_len);
  size_t total_len = data_len + 4;

  // MINIMAL style: 2 chars per byte (first + last character of word).
  // Written byte-by-byte below, so skip the zero-init.
  *encoded = safe_malloc_uninit(total_len * 2 + 1);
  if (!*encoded)
    return false;

  char *pos = *encoded;
  for (size_t i = 0; i < data_len; i++) {
    const char *word = &bytewords_minimal[data[i] * 2];
    *pos++ = word[0];
    *pos++ = word[1];
  }
  const uint8_t crc_bytes[4] = {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16),
                                (uint8_t)(crc >> 8), (uint8_t)crc};
  for (size_t i = 0; i < 4; i++) {
    const char *word = &bytewords_minimal[crc_bytes[i] * 2];
    *pos++ = word[0];
    *pos++ = word[1];
  }
  *pos = '\0';

  return true;
}

void bytewords_free(void *ptr) {
  if (ptr) {
    free(ptr);
  }
}

bool bytewords_decode_raw(const char *encoded, uint8_t **decoded,
                          size_t *decoded_len) {
  if (!encoded || !decoded || !decoded_len)
    return false;

  size_t encoded_len = strlen(encoded);
  if (encoded_len % 2 != 0 || encoded_len < 8) {
    return false;
  }

  size_t num_bytes = encoded_len / 2;
  uint8_t *buf = safe_malloc_uninit(num_bytes);
  if (!buf)
    return false;

  // Decode each 2-char pair using lookup table
  for (size_t i = 0; i < num_bytes; i++) {
    char first = tolower((unsigned char)encoded[i * 2]);
    char last = tolower((unsigned char)encoded[i * 2 + 1]);

    int x = first - 'a';
    int y = last - 'a';

    if (x < 0 || x >= 26 || y < 0 || y >= 26) {
      free(buf);
      return false;
    }

    size_t offset = y * 26 + x;
    int16_t value = lookup_table[offset];
    if (value == -1) {
      free(buf);
      return false;
    }

    buf[i] = (uint8_t)value;
  }

  // Verify and remove CRC (last 4 bytes)
  if (num_bytes < 5) {
    free(buf);
    return false;
  }

  size_t body_size = num_bytes - 4;

  // Verify CRC32 checksum in-place
  uint32_t expected_crc =
      ((uint32_t)buf[body_size] << 24) | ((uint32_t)buf[body_size + 1] << 16) |
      ((uint32_t)buf[body_size + 2] << 8) | (uint32_t)buf[body_size + 3];
  uint32_t actual_crc = crc32_calculate(buf, body_size);
  if (expected_crc != actual_crc) {
    free(buf);
    return false;
  }

  // Shrink buf in-place instead of allocating a new body buffer
  uint8_t *body = safe_realloc(buf, body_size);
  *decoded = body ? body : buf; // realloc may fail; buf is still valid
  *decoded_len = body_size;
  return true;
}
