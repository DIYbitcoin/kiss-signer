#ifndef FOUNTAIN_ENCODER_H
#define FOUNTAIN_ENCODER_H

#include "fountain_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Fountain Encoder for Luby Transform rateless coding
 * https://en.wikipedia.org/wiki/Luby_transform_code
 */

// Forward declarations
typedef struct fountain_encoder fountain_encoder_t;

// Fragment storage (array of byte arrays)
typedef struct {
  uint8_t **fragments;
  size_t *fragment_lens;
  size_t count;
  size_t capacity;
} fragment_array_t;

// Fountain encoder structure
typedef struct fountain_encoder {
  size_t message_len;
  uint32_t checksum;
  size_t fragment_len;
  fragment_array_t fragments;
  uint32_t seq_num;
  part_indexes_t last_part_indexes;
} fountain_encoder_t;

/**
 * Find nominal fragment length for given message
 * @param message_len Length of message
 * @param min_fragment_len Minimum fragment length
 * @param max_fragment_len Maximum fragment length
 * @return Optimal fragment length, or 0 on error
 */
size_t fountain_encoder_find_nominal_fragment_length(size_t message_len,
                                                     size_t min_fragment_len,
                                                     size_t max_fragment_len);

/**
 * Partition message into fragments
 * @param message Message data
 * @param message_len Message length
 * @param fragment_len Fragment length
 * @param fragments Output fragment array (allocated by function)
 * @return true on success
 */
bool fountain_encoder_partition_message(const uint8_t *message,
                                        size_t message_len, size_t fragment_len,
                                        fragment_array_t *fragments);

/**
 * Create new fountain encoder
 * @param message Message to encode
 * @param message_len Message length
 * @param max_fragment_len Maximum fragment length
 * @param first_seq_num First sequence number (default 0)
 * @param min_fragment_len Minimum fragment length (default 10)
 * @return Pointer to encoder or NULL on error
 */
fountain_encoder_t *fountain_encoder_new(const uint8_t *message,
                                         size_t message_len,
                                         size_t max_fragment_len,
                                         uint32_t first_seq_num,
                                         size_t min_fragment_len);

/**
 * Free fountain encoder
 * @param encoder Pointer to encoder
 */
void fountain_encoder_free(fountain_encoder_t *encoder);

/**
 * Get sequence length (total number of fragments)
 * @param encoder Pointer to encoder
 * @return Sequence length
 */
size_t fountain_encoder_seq_len(const fountain_encoder_t *encoder);

/**
 * Check if encoder will generate only single part
 * @param encoder Pointer to encoder
 * @return true if single part
 */
bool fountain_encoder_is_single_part(const fountain_encoder_t *encoder);

/**
 * Generate next part
 * @param encoder Pointer to encoder
 * @param part Output part (allocated by caller)
 * @return true on success
 */
bool fountain_encoder_next_part(fountain_encoder_t *encoder,
                                fountain_encoder_part_t *part);

/**
 * Encode part to CBOR
 * @param part Part to encode
 * @param cbor_out Output CBOR buffer (allocated by function)
 * @param cbor_len Output CBOR length
 * @return true on success
 */
bool fountain_encoder_part_to_cbor(const fountain_encoder_part_t *part,
                                   uint8_t **cbor_out, size_t *cbor_len);

/**
 * Free encoder part data
 * @param part Part to free
 */
void fountain_encoder_part_free(fountain_encoder_part_t *part);

// Fragment array operations
bool fragment_array_init(fragment_array_t *arr, size_t capacity);
void fragment_array_free(fragment_array_t *arr);
bool fragment_array_add(fragment_array_t *arr, const uint8_t *data, size_t len);

#endif // FOUNTAIN_ENCODER_H
