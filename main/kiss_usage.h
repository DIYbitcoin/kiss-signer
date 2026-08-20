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
// Device persists in a dedicated NVS namespace ("kissu") except in AMNESIC
// mode, where identifying fingerprint/index metadata stays in session RAM.
#pragma once
#include <stdint.h>

// Highest USED receive index for this wallet/network/type, or -1 if none yet.
int  kiss_usage_high(const uint8_t fp[4], int testnet, int script);

// Record receive index `idx` as used. Monotonic: a lower idx never lowers the
// stored high. Persists on device.
void kiss_usage_mark(const uint8_t fp[4], int testnet, int script, uint32_t idx);

// Forget everything (seed wipe, or test reset).
void kiss_usage_wipe(void);

// Session lifecycle hooks. Moving an AMNESIC wallet to persistent storage may
// promote its RAM high-water marks; locking always clears the RAM table.
void kiss_usage_persist_session(void);
void kiss_usage_forget_session(void);

// Batch several marks into one NVS commit. A multi-input spend marks one
// receive per input and a session flush marks the whole table; per-mark
// commits were an open/write/commit/close each. Wrap the burst; a mark outside
// a batch commits immediately as before.
void kiss_usage_batch_begin(void);
void kiss_usage_batch_end(void);
