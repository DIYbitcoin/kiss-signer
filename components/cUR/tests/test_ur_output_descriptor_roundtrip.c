/*
 * test_ur_output_descriptor_roundtrip.c
 *
 * Full roundtrip test starting from descriptor strings:
 *   descriptor string
 *     → output_from_descriptor_string()
 *     → output_to_cbor()
 *     → ur_encoder_new() + ur_encoder_next_part()
 *     → ur_decoder_receive_part()
 *     → output_from_cbor()
 *     → output_descriptor()
 *     → compare with original
 */

#include "../src/types/output.h"
#include "../src/ur_decoder.h"
#include "../src/ur_encoder.h"
#include "test_harness.h"
#include "test_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CASES_DIR "tests/test_cases/output"

static bool descriptor_is_rejected(const char *descriptor) {
  output_data_t *output = output_from_descriptor_string(descriptor);
  if (output) {
    output_free(output);
    return false;
  }
  return true;
}

static bool test_invalid_descriptors(void) {
  const char *invalid_descriptors[] = {
      "wsh(multi(,[65fb43fe/48'/1'/0'/"
      "2']tpubDFM6mziafLfJPA9StFuzvdC5htjaMTsVaPSA"
      "jsahgE4c2CMWpg9yKaK4JyoaBjVYJKUFX9Kdyb4fgFaFUQmZNGU71Q1wZgZiGM1Go7p59NW,"
      "[08c3586c/48'/1'/0'/2']tpubDENsrbyiJuWcD9JptRuTwGgixi5raa1fDUqFNk23Uocau"
      "yqSGcyFbQ3QjBRXb7RfNiPqWNdEfT9e9SdNaqUUiNxB42zdvvrX4oT8JhJWEBk))",
      "pkh([65fb43fe/"
      "4294967296]tpubDDe8bRQqws125ChaJ4ZoB6qVbFn1sBubiim6SYcfmFz8"
      "XVSp4WWiMj4gAuzSxJPRDZwT9rT928wQmWX993CAq4TjBXdcoCUtuG2E115mLD5)",
      "pkh([65fb43fe/44'/1'/0']tpubDDe8bRQqws125ChaJ4ZoB6qVbFn1sBubiim6SYcfmFz8"
      "XVSp4WWiMj4gAuzSxJPRDZwT9rT928wQmWX993CAq4TjBXdcoCUtuG2E115mLD5/"
      "4294967296)",
      "pkh([65fb43fe/44'/1'/0']tpubDDe8bRQqws125ChaJ4ZoB6qVbFn1sBubiim6SYcfmFz8"
      "XVSp4WWiMj4gAuzSxJPRDZwT9rT928wQmWX993CAq4TjBXdcoCUtuG2E115mLD5/"
      "4294967296h)",
      "wsh(multi(0,[65fb43fe/48'/1'/0'/"
      "2']tpubDFM6mziafLfJPA9StFuzvdC5htjaMTsVaPSA"
      "jsahgE4c2CMWpg9yKaK4JyoaBjVYJKUFX9Kdyb4fgFaFUQmZNGU71Q1wZgZiGM1Go7p59NW)"
      ")",
      "wsh(multi(3,[65fb43fe/48'/1'/0'/"
      "2']tpubDFM6mziafLfJPA9StFuzvdC5htjaMTsVaPSA"
      "jsahgE4c2CMWpg9yKaK4JyoaBjVYJKUFX9Kdyb4fgFaFUQmZNGU71Q1wZgZiGM1Go7p59NW,"
      "[08c3586c/48'/1'/0'/2']tpubDENsrbyiJuWcD9JptRuTwGgixi5raa1fDUqFNk23Uocau"
      "yqSGcyFbQ3QjBRXb7RfNiPqWNdEfT9e9SdNaqUUiNxB42zdvvrX4oT8JhJWEBk))",
      "wsh(multi(2,[65fb43fe/48'/1'/0'/"
      "2']tpubDFM6mziafLfJPA9StFuzvdC5htjaMTsVaPSA"
      "jsahgE4c2CMWpg9yKaK4JyoaBjVYJKUFX9Kdyb4fgFaFUQmZNGU71Q1wZgZiGM1Go7p59NW,"
      "not-a-valid-key))",
      NULL,
  };

  for (size_t i = 0; invalid_descriptors[i]; i++) {
    if (!descriptor_is_rejected(invalid_descriptors[i])) {
      printf("FAIL - Invalid descriptor accepted: %s\n",
             invalid_descriptors[i]);
      return false;
    }
  }
  return true;
}

