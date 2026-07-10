#define _POSIX_C_SOURCE 200809L
#include "../src/types/bip39.h"
#include "../src/ur.h"
#include "../src/ur_decoder.h"
#include "../src/utils.h"
#include "test_harness.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE_LENGTH 2048
#define MAX_WORDS 50
#define TEST_CASES_DIR "tests/test_cases/bip39"

// Function to read UR string from file
char *read_ur_from_file(const char *filepath) {
  FILE *file = fopen(filepath, "r");
  if (!file) {
    fprintf(stderr, "Failed to open file: %s\n", filepath);
    return NULL;
  }

  char line[MAX_LINE_LENGTH];
  char *ur_string = NULL;

  while (fgets(line, sizeof(line), file)) {
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

    if (start) {
      ur_string = strdup(start);
      break;
    }
  }

  fclose(file);
  return ur_string;
}

// Function to read expected words from file
char **read_words_from_file(const char *filepath, size_t *word_count) {
  FILE *file = fopen(filepath, "r");
  if (!file) {
    fprintf(stderr, "Failed to open file: %s\n", filepath);
    return NULL;
  }

  char **words = (char **)malloc(MAX_WORDS * sizeof(char *));
  if (!words) {
    fclose(file);
    return NULL;
  }

  char line[MAX_LINE_LENGTH];
  *word_count = 0;

  while (fgets(line, sizeof(line), file) && *word_count < MAX_WORDS) {
    // Remove newline
    line[strcspn(line, "\r\n")] = 0;

    // Skip empty lines
    if (strlen(line) == 0) {
      continue;
    }

    words[*word_count] = strdup(line);
    if (!words[*word_count]) {
      // Cleanup on failure
      for (size_t i = 0; i < *word_count; i++) {
        free(words[i]);
      }
      free(words);
      fclose(file);
      return NULL;
    }
    (*word_count)++;
  }

  fclose(file);
  return words;
}

// Function to free words array
void free_words(char **words, size_t count) {
  if (!words)
    return;
  for (size_t i = 0; i < count; i++) {
    free(words[i]);
  }
  free(words);
}

static bool test_file(const char *filepath) {
  printf("\n=== Testing file: %s ===\n", filepath);

  // Generate UR filename
  char ur_path[512];
  strncpy(ur_path, filepath, sizeof(ur_path) - 1);
  ur_path[sizeof(ur_path) - 1] = '\0';

  char *ext = strstr(ur_path, ".UR.txt");
  if (!ext) {
    fprintf(stderr, "❌ Invalid filename format (expected .UR.txt)\n");
    return false;
  }

  // Read UR string
  char *ur_string = read_ur_from_file(ur_path);
  if (!ur_string) {
    fprintf(stderr, "❌ Failed to read UR string from: %s\n", ur_path);
    return false;
  }
  printf("UR string: %s\n", ur_string);

  // Generate words filename
  char words_path[512];
  strncpy(words_path, filepath, sizeof(words_path) - 1);
  words_path[sizeof(words_path) - 1] = '\0';
  ext = strstr(words_path, ".UR.txt");
  if (ext) {
    strcpy(ext, ".words.txt");
  }

  // Read expected words
  size_t expected_count = 0;
  char **expected_words = read_words_from_file(words_path, &expected_count);
  if (!expected_words || expected_count == 0) {
    fprintf(stderr, "❌ Failed to read expected words from: %s\n", words_path);
    free(ur_string);
    return false;
  }
  printf("Expected word count: %zu\n", expected_count);

  // Create decoder
  ur_decoder_t *decoder = ur_decoder_new();
  if (!decoder) {
    fprintf(stderr, "❌ Failed to create UR decoder\n");
    free(ur_string);
    free_words(expected_words, expected_count);
    return false;
  }

  bool success = false;

  // Feed the UR string to the decoder
  if (ur_decoder_receive_part(decoder, ur_string)) {
    if (ur_decoder_is_complete(decoder)) {
      printf("✓ Decoding complete\n");
    } else {
      fprintf(stderr, "❌ Decoder not complete after receiving part\n");
      ur_decoder_free(decoder);
      free(ur_string);
      free_words(expected_words, expected_count);
      return false;
    }
  } else {
    fprintf(stderr, "❌ Failed to receive UR part\n");
    ur_decoder_free(decoder);
    free(ur_string);
    free_words(expected_words, expected_count);
    return false;
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

        // Decode BIP39 from CBOR
        bip39_data_t *bip39 = bip39_from_cbor(cbor_data, cbor_len);

        if (bip39) {
          // Get BIP39 words
          size_t word_count;
          char **words = bip39_get_words(bip39, &word_count);

          printf("Actual word count: %zu\n", word_count);

          if (words && word_count > 0) {
            // Compare with expected words
            if (word_count == expected_count) {
              printf("✓ Word count matches\n");

              int all_match = 1;
              for (size_t i = 0; i < word_count; i++) {
                if (strcmp(words[i], expected_words[i]) != 0) {
                  fprintf(stderr,
                          "❌ Word mismatch at position %zu: expected '%s', "
                          "got '%s'\n",
                          i, expected_words[i], words[i]);
                  all_match = 0;
                }
              }

              if (all_match) {
                printf("✅ PASS - All words match expected mnemonic\n");
                printf("Mnemonic: ");
                for (size_t i = 0; i < word_count; i++) {
                  printf("%s%s", words[i], i < word_count - 1 ? ", " : "\n");
                }
                success = true;
              } else {
                printf("❌ FAIL - Word mismatch\n");
              }
            } else {
              fprintf(stderr,
                      "❌ FAIL - Word count mismatch: expected %zu, got %zu\n",
                      expected_count, word_count);
            }
          } else {
            fprintf(stderr, "❌ Failed to get BIP39 words\n");
          }

          bip39_free(bip39);
        } else {
          fprintf(stderr, "❌ Failed to decode BIP39 from CBOR\n");
        }
      } else {
        fprintf(stderr, "❌ Failed to get CBOR data\n");
      }
    } else {
      fprintf(stderr, "❌ Failed to get UR result\n");
    }
  } else {
    fprintf(stderr, "❌ Decoding failed or incomplete\n");
  }

  ur_decoder_free(decoder);
  free(ur_string);
  free_words(expected_words, expected_count);

  return success;
}

int main(int argc, char *argv[]) {
  return run_test_suite(argc, argv, "UR Decoder Test (BIP39)", TEST_CASES_DIR,
                        ".UR.txt", test_file);
}
