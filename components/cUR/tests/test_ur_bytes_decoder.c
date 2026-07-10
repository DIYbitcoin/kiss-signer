/*
 * test_ur_bytes_decoder.c
 *
 * Test URDecoder with bytes data
 * Reads .UR_fragments.txt files, decodes to bytes,
 * then verifies against expected data in .decoded.txt.
 */

#include "../src/types/bytes_type.h"
#include "../src/ur.h"
#include "../src/ur_decoder.h"
#include "../src/utils.h"
#include "test_harness.h"
#include "test_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CASES_DIR "tests/test_cases/bytes"

static bool test_file(const char *filepath) {
  printf("\n=== Testing file: %s ===\n", filepath);

  int fragment_count = 0;
  char **fragments = read_fragments_from_file(filepath, &fragment_count);

  if (!fragments || fragment_count == 0) {
    fprintf(stderr, "❌ No fragments found in file: %s\n", filepath);
    return false;
  }

  printf("Found %d fragments\n", fragment_count);

  // Generate expected data filename - try .bin first, then .decoded.txt
  char expected_path[512];
  strncpy(expected_path, filepath, sizeof(expected_path) - 1);
  expected_path[sizeof(expected_path) - 1] = '\0';

  char *ext = strstr(expected_path, ".UR_fragments.txt");
  if (!ext) {
    free_fragments(fragments, fragment_count);
    fprintf(stderr, "❌ Invalid filename format\n");
    return false;
  }

  // Read expected data - prioritize .bin for bytes type
  size_t expected_len = 0;
  uint8_t *expected_data = NULL;

  // First try .bin file
  strcpy(ext, ".bin");
  expected_data = read_binary_file(expected_path, &expected_len);

  if (!expected_data) {
    // Fall back to .decoded.txt
    strcpy(ext, ".decoded.txt");
    expected_data = read_binary_file(expected_path, &expected_len);
  }

  if (!expected_data) {
    fprintf(stderr,
            "❌ Failed to read expected data (tried .bin and .decoded.txt)\n");
    free_fragments(fragments, fragment_count);
    return false;
  }
  printf("Expected data file: %s\n", expected_path);
  printf("Expected data length: %zu bytes\n", expected_len);

  // Create decoder
  ur_decoder_t *decoder = ur_decoder_new();
  if (!decoder) {
    fprintf(stderr, "❌ Failed to create UR decoder\n");
    free_fragments(fragments, fragment_count);
    free(expected_data);
    return false;
  }

  bool success = false;
  int parts_used = 0;

  // Feed all fragments
  for (int i = 0; i < fragment_count; i++) {
    if (ur_decoder_receive_part(decoder, fragments[i])) {
      parts_used++;

      if (ur_decoder_is_complete(decoder)) {
        printf("Decoder complete after %d parts\n", parts_used);
        break;
      }
    }
  }

  if (ur_decoder_is_complete(decoder) && ur_decoder_is_success(decoder)) {
    printf("✓ Decoding successful\n");

    // Get the UR result
    ur_result_t *result = ur_decoder_get_result(decoder);
    if (result) {
      printf("UR type: %s\n", result->type);

      // Get CBOR data
      const uint8_t *cbor_data = result->cbor_data;
      size_t cbor_len = result->cbor_len;

      if (cbor_data && cbor_len > 0) {
        printf("CBOR length: %zu bytes\n", cbor_len);

        // Decode bytes from CBOR
        bytes_data_t *bytes = bytes_from_cbor(cbor_data, cbor_len);

        if (bytes) {
          // Get bytes data
          size_t bytes_len;
          const uint8_t *bytes_data = bytes_get_data(bytes, &bytes_len);

          printf("Actual data length: %zu bytes\n", bytes_len);

          if (bytes_data && bytes_len > 0) {
            if (assert_bytes_equal(expected_data, expected_len, bytes_data,
                                   bytes_len, "bytes data")) {
              printf("✅ PASS - Bytes data matches expected\n");
              success = true;
            }
          } else {
            fprintf(stderr, "❌ Failed to get bytes data\n");
          }

          bytes_free(bytes);
        } else {
          fprintf(stderr, "❌ Failed to decode bytes from CBOR\n");
        }
      } else {
        fprintf(stderr, "❌ Failed to get CBOR data\n");
      }
    } else {
      fprintf(stderr, "❌ Failed to get UR result\n");
    }
  } else {
    printf("❌ Decoding failed or incomplete\n");
  }

  ur_decoder_free(decoder);
  free_fragments(fragments, fragment_count);
  free(expected_data);

  return success;
}

int main(int argc, char *argv[]) {
  return run_test_suite(argc, argv, "UR Decoder Test (bytes)", TEST_CASES_DIR,
                        ".UR_fragments.txt", test_file);
}
