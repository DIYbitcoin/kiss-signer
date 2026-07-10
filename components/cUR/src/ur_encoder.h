#ifndef UR_ENCODER_H
#define UR_ENCODER_H

#include "fountain_encoder.h"
#include "ur.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * UR Encoder - Encodes Uniform Resources into single or multi-part format
 */

typedef struct ur_encoder ur_encoder_t;

// UR encoder structure. The CBOR payload lives inside fountain_encoder
// (partitioned into fragments); for single-part encodes we read from
// fragments[0] directly instead of keeping a second copy.
typedef struct ur_encoder {
  char *type;
  fountain_encoder_t *fountain_encoder;
} ur_encoder_t;

/**
 * Encode a single-part UR (static utility function)
 * @param type UR type
 * @param cbor_data CBOR-encoded data
 * @param cbor_len Length of CBOR data
 * @param ur_string_out Output UR string (allocated by function, must be freed
 * by caller)
 * @return true on success
 */
bool ur_encoder_encode_single(const char *type, const uint8_t *cbor_data,
                              size_t cbor_len, char **ur_string_out);

/**
 * Create new UR encoder for multi-part encoding
 * @param type UR type
 * @param cbor_data CBOR-encoded data
 * @param cbor_len Length of CBOR data
 * @param max_fragment_len Maximum fragment length
 * @param first_seq_num First sequence number (default 0)
 * @param min_fragment_len Minimum fragment length (default 10)
 * @return Pointer to encoder or NULL on error
 */
ur_encoder_t *ur_encoder_new(const char *type, const uint8_t *cbor_data,
                             size_t cbor_len, size_t max_fragment_len,
                             uint32_t first_seq_num, size_t min_fragment_len);

/**
 * Free UR encoder
 * @param encoder Pointer to encoder
 */
void ur_encoder_free(ur_encoder_t *encoder);

/**
 * Get sequence length
 * @param encoder Pointer to encoder
 * @return Sequence length
 */
size_t ur_encoder_seq_len(const ur_encoder_t *encoder);

/**
 * Check if encoder is complete
 * @param encoder Pointer to encoder
 * @return true if complete
 */
bool ur_encoder_is_complete(const ur_encoder_t *encoder);

/**
 * Check if encoder is single part
 * @param encoder Pointer to encoder
 * @return true if single part
 */
bool ur_encoder_is_single_part(const ur_encoder_t *encoder);

/**
 * Generate next UR part (sequential retrieval)
 * @param encoder Pointer to encoder
 * @param ur_part_out Output UR part string (allocated by function, must be
 * freed by caller)
 * @return true on success
 */
bool ur_encoder_next_part(ur_encoder_t *encoder, char **ur_part_out);

#endif // UR_ENCODER_H
