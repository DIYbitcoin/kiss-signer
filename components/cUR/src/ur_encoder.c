//
// ur_encoder.c
//
// Copyright © 2025 Krux Contributors
// Licensed under the "BSD-2-Clause Plus Patent License"
//
// Implementation of Uniform Resources (UR) encoder following the specification:
// https://github.com/BlockchainCommons/Research/blob/master/papers/bcr-2020-005-ur.md
//
// This is an independent implementation written using
// foundation-ur-py as a reference for testing and validation.
//

#include "ur_encoder.h"
#include "bytewords.h"
#include "utils.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper: Encode URI with scheme and path components
static char *encode_uri(const char *scheme, const char **path_components,
                        size_t component_count) {
  if (!scheme || !path_components || component_count == 0) {
    return NULL;
  }

  // Calculate total length needed
  size_t total_len = strlen(scheme) + 1; // scheme + ':'
  for (size_t i = 0; i < component_count; i++) {
    total_len += strlen(path_components[i]) + 1; // component + '/'
  }

  char *result = (char *)malloc(total_len);
  if (!result) {
    return NULL;
  }

  // Build the URI with uppercase conversion for alphanumeric QR encoding
  char *pos = result;
  for (const char *p = scheme; *p; p++) {
    *pos++ = toupper((unsigned char)*p);
  }
  *pos++ = ':';

  for (size_t i = 0; i < component_count; i++) {
    for (const char *p = path_components[i]; *p; p++) {
      *pos++ = toupper((unsigned char)*p);
    }
    if (i < component_count - 1) {
      *pos++ = '/';
    }
  }
  *pos = '\0';

  return result;
}

// Helper: Encode UR with path components
static char *encode_ur(const char **path_components, size_t component_count) {
  return encode_uri("ur", path_components, component_count);
}

bool ur_encoder_encode_single(const char *type, const uint8_t *cbor_data,
                              size_t cbor_len, char **ur_string_out) {
  if (!type || !cbor_data || cbor_len == 0 || !ur_string_out) {
    return false;
  }

  // Encode CBOR data to bytewords (minimal style, with CRC32 checksum)
  char *bytewords = NULL;
  if (!bytewords_encode(cbor_data, cbor_len, &bytewords)) {
    return false;
  }

  // Create path components: [type, bytewords]
  const char *components[2] = {type, bytewords};
  char *ur_string = encode_ur(components, 2);

  bytewords_free(bytewords);

  if (!ur_string) {
    return false;
  }

  *ur_string_out = ur_string;
  return true;
}

ur_encoder_t *ur_encoder_new(const char *type, const uint8_t *cbor_data,
                             size_t cbor_len, size_t max_fragment_len,
                             uint32_t first_seq_num, size_t min_fragment_len) {
  if (!type || !cbor_data || cbor_len == 0) {
    return NULL;
  }

  ur_encoder_t *encoder = (ur_encoder_t *)calloc(1, sizeof(ur_encoder_t));
  if (!encoder) {
    return NULL;
  }

  // Copy type
  encoder->type = (char *)malloc(strlen(type) + 1);
  if (!encoder->type) {
    free(encoder);
    return NULL;
  }
  strcpy(encoder->type, type);

  // fountain_encoder_new partitions the payload into its own fragment
  // buffers; we don't need to keep a second copy in the ur_encoder.
  encoder->fountain_encoder = fountain_encoder_new(
      cbor_data, cbor_len, max_fragment_len, first_seq_num, min_fragment_len);

  if (!encoder->fountain_encoder) {
    free(encoder->type);
    free(encoder);
    return NULL;
  }

  return encoder;
}

void ur_encoder_free(ur_encoder_t *encoder) {
  if (!encoder) {
    return;
  }

  free(encoder->type);
  fountain_encoder_free(encoder->fountain_encoder);
  free(encoder);
}

size_t ur_encoder_seq_len(const ur_encoder_t *encoder) {
  if (!encoder || !encoder->fountain_encoder) {
    return 0;
  }
  return fountain_encoder_seq_len(encoder->fountain_encoder);
}

bool ur_encoder_is_complete(const ur_encoder_t *encoder) {
  if (!encoder || !encoder->fountain_encoder) {
    return false;
  }
  return encoder->fountain_encoder->seq_num >=
         encoder->fountain_encoder->fragments.count;
}

bool ur_encoder_is_single_part(const ur_encoder_t *encoder) {
  if (!encoder || !encoder->fountain_encoder) {
    return false;
  }
  return fountain_encoder_is_single_part(encoder->fountain_encoder);
}

// Helper: Encode multi-part UR
static bool encode_part(const char *type, const fountain_encoder_part_t *part,
                        char **ur_part_out) {
  if (!type || !part || !ur_part_out) {
    return false;
  }

  // Create sequence string: "seq_num-seq_len"
  char seq_str[64];
  snprintf(seq_str, sizeof(seq_str), "%" PRIu32 "-%" PRIu32, part->seq_num,
           (uint32_t)part->seq_len);

  // Encode part to CBOR
  uint8_t *cbor = NULL;
  size_t cbor_len = 0;
  if (!fountain_encoder_part_to_cbor(part, &cbor, &cbor_len)) {
    return false;
  }

  // Encode CBOR to bytewords (with CRC32 checksum)
  char *bytewords = NULL;
  if (!bytewords_encode(cbor, cbor_len, &bytewords)) {
    free(cbor);
    return false;
  }

  free(cbor);

  // Create path components: [type, seq, bytewords]
  const char *components[3] = {type, seq_str, bytewords};
  char *ur_string = encode_ur(components, 3);

  bytewords_free(bytewords);

  if (!ur_string) {
    return false;
  }

  *ur_part_out = ur_string;
  return true;
}

bool ur_encoder_next_part(ur_encoder_t *encoder, char **ur_part_out) {
  if (!encoder || !ur_part_out) {
    return false;
  }

  // If single part, encode directly from the fountain encoder's only
  // fragment. For single-part payloads, fragment_len == cbor_len (no
  // tail padding), so we hand it straight to ur_encoder_encode_single.
  if (ur_encoder_is_single_part(encoder)) {
    const fragment_array_t *frags = &encoder->fountain_encoder->fragments;
    return ur_encoder_encode_single(encoder->type, frags->fragments[0],
                                    frags->fragment_lens[0], ur_part_out);
  }

  // Get next fountain encoder part
  fountain_encoder_part_t part;
  memset(&part, 0, sizeof(part));

  if (!fountain_encoder_next_part(encoder->fountain_encoder, &part)) {
    return false;
  }

  // Encode the part
  bool success = encode_part(encoder->type, &part, ur_part_out);

  fountain_encoder_part_free(&part);

  return success;
}
