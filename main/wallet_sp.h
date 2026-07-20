// Silent Payments (BIP352/374/375) crypto: no LVGL, no globals besides a lazy
// secp context. Everything returns 0 on success, negative on failure.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// sp1/tsp1 bech32m re-encode of PSBT_OUT_SP_V0_INFO for display. cap >= 120.
int sp_address_encode(const uint8_t scan33[33], const uint8_t spend33[33],
                      bool testnet, char *out, size_t cap);

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
