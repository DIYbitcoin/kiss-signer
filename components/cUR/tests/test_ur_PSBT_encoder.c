/*
 * test_ur_PSBT_encoder.c
 *
 * Test UREncoder with PSBT data
 * Reads .psbt.bin files, encodes to UR fragments,
 * then decodes back to verify roundtrip.
 */

#include "../src/types/psbt.h"
#include "../src/ur_decoder.h"
#include "../src/ur_encoder.h"
#include "test_harness.h"
#include "test_utils.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Test one file: encode base64 PSBT to UR, decode back, verify roundtrip
static bool test_file(const char *filename) {
  printf("\n=== Testing: %s ===\n", filename);

  // Read PSBT bytes from binary file
  size_t original_psbt_len = 0;
  uint8_t *original_psbt_bytes = read_binary_file(filename, &original_psbt_len);

  if (!original_psbt_bytes) {
    printf("❌ Failed to read file\n");
    return false;
  }

  printf("PSBT size: %zu bytes\n", original_psbt_len);

  // Create PSBT object (psbt_new makes a copy, so we'll keep original for
  // comparison)
  psbt_data_t *psbt = psbt_new(original_psbt_bytes, original_psbt_len);

  if (!psbt) {
    printf("❌ Failed to create PSBT object\n");
    free(original_psbt_bytes);
    return false;
  }

  // Encode PSBT to CBOR
  size_t cbor_len = 0;
  uint8_t *cbor_data = psbt_to_cbor(psbt, &cbor_len);

  if (!cbor_data) {
    printf("❌ Failed to encode PSBT to CBOR\n");
    psbt_free(psbt);
    free(original_psbt_bytes);
    return false;
  }

  printf("CBOR size: %zu bytes\n", cbor_len);

  // Create UREncoder
  size_t max_fragment_len = 200; // Reasonable fragment size
  ur_encoder_t *encoder = ur_encoder_new("crypto-psbt", cbor_data, cbor_len,
                                         max_fragment_len, 0, 10);

  if (!encoder) {
    printf("❌ Failed to create encoder\n");
    free(cbor_data);
    psbt_free(psbt);
    free(original_psbt_bytes);
    return false;
  }

  printf("Encoder created:\n");
  printf("  - Is single part: %s\n",
         ur_encoder_is_single_part(encoder) ? "yes" : "no");
  printf("  - Sequence length: %zu\n", ur_encoder_seq_len(encoder));

  // Create URDecoder
  ur_decoder_t *decoder = ur_decoder_new();
  if (!decoder) {
    printf("❌ Failed to create decoder\n");
    ur_encoder_free(encoder);
    free(cbor_data);
    psbt_free(psbt);
    free(original_psbt_bytes);
    return false;
  }

  // Generate parts and feed to decoder
  int parts_sent = 0;
  size_t expected_parts = ur_encoder_seq_len(encoder);
  size_t max_parts =
      expected_parts * 2 + 10; // Allow for fountain coding overhead

  printf("Encoding and decoding:\n");

  while (!ur_decoder_is_complete(decoder) && parts_sent < (int)max_parts) {
    char *part = NULL;
    if (!ur_encoder_next_part(encoder, &part)) {
      printf("❌ Failed to get next part\n");
      ur_decoder_free(decoder);
      ur_encoder_free(encoder);
      free(cbor_data);
      psbt_free(psbt);
      free(original_psbt_bytes);
      return false;
    }

    // Feed to decoder
    ur_decoder_receive_part(decoder, part);
    free(part);
    parts_sent++;

    if (parts_sent % 10 == 0 || ur_decoder_is_complete(decoder)) {
      printf("  Parts sent: %d, Progress: %.1f%%\n", parts_sent,
             ur_decoder_estimated_percent_complete(decoder) * 100.0);
    }
  }

  bool success = false;

  if (!ur_decoder_is_complete(decoder)) {
    printf("❌ Decoder did not complete after %d parts\n", parts_sent);
  } else if (!ur_decoder_is_success(decoder)) {
    printf("❌ Decoder completed but not successful\n");
  } else {
    printf("✓ Decoder completed successfully\n");

    // Get decoded result
    ur_result_t *result = ur_decoder_get_result(decoder);
    if (result) {
      const uint8_t *decoded_cbor = result->cbor_data;
      size_t decoded_cbor_len = result->cbor_len;

      // Decode PSBT from CBOR
      psbt_data_t *decoded_psbt =
          psbt_from_cbor(decoded_cbor, decoded_cbor_len);

      if (decoded_psbt) {
        // Get PSBT data
        size_t decoded_psbt_len;
        const uint8_t *decoded_psbt_data =
            psbt_get_data(decoded_psbt, &decoded_psbt_len);

        // Compare with original bytes
        if (decoded_psbt_len == original_psbt_len &&
            memcmp(decoded_psbt_data, original_psbt_bytes, decoded_psbt_len) ==
                0) {
          printf("✅ PASS - Roundtrip successful, PSBT bytes match\n");
          success = true;
        } else {
          printf("❌ FAIL - PSBT bytes mismatch after roundtrip\n");
          printf("Original length: %zu, Decoded length: %zu\n",
                 original_psbt_len, decoded_psbt_len);

          // Show first bytes that differ
          if (decoded_psbt_len != original_psbt_len) {
            printf("Length mismatch!\n");
          } else {
            for (size_t i = 0; i < decoded_psbt_len; i++) {
              if (decoded_psbt_data[i] != original_psbt_bytes[i]) {
                printf(
                    "First mismatch at byte %zu: expected 0x%02x, got 0x%02x\n",
                    i, original_psbt_bytes[i], decoded_psbt_data[i]);
                break;
              }
            }
          }
        }

        psbt_free(decoded_psbt);
      } else {
        printf("❌ Failed to decode PSBT from roundtrip CBOR\n");
      }
    } else {
      printf("❌ Failed to get UR result\n");
    }
  }

  ur_decoder_free(decoder);
  ur_encoder_free(encoder);
  free(cbor_data);
  psbt_free(psbt);
  free(original_psbt_bytes);

  return success;
}

int main(int argc, char *argv[]) {
  return run_test_suite(argc, argv, "UR Encoder Test (PSBT)",
                        "tests/test_cases/PSBTs", ".psbt.bin", test_file);
}
