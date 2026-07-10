#include "../src/types/psbt.h"
#include "../src/ur.h"
#include "../src/ur_decoder.h"
#include "../src/utils.h"
#include "test_harness.h"
#include "test_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CASES_DIR "tests/test_cases/PSBTs"

static bool test_file(const char *filepath) {
  printf("\n=== Testing file: %s ===\n", filepath);

  int fragment_count = 0;
  char **fragments = read_fragments_from_file(filepath, &fragment_count);

  if (!fragments || fragment_count == 0) {
    fprintf(stderr, "❌ No fragments found in file: %s\n", filepath);
    return false;
  }

  printf("Found %d fragments\n", fragment_count);

  // Generate expected binary filename
  char expected_path[512];
  strncpy(expected_path, filepath, sizeof(expected_path) - 1);
  expected_path[sizeof(expected_path) - 1] = '\0';

  char *ext = strstr(expected_path, ".UR_fragments.txt");
  if (ext) {
    strcpy(ext, ".psbt.bin");
  } else {
    free_fragments(fragments, fragment_count);
    fprintf(stderr, "❌ Invalid filename format\n");
    return false;
  }

  // Read expected PSBT bytes
  size_t expected_len = 0;
  uint8_t *expected_bytes = read_binary_file(expected_path, &expected_len);
  if (!expected_bytes) {
    fprintf(stderr, "❌ Failed to read expected PSBT bytes: %s\n",
            expected_path);
    free_fragments(fragments, fragment_count);
    return false;
  }
  printf("Expected PSBT length: %zu bytes\n", expected_len);

  // Create decoder
  ur_decoder_t *decoder = ur_decoder_new();
  if (!decoder) {
    fprintf(stderr, "❌ Failed to create UR decoder\n");
    free_fragments(fragments, fragment_count);
    free(expected_bytes);
    return false;
  }

  bool success = false;
  int parts_used = 0;
  int progress_reported = 0;

  // Feed all fragments
  for (int i = 0; i < fragment_count; i++) {
    if (ur_decoder_receive_part(decoder, fragments[i])) {
      parts_used++;

      // Check ongoing decode status using the functions we want to cover
      size_t expected_parts = ur_decoder_expected_part_count(decoder);
      size_t processed_parts = ur_decoder_processed_parts_count(decoder);
      double percent_complete = ur_decoder_estimated_percent_complete(decoder);

      // Report progress once after first part (to avoid too much output)
      if (!progress_reported && expected_parts > 0) {
        printf("Expected parts: %zu, Processed: %zu, Progress: %.1f%%\n",
               expected_parts, processed_parts, percent_complete * 100.0);
        progress_reported = 1;
      }

      if (ur_decoder_is_complete(decoder)) {
        printf("Decoder complete after %d parts\n", parts_used);
        break;
      }
    } else {
      // Check error status if receive_part failed
      ur_decoder_error_t error = ur_decoder_get_last_error(decoder);
      if (error != UR_DECODER_OK) {
        fprintf(stderr, "Decode error at fragment %d: %d\n", i, error);
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
        // Decode PSBT from CBOR
        psbt_data_t *psbt = psbt_from_cbor(cbor_data, cbor_len);

        if (psbt) {
          // Get PSBT data
          size_t psbt_len;
          const uint8_t *psbt_data = psbt_get_data(psbt, &psbt_len);

          printf("Actual PSBT length: %zu bytes\n", psbt_len);

          if (psbt_data && psbt_len > 0) {
            if (assert_bytes_equal(expected_bytes, expected_len, psbt_data,
                                   psbt_len, "PSBT bytes")) {
              printf("✅ PASS - PSBT bytes match expected\n");
              success = true;
            }
          } else {
            fprintf(stderr, "❌ Failed to get PSBT data\n");
          }

          psbt_free(psbt);
        } else {
          fprintf(stderr, "❌ Failed to decode PSBT from CBOR\n");
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
  free(expected_bytes);

  return success;
}

int main(int argc, char *argv[]) {
  return run_test_suite(argc, argv, "UR Decoder Test (PSBT)", TEST_CASES_DIR,
                        ".UR_fragments.txt", test_file);
}
