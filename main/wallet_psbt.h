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
    char     reason[64];     // why STOP/CAUTION ("" when READY)
    wpsbt_out_t outs[WPSBT_MAX_OUTS];
} wpsbt_summary_t;

// Parse + verify. Returns 0 and fills *s even when s->status == WPSBT_STOP
// (the UI must say WHY); nonzero only if the bytes aren't a valid PSBT or no
// session is open. Holds the parsed PSBT internally for wallet_psbt_sign.
int wallet_psbt_load(const uint8_t *bytes, size_t len, wpsbt_summary_t *s);

// Sign the loaded PSBT with the session key, serialize into out.
// Refuses (nonzero) if nothing loaded, status is STOP, or session closed.
int wallet_psbt_sign(uint8_t *out, size_t out_len, size_t *written);

// Drop the held PSBT (call when leaving the sign flow).
void wallet_psbt_free(void);
