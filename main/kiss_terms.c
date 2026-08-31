#include "kiss_terms.h"
#include "i18n.h"
#include <stdio.h>

// The table, in kiss_term_t order. A card is four strings: the CAPTION is the
// real term an owner will meet in their coordinator (PSBT, DESCRIPTOR,
// ENTROPY -- never a plain-words substitute invented here), the value is what
// it is FOR in four words, the plain sentence is what it means, and the
// technical line under that is its full name.
//
// The plain words live in the SENTENCE. That is the whole shape: a caption
// that says "the file" teaches a reader a word this device made up, and the
// first time they open Sparrow it is worth nothing.
typedef struct {
    int cap, val, plain, term;   // i18n keys; `term` may be -1
} term_card_t;

static const term_card_t CARDS[KISS_TERM_N] = {
    [KISS_TERM_SEED]    = { STR_T_SEED_CAP,   STR_T_SEED_VAL,
                            STR_T_SEED_PLAIN, STR_T_SEED_TERM },
    [KISS_TERM_PASS]    = { STR_T_PASS_CAP,   STR_T_PASS_VAL,
                            STR_T_PASS_PLAIN, STR_T_PASS_TERM },
    [KISS_TERM_FP]      = { STR_T_FP_CAP,     STR_T_FP_VAL,
                            STR_T_FP_PLAIN,   STR_T_FP_TERM },
    [KISS_TERM_PSBT]    = { STR_T_PSBT_CAP,   STR_T_PSBT_VAL,
                            STR_T_PSBT_PLAIN, STR_T_PSBT_TERM },
    [KISS_TERM_CHANGE]  = { STR_T_CHANGE_CAP, STR_T_CHANGE_VAL,
                            STR_T_CHANGE_PLAIN, STR_T_CHANGE_TERM },
    [KISS_TERM_FEE]     = { STR_T_FEE_CAP,    STR_T_FEE_VAL,
                            STR_T_FEE_PLAIN,  STR_T_FEE_TERM },
    [KISS_TERM_DESC]    = { STR_T_WATCH_CAP,  STR_T_WATCH_VAL,
                            STR_T_WATCH_PLAIN, STR_T_WATCH_TERM },
    [KISS_TERM_ACCOUNT] = { STR_T_PATH_CAP,   STR_T_PATH_VAL,
                            STR_T_PATH_PLAIN, STR_T_PATH_TERM },
    [KISS_TERM_ENTROPY] = { STR_T_RNG_CAP,    STR_T_RNG_VAL,
                            STR_T_RNG_PLAIN,  STR_T_RNG_TERM },
    [KISS_TERM_DECOY]   = { STR_T_DECOY_CAP,  STR_T_DECOY_VAL,
                            STR_T_DECOY_PLAIN, STR_T_DECOY_TERM },
    // The word this device says most often and had never defined. Every
    // other card leans on it -- the fingerprint NAMES your keys, the account
    // says WHICH keys open, the descriptor lets a coordinator watch them --
    // and the KEYS page's own help said "it gets public keys only" to a
    // reader who had never been told what a public key is.
    //
    // Captioned PRIVATE KEY, not KEYS: the caption is the term an owner meets
    // in their coordinator, and the pair is taught from the half that matters
    // to them. The public half is the sentence's second clause, which is also
    // where it belongs -- a public key is only interesting for what it lets
    // somebody else do.
    [KISS_TERM_KEY]     = { STR_T_KEY_CAP,    STR_T_KEY_VAL,
                            STR_T_KEY_PLAIN,  STR_T_KEY_TERM },
};

const int KISS_TERMS_SIGN[3] = { KISS_TERM_PSBT, KISS_TERM_FEE,
                                 KISS_TERM_CHANGE };
const int KISS_TERMS_KEYS[3] = { KISS_TERM_KEY, KISS_TERM_FP,
                                 KISS_TERM_ACCOUNT };
