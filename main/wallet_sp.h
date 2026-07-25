// Silent Payments (BIP352/374/375) crypto: no LVGL, no globals besides a lazy
// secp context. Everything returns 0 on success, negative on failure.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// sp1/tsp1 bech32m re-encode of PSBT_OUT_SP_V0_INFO for display. cap >= 120.
int sp_address_encode(const uint8_t scan33[33], const uint8_t spend33[33],
                      bool testnet, char *out, size_t cap);

// spscan/tspscan bech32m key expression (BIP-392), no origin, no sp() wrapper:
// version 0 + convertbits(scan_priv32 || spend_pub33, 8->5). This exposes the
// scan PRIVATE key (a coordinator can then detect every payment to this wallet;
// it still cannot spend). cap >= 120. Available in all builds (bech32m only).
int sp_scan_encode(const uint8_t scan_priv32[32], const uint8_t spend_pub33[33],
                   bool testnet, char *out, size_t cap);

// This wallet's own BIP352 receive keys from the master: scan pubkey at
// m/352'/coin'/0'/1'/0 and spend pubkey at m/352'/coin'/0'/0'/0, coin' = 1 on
// testnet else 0. Compressed pubkeys out. Device/test builds only (needs secp);
// pair with sp_address_encode to show the sp1/tsp1 receive address.
struct ext_key;
int sp_receive_keys(const struct ext_key *master, bool testnet,
                    uint8_t scan_pub33[33], uint8_t spend_pub33[33]);

// Scan PRIVATE key (m/352'/coin'/0'/1'/0) + spend PUBLIC key (m/352'/coin'/0'/0'/0)
// for the spscan watch-only export. Device/test builds only (needs secp/bip32).
int sp_scan_export_keys(const struct ext_key *master, bool testnet,
                        uint8_t scan_priv32[32], uint8_t spend_pub33[33]);

// ---- BIP376: spending a received silent-payment output (device/test only) ----

// This wallet's BIP352 spend PRIVATE key (m/352'/coin'/0'/0'/0), for BIP376
// spend verification and signing. Caller must wipe it. Returns 0 on success.
int sp_spend_privkey(const struct ext_key *master, bool testnet,
                     uint8_t spend_priv32[32]);

// BIP376 signing scalar d = (b_spend + tweak) mod n, verified so that the
// x-coordinate of d*G equals output_xonly32 (the P2TR key locking the coin being
// spent). This is BIP376's mandatory anti-theft check: a coordinator-supplied
// tweak that does not reproduce the on-chain key is refused. Writes d to d_out32
// (the BIP340 signer normalizes Y parity). Returns 0, or negative on any invalid
// input or a tweak that does not reproduce output_xonly32.
int sp_spend_signing_key(const uint8_t spend_priv32[32], const uint8_t tweak32[32],
                         const uint8_t output_xonly32[32], uint8_t d_out32[32]);

// BIP340 Schnorr signature of msg32 under scalar d32 (keypair handles Y parity).
// aux32 = deterministic-per-psbt randomness (never NULL). Self-verifies before
// returning. Returns 0 on success, negative on failure.
int sp_schnorr_sign(const uint8_t d32[32], const uint8_t msg32[32],
                    const uint8_t aux32[32], uint8_t sig64[64]);

// BIP340 verify: sig64 over msg32 under x-only pubkey xonly32. 0 = valid.
int sp_schnorr_verify(const uint8_t xonly32[32], const uint8_t msg32[32],
                      const uint8_t sig64[64]);

// BIP0352/Label expected spend key: spend_pub + hash("BIP0352/Label",
// scan_priv||ser32(label))*G. Used to recognize our own SP outputs; label 0 =
// change, else a labeled self-transfer. Returns 0, negative on invalid input.
int sp_label_spend(const uint8_t scan_priv32[32], const uint8_t spend_pub33[33],
                   uint32_t label, uint8_t out_spend33[33]);

// ---- BIP352 sender-side derivation (device/test builds only, needs secp) ----

// One SP recipient output. scan/spend from PSBT_OUT_SP_V0_INFO; xonly_out is
// filled by sp_derive_group.
typedef struct { uint8_t scan[33], spend[33], xonly_out[32]; } sp_recip_t;

// a_sum = sum of eligible input scalars mod n (each negated first when
// is_xonly and its pubkey has odd Y, per BIP352); A_sum = a_sum*G.
// Fails (negative) on any invalid scalar or a zero sum.
int sp_sum_privkeys(const uint8_t *privs32, const bool *is_xonly, size_t n,
                    uint8_t a_sum32[32], uint8_t a_sum_pub33[33]);

// input_hash = tagged("BIP0352/Inputs", lowest outpoint || A_sum). outpoints36
// = n * 36 bytes (txid internal byte order || vout LE32), ALL tx inputs.
int sp_input_hash(const uint8_t *outpoints36, size_t n,
                  const uint8_t a_sum_pub33[33], uint8_t out32[32]);

// share = a_sum * B_scan (BIP375 wire form: input_hash NOT applied here).
int sp_ecdh_share(const uint8_t a_sum32[32], const uint8_t scan33[33],
                  uint8_t share33[33]);

// Verifier-style derivation for ONE scan-key group in PSBT output order
// (k = index in the array): t_k = tagged("BIP0352/SharedSecret",
// (input_hash*share) || k_be32); P_k = spend + t_k*G -> xonly_out.
int sp_derive_group(const uint8_t share33[33], const uint8_t input_hash32[32],
                    sp_recip_t *recips, size_t n);

// BIP374 DLEQ: proves share = a*B for A = a*G without revealing a. proof64 =
// e||s. aux32 = fresh (or deterministic-per-psbt) randomness, REQUIRED.
// m32 = optional 32-byte message (NULL for the BIP375 flow). g33 = optional
// custom base point (NULL = secp256k1 G; non-NULL only for test vectors).
// Prove self-verifies before returning, per the spec.
int sp_dleq_prove(const uint8_t a32[32], const uint8_t b33[33],
                  const uint8_t aux32[32], const uint8_t *m32,
                  const uint8_t *g33, uint8_t proof64[64]);
// Returns 0 = valid; anything else = invalid (never trusts its inputs).
int sp_dleq_verify(const uint8_t a_pub33[33], const uint8_t b33[33],
                   const uint8_t share33[33], const uint8_t proof64[64],
                   const uint8_t *m32, const uint8_t *g33);
