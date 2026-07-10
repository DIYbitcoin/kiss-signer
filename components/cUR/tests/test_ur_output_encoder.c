/*
 * test_ur_output_encoder.c
 *
 * Test UREncoder with crypto-output data
 * Reads UR fragments, decodes to crypto-output,
 * then encodes back to UR and verifies roundtrip.
 */

#include "../src/types/output.h"
#include "../src/ur_decoder.h"
#include "../src/ur_encoder.h"
#include "test_harness.h"
#include "test_utils.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CASES_DIR "tests/test_cases/output"

// Test one file: decode UR fragments, encode back, decode again, verify
// roundtrip
static bool test_file(const char *filepath) {
  printf("\n=== Testing: %s ===\n", filepath);

  // Read UR fragments
  int fragment_count = 0;
  char **fragments = read_fragments_from_file(filepath, &fragment_count);

  if (!fragments || fragment_count == 0) {
    fprintf(stderr, "❌ No fragments found in file\n");
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

  // Create decoder for initial decode
  ur_decoder_t *decoder1 = ur_decoder_new();
  if (!decoder1) {
    printf("❌ Failed to create initial decoder\n");
    free_fragments(fragments, fragment_count);
    free(expected_descriptor);
    return false;
  }

  // Feed all fragments to initial decoder
  for (int i = 0; i < fragment_count; i++) {
    ur_decoder_receive_part(decoder1, fragments[i]);
    if (ur_decoder_is_complete(decoder1)) {
      printf("Initial decoding complete after %d parts\n", i + 1);
      break;
    }
  }

  if (!ur_decoder_is_complete(decoder1) || !ur_decoder_is_success(decoder1)) {
    printf("❌ Initial decoding failed\n");
    ur_decoder_free(decoder1);
    free_fragments(fragments, fragment_count);
    free(expected_descriptor);
    return false;
  }

  // Get the original UR result
  ur_result_t *result1 = ur_decoder_get_result(decoder1);
  if (!result1) {
    printf("❌ Failed to get initial UR result\n");
    ur_decoder_free(decoder1);
    free_fragments(fragments, fragment_count);
    free(expected_descriptor);
    return false;
  }

  const uint8_t *original_cbor = result1->cbor_data;
  size_t original_cbor_len = result1->cbor_len;
  printf("Original CBOR size: %zu bytes\n", original_cbor_len);

  // Decode output from original CBOR
  output_data_t *original_output =
      output_from_cbor(original_cbor, original_cbor_len);
  if (!original_output) {
    printf("❌ Failed to decode original output from CBOR\n");
    ur_decoder_free(decoder1);
    free_fragments(fragments, fragment_count);
    free(expected_descriptor);
    return false;
  }

  // Generate original descriptor
  char *original_descriptor = output_descriptor(original_output, true);
  if (!original_descriptor) {
    printf("❌ Failed to generate original descriptor\n");
    output_free(original_output);
    ur_decoder_free(decoder1);
    free_fragments(fragments, fragment_count);
    free(expected_descriptor);
    return false;
  }
  printf("Original descriptor: %s\n", original_descriptor);

  // Verify original descriptor matches expected
  if (strcmp(original_descriptor, expected_descriptor) != 0) {
    printf("❌ Original descriptor doesn't match expected\n");
    printf("Expected: %s\n", expected_descriptor);
    printf("Original: %s\n", original_descriptor);
    free(original_descriptor);
    output_free(original_output);
    ur_decoder_free(decoder1);
    free_fragments(fragments, fragment_count);
    free(expected_descriptor);
    return false;
  }

  // Create UREncoder using the original CBOR data
  // (Note: output type is read-only, so we use the original CBOR for roundtrip
  // test)
  size_t max_fragment_len = 200; // Reasonable fragment size
  ur_encoder_t *encoder =
      ur_encoder_new("crypto-output", original_cbor, original_cbor_len,
                     max_fragment_len, 0, 10);

  if (!encoder) {
    printf("❌ Failed to create encoder\n");
    free(original_descriptor);
    output_free(original_output);
    ur_decoder_free(decoder1);
    free_fragments(fragments, fragment_count);
    free(expected_descriptor);
    return false;
  }

  printf("Encoder created:\n");
  printf("  - Is single part: %s\n",
         ur_encoder_is_single_part(encoder) ? "yes" : "no");
  printf("  - Sequence length: %zu\n", ur_encoder_seq_len(encoder));

  // Create URDecoder for roundtrip
  ur_decoder_t *decoder2 = ur_decoder_new();
  if (!decoder2) {
    printf("❌ Failed to create roundtrip decoder\n");
    ur_encoder_free(encoder);
    free(original_descriptor);
    output_free(original_output);
    ur_decoder_free(decoder1);
    free_fragments(fragments, fragment_count);
    free(expected_descriptor);
    return false;
  }

  // Generate parts and feed to decoder
  int parts_sent = 0;
  size_t expected_parts = ur_encoder_seq_len(encoder);
  size_t max_parts =
      expected_parts * 2 + 10; // Allow for fountain coding overhead

  printf("Encoding and decoding:\n");

  while (!ur_decoder_is_complete(decoder2) && parts_sent < (int)max_parts) {
    char *part = NULL;
    if (!ur_encoder_next_part(encoder, &part)) {
      printf("❌ Failed to get next part\n");
      ur_decoder_free(decoder2);
      ur_encoder_free(encoder);
      free(original_descriptor);
      output_free(original_output);
      ur_decoder_free(decoder1);
      free_fragments(fragments, fragment_count);
      free(expected_descriptor);
      return false;
    }

    // Feed to decoder
    ur_decoder_receive_part(decoder2, part);
    free(part);
    parts_sent++;

    if (parts_sent % 10 == 0 || ur_decoder_is_complete(decoder2)) {
      printf("  Parts sent: %d, Progress: %.1f%%\n", parts_sent,
             ur_decoder_estimated_percent_complete(decoder2) * 100.0);
    }
  }

  bool success = false;

  if (!ur_decoder_is_complete(decoder2)) {
    printf("❌ Decoder did not complete after %d parts\n", parts_sent);
  } else if (!ur_decoder_is_success(decoder2)) {
    printf("❌ Decoder completed but not successful\n");
  } else {
    printf("✓ Decoder completed successfully\n");

    // Get decoded result
    ur_result_t *result2 = ur_decoder_get_result(decoder2);
    if (result2) {
      const uint8_t *decoded_cbor = result2->cbor_data;
      size_t decoded_cbor_len = result2->cbor_len;

      // Decode output from roundtrip CBOR
      output_data_t *decoded_output =
          output_from_cbor(decoded_cbor, decoded_cbor_len);

      if (decoded_output) {
        // Generate descriptor from roundtrip output
        char *decoded_descriptor = output_descriptor(decoded_output, true);

        if (decoded_descriptor) {
          printf("Decoded descriptor: %s\n", decoded_descriptor);

          // Compare descriptors
          if (strcmp(decoded_descriptor, original_descriptor) == 0) {
            printf("✅ PASS - Roundtrip successful, descriptors match\n");
            success = true;
          } else {
            printf("❌ FAIL - Descriptor mismatch after roundtrip\n");
            printf("Original:  %s\n", original_descriptor);
            printf("Roundtrip: %s\n", decoded_descriptor);
          }

          free(decoded_descriptor);
        } else {
          printf("❌ Failed to generate descriptor from roundtrip output\n");
        }

        output_free(decoded_output);
      } else {
        printf("❌ Failed to decode output from roundtrip CBOR\n");
      }
    } else {
      printf("❌ Failed to get UR result from roundtrip\n");
    }
  }

  ur_decoder_free(decoder2);
  ur_encoder_free(encoder);
  free(original_descriptor);
  output_free(original_output);
  ur_decoder_free(decoder1);
  free_fragments(fragments, fragment_count);
  free(expected_descriptor);

  return success;
}

int main(int argc, char *argv[]) {
  return run_test_suite(argc, argv, "UR Encoder Test (crypto-output)",
                        TEST_CASES_DIR, ".UR_fragments.txt", test_file);
}
