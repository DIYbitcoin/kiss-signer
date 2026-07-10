//
// fountain_encoder.c
//
// Copyright © 2025 Krux Contributors
// Licensed under the "BSD-2-Clause Plus Patent License"
//
// Implementation of fountain codes for efficient multi-part data transmission.
// Based on the specification:
// https://github.com/BlockchainCommons/Research/blob/master/papers/bcr-2020-006-urtypes.md
//
// This is an independent implementation written using
// foundation-ur-py as a reference for testing and validation.
//

#include "fountain_encoder.h"
#include "crc32.h"
#include "fountain_utils.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Lightweight CBOR encoder
// ---------------------------------------------------------------------------

// CBOR major types
#define CBOR_MAJOR_UNSIGNED 0
#define CBOR_MAJOR_BYTES (2 << 5)
#define CBOR_MAJOR_ARRAY (4 << 5)

// CBOR additional info values
#define CBOR_MINOR_LENGTH1 24
#define CBOR_MINOR_LENGTH2 25
#define CBOR_MINOR_LENGTH4 26
#define CBOR_MINOR_LENGTH8 27

typedef struct {
  uint8_t *buffer;
  size_t capacity;
  size_t size;
} cbor_lite_encoder_t;

static size_t cbor_lite_get_byte_length(uint64_t value) {
  if (value < 24)
    return 0;
  if (value <= 0xFF)
    return 1;
  if (value <= 0xFFFF)
    return 2;
  if (value <= 0xFFFFFFFF)
    return 4;
  return 8;
}

#define CBOR_ENCODER_MAX_CAPACITY (1024 * 1024) /* 1MB ceiling */

static bool cbor_lite_ensure_capacity(cbor_lite_encoder_t *enc,
                                      size_t additional) {
  if (!enc)
    return false;

  size_t required = enc->size + additional;
  if (required <= enc->capacity)
    return true;

  if (required > CBOR_ENCODER_MAX_CAPACITY)
    return false;

  size_t new_capacity = enc->capacity * 3 / 2;
  if (new_capacity < required)
    new_capacity = required;
  if (new_capacity > CBOR_ENCODER_MAX_CAPACITY)
    new_capacity = CBOR_ENCODER_MAX_CAPACITY;

  uint8_t *new_buffer = (uint8_t *)realloc(enc->buffer, new_capacity);
  if (!new_buffer)
    return false;

  enc->buffer = new_buffer;
  enc->capacity = new_capacity;
  return true;
}

static bool cbor_lite_append_byte(cbor_lite_encoder_t *enc, uint8_t byte) {
  if (!cbor_lite_ensure_capacity(enc, 1))
    return false;
  enc->buffer[enc->size++] = byte;
  return true;
}

static bool cbor_lite_encode_tag_and_value(cbor_lite_encoder_t *enc,
                                           uint8_t major_type, uint64_t value) {
  size_t length = cbor_lite_get_byte_length(value);

  if (length == 0) {
    return cbor_lite_append_byte(enc, major_type + (uint8_t)value);
  }

  // One capacity check covers the whole head; write bytes directly
  // instead of routing each through cbor_lite_append_byte().
  if (!cbor_lite_ensure_capacity(enc, 1 + length))
    return false;

  uint8_t *p = enc->buffer + enc->size;
  switch (length) {
  case 1:
    p[0] = major_type + CBOR_MINOR_LENGTH1;
    p[1] = (uint8_t)value;
    break;
  case 2:
    p[0] = major_type + CBOR_MINOR_LENGTH2;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)value;
    break;
  case 4:
    p[0] = major_type + CBOR_MINOR_LENGTH4;
    p[1] = (uint8_t)(value >> 24);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 8);
    p[4] = (uint8_t)value;
    break;
  default: // 8
    p[0] = major_type + CBOR_MINOR_LENGTH8;
    p[1] = (uint8_t)(value >> 56);
    p[2] = (uint8_t)(value >> 48);
    p[3] = (uint8_t)(value >> 40);
    p[4] = (uint8_t)(value >> 32);
    p[5] = (uint8_t)(value >> 24);
    p[6] = (uint8_t)(value >> 16);
    p[7] = (uint8_t)(value >> 8);
    p[8] = (uint8_t)value;
    break;
  }
  enc->size += 1 + length;
  return true;
}

