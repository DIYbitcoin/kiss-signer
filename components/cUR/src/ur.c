//
// ur.c
//
// Copyright © 2025 Krux Contributors
// Licensed under the "BSD-2-Clause Plus Patent License"
//
// High-level UR parsing and type handling.
//
// This is an independent implementation written using
// foundation-ur-py as a reference for testing and validation.
//

#include "ur.h"
#include "ur_decoder.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>

ur_t *ur_new(const char *type, const uint8_t *cbor, size_t cbor_len) {
  if (!type || !cbor || cbor_len == 0)
    return NULL;

  ur_t *ur = safe_malloc(sizeof(ur_t));
  if (!ur)
    return NULL;

  ur->type = safe_strdup(type);
  if (!ur->type) {
    free(ur);
    return NULL;
  }

  ur->cbor = safe_malloc(cbor_len);
  if (!ur->cbor) {
    free(ur->type);
    free(ur);
    return NULL;
  }

  memcpy(ur->cbor, cbor, cbor_len);
  ur->cbor_len = cbor_len;

  return ur;
}

void ur_free(ur_t *ur) {
  if (!ur)
    return;

  if (ur->type) {
    free(ur->type);
  }
  if (ur->cbor) {
    free(ur->cbor);
  }
  free(ur);
}

const char *ur_get_type(const ur_t *ur) { return ur ? ur->type : NULL; }

const uint8_t *ur_get_cbor(const ur_t *ur) { return ur ? ur->cbor : NULL; }

size_t ur_get_cbor_len(const ur_t *ur) { return ur ? ur->cbor_len : 0; }

ur_t *ur_from_result(const struct ur_result *result) {
  if (!result)
    return NULL;
  const ur_result_t *ur_result = (const ur_result_t *)result;
  return ur_new(ur_result->type, ur_result->cbor_data, ur_result->cbor_len);
}