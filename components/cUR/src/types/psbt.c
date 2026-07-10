#include "psbt.h"
#include "byte_buffer.h"
#include "cbor_decoder.h"
#include "cbor_encoder.h"
#include <string.h>

// PSBT registry type (tag 310)
registry_type_t PSBT_TYPE = {.name = "crypto-psbt", .tag = CRYPTO_PSBT_TAG};

// Create and destroy PSBT (just wrappers around bytes functions)
psbt_data_t *psbt_new(const uint8_t *data, size_t len) {
  return bytes_new(data, len);
}

void psbt_free(psbt_data_t *psbt) { bytes_free(psbt); }

// CBOR conversion functions
cbor_value_t *psbt_to_data_item(registry_item_t *item) {
  if (!item || !item->data)
    return NULL;

  psbt_data_t *psbt = (psbt_data_t *)item->data;

  // Return plain bytes, NO TAG (PSBT inherits from Bytes)
  return cbor_value_new_bytes(psbt->data, psbt->len);
}

registry_item_t *psbt_from_data_item(cbor_value_t *data_item) {
  if (!data_item)
    return NULL;

  // Expect plain bytes, NOT tagged (PSBT inherits from Bytes)
  if (cbor_value_get_type(data_item) != CBOR_TYPE_BYTES)
    return NULL;

  size_t len;
  const uint8_t *data = cbor_value_get_bytes(data_item, &len);
  // Allow NULL data if len is 0 (empty PSBT)
  if (!data && len > 0)
    return NULL;

  psbt_data_t *psbt = psbt_new(data, len);
  if (!psbt)
    return NULL;

  return psbt_to_registry_item(psbt);
}

// Registry item interface for PSBT
registry_item_t *psbt_to_registry_item(psbt_data_t *psbt) {
  return registry_item_new(&PSBT_TYPE, psbt, psbt_to_data_item,
                           psbt_from_data_item);
}

psbt_data_t *psbt_from_registry_item(registry_item_t *item) {
  if (!item || item->type != &PSBT_TYPE)
    return NULL;
  return (psbt_data_t *)item->data;
}

// Convenience functions
uint8_t *psbt_to_cbor(psbt_data_t *psbt, size_t *out_len) {
  if (!psbt || !out_len)
    return NULL;
  registry_item_t item = {.type = &PSBT_TYPE,
                          .data = psbt,
                          .to_data_item = psbt_to_data_item,
                          .from_data_item = NULL,
                          .free_item = NULL};
  return registry_item_to_cbor(&item, out_len);
}

static bool read_cbor_byte_string(const uint8_t *cbor_data, size_t len,
                                  const uint8_t **data, size_t *data_len) {
  if (!cbor_data || len == 0 || !data || !data_len)
    return false;

  uint8_t head = cbor_data[0];
  if ((head >> 5) != 2)
    return false;

  uint8_t additional = head & 0x1f;
  size_t header_len = 1;
  size_t value_len = 0;

  if (additional < 24) {
    value_len = additional;
  } else if (additional == 24) {
    if (len < 2)
      return false;
    value_len = cbor_data[1];
    header_len = 2;
  } else if (additional == 25) {
    if (len < 3)
      return false;
    value_len = ((size_t)cbor_data[1] << 8) | (size_t)cbor_data[2];
    header_len = 3;
  } else if (additional == 26) {
    if (len < 5)
      return false;
    value_len = ((size_t)cbor_data[1] << 24) | ((size_t)cbor_data[2] << 16) |
                ((size_t)cbor_data[3] << 8) | (size_t)cbor_data[4];
    header_len = 5;
  } else {
    return false;
  }

  if (value_len != len - header_len)
    return false;

  *data = cbor_data + header_len;
  *data_len = value_len;
  return true;
}

psbt_data_t *psbt_from_cbor(const uint8_t *cbor_data, size_t len) {
  const uint8_t *data = NULL;
  size_t data_len = 0;
  if (!read_cbor_byte_string(cbor_data, len, &data, &data_len))
    return NULL;

  return psbt_new(data, data_len);
}

// Accessors
const uint8_t *psbt_get_data(psbt_data_t *psbt, size_t *out_len) {
  return bytes_get_data(psbt, out_len);
}