static bool cbor_lite_encoder_init(cbor_lite_encoder_t *enc,
                                   size_t initial_capacity) {
  if (!enc)
    return false;

  enc->buffer = (uint8_t *)malloc(initial_capacity);
  if (!enc->buffer)
    return false;

  enc->capacity = initial_capacity;
  enc->size = 0;
  return true;
}

static void cbor_lite_encoder_free(cbor_lite_encoder_t *enc) {
  if (enc) {
    safe_free(enc->buffer);
    enc->capacity = 0;
    enc->size = 0;
  }
}

// Takes ownership of the encoder's buffer. After this call the encoder is
// drained and only needs cbor_lite_encoder_free for consistency.
static uint8_t *cbor_lite_encoder_get_buffer(cbor_lite_encoder_t *enc,
                                             size_t *size_out) {
  if (!enc || !size_out || enc->size == 0)
    return NULL;

  *size_out = enc->size;
  uint8_t *result = enc->buffer;

  if (enc->size < enc->capacity) {
    uint8_t *shrunk = (uint8_t *)realloc(result, enc->size);
    if (shrunk)
      result = shrunk;
  }

  enc->buffer = NULL;
  enc->size = 0;
  enc->capacity = 0;
  return result;
}

static bool cbor_lite_encode_unsigned(cbor_lite_encoder_t *enc,
                                      uint64_t value) {
  return cbor_lite_encode_tag_and_value(enc, CBOR_MAJOR_UNSIGNED, value);
}

static bool cbor_lite_encode_array_start(cbor_lite_encoder_t *enc,
                                         size_t count) {
  return cbor_lite_encode_tag_and_value(enc, CBOR_MAJOR_ARRAY, count);
}

static bool cbor_lite_encode_bytes(cbor_lite_encoder_t *enc,
                                   const uint8_t *data, size_t len) {
  if (!enc || !data)
    return false;

  if (!cbor_lite_encode_tag_and_value(enc, CBOR_MAJOR_BYTES, len))
    return false;

  if (!cbor_lite_ensure_capacity(enc, len))
    return false;

  memcpy(enc->buffer + enc->size, data, len);
  enc->size += len;
  return true;
}

// ---------------------------------------------------------------------------

// Fragment array operations

bool fragment_array_init(fragment_array_t *arr, size_t capacity) {
  if (!arr || capacity == 0)
    return false;

  arr->fragments = (uint8_t **)calloc(capacity, sizeof(uint8_t *));
  arr->fragment_lens = (size_t *)calloc(capacity, sizeof(size_t));
  if (!arr->fragments || !arr->fragment_lens) {
    free(arr->fragments);
    free(arr->fragment_lens);
    return false;
  }

  arr->count = 0;
  arr->capacity = capacity;
  return true;
}

void fragment_array_free(fragment_array_t *arr) {
  if (!arr)
    return;

  if (arr->fragments) {
    for (size_t i = 0; i < arr->count; i++) {
      free(arr->fragments[i]);
    }
    safe_free(arr->fragments);
  }

  safe_free(arr->fragment_lens);
  arr->count = 0;
  arr->capacity = 0;
}

bool fragment_array_add(fragment_array_t *arr, const uint8_t *data,
                        size_t len) {
  if (!arr || !data || arr->count >= arr->capacity)
    return false;

  arr->fragments[arr->count] = (uint8_t *)malloc(len);
  if (!arr->fragments[arr->count])
    return false;

  memcpy(arr->fragments[arr->count], data, len);
  arr->fragment_lens[arr->count] = len;
  arr->count++;
  return true;
}

