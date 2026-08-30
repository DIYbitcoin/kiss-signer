// The ten words this device teaches, in one place.
//
// Every page that carries a [ ? ] shows a SUBSET of them -- SIGN owes a
// reader PSBT, THE FEE and CHANGE; PAIRING owes DESCRIPTOR and FINGERPRINT --
// and before this the copy for each one lived on the page that happened to
// explain it. Two pages explaining the same word is two chances to explain it
// differently, which is how a reader ends up with two ideas of what an
// account is.
//
// So the CARD is the unit and the page names which cards it wants. The table
// carries the caption (the real term, never a plain-words substitute), a
// value, the plain sentence, and the technical name that goes under it.
#ifndef KISS_TERMS_H
#define KISS_TERMS_H

#include "lvgl.h"
#include "kiss_theme.h"

// ---- the terms an owner has read -----------------------------------------
// One bit per term, set when an open definition row is CLOSED -- closing is
// the moment somebody is done reading, and it is the only moment this device
// can honestly observe.
//
// The order below is the STORAGE order and must never be reshuffled: a bit
// means whichever term stood in its slot when it was set, so moving one
// silently marks a different word as read.
//
// It SURVIVES AN ERASE, deliberately. Learning is not a secret, and
// re-teaching an owner who already read all ten is a worse outcome than the
// leak of "this device has been used before" -- which the home page implies
// anyway. kiss_seed.c's storage_erase carries the key across the partition
// wipe beside the display preferences.
//
// The MASK lives here and the NVS byte lives in kiss_settings.c, which
// registers the hook at boot -- the same split wt_help_seen already uses, and
// the reason is the same: the gates that build the kit without the settings
// module still have to be able to ask.
typedef enum {
    KISS_TERM_SEED = 0,
    KISS_TERM_PASS,
    KISS_TERM_FP,
    KISS_TERM_PSBT,
    KISS_TERM_CHANGE,
    KISS_TERM_FEE,
    KISS_TERM_DESC,
    KISS_TERM_ACCOUNT,
    KISS_TERM_ENTROPY,
    KISS_TERM_DECOY,
    KISS_TERM_N
} kiss_term_t;
bool kiss_term_read(int id);
void kiss_term_mark_read(int id);
// How many of `ids` this owner has not read yet -- what a page's [ ? n ]
// counts, and what the SETTINGS row reports for all ten.
int  kiss_terms_unread(const int *ids, int n);
// The boot restore, and where a change goes.
void kiss_terms_set_mask(uint16_t mask);
void kiss_terms_persist_hook(void (*persist)(uint16_t mask));


// Build a definition list of the named terms on `scr`, with an unread dot on
// every one this owner has not read and the read-marking already wired: a row
// closed, or the page left with a row open, marks that term.
//
// The caller still owns the page -- title, trail, exits -- and MUST call
// kiss_terms_leaving() on every way out, or the last term read on the page
// does not count.
lv_obj_t *kiss_terms_list(lv_obj_t *scr, const int *ids, int n);
void kiss_terms_leaving(void);

// The four sets that have a tab today. Each is `const int[]` plus its count,
// so a page passes one pair and nothing else knows the order.
extern const int KISS_TERMS_SIGN[3];      // PSBT, THE FEE, CHANGE
extern const int KISS_TERMS_KEYS[2];      // FINGERPRINT, ACCOUNT
extern const int KISS_TERMS_PAIR[2];      // DESCRIPTOR, FINGERPRINT
extern const int KISS_TERMS_RECV[1];      // ACCOUNT
// All ten, for the SETTINGS reference. THE DECOY is in this list, which is
// why the row that opens it is absent in a decoy session.
extern const int KISS_TERMS_ALL[KISS_TERM_N];

#endif
