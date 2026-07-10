#ifndef CRC32_H
#define CRC32_H

#include <stddef.h>
#include <stdint.h>

/**
 * Calculate CRC32 checksum for data
 * Uses IEEE 802.3 polynomial (0xEDB88320)
 * @param data Pointer to data buffer
 * @param length Length of data in bytes
 * @return CRC32 checksum
 */
uint32_t crc32_calculate(const uint8_t *data, size_t length);

#endif // CRC32_H