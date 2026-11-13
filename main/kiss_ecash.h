// Cashu ecash keys (NUT-11 pay to public key, derived per NUT-13).
//
// Ecash locked to a public key can only be spent with a Schnorr signature
// from the matching private key, and the mint checks that signature. NUT-13
// derives those keys from the BIP39 seed at m/129373'/10'/0'/0'/{counter},
// with the counter level NOT hardened. So the account key one level up can be
// exported as an xpub: a wallet holding it derives a fresh lock key for every
// incoming payment and holds ecash it cannot spend, the same way a watch only
// bitcoin wallet holds a descriptor.
//
// The keys are network free: a test mint and a real mint use the same keys,
// so the export is always an xpub, never a tpub.
//
// Everything returns 0 on success, non-zero on failure.
#pragma once
#include <stddef.h>
#include <stdint.h>

// The NUT-13 P2PK account, m/129373'/10'/0'/0'.
#define KISS_ECASH_PURPOSE 129373u
#define KISS_ECASH_ACCOUNT 10u

struct ext_key;

// The P2PK account key (private) from a master key. Caller wipes it.
int kiss_ecash_account(const struct ext_key *master, struct ext_key *out);

// The compressed public key at {counter} under an account key, private or
// public. A public account gives the same key, which is what lets a wallet
// lock ecash to this seed while holding only the export.
int kiss_ecash_pubkey(const struct ext_key *account, uint32_t counter,
                      uint8_t pub33[33]);

// Watch only export of the OPEN session:
// "[<fp>/129373h/10h/0h/0h]xpub..." with the master fingerprint as origin.
// Refused while locked and for a card session, whose keys never come here.
int kiss_session_ecash_export(char *out, size_t out_len);
