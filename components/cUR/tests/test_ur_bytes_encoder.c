/*
 * test_ur_bytes_encoder.c
 *
 * Test UREncoder with bytes data
 * Reads UR fragments, decodes to bytes,
 * then encodes back to UR and verifies roundtrip.
 */

#include "../src/types/bytes_type.h"
#include "../src/ur_decoder.h"
#include "../src/ur_encoder.h"
#include "test_harness.h"
#include "test_utils.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CASES_DIR "tests/test_cases/bytes"

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

  // Create decoder for initial decode
  ur_decoder_t *decoder1 = ur_decoder_new();
  if (!decoder1) {
    printf("❌ Failed to create initial decoder\n");
    free_fragments(fragments, fragment_count);
    free(expected_data);
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
    free(expected_data);
    return false;
  }

  // Get the original UR result
  ur_result_t *result1 = ur_decoder_get_result(decoder1);
  if (!result1) {
    printf("❌ Failed to get initial UR result\n");
    ur_decoder_free(decoder1);
    free_fragments(fragments, fragment_count);
    free(expected_data);
    return false;
  }

  const uint8_t *original_cbor = result1->cbor_data;
  size_t original_cbor_len = result1->cbor_len;
  printf("Original CBOR size: %zu bytes\n", original_cbor_len);

  // Decode bytes from original CBOR
  bytes_data_t *original_bytes =
      bytes_from_cbor(original_cbor, original_cbor_len);
  if (!original_bytes) {
    printf("❌ Failed to decode original bytes from CBOR\n");
    ur_decoder_free(decoder1);
    free_fragments(fragments, fragment_count);
    free(expected_data);
    return false;
  }

  // Get original bytes data
  size_t original_bytes_len;
  const uint8_t *original_bytes_data =
      bytes_get_data(original_bytes, &original_bytes_len);
  if (!original_bytes_data) {
    printf("❌ Failed to get original bytes data\n");
    bytes_free(original_bytes);
    ur_decoder_free(decoder1);
    free_fragments(fragments, fragment_count);
    free(expected_data);
    return false;
  }
  printf("Original bytes length: %zu bytes\n", original_bytes_len);

  // Verify original bytes match expected
  if (original_bytes_len != expected_len ||
      memcmp(original_bytes_data, expected_data, expected_len) != 0) {
    printf("❌ Original bytes don't match expected data\n");
    printf("Expected length: %zu, Original length: %zu\n", expected_len,
           original_bytes_len);
    bytes_free(original_bytes);
    ur_decoder_free(decoder1);
    free_fragments(fragments, fragment_count);
    free(expected_data);
    return false;
  }

  // Encode bytes to CBOR (tests bytes_to_cbor path)
  size_t encoded_cbor_len = 0;
  uint8_t *encoded_cbor = bytes_to_cbor(original_bytes, &encoded_cbor_len);
  if (!encoded_cbor) {
    printf("❌ Failed to encode bytes to CBOR\n");
    bytes_free(original_bytes);
    ur_decoder_free(decoder1);
    free_fragments(fragments, fragment_count);
    free(expected_data);
    return false;
  }
  printf("Encoded CBOR size: %zu bytes\n", encoded_cbor_len);

  // Create UREncoder using the encoded CBOR data
  size_t max_fragment_len = 200; // Reasonable fragment size
  ur_encoder_t *encoder = ur_encoder_new(
      "bytes", encoded_cbor, encoded_cbor_len, max_fragment_len, 0, 10);

  if (!encoder) {
    printf("❌ Failed to create encoder\n");
    free(encoded_cbor);
    bytes_free(original_bytes);
    ur_decoder_free(decoder1);
    free_fragments(fragments, fragment_count);
    free(expected_data);
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
    free(encoded_cbor);
    bytes_free(original_bytes);
    ur_decoder_free(decoder1);
    free_fragments(fragments, fragment_count);
    free(expected_data);
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
      free(encoded_cbor);
      bytes_free(original_bytes);
      ur_decoder_free(decoder1);
      free_fragments(fragments, fragment_count);
      free(expected_data);
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

      // Decode bytes from roundtrip CBOR
      bytes_data_t *decoded_bytes =
          bytes_from_cbor(decoded_cbor, decoded_cbor_len);

      if (decoded_bytes) {
        // Get bytes data from roundtrip
        size_t decoded_bytes_len;
        const uint8_t *decoded_bytes_data =
            bytes_get_data(decoded_bytes, &decoded_bytes_len);

        if (decoded_bytes_data) {
          printf("Decoded bytes length: %zu bytes\n", decoded_bytes_len);

          // Compare bytes data
          if (decoded_bytes_len == original_bytes_len &&
              memcmp(decoded_bytes_data, original_bytes_data,
                     decoded_bytes_len) == 0) {
            printf("✅ PASS - Roundtrip successful, bytes data matches\n");
            success = true;
          } else {
            printf("❌ FAIL - Bytes data mismatch after roundtrip\n");
            printf("Original length: %zu, Decoded length: %zu\n",
                   original_bytes_len, decoded_bytes_len);

            // Show first bytes that differ
            if (decoded_bytes_len != original_bytes_len) {
              printf("Length mismatch!\n");
            } else {
              for (size_t i = 0; i < decoded_bytes_len; i++) {
                if (decoded_bytes_data[i] != original_bytes_data[i]) {
                  printf("First mismatch at byte %zu: expected 0x%02x, got "
                         "0x%02x\n",
                         i, original_bytes_data[i], decoded_bytes_data[i]);
                  break;
                }
              }
            }
          }
        } else {
          printf("❌ Failed to get bytes data from roundtrip\n");
        }

        bytes_free(decoded_bytes);
      } else {
        printf("❌ Failed to decode bytes from roundtrip CBOR\n");
      }
    } else {
      printf("❌ Failed to get UR result from roundtrip\n");
    }
  }

  ur_decoder_free(decoder2);
  ur_encoder_free(encoder);
  free(encoded_cbor);
  bytes_free(original_bytes);
  ur_decoder_free(decoder1);
  free_fragments(fragments, fragment_count);
  free(expected_data);

  return success;
}

int main(int argc, char *argv[]) {
  return run_test_suite(argc, argv, "UR Encoder Test (bytes)", TEST_CASES_DIR,
                        ".UR_fragments.txt", test_file);
}
