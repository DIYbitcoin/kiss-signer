// Silent Payments (BIP352/374/375) crypto: no LVGL, no globals besides a lazy
// secp context. Everything returns 0 on success, negative on failure.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// sp1/tsp1 bech32m re-encode of PSBT_OUT_SP_V0_INFO for display. cap >= 120.
int sp_address_encode(const uint8_t scan33[33], const uint8_t spend33[33],
                      bool testnet, char *out, size_t cap);
