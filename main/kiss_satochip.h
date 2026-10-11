// Satochip: the card keeps the wallet and signs; this signer never holds a
// private key.
//
// On top of an open, PIN-verified, genuine kiss_satocard session
// (kiss_satocard.h): read a public key and chain code at a BIP32 path, and
// have the card sign a 32-byte hash with the key at a path. Every public key
// the card returns is signed by its certified authentikey and checked here;
// every signature it makes is checked against the key it was asked to use
// before it leaves this module.
//
// Written from Toporin/SatochipApplet (CardEdge.java).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "kiss_satocard.h"

#define SATO_MAX_DEPTH 10

// Public key (compressed) and chain code at path, depth entries long; depth 0
// is the master key. Hardened indexes carry the 0x80000000 bit as usual.
int kiss_satochip_pubkey(kiss_satocard *c, const uint32_t *path, int depth,
                         uint8_t pub33[33], uint8_t chaincode[32]);

// Sign hash32 with the key at path. The card's signature is normalised to
// low S, verified against that key, and returned as DER (no sighash byte).
// *der_len is the length written; der must hold 72 bytes.
int kiss_satochip_sign(kiss_satocard *c, const uint32_t *path, int depth,
                       const uint8_t hash32[32], uint8_t *der, size_t *der_len);
