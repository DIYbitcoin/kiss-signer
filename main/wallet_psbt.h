// KISS Wallet — step 5: PSBT parse / safety-verify / sign (libwally only).
// Spec safety model: BLOCK (won't sign), CAUTION (user decides), READY.
// Needs an open wallet session (change re-derivation uses the session key).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define WPSBT_MAX_OUTS 16   // beyond this the signer STOPs (can't fully show/verify)

typedef enum {
    WPSBT_READY   = 0,
    WPSBT_CAUTION = 1,   // signable, but the user must accept the flagged reason
    WPSBT_STOP    = 2,   // blocked: wallet_psbt_sign will refuse
} wpsbt_status_t;

// Caution reasons, accumulated so several can coexist (fee + dust + ...). The
// verify screen shows a short summary; DETAILS explains each. STOP uses `reason`
// for its single root cause and ignores these.
#define WPSBT_C_HIGHFEE      (1u << 0)   // fee is a big share of the send, or a fat rate
#define WPSBT_C_DUST_INPUT   (1u << 1)   // spending a tiny KISS-owned coin (dust-attack tell)
#define WPSBT_C_SMALL_CHANGE (1u << 2)   // change below the privacy threshold
#define WPSBT_C_DUST_CHANGE  (1u << 3)   // change below the standardness dust limit

// Privacy threshold: coins/change under this are flagged (soft). Not a dust
// limit — that is a per-type standardness floor (see wallet_psbt.c).
#define WPSBT_PRIVACY_SATS   5000

// High-fee-rate backstop (sat/vB * 10). Krux warns only on the fee-as-share-of
// -send (>=10%, which we match) and deliberately never thresholds sat/vB, since
// a signer can't know the going rate and a low bar just fatigues users in
// congestion. We keep a *high* backstop (300 sat/vB) purely to catch the case
// the share check misses: a large send at a fat-finger rate reads as a tiny %.
// Soft CAUTION either way; 300 stays clear of all but rare peak-day rates.
#define WPSBT_HIGH_RATE_X10  3000

typedef struct {
    char     addr[92];
    uint64_t sats;
    bool     is_change;  // carries OUR keypath AND the re-derived script matches
} wpsbt_out_t;

typedef struct {
    uint32_t n_in, n_out;
    uint64_t in_sats, send_sats, change_sats, fee_sats;
    uint32_t est_vsize;      // estimated final vsize (P2WPKH witness estimate)
    uint32_t fee_rate_x10;   // sat/vB * 10 (one decimal, no floats on device)
    bool     rbf;
    uint32_t locktime;
    uint32_t n_unknown;      // unknown/proprietary PSBT fields (global+in+out)
    bool     testnet;        // network this summary was verified under
    uint32_t purpose;        // detected input type: 44/49/84, or 0 = mixed types
    wpsbt_status_t status;
    uint16_t caution_flags;  // WPSBT_C_* bitset (all triggered cautions)
    char     reason[64];     // STOP root cause, or the first caution ("" when READY)
    wpsbt_out_t outs[WPSBT_MAX_OUTS];
} wpsbt_summary_t;

// ---- DETAILS page data (read on demand from the held PSBT) ----
#define WPSBT_MAX_INS 16

typedef struct {
    char     txid[65];       // previous txid, display (big-endian) hex
    uint32_t vout;
    uint64_t sats;
    uint32_t purpose;        // 44/49/84 (verify already proved it's ours)
    uint32_t change, index;  // our derivation tail m/../<change>/<index>
} wpsbt_in_t;

typedef struct {
    char     txid[65];       // txid of the tx being signed, display hex
    bool     txid_final;     // segwit-only spends: signing can't change the txid
    uint32_t version;
    uint32_t locktime;
    uint32_t n_in;           // entries filled in ins[] (capped at WPSBT_MAX_INS)
    uint32_t n_total;        // real input count; > n_in means ins[] is partial
    wpsbt_in_t ins[WPSBT_MAX_INS];
} wpsbt_details_t;

// Fill *d from the currently loaded PSBT. 0 on success; nonzero if none
// held or the verifier said STOP (a refused tx gets no details page).
int wallet_psbt_details(wpsbt_details_t *d);

// Parse + verify. Returns 0 and fills *s even when s->status == WPSBT_STOP
// (the UI must say WHY); nonzero only if the bytes aren't a valid PSBT or no
// session is open. Holds the parsed PSBT internally for wallet_psbt_sign.
int wallet_psbt_load(const uint8_t *bytes, size_t len, wpsbt_summary_t *s);

// Sign the loaded PSBT with the session key, serialize into out.
// Refuses (nonzero) if nothing loaded, status is STOP, or session closed.
int wallet_psbt_sign(uint8_t *out, size_t out_len, size_t *written);

// Drop the held PSBT (call when leaving the sign flow).
void wallet_psbt_free(void);
