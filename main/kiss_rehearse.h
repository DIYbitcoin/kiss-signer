// The backup rehearsal's decision core, and the fingerprint render guard.
//
// Extracted from kiss_ui.c for the same reason the KISS recogniser moved to
// kiss_coverword.c: kiss_ui.c links into no test binary, so the decisions it
// makes are invisible to every gate. Two of them shipped wrong and reached a
// bench before anything here noticed -- a wallet with no passphrase was asked
// to type "the exact backup passphrase", and a zeroed fingerprint rendered as
// 00000000 in a value card, a code that looks real and was about to be copied
// onto paper. Both are one-line decisions; both now live where kisstest links.
//
// No LVGL, no session state: callers pass in what they know, this answers.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Whether a 4 byte fingerprint is a real identity. Zero is the "no keys open"
// placeholder kiss_ui_forget_fp writes, and it is also what a failed
// derivation leaves behind -- so zero must never render as a code and never
// verify as one. (A real fingerprint of 00000000 exists with probability
// 2^-32 and costs that owner the display of this one figure, which is the
// safe way to be wrong.)
bool kiss_fp_known(const uint8_t fp[4]);

// What the rehearsal still needs once every word has matched.
typedef enum {
    KISS_REHEARSE_VERIFIED,         // words alone are the whole backup
    KISS_REHEARSE_NEED_PASSPHRASE,  // the exact passphrase must rederive the id
} kiss_rehearse_next_t;

// `no_passphrase` is kiss_session_decoy() at the caller: a wallet opened with
// no passphrase has nothing to rehearse beyond its words, and prompting for
// one anyway is unanswerable -- the fault this module exists to pin down.
kiss_rehearse_next_t kiss_rehearse_after_words(int no_passphrase);

// The passphrase leg's verdict. `got` is the fingerprint the retyped
// passphrase rederived; `want` is the identity of the keys that are open.
// A zeroed `want` never verifies, whatever `got` says.
bool kiss_rehearse_pass_ok(const uint8_t got[4], const uint8_t want[4]);
