// Did the owner ever prove their paper backup against THIS device?
//
// The answer used to be a session flag in kiss_ui.c, cleared on every login
// screen close and set from exactly one place inside the setup wizard. So it
// read "paper never checked" on every ordinary boot, and the Settings route to
// the check (RECOVERY WORDS > VERIFY WORDS) could not turn it green at all.
// That is fine for a footnote and useless for the caution mark the SETTINGS
// backup row now carries, which has to mean something the second time it is
// seen.
//
// State is keyed per wallet (master fingerprint), so a different
// passphrase-wallet answers for its own paper and a replaced wallet starts
// unchecked. Keyed on the fingerprint itself, not a hash of it: a hash of four
// bytes is four bytes of brute force, so it would buy privacy it cannot
// deliver, and kiss_usage.c already writes raw fingerprints into its own NVS
// key names in the same partition. What actually keeps a fingerprint off a
// seized chip is flash encryption, not the shape of this key.
//
// Device persists in its own NVS namespace ("kissb"), which the whole-partition
// erase behind ERASE THIS WALLET and behind a wallet replacement takes with it.
// AMNESIC mode never persists, same rule as kiss_usage: a device that gets
// searched holds no wallet metadata either.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Has the paper for the wallet with this master fingerprint been checked?
bool kiss_backup_checked(const uint8_t fp[4]);

// Record that it has. Persists on device, except in AMNESIC mode.
void kiss_backup_mark(const uint8_t fp[4]);

// Which secret of these keys this signer has locked into an encrypted QR:
// the seed words (the encrypted backup) or the passphrase. One or the other
// is the rule, because each locked QR has its own password and two of them
// are two passwords to keep, and this is what lets the screens say so at
// the moment the second one is asked for.
//
// It records what this signer MADE, never what still exists: the paper may
// have been destroyed, or made on another device. So it is never shown as a
// state, only used to stop once and ask. Same store and the same erase as
// the flag above. On flash that is not encrypted it lives in RAM until the
// power goes, across a lock. In AMNESIC mode there is none at all: nothing
// is recorded and nothing is answered.
enum { KISS_LOCKED_NONE = 0, KISS_LOCKED_SEED = 1, KISS_LOCKED_PASS = 2 };
int  kiss_backup_locked(const uint8_t fp[4]);
void kiss_backup_set_locked(const uint8_t fp[4], int which);

// Forget everything (seed wipe, wallet replacement, or test reset).
void kiss_backup_forget(void);