// Pairing is the moment a public key leaves, so it is the moment to say
// which half left.
const int KISS_TERMS_PAIR[3] = { KISS_TERM_KEY, KISS_TERM_DESC,
                                 KISS_TERM_FP };
const int KISS_TERMS_RECV[1] = { KISS_TERM_ACCOUNT };
const int KISS_TERMS_ALL[KISS_TERM_N] = {
    KISS_TERM_SEED, KISS_TERM_PASS, KISS_TERM_FP, KISS_TERM_PSBT,
    KISS_TERM_CHANGE, KISS_TERM_FEE, KISS_TERM_DESC, KISS_TERM_ACCOUNT,
    KISS_TERM_ENTROPY, KISS_TERM_DECOY, KISS_TERM_KEY,
};

static uint16_t s_mask;
static void (*s_persist)(uint16_t);

void kiss_terms_set_mask(uint16_t mask) { s_mask = mask; }
void kiss_terms_persist_hook(void (*persist)(uint16_t)) { s_persist = persist; }

bool kiss_term_read(int id)
{
    if (id < 0 || id >= KISS_TERM_N) return false;
    return (s_mask >> id) & 1u;
}

void kiss_term_mark_read(int id)
{
    if (id < 0 || id >= KISS_TERM_N) return;
    const uint16_t next = (uint16_t)(s_mask | (1u << id));
    if (next == s_mask) return;                // already read: no write
    s_mask = next;
    if (s_persist) s_persist(next);
}

int kiss_terms_unread(const int *ids, int n)
{
    int u = 0;
    for (int i = 0; i < n; i++)
        if (!kiss_term_read(ids[i])) u++;
    return u;
}

// The list on screen right now, and which of its rows is open. One page at a
// time carries terms, so one of each is enough -- and a stale pointer cannot
// outlive its screen, because every exit runs kiss_terms_leaving().
static lv_obj_t *s_list;
static int       s_ids[KISS_TERM_N];
static int       s_n;
static int       s_open = -1;

static lv_obj_t *s_band_scr;
static lv_obj_t *s_band;
// The scope the screen holding the band is responsible for. See the header:
// counting the whole device on a page showing two terms is a number pointing
// somewhere the reader cannot see.
static const int *s_band_ids;
static int        s_band_n;
static void terms_band_draw(void);

// A term is READ when its row is CLOSED. Opening one proves curiosity;
// closing it is the only moment this device can honestly observe somebody
// finishing, and it is the moment the dot goes out under their own finger.
static void terms_changed_cb(int open_idx, void *ud)
{
    (void)ud;
    // Pressing one is what teaches the plus, so the hint is owed no longer.
    if (open_idx >= 0) wt_row_seen_mark();
    if (s_open >= 0 && s_open < s_n && s_open != open_idx) {
        kiss_term_mark_read(s_ids[s_open]);
        wt_def_row_read(s_list, s_open);
    }
    s_open = open_idx;
    terms_band_draw();
}

void kiss_terms_turned(void)
{
    if (s_open >= 0 && s_open < s_n) kiss_term_mark_read(s_ids[s_open]);
    s_open = -1;
    s_list = NULL;
    s_n = 0;
}

void kiss_terms_leaving(void)
{
    // LEAVING with a row open counts too. Somebody who opens a term, reads it
    // and taps BACK has finished with it exactly as much as somebody who taps
    // the row again -- and only the second of those produces a close event,
    // so without this the commonest way to read a term never marked it.
    if (s_open >= 0 && s_open < s_n) kiss_term_mark_read(s_ids[s_open]);
    s_open = -1;
    s_list = NULL;
    s_n = 0;
    s_band = NULL;
    s_band_scr = NULL;
}

static bool (*s_more_has)(int);
static void (*s_more_open)(int);

// The band's left lane on a terms page, and the one object in it. It changes
// with the open row -- a term with a second page puts MORE there -- so the
// module owns it rather than the caller drawing once and forgetting.
static void more_cb(lv_event_t *e)
{
    (void)e;
    const int id = kiss_terms_open_id();
    if (id >= 0 && s_more_open) s_more_open(id);
}

