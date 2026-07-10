#ifndef URTYPES_PSBT_H
#define URTYPES_PSBT_H

#include "bytes_type.h"
#include "registry.h"
#include <stddef.h>
#include <stdint.h>

// PSBT tag number
#define CRYPTO_PSBT_TAG 310

// PSBT type (extends Bytes with tag 310)
typedef bytes_data_t psbt_data_t;

// PSBT registry type
extern registry_type_t PSBT_TYPE;

// Create and destroy PSBT
psbt_data_t *psbt_new(const uint8_t *data, size_t len);
void psbt_free(psbt_data_t *psbt);

// Registry item interface for PSBT
registry_item_t *psbt_to_registry_item(psbt_data_t *psbt);
psbt_data_t *psbt_from_registry_item(registry_item_t *item);

// CBOR conversion functions
cbor_value_t *psbt_to_data_item(registry_item_t *item);
registry_item_t *psbt_from_data_item(cbor_value_t *data_item);

// Convenience functions
uint8_t *psbt_to_cbor(psbt_data_t *psbt, size_t *out_len);
psbt_data_t *psbt_from_cbor(const uint8_t *cbor_data, size_t len);

// Accessors
const uint8_t *psbt_get_data(psbt_data_t *psbt, size_t *out_len);

#endif // URTYPES_PSBT_H
