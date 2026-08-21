// Receive-address reuse guard: remembers the highest receive index these keys
// has actually USED (KISS showed it, or later signed a spend from it), so the
// Receive screen can hand out a fresh one and warn on a spent address.
//
// IMPORTANT: KISS has no chain view. "used" here means "KISS saw it used", a
// subset of on-chain reality; the coordinator remains the source of truth for
// next-unused. This guard prevents KISS-initiated reuse and teaches, it does
// not guarantee no reuse.
//
// State is keyed per key set (master fingerprint) + network + script type, so a
// different passphrase, network, or address type keeps its own count.
// Device persists in a dedicated NVS namespace ("kissu") except in AMNESIC
// mode, where identifying fingerprint/index metadata stays in session RAM.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Highest USED receive index for these keys/network/type, or -1 if none yet.
int  kiss_usage_high(const uint8_t fp[4], int testnet, int script);

// Record receive index `idx` as used. Monotonic: a lower idx never lowers the
// stored high. Persists on device.
void kiss_usage_mark(const uint8_t fp[4], int testnet, int script, uint32_t idx);

// Forget everything (seed wipe, or test reset).
void kiss_usage_wipe(void);

// Session lifecycle hooks. Moving an AMNESIC signer to persistent storage may
// promote its RAM high-water marks; locking always clears the RAM table.
void kiss_usage_persist_session(void);
void kiss_usage_forget_session(void);

// Batch several marks into one NVS commit. A multi-input spend marks one
// receive per input and a session flush marks the whole table; per-mark
// commits were an open/write/commit/close each. Wrap the burst; a mark outside
// a batch commits immediately as before.
void kiss_usage_batch_begin(void);
void kiss_usage_batch_end(void);

// ---- PERSIST: does this signer save anything it can avoid saving ----
// The switch every signer in this class ships for settings storage, covering
// the two kinds of write the seed chooser does not: settings changes
// (kiss_settings.c gates its own store_u8 on this) and what this signer has
// seen -- the high-water marks in this file and the paid-before marks in
// kiss_payee.h. Default ON. OFF gates may_persist() in both of those;
// the session RAM tables keep working either way, so the reuse guard still
// answers within an unlocked session.
int  kiss_persist_enabled(void);
// Raw setter for the boot-time settings load only: no wipes, no promotion.
void kiss_persist_set_enabled(int on);
// The owner's switch. OFF also erases both history stores ("kissu", "kissp")
// at that moment -- stored settings stay, they are what the next boot runs
// on; ON promotes what the current session has learned. The NVS "prst" byte
// itself is written by kiss_settings.c, which owns that namespace.
void kiss_persist_apply(int on);

// ---- what a coordinator says the chain shows ----
// This device has no chain view, so UNUSED is a claim it cannot make alone. A
// coordinator can, and sends one line of text in the QR the Receive screen
// already scans at VERIFY.
//
// DISPLAY DATA. It picks a chip's text and colour and reaches no derivation, no
// address search and no signature. A wrong value costs the owner a fresher
// address than they needed, which is the direction to be wrong in.

// Not a wallet's ceiling: an absurdity bound, so a scanned number cannot strand
// the chip somewhere no owner could walk back from with NEXT.
#define KISS_USAGE_MAX_INDEX 100000

typedef struct {
    uint8_t  fp[4];
    int      testnet;    // 0 / 1
    int      script;     // WSCRIPT_*
    int      high;       // highest used external index; -1 = chain shows none
    uint32_t height;     // chain tip the coordinator synced to
    char     addr[128];  // the address to verify; holds a silent payment whole
} kiss_usage_msg_t;

// Parse "KISSU1 <FP8> <net> <script> <high> <height> <ADDR>".
// 0 = a well formed payload. Nonzero means this is not one, and the caller
// feeds it to the address path unchanged.
int kiss_usage_parse(const char *txt, size_t len, kiss_usage_msg_t *out);

// Has a coordinator spoken about these keys? 1 = yes, and *high / *height are
// filled. *high may be -1, meaning the chain shows nothing used, which is a
// real answer and not an absence.
int kiss_usage_chain_known(const uint8_t fp[4], int testnet, int script,
                           int *high, uint32_t *height);

// Record what a coordinator claims. Accepted only when height beats the stored
// one, which makes a re-shown QR inert and still lets a real correction land
// after a reorg or a rebuilt wallet. 1 = accepted, 0 = older, equal or refused.
int kiss_usage_chain_set(const uint8_t fp[4], int testnet, int script,
                         int high, uint32_t height);
