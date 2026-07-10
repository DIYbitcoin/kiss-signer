#ifndef TEST_UTILS_H
#define TEST_UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Fragment reading utilities
char **read_fragments_from_file(const char *filepath, int *count);
void free_fragments(char **fragments, int count);

// Base64 encoding/decoding utilities
char *base64_encode(const uint8_t *data, size_t len);
bool base64_decode(const char *input, uint8_t **output, size_t *out_len);

// File reading utilities
uint8_t *read_binary_file(const char *filepath, size_t *out_len);
char *read_text_file_first_line(const char *filepath);

// Compare byte buffers. On mismatch, prints a FAIL diagnostic (length + first
// differing byte) to stderr. Returns true on match. `label` is used in the
// FAIL message and may be NULL.
bool assert_bytes_equal(const uint8_t *expected, size_t expected_len,
                        const uint8_t *actual, size_t actual_len,
                        const char *label);

#endif // TEST_UTILS_H
