// The card worker: everything that talks to the Satochip card runs on one
// task on CPU1, never on the UI task, because a single card operation takes
// from a tenth of a second to several seconds.
//
// The UI starts a step and polls kiss_cardtask_state() from a timer, the same
// shape the unlock's prep_task uses in main.c. While a card wallet is open
// the worker keeps checking that the card is still in the reader; pulling it
// out is reported as CT_REMOVED, and main.c locks the wallet when it sees it.
//
// The simulator has no reader: its version only moves between states when a
// test seam tells it to, so the walk can photograph every screen.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CT_IDLE,
    CT_FIND_READER,   // waiting for a reader on the OTG port
    CT_FIND_CARD,     // a reader, no card in it yet
    CT_CONNECTING,    // a card: opening its applet and secure channel
    CT_NEED_PIN,      // waiting for the owner's PIN; detail = tries left
    CT_UNLOCKING,     // PIN sent: genuine check, then the keys; detail = keys read
    CT_READY,         // the card wallet is open; the UI opens the session
    CT_SIGNING,
    CT_SIGNED,        // detail = kiss_psbt_sign's result, length below
    CT_FAILED,        // detail = CT_E_*
    CT_REMOVED,       // the card left the reader while its wallet was open
} kiss_ct_state;

#define CT_E_NOT_CCID     1   // something is plugged in, but not a card reader
#define CT_E_CARD         2   // the card would not talk
#define CT_E_NOT_SATOCHIP 3   // a card, but no Satochip on it
#define CT_E_BLANK        4   // a Satochip with no wallet set up yet
#define CT_E_BLOCKED      5   // PIN blocked
#define CT_E_NOT_GENUINE  6   // the genuine card check failed
#define CT_E_NO_CERT      7   // a card with no certificate: cannot be checked
#define CT_E_TWO_FA       8   // 2FA is on: it signs only with a second device
#define CT_E_GONE         9   // the card or reader went away mid step

// Start looking for a reader and a card. From CT_IDLE, CT_FAILED or
// CT_REMOVED; anything open is closed first.
void kiss_cardtask_begin(void);

// One PIN attempt, in CT_NEED_PIN. The PIN is copied and the copy wiped once
// the card has answered; the caller wipes its own.
void kiss_cardtask_pin(const char *pin, size_t len);

// Sign the loaded PSBT with the card, in CT_READY. out stays the worker's
// until the state leaves CT_SIGNING.
void kiss_cardtask_sign(uint8_t *out, size_t cap);

// Stop: close the card wallet, power the card down, back to CT_IDLE. A
// signature in flight finishes first.
void kiss_cardtask_cancel(void);

kiss_ct_state kiss_cardtask_state(void);
int kiss_cardtask_detail(void);
size_t kiss_cardtask_signed_len(void);

// True while the worker is in the middle of something the UI must not
// interrupt (the autolock waits for it).
bool kiss_cardtask_busy(void);

#ifdef SIMULATOR
void kiss_cardtask_sim_set(kiss_ct_state st, int detail);
#endif
