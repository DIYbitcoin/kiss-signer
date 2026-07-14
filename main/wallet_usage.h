// Receive-address reuse guard: remembers the highest receive index this wallet
// has actually USED (KISS showed it, or later signed a spend from it), so the
// Receive screen can hand out a fresh one and warn on a spent address.
//
// IMPORTANT: KISS has no chain view. "used" here means "KISS saw it used", a
// subset of on-chain reality; the coordinator remains the source of truth for
// next-unused. This guard prevents KISS-initiated reuse and teaches, it does
// not guarantee no reuse.
//
// State is keyed per wallet (master fingerprint) + network + script type, so a
// different passphrase-wallet, network, or address type keeps its own count.
// Device persists in a dedicated NVS namespace ("kissu"); host builds keep it
// in RAM (enough for the sim walk and the desktop tests).
#pragma once
#include <stdint.h>

// Highest USED receive index for this wallet/network/type, or -1 if none yet.
int  wallet_usage_high(const uint8_t fp[4], int testnet, int script);

// Record receive index `idx` as used. Monotonic: a lower idx never lowers the
// stored high. Persists on device.
void wallet_usage_mark(const uint8_t fp[4], int testnet, int script, uint32_t idx);

// Forget everything (seed wipe, or test reset).
void wallet_usage_wipe(void);
