// KISS Signer — step 5: PSBT parse / safety-verify / sign (libwally only).
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
    WPSBT_STOP    = 2,   // blocked: kiss_psbt_sign will refuse
} wpsbt_status_t;

// Caution reasons, accumulated so several can coexist (fee + dust + ...). The
// verify screen shows a short summary; DETAILS explains each. STOP uses `reason`
// for its single root cause and ignores these.
#define WPSBT_C_HIGHFEE      (1u << 0)   // fee is a big share of the send, or a fat rate
#define WPSBT_C_DUST_INPUT   (1u << 1)   // spending a tiny KISS-owned coin (dust-attack tell)
#define WPSBT_C_SMALL_CHANGE (1u << 2)   // change below the privacy threshold
#define WPSBT_C_DUST_CHANGE  (1u << 3)   // change below the standardness dust limit
#define WPSBT_C_MERGE_INS    (1u << 4)   // many coins spent at once (linked forever)
// Bit 5 was WPSBT_C_UNPROVEN_IN, an input amount we were TOLD but could not
// PROVE. It is a STOP now ("input amounts not proven"), not a caution — see
// the fee-lie note in kiss_psbt.c. The bit stays retired rather than reused:
// the flag rode on the SD fixtures and the settings a device already has.
#define WPSBT_C_GAP_CHANGE   (1u << 6)   // change parked past any scanner's reach

// Privacy threshold: coins/change under this are flagged (soft). Not a dust
// limit — that is a per-type standardness floor (see kiss_psbt.c).
#define WPSBT_PRIVACY_SATS   5000

// Merge bar: spending this many of our coins in one transaction proves to every
// observer that they share an owner, and no later behaviour undoes it. The bar
// is deliberately well above everyday coin selection. Two- and three-input
// spends are what a wallet does when no single coin covers the amount — the
// user did not choose that, so warning about it is fatigue, not information,
// and the same reasoning keeps WPSBT_HIGH_RATE_X10 high. At five the count
// stops looking like coin selection and starts looking like a decision:
// consolidating, or sweeping a wallet somewhere else. That is the case worth
// interrupting, because the coordinator can still be told to split it.
// Distinct input ADDRESSES, not inputs: see n_in_addr.
#define WPSBT_MERGE_INS      5

// Gap bar: the address index a change output has to reach before the signer
// says anything. An attacker holding only the account xpub can compute the
// address at m/../1/99999, hand it back as change, and the device will confirm
// it is genuinely ours -- because it is. The coins are not stolen and the seed
// still reaches them. What is lost is that every coordinator scans a WINDOW,
// twenty unused addresses past the last one it has seen, so change parked
// beyond that window does not appear in the balance and the owner is told
// their money is gone until somebody thinks to rescan deeply.
//
// A signer cannot know the real window: that is last_used + gap, and last_used
// lives in the coordinator. So the bar is a blunt one, set where it can only
// ever mean "nothing chose this". A coordinator burns roughly one change index
// per transaction, so a thousand is a decade of daily spends -- a real wallet
// does not arrive here, and a hidden one has to pass through.
//
// It is deliberately a CAUTION and not a STOP. Every STOP in this file is a
// coin that leaves and does not come back; this one comes back the moment
// anyone looks in the right place, and refusing it outright would hard-block
// the one owner who genuinely has spent that many times.
#define WPSBT_GAP_INDEX      1000

// High-fee-rate backstop (sat/vB * 10). Krux warns only on the fee-as-share-of
// -send (>=10%, which we match) and deliberately never thresholds sat/vB, since
// a signer can't know the going rate and a low bar just fatigues users in
// congestion. We keep a *high* backstop (300 sat/vB) purely to catch the case
// the share check misses: a large send at a fat-finger rate reads as a tiny %.
// Soft CAUTION either way; 300 stays clear of all but rare peak-day rates.
#define WPSBT_HIGH_RATE_X10  3000

typedef struct {
    char     addr[120];  // longest form: a ~117-char sp1/tsp1 silent payment address
    uint64_t sats;
    bool     is_change;  // carries OUR keypath AND the re-derived script matches
    bool     is_sp;      // BIP375 silent payment output: addr shows the sp1/tsp1
                         // re-encoding of its scan+spend keys, script derived here
    // Address index of a change output, m/../<change>/<index>. Only meaningful
    // when is_change; zero otherwise. Nothing carried it until WPSBT_GAP_INDEX
    // needed it, which is exactly why no screen could show WHERE change went --
    // the amount was on the glass and the address it landed on was not.
    uint32_t index;
} wpsbt_out_t;

