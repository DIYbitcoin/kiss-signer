#ifndef FOUNTAIN_TYPES_H
#define FOUNTAIN_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Part indexes set (simplified as dynamic array for C)
typedef struct {
  size_t *indexes;
  size_t count;
  size_t capacity;
} part_indexes_t;

// Fountain encoder part structure
typedef struct {
  uint32_t seq_num;
  size_t seq_len;
  size_t message_len;
  uint32_t checksum;
  uint8_t *data;
  size_t data_len;
} fountain_encoder_part_t;

// Internal decoder part structure
typedef struct {
  part_indexes_t indexes;
  uint8_t *data;
  size_t data_len;
} decoder_part_t;

// Helper functions for part indexes
part_indexes_t *part_indexes_new(void);
void part_indexes_free(part_indexes_t *indexes);
bool part_indexes_add(part_indexes_t *indexes, size_t index);
bool part_indexes_contains(const part_indexes_t *indexes, size_t index);
void part_indexes_clear(part_indexes_t *indexes);

// Part operations
void decoder_part_free(decoder_part_t *part);
bool decoder_part_copy(const decoder_part_t *src, decoder_part_t *dst);

#endif // FOUNTAIN_TYPES_H
