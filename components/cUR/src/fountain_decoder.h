#ifndef FOUNTAIN_DECODER_H
#define FOUNTAIN_DECODER_H

// #define DEBUG_STATS

#include "fountain_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Opaque decoder type
typedef struct fountain_decoder fountain_decoder_t;

// Fountain decoder result
typedef struct {
  uint8_t *data;
  size_t data_len;
  bool is_success;
  bool is_error;
} fountain_decoder_result_t;

// Function declarations

/**
 * Create a new fountain decoder
 * @return Pointer to fountain decoder or NULL on error
 */
fountain_decoder_t *fountain_decoder_new(void);

/**
 * Free fountain decoder
 * @param decoder Pointer to fountain decoder
 */
void fountain_decoder_free(fountain_decoder_t *decoder);

/**
 * Receive a fountain encoder part
 * @param decoder Pointer to fountain decoder
 * @param part Pointer to encoder part
 * @return true on success, false on error
 */
bool fountain_decoder_receive_part(fountain_decoder_t *decoder,
                                   fountain_encoder_part_t *part);

/**
 * Check if decoding is complete
 * @param decoder Pointer to fountain decoder
 * @return true if complete, false otherwise
 */
bool fountain_decoder_is_complete(fountain_decoder_t *decoder);

/**
 * Check if decoding was successful
 * @param decoder Pointer to fountain decoder
 * @return true if successful, false otherwise
 */
bool fountain_decoder_is_success(fountain_decoder_t *decoder);

/**
 * Get expected part count
 * @param decoder Pointer to fountain decoder
 * @return Expected part count
 */
size_t fountain_decoder_expected_part_count(fountain_decoder_t *decoder);

/**
 * Get processed parts count
 * @param decoder Pointer to fountain decoder
 * @return Number of processed parts
 */
size_t fountain_decoder_processed_parts_count(fountain_decoder_t *decoder);

/**
 * Get estimated completion percentage
 * @param decoder Pointer to fountain decoder
 * @return Completion percentage (0.0 to 1.0)
 */
double fountain_decoder_estimated_percent_complete(fountain_decoder_t *decoder);

/**
 * Get result message
 * @param decoder Pointer to fountain decoder
 * @return Pointer to result data or NULL
 */
uint8_t *fountain_decoder_result_message(fountain_decoder_t *decoder);

/**
 * Get result message length
 * @param decoder Pointer to fountain decoder
 * @return Result data length
 */
size_t fountain_decoder_result_message_len(fountain_decoder_t *decoder);

/**
 * Transfer ownership of the result message to the caller. The decoder
 * drops its reference (internal result->data set to NULL) so a later
 * fountain_decoder_free does not double-free. Returns NULL if no result
 * is available.
 * @param decoder Pointer to fountain decoder
 * @return Heap pointer the caller must free, or NULL
 */
uint8_t *fountain_decoder_take_result_message(fountain_decoder_t *decoder);

#endif // FOUNTAIN_DECODER_H
