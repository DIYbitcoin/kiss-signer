#ifndef URTYPES_OUTPUT_H
#define URTYPES_OUTPUT_H

#include "hd_key.h"
#include "multi_key.h"
#include "registry.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Output tag number
#define CRYPTO_OUTPUT_TAG 308

// Account tag number (for helper function)
#define CRYPTO_ACCOUNT_TAG 311

// Script expression tags (from BCR-2020-010)
#define SCRIPT_EXPR_ADDR 307
#define SCRIPT_EXPR_SH 400
#define SCRIPT_EXPR_WSH 401
#define SCRIPT_EXPR_PK 402
#define SCRIPT_EXPR_PKH 403
#define SCRIPT_EXPR_WPKH 404
#define SCRIPT_EXPR_COMBO 405
#define SCRIPT_EXPR_MULTI 406
#define SCRIPT_EXPR_SORTEDMULTI 407
#define SCRIPT_EXPR_RAW 408
#define SCRIPT_EXPR_TR 409
#define SCRIPT_EXPR_COSIGNER 410

// Script expression structure
typedef struct {
  uint64_t tag;
  const char *expression;
} script_expression_t;

// Key type enum
typedef enum { KEY_TYPE_HD, KEY_TYPE_MULTI } key_type_t;

// Output type structure
typedef struct {
  script_expression_t **script_expressions;
  size_t script_expression_count;
  key_type_t key_type;
  union {
    hd_key_data_t *hd_key;
    multi_key_data_t *multi_key;
  } crypto_key;
} output_data_t;

// Output registry type
extern registry_type_t OUTPUT_TYPE;

// Create and destroy Output
output_data_t *output_new(void);
void output_free(output_data_t *output);

// Registry item interface for Output
registry_item_t *output_to_registry_item(output_data_t *output);
output_data_t *output_from_registry_item(registry_item_t *item);

// CBOR conversion functions
registry_item_t *output_from_data_item(cbor_value_t *data_item);
output_data_t *output_from_cbor(const uint8_t *cbor_data, size_t len);
cbor_value_t *output_to_data_item(output_data_t *output);
uint8_t *output_to_cbor(output_data_t *output, size_t *out_len);

// Parse descriptor string into output_data_t
output_data_t *output_from_descriptor_string(const char *descriptor);

// Generate output descriptor string
char *output_descriptor(output_data_t *output, bool include_checksum);

// Helper function to extract first output descriptor from Account CBOR
char *output_descriptor_from_cbor_account(const uint8_t *account_cbor,
                                          size_t len);

// Script expression helpers
const script_expression_t *get_script_expression_by_tag(uint64_t tag);

#endif // URTYPES_OUTPUT_H
