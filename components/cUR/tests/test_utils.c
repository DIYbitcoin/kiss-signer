#define _POSIX_C_SOURCE 200809L
#include "test_utils.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAGMENTS 1000
#define MAX_LINE_LENGTH 2048

// Function to read fragments from a file
char **read_fragments_from_file(const char *filepath, int *count) {
  FILE *file = fopen(filepath, "r");
  if (!file) {
    fprintf(stderr, "Failed to open file: %s\n", filepath);
    return NULL;
  }

  char **fragments = (char **)malloc(MAX_FRAGMENTS * sizeof(char *));
  if (!fragments) {
    fclose(file);
    return NULL;
  }

  char line[MAX_LINE_LENGTH];
  *count = 0;

  while (fgets(line, sizeof(line), file) && *count < MAX_FRAGMENTS) {
    // Remove newline
    line[strcspn(line, "\r\n")] = 0;

    // Skip empty lines
    if (strlen(line) == 0) {
      continue;
    }

    // Find the start of the UR string (case-insensitive)
    char *start = NULL;
    char *pos = line;
    while (*pos) {
      if ((pos[0] == 'u' || pos[0] == 'U') &&
          (pos[1] == 'r' || pos[1] == 'R') && pos[2] == ':') {
        start = pos;
        break;
      }
      pos++;
    }

    if (!start)
      continue;

    // Find the end (before quote or comma if present)
    char *end = start;
    while (*end && *end != '"' && *end != ',' && *end != '\r' && *end != '\n') {
      end++;
    }

    size_t len = end - start;
    if (len > 0) {
      fragments[*count] = (char *)malloc(len + 1);
      if (!fragments[*count]) {
        // Cleanup on failure
        for (int i = 0; i < *count; i++) {
          free(fragments[i]);
        }
        free(fragments);
        fclose(file);
        return NULL;
      }

      strncpy(fragments[*count], start, len);
      fragments[*count][len] = '\0';
      (*count)++;
    }
  }

  fclose(file);
  return fragments;
}

// Function to free fragments array
void free_fragments(char **fragments, int count) {
  if (!fragments)
    return;
  for (int i = 0; i < count; i++) {
    free(fragments[i]);
  }
  free(fragments);
}

// Base64 decode table
static const int b64_decode_table[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, 62, -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60,
    61, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1,
    -1, -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42,
    43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1};

// Function to decode base64
bool base64_decode(const char *input, uint8_t **output, size_t *out_len) {
  size_t input_len = strlen(input);
  if (input_len == 0)
    return false;

  // Calculate output length
  size_t max_output_len = (input_len / 4) * 3;
  uint8_t *decoded = (uint8_t *)malloc(max_output_len);
  if (!decoded)
    return false;

  size_t output_idx = 0;
  uint32_t buffer = 0;
  int bits_in_buffer = 0;

  for (size_t i = 0; i < input_len; i++) {
    unsigned char c = input[i];

    // Skip whitespace
    if (isspace(c))
      continue;

    // End of data
    if (c == '=')
      break;

    int value = b64_decode_table[c];
    if (value == -1) {
      free(decoded);
      return false;
    }

    buffer = (buffer << 6) | value;
    bits_in_buffer += 6;

    if (bits_in_buffer >= 8) {
      bits_in_buffer -= 8;
      decoded[output_idx++] = (buffer >> bits_in_buffer) & 0xFF;
    }
  }

  *output = decoded;
  *out_len = output_idx;
  return true;
}

// Function to encode data to base64
char *base64_encode(const uint8_t *data, size_t len) {
  size_t encoded_len = 4 * ((len + 2) / 3);
  char *encoded = (char *)malloc(encoded_len + 1);
  if (!encoded) {
    return NULL;
  }

  static const char b64_table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (size_t i = 0, j = 0; i < len;) {
    uint32_t octet_a = i < len ? data[i++] : 0;
    uint32_t octet_b = i < len ? data[i++] : 0;
    uint32_t octet_c = i < len ? data[i++] : 0;

    uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

    encoded[j++] = b64_table[(triple >> 18) & 0x3F];
    encoded[j++] = b64_table[(triple >> 12) & 0x3F];
    encoded[j++] = (i > len + 1) ? '=' : b64_table[(triple >> 6) & 0x3F];
    encoded[j++] = (i > len) ? '=' : b64_table[triple & 0x3F];
  }
  encoded[encoded_len] = '\0';

  return encoded;
}

// Function to read binary file
uint8_t *read_binary_file(const char *filepath, size_t *out_len) {
  FILE *file = fopen(filepath, "rb");
  if (!file) {
    return NULL;
  }

  // Get file size
  fseek(file, 0, SEEK_END);
  long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);

  if (file_size <= 0) {
    fclose(file);
    return NULL;
  }

  // Allocate buffer
  uint8_t *bytes = (uint8_t *)malloc(file_size);
  if (!bytes) {
    fclose(file);
    return NULL;
  }

  // Read file
  size_t read_size = fread(bytes, 1, file_size, file);
  fclose(file);

  if (read_size != (size_t)file_size) {
    free(bytes);
    return NULL;
  }

  *out_len = file_size;
  return bytes;
}

bool assert_bytes_equal(const uint8_t *expected, size_t expected_len,
                        const uint8_t *actual, size_t actual_len,
                        const char *label) {
  const char *tag = label ? label : "bytes";
  if (expected_len != actual_len) {
    fprintf(stderr, "FAIL: %s length mismatch (expected %zu, got %zu)\n", tag,
            expected_len, actual_len);
    return false;
  }
  if (memcmp(expected, actual, expected_len) != 0) {
    for (size_t i = 0; i < expected_len; i++) {
      if (expected[i] != actual[i]) {
        fprintf(stderr,
                "FAIL: %s first diff @byte %zu: expected 0x%02x, got 0x%02x\n",
                tag, i, expected[i], actual[i]);
        break;
      }
    }
    return false;
  }
  return true;
}

// Function to read first line from text file
char *read_text_file_first_line(const char *filepath) {
  FILE *file = fopen(filepath, "r");
  if (!file) {
    return NULL;
  }

  // Allocate buffer
  char *line = (char *)malloc(4096);
  if (!line) {
    fclose(file);
    return NULL;
  }

  // Read first line
  if (!fgets(line, 4096, file)) {
    free(line);
    fclose(file);
    return NULL;
  }

  // Remove trailing newline
  line[strcspn(line, "\r\n")] = 0;

  fclose(file);
  return line;
}
