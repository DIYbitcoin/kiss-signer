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
//
// There is exactly ONE configurable stroke, the owner's. An earlier version let
// them choose a second one for the decoy as well, which was redundant the
// moment plain KISS became a permanent way in: it offered six ways to reach
// something already reachable with no stroke at all, and cost a second secret
// to recall under stress.
#pragma once
#include <stdbool.h>
#include <stdint.h>

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

// ---- unlock routing ----
// Which signer a finished draw opens. Values match the legacy unlock_kind
// return, so main.c's caller does not have to be rewritten around them.
enum {
    WDR_NONE  = -1,   // not a word: fall through to the game
    WDR_DECOY =  0,   // open the spare now, no prompt
    WDR_REAL  =  1,   // ask for the passphrase
};

// word_ok: the drawing before the final stroke read as the opening word.
// stroke:  WDG_* for the final stroke, WDG_NONE when there was not one.
//
// Deliberately does NOT consult kiss_duress_real(). It used to, and that was
// the whole leak: a device with a stroke configured opened the decoy on the
// bare word, while a device without one showed a passphrase keyboard, so a
// single gesture told an attacker which kind of device they were holding. Any
// recognised stroke now reaches the passphrase on every device, which costs
// nothing -- the stroke was never the secret, the passphrase is.
//
// Pure, and in this file rather than in main.c, because main.c is not linked
// into any test binary (sim/build_test.sh takes eighteen sources from main/ and
// not that one). A routing rule kept there cannot be tested on the host at all,
// which is how the fork survived long enough to become an audit finding.
int kiss_duress_route(bool word_ok, int stroke);

// The configured stroke, WDG_NONE when unset. No longer routes anything: it is
// the preference the Settings row shows, and what a future custom word will
// hang off. Its value is not observable from outside the device.
int kiss_duress_real(void);

// Persist it. WDG_NONE turns the feature off. Returns 0 only once the write is
// committed; rejects an out-of-range id rather than storing something the
// classifier can never match.
int kiss_duress_set(int gesture);

// Forget the configuration (seed wipe). The key is deliberately absent from
// kiss_seed.c's KEEP_KEYS, so a whole-partition erase already takes it; this
// exists for the host builds and for an explicit reset.
void kiss_duress_forget(void);

// Classify one stroke against the bounding box of the KISS the user just drew.
// Returns a WDG_* id, or WDG_NONE when the stroke is not a deliberate modifier.
//
// Plain ints, not lv_point_t: the desktop test runner links this without LVGL,
// and a false positive here is the one failure that shows an attacker a
// passphrase prompt, so it has to be testable off-device.
int kiss_duress_classify(const int *xs, const int *ys, int n,
                           int bx0, int by0, int bx1, int by1);

// ---- free marks: a stroke with nothing underneath it ----------------------
//
// The six ids above are meaningful only RELATIVE TO A WORD. An underline, a
// strike and an overline are the same flat stroke and differ solely by where
// they sit in the word's box. Take the word away and those three collapse into
// one, so a mark drawn on its own has FOUR shapes, not six. Saying that in a
// separate enum rather than reusing WDG_* keeps the two ideas from being
// silently interchangeable: a WDG_UNDERLINE means "below the word", and there
// is no below when there is no word.
//
// This is what a custom way in is built from -- an ordered sequence of these,
// replacing KISS on a device whose owner chose their own. 4 shapes over 4
// positions is 256 sequences, and that is the right order of magnitude: this
// is a door, not a key. The passphrase is the key, and a wrong sequence opens
// nothing and says nothing, so there is no oracle to grind against.
enum {
    WDF_NONE = 0,
    WDF_LINE,     // one flat stroke
    WDF_SLASH,    // one diagonal
    WDF_CIRCLE,   // a closed loop
    WDF_CHECK,    // a tick: down-right to a vertex, then up-right past it
    WDF_N
};

// Classify ONE free mark. No bounding box, because there is nothing to measure
// against: scale comes from an absolute floor instead, since a mark drawn on
// the game screen either takes a deliberate amount of room or is a wobble.
//
// Same refusal discipline as the framed classifier: anything ambiguous returns
// WDF_NONE rather than the nearest guess.
int kiss_duress_classify_free(const int *xs, const int *ys, int n);

// Display name key for a free-mark id. Returns -1 if invalid.
int kiss_duress_free_label_key(int mark);

// ---- the word: what the owner draws to reach either signer ----------------
//
// KISS by default, and KISS is what a device that has never been changed
// answers to. An owner who sets their own word replaces it outright: KISS then
// opens nothing at all, which is the point. A locked device shows Fruit Island
// and the KISS branding only exists past the unlock, so a device that does not
// answer to the word has the honest cover story of not being a signer.
//
// The word is an ordered run of WDF_* marks. Four is what the UI offers; the
// storage takes up to WDW_MAX so a longer one costs no migration.
//
// This is NOT a key and must not be treated as one. The passphrase is the key.
// A wrong sequence opens nothing and reports nothing -- there is no counter, no
// lockout and no oracle -- so the sequence only has to be beyond casual, which
// 256 is. What it must be instead is REPRODUCIBLE: the owner has to draw it the
// same way on a cold morning, months later, or their coins are paper-only.
#define WDW_MAX 8

// How many marks the configured word has. 0 means none is set, so KISS stands.
int kiss_duress_word_len(void);

// Copy the configured word out. Returns its length (0 when unset); out may be
// NULL to ask only for the length.
int kiss_duress_word_get(uint8_t out[WDW_MAX]);

// Persist a word. n == 0 clears it and puts KISS back. Every mark must be a
// real WDF_* id. Returns 0 only once the write is committed.
int kiss_duress_word_set(const uint8_t *marks, int n);

// Does this run of marks BEGIN with the configured word? Returns the number of
// marks consumed (the word's length) on a match, 0 otherwise.
//
// Prefix rather than equality, because the mark AFTER the word is what picks
// the door -- exactly as one stroke after KISS does today. The caller compares
// what is left over, and kiss_duress_route still decides.
int kiss_duress_word_match(const uint8_t *marks, int n);

// Which signer a custom word opens. `marked` is whether a mark followed it.
// The same rule as kiss_duress_route and implemented on top of it, so a WDF_*
// id never has to be passed into a WDG_* parameter.
int kiss_duress_route_marked(bool word_ok, bool marked);

// Display name key for a modifier id (an STR_* index from i18n_keys.h), so the
// picker and the confirm screens name them identically. Returns -1 if invalid.
int kiss_duress_label_key(int gesture);