static void terms_band_draw(void)
{
    if (s_band) { lv_obj_delete(s_band); s_band = NULL; }
    if (!s_band_scr) return;
    const int id = kiss_terms_open_id();
    if (id >= 0 && s_more_has && s_more_has(id)) {
        // A direction, so the arrow trails the word: page two is a page.
        s_band = wt_word_action(s_band_scr, LV_SYMBOL_RIGHT, tr(STR_H_MORE),
                                false, wt_accent(), true, more_cb, NULL);
        lv_obj_align(s_band, LV_ALIGN_BOTTOM_LEFT, WT_ACT_X,
                     -(LV_VER_RES - WT_ACTION_Y - WT_ACTION_H) - 8);
        return;
    }
    // ONE line, by rank. The hint is an instruction and outranks a report:
    // somebody who does not know the mark opens can do nothing with a count
    // of what they have not read.
    if (!wt_row_seen() && wt_help_seen()) {
        s_band = wt_standing(s_band_scr, tr(STR_H_HINT_ROW), WT_DIM, false);
        return;
    }
    const int unread = s_band_ids && s_band_n > 0
                     ? kiss_terms_unread(s_band_ids, s_band_n) : 0;
    if (unread <= 0) return;
    char band[64];
    snprintf(band, sizeof band, tr(STR_H_UNREAD_FMT), unread);
    s_band = wt_standing(s_band_scr, band, WT_DIM, false);
}

void kiss_terms_hint(lv_obj_t *scr, const int *ids, int n)
{
    s_band_ids = ids;
    s_band_n   = n;
    // A NEW screen means the old band went with the old screen, so the
    // pointer is dropped rather than deleted. The SAME screen means a page
    // turn, and then the old band is still there and has to go -- nulling it
    // and drawing another is how two of them ended up printed on top of each
    // other, which is what the TEXT gate reported.
    if (scr != s_band_scr) s_band = NULL;
    s_band_scr = scr;
    terms_band_draw();
}

void kiss_terms_more_hook(bool (*has)(int id), void (*open)(int id))
{
    s_more_has = has;
    s_more_open = open;
}

int kiss_terms_open_id(void)
{
    return (s_open >= 0 && s_open < s_n) ? s_ids[s_open] : -1;
}

lv_obj_t *kiss_terms_list(lv_obj_t *scr, const int *ids, int n)
{
    return kiss_terms_list_at(scr, ids, n, -1);
}

lv_obj_t *kiss_terms_list_at(lv_obj_t *scr, const int *ids, int n, int open_id)
{
    if (!scr || !ids || n <= 0) return NULL;
    if (n > KISS_TERM_N) n = KISS_TERM_N;

    wt_def_t defs[KISS_TERM_N];
    lv_memzero(defs, sizeof defs);
    for (int i = 0; i < n; i++) {
        const term_card_t *c = &CARDS[ids[i]];
        defs[i].cap        = tr(c->cap);
        defs[i].val        = tr(c->val);
        defs[i].plain      = tr(c->plain);
        defs[i].term       = tr(c->term);
        defs[i].term_label = tr(STR_G_TECHNICAL);
        // The unread DOT, on the same 8px lamp a state row uses. At rest and
        // with no pulse: it is a fact about this owner, not an alarm.
        defs[i].lamp       = !kiss_term_read(ids[i]);
        defs[i].lamp_col   = wt_accent();
        s_ids[i] = ids[i];
    }
    s_n = n;
    s_open = -1;
    s_list = wt_def_list(scr, defs, n);
    wt_def_list_on_change(s_list, terms_changed_cb, NULL);
    if (open_id >= 0)
        for (int i = 0; i < n; i++)
            if (ids[i] == open_id) { wt_def_list_open(s_list, i); break; }
    return s_list;
}
