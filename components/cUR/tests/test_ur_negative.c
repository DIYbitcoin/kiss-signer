/*
 * test_ur_negative.c
 *
 * Negative-path tests: NULL / empty inputs, malformed UR strings,
 * bad CRC, truncated fragment, malformed CBOR. Every assertion
 * verifies that the API rejects cleanly and does not crash. Running
 * this under `make DEBUG=1 test` also checks memory safety on every
 * rejection path via ASan/UBSan.
 */

#include "../src/bytewords.h"
#include "../src/types/bytes_type.h"
#include "../src/types/psbt.h"
#include "../src/ur.h"
#include "../src/ur_decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int asserts = 0;
static int failures = 0;

#define ASSERT(cond, msg)                                                      \
  do {                                                                         \
    asserts++;                                                                 \
    if (!(cond)) {                                                             \
      fprintf(stderr, "  FAIL @%d: %s\n", __LINE__, msg);                      \
      failures++;                                                              \
    } else {                                                                   \
      printf("  PASS %s\n", msg);                                              \
    }                                                                          \
  } while (0)

// A known-good multi-part UR fragment from tests/test_cases/bytes/bytes_2.
// Lowercased so mutations below can flip letters predictably.
static const char VALID_FRAGMENT[] =
    "ur:bytes/26-6/lpcscyamcfadflcyvapeswvthdemjtfzjkimchhtkpinkkhyjocyhfckiol"
    "bheatcldaeedkbgaecmhyflioahkncpaecskpbsdndmetiheefzjofsceatfgcxhdcacfcp"
    "dagwesckwpmomnos";

static char *dup_fragment(void) {
  size_t n = strlen(VALID_FRAGMENT) + 1;
  char *s = malloc(n);
  if (s)
    memcpy(s, VALID_FRAGMENT, n);
  return s;
}

static void test_null_and_empty(void) {
  printf("\n=== null_and_empty ===\n");
  ur_decoder_t *d = ur_decoder_new();
  ASSERT(d != NULL, "decoder_new returns non-NULL");

  ASSERT(!ur_decoder_receive_part(d, NULL), "receive_part rejects NULL string");
  ASSERT(ur_decoder_get_last_error(d) == UR_DECODER_ERROR_NULL_POINTER,
         "  -> sets NULL_POINTER error code");

  ASSERT(!ur_decoder_receive_part(NULL, VALID_FRAGMENT),
         "receive_part rejects NULL decoder");

  ASSERT(!ur_decoder_receive_part(d, ""), "receive_part rejects empty string");

  ur_decoder_free(d);
  ur_decoder_free(NULL); // no-op, must not crash

  ASSERT(bytes_from_cbor(NULL, 0) == NULL,
         "bytes_from_cbor(NULL, 0) returns NULL");
  ASSERT(bytes_from_cbor(NULL, 100) == NULL,
         "bytes_from_cbor(NULL, 100) returns NULL");
  uint8_t buf[1] = {0};
  ASSERT(bytes_from_cbor(buf, 0) == NULL,
         "bytes_from_cbor(buf, 0) returns NULL");
}

static void test_malformed_ur(void) {
  printf("\n=== malformed_ur ===\n");
  const char *cases[] = {
      "not-a-ur-at-all",       "http://bytes/abc", "ur:", "ur:/foo",
      "ur:bytes/zzzzzzzzzzzz", // not valid bytewords
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    ur_decoder_t *d = ur_decoder_new();
    bool ok = ur_decoder_receive_part(d, cases[i]);
    ASSERT(!ok, cases[i]);
    ur_decoder_free(d);
  }
}

static void test_bad_crc(void) {
  printf("\n=== bad_crc ===\n");
  char *mutated = dup_fragment();
  ASSERT(mutated != NULL, "dup_fragment");

  // Flip a letter deep in the bytewords body. Bytewords is 4-letter words
  // where only the 1st and 4th letter are used; swapping either changes the
  // decoded byte, so the trailing CRC32 mismatches and the fragment is
  // rejected.
  size_t body_start = strlen("ur:bytes/26-6/");
  size_t mid = body_start + (strlen(mutated) - body_start) / 2;
  mutated[mid] = (mutated[mid] == 'a' ? 'b' : 'a');

  ur_decoder_t *d = ur_decoder_new();
  ASSERT(!ur_decoder_receive_part(d, mutated),
         "rejects fragment with flipped letter");
  ur_decoder_free(d);
  free(mutated);
}

