// The SLH-DSA-SHA2-128s public key this firmware checks update images against.
//
// All zeroes means NO KEY, and a build with no key refuses to install anything
// rather than installing whatever it is handed -- kiss_pqsig_available() is
// false and the update screen says the image cannot be checked. That is the
// same fail closed shape the secp256r1 side already has in kiss_fw_available().
//
// Public keys belong in the repository; docs/installer/kiss_signer.pub is here
// for the same reason. The SECRET half never is: tools/pq_release_key.py puts it
// in ~/.kiss-signer/, beside the minisign key, and tools/make_web_release.sh
// reads it from there.
//
// 32 bytes, which for this parameter set is PK.seed || PK.root.
#pragma once
#include <stdint.h>

static const uint8_t PQ_RELEASE_PUBKEY[32] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
