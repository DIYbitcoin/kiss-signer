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

// ---- NUT-00 and NUT-11 primitives ----

// Longest proof secret accepted, in bytes. A P2PK secret with a nonce, a key
// and a few tags is under 300; this leaves room for multisig tags and no more.
#define KISS_ECASH_SECRET_MAX 1024
// Longest SIG_ALL message built, in bytes, before it is hashed.
#define KISS_ECASH_MSG_MAX 16384

// NUT-00 hash_to_curve: Y = 02 || SHA256(SHA256(domain || x) || counter),
// counter a little endian uint32 from 0 until Y is a valid point.
int kiss_ecash_hash_to_curve(const uint8_t *x, size_t x_len, uint8_t y33[33]);

// NUT-00 blinded message B_ = hash_to_curve(x) + r*G. This is how the signer
// recomputes a change output's B_ from the secret and blinding factor the
// phone supplies, so it can tell its own change from anyone else's output.
int kiss_ecash_blind(const uint8_t *x, size_t x_len, const uint8_t r32[32],
                     uint8_t b33[33]);

// One input and one output of a swap or melt, as the mint will receive them.
// The hex strings are compared and hashed as given, so they must be the exact
// lowercase strings the phone submits; anything else is refused.
typedef struct {
    const char *secret;   // the proof's secret string, verbatim
    const char *c_hex;    // the proof's C, 66 lowercase hex characters
} kiss_ecash_in_t;

typedef struct {
    uint64_t amount;
    const char *b_hex;    // the blinded message B_, 66 lowercase hex characters
} kiss_ecash_out_t;

// SHA256 of the NUT-11 SIG_ALL message:
// secret_0 || C_0 || ... || amount_0 || B_0 || ... [|| quote_id]
// quote_id is NULL for a swap and the melt quote's id for a melt.
int kiss_ecash_sigall_hash(const kiss_ecash_in_t *in, size_t n_in,
                           const kiss_ecash_out_t *out, size_t n_out,
                           const char *quote_id, uint8_t hash32[32]);

// BIP340 Schnorr signature over hash32 with the key at {counter} under a
// PRIVATE account key: the 64 bytes NUT-11 puts in the witness.
int kiss_ecash_sign(const struct ext_key *account, uint32_t counter,
                    const uint8_t hash32[32], uint8_t sig64[64]);

// Watch only export of the OPEN session:
// "[<fp>/129373h/10h/0h/0h]xpub..." with the master fingerprint as origin.
// Refused while locked and for a card session, whose keys never come here.
int kiss_session_ecash_export(char *out, size_t out_len);
