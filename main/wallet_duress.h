// Duress unlock: which signer the game's hidden gesture opens.
//
// A BIP39 passphrase already makes a completely different signer from the same
// words, so the decoy needs no new crypto -- it is this seed with an EMPTY
// passphrase. What this module adds is the choice of which one you land in,
// made before the passphrase keyboard is ever drawn.
//
// The threat this answers: a passphrase field on screen is itself a tell. It
// proves there is something to leave out of it, and you can never demonstrate
// that the passphrase you gave was the last one. So the obvious gesture --
// drawing KISS, which is the one an attacker who researched the device would
// try -- opens a real, working, funded signer with no prompt at all. The
// passphrase lives behind one extra stroke.
//
// Plain KISS ALWAYS opens the decoy once a gesture is configured. That is
// deliberate on two counts: a KISS-branded device where drawing KISS does
// nothing is more suspicious than one that opens a modest wallet, and nobody
// can be locked out by forgetting a stroke they chose months ago.
#pragma once

// The extra stroke drawn AFTER the word. Geometry only -- no recorded
// templates, nothing to match against, nothing that can drift out of tune with
// the hand that recorded it.
enum {
    WDG_NONE = 0,     // no gesture configured (or nothing recognized)
    WDG_UNDERLINE,    // wide flat stroke below the word
    WDG_OVERLINE,     // wide flat stroke above it
    WDG_STRIKE,       // wide flat stroke through the middle
    WDG_SLASH,        // one diagonal across the whole word
    WDG_CIRCLE,       // a loop around it (starts and ends together)
    WDG_CHECK,        // a tick: down-right, then up-right
    WDG_N
};

// Configured modifiers, WDG_NONE when unset. Unset means this device has never
// been through the duress step, so the unlock behaves exactly as it did before
// the feature existed (KISS -> passphrase login).
int wallet_duress_real(void);
int wallet_duress_decoy(void);

// Persist both. Pass WDG_NONE for both to turn the feature off. Returns 0 only
// once the write is committed. Rejects real == decoy, and rejects setting one
// without the other -- half a configuration is a lockout waiting to happen.
int wallet_duress_set(int real, int decoy);

// Forget the configuration (seed wipe). The keys are deliberately absent from
// wallet_seed.c's KEEP_KEYS, so a whole-partition erase already takes them;
// this exists for the host builds and for an explicit reset.
void wallet_duress_forget(void);

// Classify one stroke against the bounding box of the KISS the user just drew.
// Returns a WDG_* id, or WDG_NONE when the stroke is not a deliberate modifier.
//
// Plain ints, not lv_point_t: the desktop test runner links this without LVGL,
// and a false positive here is the one failure that shows an attacker a
// passphrase prompt, so it has to be testable off-device.
int wallet_duress_classify(const int *xs, const int *ys, int n,
                           int bx0, int by0, int bx1, int by1);

// Display name key for a modifier id (an STR_* index from i18n_keys.h), so the
// picker and the confirm screens name them identically. Returns -1 if invalid.
int wallet_duress_label_key(int gesture);
