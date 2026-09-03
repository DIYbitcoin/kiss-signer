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
    // Appended, never inserted: the enum is NVS storage order (above), so a
    // new term goes on the end or every bit means a different word.
    KISS_TERM_KEY,
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
// The same, landing with one term ALREADY OPEN. A caution that names a term
// links straight into it: an owner who taps "high fee" is asking what a fee
// is, and a list of three closed rows makes them ask again.
lv_obj_t *kiss_terms_list_at(lv_obj_t *scr, const int *ids, int n, int open_id);

// Whatever the band's left lane is owed, in rank order: the + hint first
// (once ever), then the count of terms nobody has read. The CALLER decides
// whether to call this at all, because the kit cannot see whether that lane
// already carries an action, a caution or a standing statement -- and a lane
// holding two lines holds none.
//
// The + hint waits until the [ ? ] hint is spent: two new marks arrived in one
// pass and only one of them is taught at a time.
//
// (ids, n) is the SCOPE this screen is responsible for, not the rows it has
// drawn: the settings page pages the full reference five at a time and still
// owns all of it, while the sign and pairing glossaries own three terms and
// two. It used to count the whole device on all three, so a page showing two
// terms reported eight unread and pointed nowhere -- "its not clear where is
// left to read". Counted this way the number and the unread dots under it are
// the same claim, and the answer to where is: look down.
void kiss_terms_hint(lv_obj_t *scr, const int *ids, int n);

// PAGE TWO, the one addition the def-row idiom needed. A term whose value is
// an artefact (a descriptor, 150 characters) or a figure with arithmetic
// behind it (the fee) has nowhere to put that inside a row, so the page it is
// on says which of its terms have a second page and builds them.
//
// `has` answers for a term id; `open` builds the page and owns everything on
// it, including the way back. Both NULL is the ordinary case -- the SETTINGS
// reference is a reference and has no transaction to do arithmetic on.
void kiss_terms_more_hook(bool (*has)(int id), void (*open)(int id));
// Which term is open right now, or -1. What a MORE control acts on.
int  kiss_terms_open_id(void);
void kiss_terms_leaving(void);
// A PAGE TURN inside the same screen: the open row is finished with, exactly
// as it is when the page is left, but the band's left lane is not -- it lives
// on the screen, which is still there. Calling kiss_terms_leaving here drops
// the band's pointer without deleting the object, and the next draw prints a
// second one over it.
void kiss_terms_turned(void);

// The four sets that have a tab today. Each is `const int[]` plus its count,
// so a page passes one pair and nothing else knows the order.
extern const int KISS_TERMS_SIGN[3];      // PSBT, THE FEE, CHANGE
extern const int KISS_TERMS_KEYS[3];      // PRIVATE KEY, FINGERPRINT, ACCOUNT
extern const int KISS_TERMS_PAIR[3];      // PRIVATE KEY, DESCRIPTOR, FINGERPRINT
extern const int KISS_TERMS_RECV[1];      // ACCOUNT
// All ten, for the SETTINGS reference. THE DECOY is in this list, which is
// why the row that opens it is absent in a decoy session.
extern const int KISS_TERMS_ALL[KISS_TERM_N];
// FOUR to a page. A definition row at n=5 opens to 148px and a two line
// definition with its TECHNICAL line under it needs 153, so the technical
// name lands past the row's own floor -- invisible until the row is open, and
// the TERM gate is what found it. n=4 opens to 182.
#define KISS_TERMS_PER_PAGE 4
#define KISS_TERMS_PAGES    ((KISS_TERM_N + KISS_TERMS_PER_PAGE - 1) / \
                             KISS_TERMS_PER_PAGE)

#endif
