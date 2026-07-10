/*
 * test_ur_output_decoder.c
 *
 * Test URDecoder with crypto-output data
 * Reads .UR_fragments.txt files, decodes to crypto-output,
 * then verifies against expected descriptor.
 */

#include "../src/types/output.h"
#include "../src/ur.h"
#include "../src/ur_decoder.h"
#include "../src/utils.h"
#include "test_harness.h"
#include "test_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CASES_DIR "tests/test_cases/output"

static bool test_file(const char *filepath) {
  printf("\n=== Testing file: %s ===\n", filepath);

  int fragment_count = 0;
  char **fragments = read_fragments_from_file(filepath, &fragment_count);

  if (!fragments || fragment_count == 0) {
    fprintf(stderr, "❌ No fragments found in file: %s\n", filepath);
    return false;
  }

  printf("Found %d fragments\n", fragment_count);

  // Generate expected descriptor filename
  char expected_path[512];
  strncpy(expected_path, filepath, sizeof(expected_path) - 1);
  expected_path[sizeof(expected_path) - 1] = '\0';

  char *ext = strstr(expected_path, ".UR_fragments.txt");
  if (ext) {
    strcpy(ext, ".txt");
  } else {
    free_fragments(fragments, fragment_count);
    fprintf(stderr, "❌ Invalid filename format\n");
    return false;
  }

  // Read expected descriptor
  char *expected_descriptor = read_text_file_first_line(expected_path);
  if (!expected_descriptor) {
    fprintf(stderr, "❌ Failed to read expected descriptor: %s\n",
            expected_path);
    free_fragments(fragments, fragment_count);
    return false;
  }
  printf("Expected descriptor: %s\n", expected_descriptor);

  // Create decoder
  ur_decoder_t *decoder = ur_decoder_new();
  if (!decoder) {
    fprintf(stderr, "❌ Failed to create UR decoder\n");
    free_fragments(fragments, fragment_count);
    free(expected_descriptor);
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

        // Decode output from CBOR
        output_data_t *output = output_from_cbor(cbor_data, cbor_len);

        if (output) {
          // Generate descriptor (with checksum)
          char *actual_descriptor = output_descriptor(output, true);

          if (actual_descriptor) {
            printf("Actual descriptor: %s\n", actual_descriptor);

            // Compare descriptors
            if (strcmp(actual_descriptor, expected_descriptor) == 0) {
              printf("✅ PASS - Descriptor matches expected\n");
              success = true;
            } else {
              printf("❌ FAIL - Descriptor mismatch\n");
              printf("Expected: %s\n", expected_descriptor);
              printf("Actual:   %s\n", actual_descriptor);
            }

            free(actual_descriptor);
          } else {
            fprintf(stderr, "❌ Failed to generate descriptor\n");
          }

          output_free(output);
        } else {
          fprintf(stderr, "❌ Failed to decode output from CBOR\n");
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
  free(expected_descriptor);

  return success;
}

int main(int argc, char *argv[]) {
  return run_test_suite(argc, argv, "UR Decoder Test (crypto-output)",
                        TEST_CASES_DIR, ".UR_fragments.txt", test_file);
}