static void test_truncated_fragment(void) {
  printf("\n=== truncated_fragment ===\n");
  char *truncated = dup_fragment();
  ASSERT(truncated != NULL, "dup_fragment");

  size_t n = strlen(truncated);
  ASSERT(n > 20, "fragment long enough to truncate");
  // Cut the last 20 chars — removes the trailing CRC32 region entirely.
  truncated[n - 20] = '\0';

  ur_decoder_t *d = ur_decoder_new();
  ASSERT(!ur_decoder_receive_part(d, truncated), "rejects truncated fragment");
  ur_decoder_free(d);
  free(truncated);
}

// Regression for the empty-fragment double-free: a multi-part UR whose
// CBOR body carries a zero-length byte string (head 0x40). The old
// zero-copy path called safe_realloc(cbor_data, 0) which on glibc/musl
// frees the buffer and returns NULL, leaving fragment_data dangling and
// causing a double-free on the create_fountain_part_from_cbor fail
// branch. Must reject cleanly.
static void test_empty_fragment_payload(void) {
  printf("\n=== empty_fragment_payload ===\n");
  // CBOR: [seq_num=1, seq_len=1, message_len=1, checksum=0, h''].
  uint8_t cbor[] = {0x85, 0x01, 0x01, 0x01, 0x00, 0x40};
  char *bytewords = NULL;
  ASSERT(bytewords_encode(cbor, sizeof(cbor), &bytewords),
         "bytewords_encode crafted fragment");

  size_t n = strlen("ur:bytes/1-1/") + strlen(bytewords) + 1;
  char *ur = malloc(n);
  ASSERT(ur != NULL, "alloc ur string");
  snprintf(ur, n, "ur:bytes/1-1/%s", bytewords);

  ur_decoder_t *d = ur_decoder_new();
  ASSERT(!ur_decoder_receive_part(d, ur),
         "rejects empty-byte-string fragment payload");
  ASSERT(ur_decoder_get_last_error(d) == UR_DECODER_ERROR_INVALID_FRAGMENT,
         "  -> sets INVALID_FRAGMENT error code");

  ur_decoder_free(d);
  free(bytewords);
  free(ur);
}

static void test_malformed_cbor(void) {
  printf("\n=== malformed_cbor ===\n");

  // Major type 2 (byte string), says "1-byte length follows" but buffer ends.
  uint8_t truncated_len[] = {0x58};
  ASSERT(bytes_from_cbor(truncated_len, sizeof(truncated_len)) == NULL,
         "rejects CBOR with missing length byte");

  // Declares 0x20 (32) bytes of body, but buffer has 0 body bytes.
  uint8_t len_overrun[] = {0x58, 0x20};
  ASSERT(bytes_from_cbor(len_overrun, sizeof(len_overrun)) == NULL,
         "rejects CBOR with length exceeding buffer");

  // Major type 4 (array) — valid CBOR, but wrong type for bytes.
  uint8_t wrong_type[] = {0x80};
  ASSERT(bytes_from_cbor(wrong_type, sizeof(wrong_type)) == NULL,
         "rejects CBOR with wrong major type for bytes");

  uint8_t psbt_trailing[] = {0x41, 0x00, 0x00};
  ASSERT(psbt_from_cbor(psbt_trailing, sizeof(psbt_trailing)) == NULL,
         "psbt_from_cbor rejects trailing bytes");
}

int main(void) {
  printf("=== UR Negative-Path Tests ===\n");
  test_null_and_empty();
  test_malformed_ur();
  test_bad_crc();
  test_truncated_fragment();
  test_empty_fragment_payload();
  test_malformed_cbor();

  printf("\n=== Summary ===\n");
  printf("Tests passed: %d/%d\n", asserts - failures, asserts);
  return failures == 0 ? 0 : 1;
}