// Encoder functions

size_t fountain_encoder_find_nominal_fragment_length(size_t message_len,
                                                     size_t min_fragment_len,
                                                     size_t max_fragment_len) {
  if (message_len == 0 || min_fragment_len == 0 ||
      max_fragment_len < min_fragment_len) {
    return 0;
  }

  size_t max_fragment_count = message_len / min_fragment_len;
  size_t fragment_len = 0;

  for (size_t fragment_count = 1; fragment_count <= max_fragment_count;
       fragment_count++) {
    // Integer ceiling division: ceil(a/b) = (a + b - 1) / b
    fragment_len = (message_len + fragment_count - 1) / fragment_count;
    if (fragment_len <= max_fragment_len) {
      break;
    }
  }

  return fragment_len > 0 ? fragment_len : 0;
}

bool fountain_encoder_partition_message(const uint8_t *message,
                                        size_t message_len, size_t fragment_len,
                                        fragment_array_t *fragments) {
  if (!message || !fragments || message_len == 0 || fragment_len == 0) {
    return false;
  }

  size_t num_fragments = (message_len + fragment_len - 1) / fragment_len;

  if (!fragment_array_init(fragments, num_fragments)) {
    return false;
  }

  // Install each fragment directly into the array to avoid the extra
  // malloc + memcpy that fragment_array_add would do on top of the
  // local buffer. Only the last fragment may need tail zero-padding.
  size_t offset = 0;
  while (offset < message_len) {
    uint8_t *fragment = (uint8_t *)malloc(fragment_len);
    if (!fragment) {
      fragment_array_free(fragments);
      return false;
    }

    size_t copy_len = message_len - offset;
    if (copy_len > fragment_len) {
      copy_len = fragment_len;
    }
    memcpy(fragment, message + offset, copy_len);
    if (copy_len < fragment_len) {
      memset(fragment + copy_len, 0, fragment_len - copy_len);
    }

    fragments->fragments[fragments->count] = fragment;
    fragments->fragment_lens[fragments->count] = fragment_len;
    fragments->count++;
    offset += fragment_len;
  }

  return true;
}

fountain_encoder_t *fountain_encoder_new(const uint8_t *message,
                                         size_t message_len,
                                         size_t max_fragment_len,
                                         uint32_t first_seq_num,
                                         size_t min_fragment_len) {
  if (!message || message_len == 0) {
    return NULL;
  }

  fountain_encoder_t *encoder =
      (fountain_encoder_t *)calloc(1, sizeof(fountain_encoder_t));
  if (!encoder) {
    return NULL;
  }

  encoder->message_len = message_len;
  encoder->checksum = crc32_calculate(message, message_len);
  encoder->fragment_len = fountain_encoder_find_nominal_fragment_length(
      message_len, min_fragment_len, max_fragment_len);

  if (encoder->fragment_len == 0) {
    free(encoder);
    return NULL;
  }

  if (!fountain_encoder_partition_message(
          message, message_len, encoder->fragment_len, &encoder->fragments)) {
    free(encoder);
    return NULL;
  }

  encoder->seq_num = first_seq_num;

  // Initialize last_part_indexes (allocate it properly)
  encoder->last_part_indexes.indexes = NULL;
  encoder->last_part_indexes.count = 0;
  encoder->last_part_indexes.capacity = 0;

  return encoder;
}

void fountain_encoder_free(fountain_encoder_t *encoder) {
  if (!encoder) {
    return;
  }

  fragment_array_free(&encoder->fragments);
  // Free indexes array only, not the struct itself (it's embedded)
  free(encoder->last_part_indexes.indexes);
  free(encoder);
}

size_t fountain_encoder_seq_len(const fountain_encoder_t *encoder) {
  return encoder ? encoder->fragments.count : 0;
}

