// Payee memory: has this wallet paid this destination before?
//
// A signer can prove an output is NOT its own change. It can never prove an
// address belongs to the payee the owner meant, so the destination is still
// compared by eye against a coordinator that may be lying. This narrows that
// one case where the owner has a history to compare against: paying the same
// exchange deposit, the same rent address, the same friend. If the device has
// signed to it before, the verify screen says so, and an address swapped
// underneath a recurring payment loses the mark.
//
// RECOGNITION ONLY. A first payment is silent. Marking every new destination
// would put a badge on the ordinary case, and a badge that fires on the
// ordinary case is read past within a week -- the whole reason it is worth
// showing "paid before" is that it is rare enough to notice missing.
//
// Nothing here is a security claim: seeing an address before does not make it
// the right one, and the owner may simply never have paid this payee from this
// device. It is a memory aid over an eye compare, and the screen says nothing
// stronger.
//
// WHAT IS STORED IS NOT AN ADDRESS. Each destination becomes a 56-bit id under
// a salt derived from this wallet's own master key, so the record cannot be
// read back into a list of who the owner pays, and two wallets on one device
// produce unrelated ids for the same payee. Persistence carries the same gate
// kiss_usage.h documents: encrypted flash and a non AMNESIC wallet, or the
// table lives in session RAM only.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Have we signed a payment to this destination before? `dest` is the address as
// the verify screen shows it -- the sp1/tsp1 string for a silent payment, whose
// on-chain script is different every time and would never match twice.
// False whenever no session is open, which is also what a fresh wallet says.
bool kiss_payee_seen(const char *dest);

// Record a signed payment to `dest`. Called once per non-change output, after
// the signature exists: an abandoned or refused transaction leaves no memory.
void kiss_payee_mark(const char *dest);

// Forget everything (seed wipe, or test reset).
void kiss_payee_wipe(void);

// Session lifecycle, mirroring kiss_usage: moving a wallet to persistent
// storage may promote what the session learned; locking clears the RAM table.
void kiss_payee_persist_session(void);
void kiss_payee_forget_session(void);

// Batch several marks into one NVS commit. A multi-output spend marks one payee
// per output and a session flush marks the whole table; per-mark commits were
// an open/write/commit/close each. Wrap the burst; a mark outside a batch
// commits immediately as before.
void kiss_payee_batch_begin(void);
void kiss_payee_batch_end(void);
