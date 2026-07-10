#ifndef UR_H
#define UR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  char *type;
  uint8_t *cbor;
  size_t cbor_len;
} ur_t;

/**
 * Create a new UR object
 * @param type UR type string (will be copied)
 * @param cbor CBOR data (will be copied)
 * @param cbor_len Length of CBOR data
 * @return Pointer to UR object or NULL on error
 */
ur_t *ur_new(const char *type, const uint8_t *cbor, size_t cbor_len);

/**
 * Free UR object
 * @param ur Pointer to UR object
 */
void ur_free(ur_t *ur);

/**
 * Get UR type
 * @param ur Pointer to UR object
 * @return Type string (do not free)
 */
const char *ur_get_type(const ur_t *ur);

/**
 * Get UR CBOR data
 * @param ur Pointer to UR object
 * @return CBOR data pointer (do not free)
 */
const uint8_t *ur_get_cbor(const ur_t *ur);

/**
 * Get UR CBOR data length
 * @param ur Pointer to UR object
 * @return CBOR data length
 */
size_t ur_get_cbor_len(const ur_t *ur);

/**
 * Create UR from ur_result_t (convenience function)
 * Note: Include ur_decoder.h before using this function
 * @param result ur_result_t from decoder
 * @return Pointer to UR object or NULL on error
 */
struct ur_result;
ur_t *ur_from_result(const struct ur_result *result);

#endif // UR_H