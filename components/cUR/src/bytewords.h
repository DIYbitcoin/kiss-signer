#ifndef BYTEWORDS_H
#define BYTEWORDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Encode bytes to bytewords minimal string (with CRC32)
 * @param data Data to encode
 * @param data_len Length of data
 * @param encoded Output encoded string (allocated)
 * @return true on success, false on error
 */
bool bytewords_encode(const uint8_t *data, size_t data_len, char **encoded);

/**
 * Free bytewords result
 * @param ptr Pointer to free (can be char* or uint8_t*)
 */
void bytewords_free(void *ptr);

/**
 * Raw bytewords minimal decode (no CRC32 validation) - for internal UR use
 * @param encoded Encoded bytewords string
 * @param decoded Output decoded bytes (allocated)
 * @param decoded_len Output decoded length
 * @return true on success, false on error
 */
bool bytewords_decode_raw(const char *encoded, uint8_t **decoded,
                          size_t *decoded_len);

#endif // BYTEWORDS_H
