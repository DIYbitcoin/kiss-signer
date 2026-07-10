#include "cbor_encoder.h"
#include <string.h>

// Encode CBOR head (major type + argument value)
static bool encode_head(byte_buffer_t *buf, uint8_t major_type,
                        uint64_t value) {
  uint8_t mt = major_type << 5;
  if (value < 24) {
    uint8_t byte = mt | (uint8_t)value;
    return byte_buffer_append_byte(buf, byte);
  } else if (value <= 0xFF) {
    uint8_t bytes[2] = {mt | 24, (uint8_t)value};
    return byte_buffer_append(buf, bytes, 2);
  } else if (value <= 0xFFFF) {
    uint8_t bytes[3] = {mt | 25, (uint8_t)(value >> 8), (uint8_t)value};
    return byte_buffer_append(buf, bytes, 3);
  } else if (value <= 0xFFFFFFFF) {
    uint8_t bytes[5] = {mt | 26, (uint8_t)(value >> 24), (uint8_t)(value >> 16),
                        (uint8_t)(value >> 8), (uint8_t)value};
    return byte_buffer_append(buf, bytes, 5);
  } else {
    uint8_t bytes[9] = {mt | 27,
                        (uint8_t)(value >> 56),
                        (uint8_t)(value >> 48),
                        (uint8_t)(value >> 40),
                        (uint8_t)(value >> 32),
                        (uint8_t)(value >> 24),
                        (uint8_t)(value >> 16),
                        (uint8_t)(value >> 8),
                        (uint8_t)value};
    return byte_buffer_append(buf, bytes, 9);
  }
}

static bool encode_value(byte_buffer_t *buf, cbor_value_t *value) {
  if (!value)
    return false;

  switch (value->type) {
  case CBOR_TYPE_UNSIGNED_INT:
    return encode_head(buf, 0, value->value.uint_val);

  case CBOR_TYPE_BYTES: {
    size_t len;
    const uint8_t *data = cbor_value_get_bytes(value, &len);
    if (!encode_head(buf, 2, len))
      return false;
    if (len > 0)
      return byte_buffer_append(buf, data, len);
    return true;
  }

  case CBOR_TYPE_STRING: {
    const char *str = cbor_value_get_string(value);
    if (!str)
      return false;
    size_t len = strlen(str);
    if (!encode_head(buf, 3, len))
      return false;
    if (len > 0)
      return byte_buffer_append(buf, (const uint8_t *)str, len);
    return true;
  }

  case CBOR_TYPE_ARRAY: {
    size_t count = cbor_value_get_array_size(value);
    if (!encode_head(buf, 4, count))
      return false;
    for (size_t i = 0; i < count; i++) {
      if (!encode_value(buf, cbor_value_get_array_item(value, i)))
        return false;
    }
    return true;
  }

  case CBOR_TYPE_MAP: {
    size_t count = value->value.map_val.count;
    if (!encode_head(buf, 5, count))
      return false;
    for (size_t i = 0; i < count; i++) {
      if (!encode_value(buf, value->value.map_val.keys[i]))
        return false;
      if (!encode_value(buf, value->value.map_val.values[i]))
        return false;
    }
    return true;
  }

  case CBOR_TYPE_TAG: {
    uint64_t tag = cbor_value_get_tag(value);
    if (!encode_head(buf, 6, tag))
      return false;
    return encode_value(buf, cbor_value_get_tag_content(value));
  }

  case CBOR_TYPE_BOOL:
    return byte_buffer_append_byte(buf,
                                   cbor_value_get_bool(value) ? 0xF5 : 0xF4);

  default:
    return false;
  }
}

// Create and destroy encoder
urtypes_cbor_encoder_t *urtypes_cbor_encoder_new(void) {
  urtypes_cbor_encoder_t *encoder = safe_malloc(sizeof(urtypes_cbor_encoder_t));
  if (!encoder)
    return NULL;

  encoder->buffer = byte_buffer_new();
  if (!encoder->buffer) {
    free(encoder);
    return NULL;
  }

  return encoder;
}

void urtypes_cbor_encoder_free(urtypes_cbor_encoder_t *encoder) {
  if (!encoder)
    return;

  byte_buffer_free(encoder->buffer);
  free(encoder);
}

// Encode CBOR value
bool urtypes_cbor_encoder_encode(urtypes_cbor_encoder_t *encoder,
                                 cbor_value_t *value) {
  if (!encoder || !value)
    return false;

  return encode_value(encoder->buffer, value);
}

// Get encoded data
uint8_t *urtypes_cbor_encoder_get_data(urtypes_cbor_encoder_t *encoder,
                                       size_t *out_len) {
  if (!encoder || !out_len)
    return NULL;

  *out_len = byte_buffer_get_len(encoder->buffer);

  // Return a copy of the data
  uint8_t *data = safe_malloc(*out_len);
  if (!data)
    return NULL;

  memcpy(data, byte_buffer_get_data(encoder->buffer), *out_len);
  return data;
}

// Convenience function to encode a value to bytes
uint8_t *cbor_encode(cbor_value_t *value, size_t *out_len) {
  if (!value || !out_len)
    return NULL;

  urtypes_cbor_encoder_t *encoder = urtypes_cbor_encoder_new();
  if (!encoder)
    return NULL;

  if (!urtypes_cbor_encoder_encode(encoder, value)) {
    urtypes_cbor_encoder_free(encoder);
    return NULL;
  }

  // Steal the buffer's storage instead of copying it; the encoder is
  // freed immediately after, so nothing else can read from it.
  byte_buffer_t *buf = encoder->buffer;
  uint8_t *data = buf->data;
  *out_len = buf->len;
  buf->data = NULL;
  buf->len = 0;
  buf->capacity = 0;
  urtypes_cbor_encoder_free(encoder);

  if (!data || *out_len == 0) {
    free(data);
    *out_len = 0;
    return NULL;
  }

  // Shrink-to-fit: the buffer doubles on grow, so capacity can be up to
  // 2× the encoded size. Release the slack for embedded heaps.
  uint8_t *shrunk = safe_realloc(data, *out_len);
  return shrunk ? shrunk : data;
}
