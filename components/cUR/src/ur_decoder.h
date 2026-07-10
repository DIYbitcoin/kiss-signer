#ifndef UR_DECODER_H
#define UR_DECODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  UR_DECODER_OK = 0,
  UR_DECODER_ERROR_INVALID_SCHEME,
  UR_DECODER_ERROR_INVALID_TYPE,
  UR_DECODER_ERROR_INVALID_PATH_LENGTH,
  UR_DECODER_ERROR_INVALID_SEQUENCE_COMPONENT,
  UR_DECODER_ERROR_INVALID_FRAGMENT,
  UR_DECODER_ERROR_INVALID_PART,
  UR_DECODER_ERROR_INVALID_CHECKSUM,
  UR_DECODER_ERROR_MEMORY,
  UR_DECODER_ERROR_NULL_POINTER
} ur_decoder_error_t;

typedef struct ur_decoder ur_decoder_t;
typedef struct fountain_decoder fountain_decoder_t;

typedef struct {
  char *type;
  uint8_t *cbor_data;
  size_t cbor_len;
} ur_result_t;

typedef struct ur_decoder {
  fountain_decoder_t *fountain_decoder;
  char *expected_type;
  ur_result_t *result;
  bool is_complete_flag;
  ur_decoder_error_t last_error;
} ur_decoder_t;

/**
 * Create a new URDecoder instance
 * @return Pointer to URDecoder instance or NULL on error
 */
ur_decoder_t *ur_decoder_new(void);

/**
 * Free URDecoder instance
 * @param decoder Pointer to URDecoder instance
 */
void ur_decoder_free(ur_decoder_t *decoder);

/**
 * Receive and process a UR part
 * @param decoder Pointer to URDecoder instance
 * @param part_str UR part string to process
 * @return true on success, false on error
 */
bool ur_decoder_receive_part(ur_decoder_t *decoder, const char *part_str);

/**
 * Check if decoding is complete
 * @param decoder Pointer to URDecoder instance
 * @return true if complete, false otherwise
 */
bool ur_decoder_is_complete(ur_decoder_t *decoder);

/**
 * Check if decoding was successful
 * @param decoder Pointer to URDecoder instance
 * @return true if successful, false otherwise
 */
bool ur_decoder_is_success(ur_decoder_t *decoder);

/**
 * Get the decoded result
 * @param decoder Pointer to URDecoder instance
 * @return Pointer to ur_result_t or NULL if not complete/failed
 */
ur_result_t *ur_decoder_get_result(ur_decoder_t *decoder);

/**
 * Get expected part count
 * @param decoder Pointer to URDecoder instance
 * @return Expected part count
 */
size_t ur_decoder_expected_part_count(ur_decoder_t *decoder);

/**
 * Get processed parts count
 * @param decoder Pointer to URDecoder instance
 * @return Processed parts count
 */
size_t ur_decoder_processed_parts_count(ur_decoder_t *decoder);

/**
 * Get estimated completion percentage
 * @param decoder Pointer to URDecoder instance
 * @return Completion percentage (0.0 to 1.0)
 */
double ur_decoder_estimated_percent_complete(ur_decoder_t *decoder);

/**
 * Get last error
 * @param decoder Pointer to URDecoder instance
 * @return Last error code
 */
ur_decoder_error_t ur_decoder_get_last_error(ur_decoder_t *decoder);

void ur_result_free(ur_result_t *result);

#endif // UR_DECODER_H