typedef struct {
    uint32_t n_in, n_out;
    uint64_t in_sats, send_sats, change_sats, fee_sats;
    uint32_t est_vsize;      // estimated final vsize (P2WPKH witness estimate)
    uint32_t fee_rate_x10;   // sat/vB * 10 (one decimal, no floats on device)
    bool     rbf;
    uint32_t locktime;
    uint32_t n_unknown;      // unknown/proprietary PSBT fields (global+in+out)
    uint32_t n_sp;           // silent payment outputs among outs[]
    uint32_t n_sp_in;        // BIP376 inputs that spend a received silent payment
    uint32_t n_unproven_in;  // inputs whose amount came from a bare witness_utxo
    // Distinct scriptPubKeys among the inputs, which is the real size of a
    // consolidation. Two coins sitting on ONE address are already public proof
    // of a single owner, so joining them tells the chain nothing it did not
    // already have -- counting coins called that a privacy loss and it is not
    // one. This is what WPSBT_C_MERGE_INS is raised on and what the caution
    // states. Silent-payment inputs are not counted: they carry no address of
    // ours to reuse.
    uint32_t n_in_addr;
    bool     testnet;        // network this summary was verified under
    // Which test network the SCREEN names. The check above is the boolean --
    // a signet transaction and a testnet one are byte-identical here -- so
    // this is a label carried alongside the verdict, never part of it.
    uint8_t  net;            // KISS_NET_*
    uint32_t purpose;        // detected input type: 44/49/84, or 0 = mixed types
    wpsbt_status_t status;
    uint16_t caution_flags;  // WPSBT_C_* bitset (all triggered cautions)
    char     reason[64];     // STOP root cause, or the first caution ("" when READY)
    // Both sides of the compare that failed. "input is not this wallet's" is a
    // 4-byte memcmp (our_keypath) and it cannot say WHICH four bytes failed --
    // so a coordinator that displays the right master fingerprint but writes a
    // different one into the input derivation is indistinguishable from a coin
    // that genuinely belongs to someone else. That happens for real: importing
    // a bare zpub instead of the full descriptor leaves the coordinator without
    // the true origin, so it invents one.
    //
    // These went to the log in kiss_sign.c and nowhere else, on the reading
    // that an ambiguous diff is worse than none. That was backwards. The DIFF
    // is not ambiguous -- these eight characters are not those eight
    // characters, and that is a fact the owner can check against their own
    // paper. What is ambiguous is which of the two causes produced it, and a
    // refusal screen that states both is strictly more than one that states
    // neither: on this signer the likelier cause by far is that the owner is
    // in the wrong keys, which is a thing they can walk out of, and the screen
    // used to end the conversation there. The sign screen draws them now, and
    // the log line stays.
    uint8_t  our_fp[4];      // this device's master fingerprint
    uint8_t  in0_fp[4];      // first keypath fingerprint on input 0 (zero if none)
    uint32_t in0_keypaths;   // how many keypath entries input 0 carried at all
    wpsbt_out_t outs[WPSBT_MAX_OUTS];
} wpsbt_summary_t;

// ---- DETAILS page data (read on demand from the held PSBT) ----
#define WPSBT_MAX_INS 16

typedef struct {
    char     txid[65];       // previous txid, display (big-endian) hex
    uint32_t vout;
    uint64_t sats;
    uint32_t purpose;        // 44/49/84 (verify already proved it's ours), 352 = SP spend
    uint32_t change, index;  // our derivation tail m/../<change>/<index>
    bool     is_sp;          // BIP376: spends a received silent-payment (P2TR) coin
    bool     proven;         // sats came from a previous tx that hashes to the outpoint
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
int kiss_psbt_details(wpsbt_details_t *d);

// True while a transaction is parsed in RAM. Every reader above refuses
// without a session; this says whether the bytes are still there at all.
bool kiss_psbt_held(void);

// Parse + verify. Returns 0 and fills *s even when s->status == WPSBT_STOP
// (the UI must say WHY); nonzero only if the bytes aren't a valid PSBT or no
// session is open. Holds the parsed PSBT internally for kiss_psbt_sign.
int kiss_psbt_load(const uint8_t *bytes, size_t len, wpsbt_summary_t *s);

// Sign the loaded PSBT with the session key, serialize into out.
// Refuses (nonzero) if nothing loaded, status is STOP, or session closed.
int kiss_psbt_sign(uint8_t *out, size_t out_len, size_t *written);

// First 8 lower-case hex of sha256 over every input's signature bytes,
// concatenated in input order (ECDSA partial sigs = DER+sighash; taproot key
// sig = the 64/65-byte 0x13 field). Writes 8 chars + NUL to out. Signatures
// only, so it is transport- and PSBT-framing-independent: any signer that
// produced the same signatures yields the same fingerprint. Nonzero on a parse
// failure or a PSBT carrying no signatures.
int kiss_psbt_sig_fingerprint(const uint8_t *signed_psbt, size_t len,
                                char out[9]);

// Drop the held PSBT (call when leaving the sign flow).
void kiss_psbt_free(void);