static bool test_file(const char *filepath) {
  printf("\n=== Testing: %s ===\n", filepath);

  // Read descriptor from file
  char *descriptor = read_text_file_first_line(filepath);
  if (!descriptor) {
    fprintf(stderr, "Failed to read descriptor from: %s\n", filepath);
    return false;
  }
  printf("Input descriptor: %s\n", descriptor);

  // Step 1: Parse descriptor string into output_data_t
  output_data_t *output = output_from_descriptor_string(descriptor);
  if (!output) {
    fprintf(stderr, "Failed to parse descriptor string\n");
    free(descriptor);
    return false;
  }
  printf("Parsed descriptor successfully\n");

  // Step 2: Encode output to CBOR
  size_t cbor_len = 0;
  uint8_t *cbor_data = output_to_cbor(output, &cbor_len);
  if (!cbor_data || cbor_len == 0) {
    fprintf(stderr, "Failed to encode output to CBOR\n");
    output_free(output);
    free(descriptor);
    return false;
  }
  printf("CBOR encoded: %zu bytes\n", cbor_len);

  // Step 3: Create UR encoder
  size_t max_fragment_len = 200;
  ur_encoder_t *encoder = ur_encoder_new("crypto-output", cbor_data, cbor_len,
                                         max_fragment_len, 0, 10);
  if (!encoder) {
    fprintf(stderr, "Failed to create UR encoder\n");
    free(cbor_data);
    output_free(output);
    free(descriptor);
    return false;
  }
  printf("Encoder: single_part=%s, seq_len=%zu\n",
         ur_encoder_is_single_part(encoder) ? "yes" : "no",
         ur_encoder_seq_len(encoder));

  // Step 4: Create UR decoder and feed fragments
  ur_decoder_t *decoder = ur_decoder_new();
  if (!decoder) {
    fprintf(stderr, "Failed to create UR decoder\n");
    ur_encoder_free(encoder);
    free(cbor_data);
    output_free(output);
    free(descriptor);
    return false;
  }

  int parts_sent = 0;
  size_t max_parts = ur_encoder_seq_len(encoder) * 2 + 10;

  while (!ur_decoder_is_complete(decoder) && parts_sent < (int)max_parts) {
    char *part = NULL;
    if (!ur_encoder_next_part(encoder, &part)) {
      fprintf(stderr, "Failed to get next UR part\n");
      ur_decoder_free(decoder);
      ur_encoder_free(encoder);
      free(cbor_data);
      output_free(output);
      free(descriptor);
      return false;
    }
    ur_decoder_receive_part(decoder, part);
    free(part);
    parts_sent++;
  }

  if (!ur_decoder_is_complete(decoder) || !ur_decoder_is_success(decoder)) {
    fprintf(stderr, "UR decoding failed after %d parts\n", parts_sent);
    ur_decoder_free(decoder);
    ur_encoder_free(encoder);
    free(cbor_data);
    output_free(output);
    free(descriptor);
    return false;
  }
  printf("UR decode complete after %d parts\n", parts_sent);

  // Step 5: Get CBOR from decoded result and parse back to output
  ur_result_t *result = ur_decoder_get_result(decoder);
  if (!result) {
    fprintf(stderr, "Failed to get UR result\n");
    ur_decoder_free(decoder);
    ur_encoder_free(encoder);
    free(cbor_data);
    output_free(output);
    free(descriptor);
    return false;
  }

  output_data_t *decoded_output =
      output_from_cbor(result->cbor_data, result->cbor_len);
  if (!decoded_output) {
    fprintf(stderr, "Failed to decode output from roundtrip CBOR\n");
    ur_decoder_free(decoder);
    ur_encoder_free(encoder);
    free(cbor_data);
    output_free(output);
    free(descriptor);
    return false;
  }

  // Step 6: Generate descriptor from decoded output and compare
  char *roundtrip_descriptor = output_descriptor(decoded_output, true);
  if (!roundtrip_descriptor) {
    fprintf(stderr, "Failed to generate descriptor from decoded output\n");
    output_free(decoded_output);
    ur_decoder_free(decoder);
    ur_encoder_free(encoder);
    free(cbor_data);
    output_free(output);
    free(descriptor);
    return false;
  }

  printf("Roundtrip descriptor: %s\n", roundtrip_descriptor);

  bool success = (strcmp(descriptor, roundtrip_descriptor) == 0);
  if (success) {
    printf("PASS - Roundtrip successful\n");
  } else {
    printf("FAIL - Descriptor mismatch\n");
    printf("  Input:     %s\n", descriptor);
    printf("  Roundtrip: %s\n", roundtrip_descriptor);
  }

  free(roundtrip_descriptor);
  output_free(decoded_output);
  ur_decoder_free(decoder);
  ur_encoder_free(encoder);
  free(cbor_data);
  output_free(output);
  free(descriptor);

  return success;
}

int main(int argc, char *argv[]) {
  int rc = run_test_suite(argc, argv, "UR Output Descriptor Roundtrip Test",
                          TEST_CASES_DIR, ".descriptor.txt", test_file);

  printf("\n=== Testing invalid descriptors ===\n");
  if (test_invalid_descriptors()) {
    printf("PASS - Invalid descriptors are rejected\n");
  } else {
    rc = 1;
  }

  return rc;
}