bool fountain_encoder_is_single_part(const fountain_encoder_t *encoder) {
  return encoder && encoder->fragments.count == 1;
}

// XOR mix fragments
static bool mix_fragments(const fountain_encoder_t *encoder,
                          const part_indexes_t *indexes, uint8_t **result_out,
                          size_t *result_len) {
  if (!encoder || !indexes || !result_out || !result_len) {
    return false;
  }

  uint8_t *result = (uint8_t *)calloc(encoder->fragment_len, 1);
  if (!result) {
    return false;
  }

  for (size_t i = 0; i < indexes->count; i++) {
    size_t index = indexes->indexes[i];
    if (index >= encoder->fragments.count) {
      free(result);
      return false;
    }

    const uint8_t *fragment = encoder->fragments.fragments[index];
    for (size_t j = 0; j < encoder->fragment_len; j++) {
      result[j] ^= fragment[j];
    }
  }

  *result_out = result;
  *result_len = encoder->fragment_len;
  return true;
}

bool fountain_encoder_next_part(fountain_encoder_t *encoder,
                                fountain_encoder_part_t *part) {
  if (!encoder || !part) {
    return false;
  }

  // Increment sequence number
  encoder->seq_num++;

  // Choose fragments for this part
  part_indexes_t *indexes = part_indexes_new();
  if (!indexes) {
    return false;
  }

  if (!choose_fragments(encoder->seq_num, encoder->fragments.count,
                        encoder->checksum, indexes)) {
    part_indexes_free(indexes);
    return false;
  }

  // Mix fragments
  uint8_t *mixed_data = NULL;
  size_t mixed_len = 0;
  if (!mix_fragments(encoder, indexes, &mixed_data, &mixed_len)) {
    part_indexes_free(indexes);
    return false;
  }

  // Update last_part_indexes (free old indexes array only, not the struct)
  safe_free(encoder->last_part_indexes.indexes);
  encoder->last_part_indexes.count = 0;
  encoder->last_part_indexes.capacity = 0;

  part_indexes_copy(indexes, &encoder->last_part_indexes);

  // Fill part structure
  part->seq_num = encoder->seq_num;
  part->seq_len = encoder->fragments.count;
  part->message_len = encoder->message_len;
  part->checksum = encoder->checksum;
  part->data = mixed_data;
  part->data_len = mixed_len;

  part_indexes_free(indexes);
  return true;
}

bool fountain_encoder_part_to_cbor(const fountain_encoder_part_t *part,
                                   uint8_t **cbor_out, size_t *cbor_len) {
  if (!part || !cbor_out || !cbor_len || !part->data) {
    return false;
  }

  // Create encoder
  cbor_lite_encoder_t encoder;
  if (!cbor_lite_encoder_init(&encoder, 64 + part->data_len)) {
    return false;
  }

  // Encode array of 5 elements
  if (!cbor_lite_encode_array_start(&encoder, 5)) {
    cbor_lite_encoder_free(&encoder);
    return false;
  }

  // Encode fields: seq_num, seq_len, message_len, checksum, data
  if (!cbor_lite_encode_unsigned(&encoder, part->seq_num) ||
      !cbor_lite_encode_unsigned(&encoder, part->seq_len) ||
      !cbor_lite_encode_unsigned(&encoder, part->message_len) ||
      !cbor_lite_encode_unsigned(&encoder, part->checksum) ||
      !cbor_lite_encode_bytes(&encoder, part->data, part->data_len)) {
    cbor_lite_encoder_free(&encoder);
    return false;
  }

  // Get result buffer (makes a copy)
  *cbor_out = cbor_lite_encoder_get_buffer(&encoder, cbor_len);

  // Free encoder
  cbor_lite_encoder_free(&encoder);

  return *cbor_out != NULL;
}

void fountain_encoder_part_free(fountain_encoder_part_t *part) {
  if (!part) {
    return;
  }
  safe_free(part->data);
  part->data_len = 0;
}
