//
// crc32.c
//
// Copyright © 2025 Krux Contributors
// Licensed under the "BSD-2-Clause Plus Patent License"
//
// CRC32 checksum implementation for data integrity verification.
// Uses nibble-based (4-bit) lookup table to save 960 bytes ROM
// compared to full 256-entry table, at cost of 2 lookups per byte.
//
// This is an independent implementation written using
// foundation-ur-py as a reference for testing and validation.
//

#include "crc32.h"

// Nibble-based CRC32 table (polynomial 0xEDB88320) - 64 bytes ROM
static const uint32_t crc32_table[16] = {
    0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
    0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
    0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c};

uint32_t crc32_calculate(const uint8_t *data, size_t length) {
  if (!data || length == 0)
    return 0;

  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < length; i++) {
    crc = (crc >> 4) ^ crc32_table[(crc ^ data[i]) & 0x0F];
    crc = (crc >> 4) ^ crc32_table[(crc ^ (data[i] >> 4)) & 0x0F];
  }
  return ~crc;
}
