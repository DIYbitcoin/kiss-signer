// Step 7: first-boot seed wizard.
//   NEW:     word count -> camera entropy -> words on screen (write them down)
//            -> prove-backup quiz (3 rounds, 4 choices) -> stored.
//   RESTORE: word count -> type words (letter keyboard + wordlist autocomplete)
//            -> BIP39 checksum -> stored.
// The passphrase is NOT part of this file: after done_cb the caller runs the
// type-twice login (wallet_login_open_setup), where the fingerprint gets
// recorded. Compiled in both builds; sim feeds entropy via wallet_setup_entropy.
#include "wallet_setup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flag_imgs.h"
#include "i18n.h"
#include "wallet_crypto.h"   // wallet_entropy_mix3: camera + chip + taps -> seed
#include "wallet_scan.h"     // wallet_scan_open_raw: seed-QR import (amnesic load)
#include "wallet_seed.h"
#include "wallet_settings.h"   // wallet_lang_picker_open: first-boot language switch
#include "wallet_tapent.h"   // source 3: the timing of the user's own taps
#include "wallet_dice.h"     // alternate path: verifiable off-device dice rolls
#include "wallet_lastword.h" // cards path: the checksum valid last words
#include "wallet_cards_q.h"  // and whether those words were drawn or chosen
#include "wallet_proof.h"    // PROVE IT: one frame -> SD file + hash + burned words
#include "platform_sd.h"     // the proof needs a card before it can start
#include "wallet_theme.h"

// memset can be optimized away once the compiler sees a buffer is dead, and
// every wipe in this file is exactly that shape: the last read of the seed,
// the mnemonic or a source of entropy is the line above the wipe. libwally is
// not linked into the simulator build of this translation unit, so this is the
// same volatile store loop wallet_scan.c and wallet_seed_sd.c already carry
// rather than a fourth spelling of the idea.
static void wz_bzero(void *ptr, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len--) *p++ = 0;
}
#include "wallet_ui.h"

#ifndef SIMULATOR
#include "camera_spike.h"
#include "esp_cpu.h"         // esp_cpu_get_cycle_count: where the tap entropy is
#include "esp_random.h"      // esp_fill_random: TRNG when the camera never ran
#include "esp_timer.h"       // esp_timer_get_time
#endif

// main.c owns the full replacement/setup hand-off (including the type-twice
// passphrase ritual). The missing-SD recovery action must use that path rather
// than treating restored words like an ordinary one-passphrase unlock.
void wallet_begin_setup(void);

#define BG_COL   WT_BG
#define INK_COL  WT_INK
#define MUT_COL  WT_MUT
#define KEY_COL  WT_KEY
#define OK_COL   WT_OK
#define WARN_COL WT_WARN
#define STOP_COL WT_STOP

#define QUIZ_ROUNDS 3

static lv_obj_t *s_scr;
static lv_obj_t *s_parent;
static void (*s_done)(void);

static char s_w[24][12];        // the mnemonic under construction, word by word
static int s_nw;                // words collected so far
static int s_count;             // 12 or 24
static int s_wpage;             // which 12-word page the reveal is showing
static bool s_restore;
static bool s_verify;           // reuse the restore keypad to CHECK the paper backup
static bool s_verify_ok;        // result returned to the caller after this check
static bool s_load;             // AMNESIC per-session load, not first-boot setup
static int s_sd_problem;         // WSEED_ERR_* shown by the missing-card gate

static int s_quiz_round;
static int s_quiz_pos;          // word index being asked this round
static int s_quiz_correct;     // which of the 4 pills is right
static int s_quiz_asked[QUIZ_ROUNDS];   // positions already asked this pass

static char s_prefix[12];       // restore: letters typed for the current word
static lv_obj_t *s_word_lbl, *s_sug[3];

// cards mode (BLIND DRAW): 11 or 23 words drawn blind from the cut up BIP39
// word list and typed on the restore keyboard, then a last word picked from the
// checksum valid candidates. No machine randomness enters the seed.
//
// The identifiers still say "cards" because that was the first medium; the
// owner-facing copy deliberately does not, since the list can equally be 3D
// printed as tiles and shaken in a bag, and "deck" was read as playing cards.
static bool s_cards;
static bool s_dice;                     // the count screen is on the way to the keypad
static uint16_t s_cand[WLAST_MAX];   // checksum valid last word indices
static int s_ncand, s_cpage;
// The typed words as wordlist indices, and what the judge made of them. Only
// ever filled on the cards path; see cards_cksum_open.
static uint16_t s_cidx[24];
static wallet_cards_q_t s_cq;

static void choose_screen(void);
static void count_screen(void);
// SeedQR was only reachable from the amnesic per-session load, so someone
// restoring a wallet during setup had to type words they were holding as a QR.
// Same decoder, same staging; only the way back differs.
static void restore_scan_cb(lv_event_t *e);
static void cancel_cb(lv_event_t *e);
static void goto_count_cb(lv_event_t *e);
static bool s_qr_from_restore;
static void entropy_screen(void);
static void dice_screen(void);
static void method_screen(void);
// PROVE IT (docs/specs/prove-it.md): the burned proof run off the WHY overlay.
static void proof_screen(void);
static void proof_result_screen(void);
static void proof_words_screen(void);
static void ent_prove_cb(lv_event_t *e);
static void words_screen(void);
static void quiz_screen(void);
static void restore_screen(void);
static void cards_intro_screen(void);
static void cards_cksum_open(void);
static void cards_cksum_screen(void);
static void cards_warn_screen(void);
static void cards_block_screen(void);
static void cards_pick_screen(void);
static void verify_finish(void);
static void verify_finish_exit(void);

static void load_screen_fwd(void);
// BACK from the word-count screen: first-boot came from choose, an amnesic
// session came from the load screen
static void goto_choose_cb(lv_event_t *e)
{
    (void)e;
    if (s_load) load_screen_fwd(); else choose_screen();
}
static void goto_restore_cb(lv_event_t *e) { (void)e; restore_screen(); }

bool wallet_setup_active(void) { return s_scr != NULL; }
bool wallet_setup_verify_succeeded(void) { return s_verify_ok; }

static void wipe_state(void)
{
    memset(s_w, 0, sizeof s_w);
    memset(s_prefix, 0, sizeof s_prefix);
    s_nw = 0;
    // The candidate set narrows the last word to 128 (or 8) possibilities for
    // a seed the owner may still finish elsewhere, so it wipes with the words.
    s_cards = false;
    s_dice = false;
    memset(s_cand, 0, sizeof s_cand);
    s_ncand = 0;
    s_cpage = 0;
    // Same argument: the indices ARE the typed words, in a smaller container.
    memset(s_cidx, 0, sizeof s_cidx);
    s_cq = (wallet_cards_q_t){0};
}

static void close_all(void)
{
    wipe_state();
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
}

// A storage readiness check necessarily opens the sealed file, because a
// mounted card may still contain the wrong, corrupt, or incomplete wallet
// file. Keep the plaintext lifetime to this stack frame and defeat dead-store
// elimination explicitly; the normal login will load it again only after this
// gate succeeds.
static void setup_wipe(void *ptr, size_t n)
{
    volatile uint8_t *p = ptr;
    while (n--) *p++ = 0;
}

int wallet_setup_sd_status(void)
{
    char words[WSEED_MAX_MNEMONIC];
    int rc = wallet_seed_load(words, sizeof words);
    setup_wipe(words, sizeof words);
    return rc;
}

// ---- shared widgets: thin wrappers over the wallet_theme kit ----
static void mk_screen(const char *title, const char *sub)
{
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_scr = wt_screen(s_parent, title, sub);
}

// wt_screen's subtitle is deliberately one line: it has to land clear of the
// y=96 content line every screen builds against. Two setup screens were
// written with a two-sentence subtitle, so that one line could never hold them
// and both rendered the page's own explanation at font14. Both have their
// first content well below 96 -- the quiz question at 128, the entropy body at
// 140 -- so they draw the subtitle themselves with the second line they need.
static void mk_screen2(const char *title, const char *sub)
{
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_scr = wt_screen(s_parent, title, NULL);
    // The quiz puts its round counter at x=500 on the title's own row, so the
    // title gets 436 rather than the full 704. Without this the Italian and
    // Scandinavian titles ran straight through "spot check 1 of 3".
    wt_title_fit(s_scr, 436);
    lv_obj_t *l = wt_note(s_scr, sub, 48, 66, 704, 58);
    lv_obj_set_style_text_color(l, MUT_COL, 0);
}

static lv_obj_t *mk_pill(const char *txt, int x, int y, int w, lv_event_cb_t cb, void *ud)
{
    return wt_pill(s_scr, txt, x, y, w, cb, ud);
}

static lv_obj_t *mk_lbl(const char *txt, int x, int y, const lv_font_t *f, lv_color_t col)
{
    return wt_lbl(s_scr, txt, x, y, f, col);
}

// An explainer paragraph: reads at the big font when the copy is short enough
// to fit in w x h, drops to the small one when a translation is longer. Width
// is set so the hard newlines written for the small font can never run off the
// edge at the big one.
static lv_obj_t *mk_body(const char *txt, int x, int y, int w, int h, lv_color_t col)
{
    lv_obj_t *l = wt_lbl(s_scr, txt, x, y, wt_body_font(txt, w, h), col);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

// How many words the keyboard collects. In cards mode it stops one short of
// s_count: the last word is picked from the checksum candidates, never typed.
// s_count itself stays 12/24 the whole flow, because the reveal pager, the
// quiz and the entropy paths all read it as the seed length. Any future
// consumer of "how many words does the keyboard take" belongs here.
static int entry_target(void) { return s_cards ? s_count - 1 : s_count; }

// join s_w[0..s_nw) into a mnemonic string
static void join_words(char *out, size_t out_len)
{
    size_t o = 0;
    for (int i = 0; i < s_nw && o + 12 < out_len; i++) {
        if (i) out[o++] = ' ';
        o += (size_t)snprintf(out + o, out_len - o, "%s", s_w[i]);
    }
    out[o] = 0;
}

// small deterministic-enough RNG for quiz layout (NOT for entropy!)
static uint32_t ui_rand(void)
{
#ifdef SIMULATOR
    static uint32_t x = 7;                 // scripted taps need fixed layouts
#else
    static uint32_t x;
    if (!x) x = lv_tick_get() | 1;
#endif
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

// ---- store + hand over to the login ----
static void store_and_finish(void)
{
    char words[WSEED_MAX_MNEMONIC];
    join_words(words, sizeof words);
    // STAGE only: the seed reaches flash after the passphrase-twice + fingerprint
    // ritual (setup login commits it). Abandoning that leaves no half-made wallet.
    int rc = wallet_seed_stage(words);
    wz_bzero(words, sizeof words);
    if (rc == 0) {
        // Every path that stages a NEW seed says what it made of the draw, so a
        // clean rebuild clears the previous one. Camera and dice write theirs
        // through wallet_setup_entropy before words exist; cards has no entropy
        // call at all, and a restore is somebody else's draw with nothing to
        // say about it. WC_Q_OK packs to a nonzero byte, so it is written as 0
        // instead: the reader's "!= 0" has to keep meaning "there is something
        // to say".
        if (s_cards)
            wallet_seed_set_entropy_note(
                s_cq.verdict == WC_Q_OK || s_cq.verdict == WC_Q_SHORT
                    ? WSEED_ENTQ_NONE
                    : WSEED_ENTQ_CARDS | (unsigned)s_cq.verdict);
        else if (s_restore)
            wallet_seed_set_entropy_note(WSEED_ENTQ_NONE);
    }
    if (rc != 0) {                          // restore path: checksum failed
        mk_screen(tr(STR_W_CHECK_T), tr(STR_W_CHECK_S));
        mk_body(tr(STR_W_CHECK_B), 48, 140, 704, 256, STOP_COL);
        mk_pill(tr(STR_W_START_OVER), 48, WT_ACTION_Y, 240, goto_restore_cb, NULL);
        return;
    }
    void (*cb)(void) = s_done;
    close_all();
    if (cb) cb();
}

// ---- verify an existing backup: type the paper words, confirm they match the
// stored seed WITHOUT revealing it. Reuses the restore keypad; never stages or
// touches the seed. Reached from the wallet's BACKUP screen. ----
static void verify_finish_exit(void)
{
    s_verify = false;
    void (*cb)(void) = s_done;
    close_all();                        // wipes the typed words too
    if (cb) cb();                       // back to the wallet section
}

static void verify_exit_cb(lv_event_t *e)  { (void)e; verify_finish_exit(); }
static void verify_retry_cb(lv_event_t *e) { (void)e; restore_screen(); }

static void verify_finish(void)
{
    char typed[WSEED_MAX_MNEMONIC], stored[WSEED_MAX_MNEMONIC];
    join_words(typed, sizeof typed);
    int mism = wallet_seed_load(stored, sizeof stored) == 0
             ? wallet_seed_diff_word(typed, stored) : 0;
    memset(typed, 0, sizeof typed);
    memset(stored, 0, sizeof stored);
    wipe_state();                       // the entered words never linger

    if (mism < 0) {
        s_verify_ok = true;
        mk_screen(tr(STR_W_VOK_T), tr(STR_W_VOK_S));
        mk_lbl(tr_sym(LV_SYMBOL_OK, STR_W_VOK_MATCH), 48, 150,
               wt_font28(), OK_COL);

        // THIS is where the fingerprint gets written down, and it is the only
        // screen in the product where that instruction can land.
        //
        // The fingerprint is shown three times during setup: the reveal after
        // the passphrase, the warning screen after that, and the home chip
        // forever after. All three come after the words are written and the
        // paper is away. So STR_L_FP_NOTE2 has always asked "not the code you
        // wrote down?" about a code nothing ever told anyone to write down.
        //
        // Here the owner has just read every word off the paper into the
        // keypad. The paper is in their hand and a pen is next to it. Both ways
        // in reach this: the setup rehearsal, and WALLET > VERIFY BACKUP later.
        uint8_t fp[4];
        wallet_ui_last_fp(fp);
        const bool fp_known = (fp[0] | fp[1] | fp[2] | fp[3]) != 0;

        // Body height follows: 58 is two lines of font23 and leaves the block
        // below its room; with no fingerprint to show, the old 190 is free
        // again. (A real fingerprint of 00000000 exists with probability 2^-32
        // and costs that owner this one block, which is the safe way to be
        // wrong: never print a fingerprint that might be a zeroed buffer.)
        mk_body(tr(STR_W_VOK_B), 48, 196, 704, fp_known ? 58 : 190, MUT_COL);

        if (fp_known) {
            char fpbuf[16];
            snprintf(fpbuf, sizeof fpbuf, "%02X%02X%02X%02X",
                     fp[0], fp[1], fp[2], fp[3]);
            mk_lbl(tr(STR_L_FP_CAP), 48, 268, wt_font14(), MUT_COL);
            lv_obj_t *f = mk_lbl(fpbuf, 48, 290, wt_font28(), INK_COL);
            lv_obj_set_style_text_letter_space(f, 4, 0);
            // 58, not 48. At 48 this fitted two lines only at font14, which
            // made the one instruction the screen exists to give the smallest
            // text on it. 334 + 58 = 392 clears the 398 floor, and a long
            // translation still falls back to font14 inside the same slot.
            //
            // INK, matching setup_warn_screen. Not the accent and not WT_OK:
            // in GREEN theme those are the same colour, and the green tick
            // above is already carrying the status.
            mk_body(tr(STR_W_VOK_FP), 48, 334, 704, 58, INK_COL);
        }

        lv_obj_t *p = mk_pill(tr(STR_C_DONE), 48, WT_ACTION_Y, 300, verify_exit_cb, NULL);
        wt_pill_primary(p);
    } else {
        char buf[128];   // Cyrillic runs 2 bytes/char: 48 truncated every ru render
        snprintf(buf, sizeof buf, tr(STR_W_VBAD_FMT), mism + 1);
        mk_screen(tr(STR_W_VBAD_T), tr(STR_W_VBAD_S));
        mk_lbl(buf, 48, 150, wt_font28(), STOP_COL);
        wt_why_body(s_scr, tr(STR_W_VBAD_B), 206, STOP_COL, true);
        lv_obj_t *p = mk_pill(tr(STR_W_TYPE_AGAIN_BTN), 48, WT_ACTION_Y, 300, verify_retry_cb, NULL);
        wt_pill_primary(p);
        mk_pill(tr(STR_C_DONE), 610, WT_ACTION_Y, 140, verify_exit_cb, NULL);
    }
}

static void verify_start_cb(lv_event_t *e) { (void)e; restore_screen(); }

static void verify_intro_screen(void)
{
    mk_screen(tr(STR_W_VINTRO_T),
              tr(STR_W_VINTRO_S));

    // Band one: what the check claims, framed and drawn. The screen used to open
    // with three stacked grey paragraphs, which is the arrangement a reader
    // skips on the way to the button -- and this is the screen whose whole point
    // is that the reader understands what is about to be proven.
    //
    // 128..212, matching the passphrase intro and the fingerprint reveal, so the
    // setup flow keeps one skeleton from screen to screen.
    lv_obj_t *vcard = wt_card(s_scr, 48, 128, 704, 84);
    lv_obj_t *vcol = lv_obj_create(vcard);
    lv_obj_remove_style_all(vcol);
    lv_obj_set_pos(vcol, 0, 0);
    lv_obj_set_size(vcol, 704, 84);
    lv_obj_set_flex_flow(vcol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(vcol, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(vcol, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(vcol, LV_OBJ_FLAG_SCROLLABLE);
    wt_diagram_verify(vcol);

    // Band two: what it proves, and what it will never do. Accent on the claim,
    // WT_WARN on the limit, the same colour argument every other paired block on
    // the device makes. HEAD_ROOM budgets the font14 heading wt_why_block draws
    // above the body; see the identical note on the passphrase intro.
    {
        const char *b1 = tr(STR_W_VINTRO_W1_B), *b2 = tr(STR_W_VINTRO_W2_B);
        const int BW = 344, BY = 232, BH = WT_CONTENT_BOTTOM - BY;
        const int HEAD_ROOM = 46;
        const lv_font_t *f = wt_body_font2(b1, b2, BW - 14, BH - HEAD_ROOM - 8);
        wt_why_block(s_scr, tr(STR_W_VINTRO_W1_H), b1,  48, BY, BW, BH, f, wt_accent());
        wt_why_block(s_scr, tr(STR_W_VINTRO_W2_H), b2, 408, BY, BW, BH, f, WARN_COL);
    }

    lv_obj_t *p = mk_pill(tr(STR_W_TYPE_MY_WORDS), 48, WT_ACTION_Y, 300, verify_start_cb, NULL);
    wt_pill_primary(p);
    mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, verify_exit_cb, NULL);
}

// ---- quiz (prove the backup) ----
// Same reset as a wrong answer, but reached deliberately: an offered escape
// hatch back to the words when the person doing the check is not sure. The
// quiz round counter goes to zero so they start over, since answering any
// previous rounds correctly before doubting the paper does not prove them
// against the paper they now want to re-read.
static void words_go_again_cb(lv_event_t *e)
{
    (void)e;
    s_quiz_round = 0;
    s_wpage = 0;
    words_screen();
}

static void quiz_pick_cb(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (slot != s_quiz_correct) {           // wrong: look at the paper again
        s_quiz_round = 0;
        s_wpage = 0;                        // re-read from word 1, not mid-seed
        words_screen();
        return;
    }
    if (++s_quiz_round >= QUIZ_ROUNDS) {
        store_and_finish();
        return;
    }
    quiz_screen();
}

static void quiz_screen(void)
{
    char buf[96];   // translated prompt, 3 bytes/char worst
#ifdef SIMULATOR
    s_quiz_pos = (s_quiz_round * 5) % s_count;   // fixed for scripted taps
    s_quiz_correct = s_quiz_round;               // round 0 -> pill 0, etc.
#else
    // never re-ask a word already proven this pass: asking #6 twice checks
    // less of the backup (QUIZ_ROUNDS <= every word count, so this terminates)
    int again;
    do {
        s_quiz_pos = (int)(ui_rand() % (uint32_t)s_count);
        again = 0;
        for (int i = 0; i < s_quiz_round; i++)
            if (s_quiz_asked[i] == s_quiz_pos) again = 1;
    } while (again);
    s_quiz_correct = (int)(ui_rand() % 4);
#endif
    s_quiz_asked[s_quiz_round] = s_quiz_pos;
    mk_screen2(tr(STR_W_PROVE_T), tr(STR_W_PROVE_S));
    // 146, not 120: the subtitle is two readable lines now and bottomed at 124,
    // so the question was overlapping it by 4px.
    snprintf(buf, sizeof buf, tr(STR_W_WHICH_FMT), s_quiz_pos + 1);
    mk_lbl(buf, 48, 146, wt_font28(), INK_COL);

    for (int i = 0; i < 4; i++) {
        const char *w = s_w[s_quiz_pos];
        if (i != s_quiz_correct) {          // decoy from the wordlist
            const char *d = NULL;
            do {
                wallet_seed_word((int)(ui_rand() % 2048u), &d);
            } while (d && strcmp(d, s_w[s_quiz_pos]) == 0);
            w = d ? d : "static";
        }
        mk_pill(w, 48 + (i % 2) * 380, 208 + (i / 2) * 80, 340,
                quiz_pick_cb, (void *)(intptr_t)i);
    }

    // Progress as dots, not as text. Per SWEEP-01 edit 3: three 10px dots
    // under the title, WT_OK when the round has been passed and WT_EDGE when
    // not. The title already names the check, so the dots are the only counter.
    for (int i = 0; i < QUIZ_ROUNDS; i++) {
        lv_obj_t *dot = lv_obj_create(s_scr);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_pos(dot, 48 + i * 18, 176);
        lv_obj_set_style_radius(dot, 5, 0);
        lv_obj_set_style_bg_color(dot, i < s_quiz_round ? WT_OK : WT_EDGE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    }
    // The bottom 180px was empty, per SWEEP-01. A wrong answer already means
    // the paper is wrong, but there was no way back to the words without
    // leaving setup. This pill takes s_wpage back to 0 and reopens the words
    // screen, same as the wrong-answer path but reached deliberately.
    mk_pill(tr(STR_W_QUIZ_SHOW_AGAIN), 48, WT_ACTION_Y, 300,
            words_go_again_cb, NULL);
}

// ---- words on screen (the backup moment) ----
static void words_go_cb(lv_event_t *e)
{
    (void)e;
    s_quiz_round = 0;
    quiz_screen();
}

// This is the screen where the words get copied onto paper, and it rendered
// them at font14 -- the smallest type on the device, on the one screen where a
// misread character loses the wallet. Now font28, which means 24 words no
// longer fit beside the PAPER ONLY warning, so they page: 12 per page, 2
// columns of 6 at a 40px pitch from y=104, last row bottoming at 341 with the
// warning at 352.
//
// Paging changes one thing on purpose: I WROTE THEM DOWN appears only on the
// LAST page. Previously all 24 were on screen at once, so the button meant
// "I copied all of them"; with pages it would otherwise be reachable having
// seen half. NEXT comes first, the claim comes after.
#define WORDS_PER_PAGE 12

static void words_page_cb(lv_event_t *e)
{
    s_wpage += (int)(intptr_t)lv_event_get_user_data(e);
    words_screen();
}

// The centred column an explainer's aside band draws into. Two asides want the
// identical scaffold, and wallet_info.c already keeps its own copy of it; a
// third hand placed one is how the shapes start disagreeing.
static lv_obj_t *aside_col(lv_obj_t *p, int x, int y, int w)
{
    lv_obj_t *col = lv_obj_create(p);
    lv_obj_remove_style_all(col);
    lv_obj_set_pos(col, x, y);
    lv_obj_set_width(col, w);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 8, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    return col;
}

// The checksum, drawn: the owner's own first word beside the number the list
// gives it, then the arithmetic that decides the last one. Shared by the cards
// path's full screen and the words screen's "?" card, because two copies of an
// equation is how a product ends up telling two stories about the same rule.
//
// Numerals and glyphs, so it is the same width in every locale and needs no
// translation at all.
static void cksum_diagram(lv_obj_t *col)
{
    char n1[16], okw[16];   // 16: device gcc sizes %d for a full int
    snprintf(n1, sizeof n1, "%d", s_count - 1);
    snprintf(okw, sizeof okw, "%s 1", LV_SYMBOL_OK);

    // The premise, and until this row nothing in any flow had shown one: the
    // words ARE numbers. On the cards path it is checkable against the deck in
    // the owner's hand; on a generated seed it is the same claim about a word
    // they are looking at.
    int i0 = wallet_lastword_index(s_w[0]);
    if (i0 >= 0) {
        char num[16];
        snprintf(num, sizeof num, "%d", i0);
        lv_obj_t *r0 = wt_diagram_row(col);
        wt_chip(r0, s_w[0], false);
        wt_diagram_op(r0, "=");
        wt_chip(r0, num, false);
    }

    lv_obj_t *r1 = wt_diagram_row(col);
    wt_chip(r1, n1, false);
    wt_diagram_op(r1, "+");
    wt_chip(r1, okw, true);
    wt_diagram_op(r1, LV_SYMBOL_RIGHT);
    wt_chip(r1, LV_SYMBOL_OK, true);
}

static int aside_cksum(lv_obj_t *p, int x, int y, int w)
{
    lv_obj_t *col = aside_col(p, x, y, w);
    cksum_diagram(col);
    lv_obj_update_layout(col);
    return lv_obj_get_height(col);
}

// One mark per claim: the check passing, and the check failing.
static const char *const CKSUM_ICONS[] = { LV_SYMBOL_OK, LV_SYMBOL_WARNING };

// The "?" on the words screen. A newcomer copying twelve words has no reason to
// believe a miscopied one will ever be noticed, and that belief is what decides
// how carefully they write. The last word is a check on the other eleven, so a
// typo cannot silently open a different wallet -- and the whole lesson was
// already written, translated into 21 locales and shown to nobody outside the
// BLIND DRAW path, which is the one path a newcomer never takes.
//
// An overlay rather than a step in the flow: the words screen bottoms its grid
// at 344 against a paper warning at 352, so there is no band to put this in,
// and a screen between the words and the quiz would interrupt the copying it
// exists to improve.
static void words_help_cb(lv_event_t *e)
{
    (void)e;
    // Composed rather than a new string: WT_GRID_ICONS reads `TERM: definition`
    // per line, and the four keys it needs already ship in every locale. 768
    // matches the other composed body on the device (wallet_info.c).
    char body[768];
    snprintf(body, sizeof body, "%s: %s\n%s: %s",
             tr(STR_W_CKSUM_W1_H), tr(STR_W_CKSUM_W1_B),
             tr(STR_W_CKSUM_W2_H), tr(STR_W_CKSUM_W2_B));
    wt_explain_t x = {
        .title  = tr(STR_W_CKSUM_T),
        .sub    = tr(STR_W_CKSUM_S),
        .icon   = LV_SYMBOL_OK,
        .body   = body,
        .ok_txt = tr(STR_C_OK),
        .mode   = WT_GRID_ICONS,
        .icons  = CKSUM_ICONS,
        .aside  = aside_cksum,
    };
    wt_explain_open(s_scr, &x);
}

static void words_screen(void)
{
    const int pages = (s_count + WORDS_PER_PAGE - 1) / WORDS_PER_PAGE;
    if (s_wpage < 0) s_wpage = 0;
    if (s_wpage >= pages) s_wpage = pages - 1;

    mk_screen(tr(STR_W_WRITE_T), tr(STR_W_WRITE_S));
    // The "?" takes the right end of the title's own row, so the title gets 650
    // rather than the full 704 -- the same arrangement the quiz uses for its
    // round counter, and for the same reason: without it the longer titles run
    // straight through the chip.
    wt_title_fit(s_scr, 650);
    wt_help_chip(s_scr, 722, 24, MUT_COL, words_help_cb, NULL);

    const int first = s_wpage * WORDS_PER_PAGE;
    int on = s_count - first;
    if (on > WORDS_PER_PAGE) on = WORDS_PER_PAGE;
    const int rows = (on + 1) / 2;
    for (int k = 0; k < on; k++) {
        char buf[32];
        snprintf(buf, sizeof buf, "%2d. %.11s", first + k + 1, s_w[first + k]);
        mk_lbl(buf, 48 + (k / rows) * 352, 104 + (k % rows) * 40,
               wt_font28(), INK_COL);
    }
    // the one rule that matters while they are copying: loud, under the grid,
    // not buried at the end of the subtitle
    lv_obj_t *po = mk_lbl(tr(STR_W_PAPER_ONLY), 48, 352,
                          wt_body_font(tr(STR_W_PAPER_ONLY), 700, 40), WARN_COL);
    lv_obj_set_width(po, 700);
    lv_label_set_long_mode(po, LV_LABEL_LONG_WRAP);

    if (pages > 1 && s_wpage > 0)
        mk_pill(tr(STR_C_BACK), 48, WT_ACTION_Y, 160, words_page_cb,
                (void *)(intptr_t)-1);
    // There was no way OUT of this screen: BACK only pages between halves of
    // the word list, so someone who picked the wrong length, or who simply has
    // no paper to hand, could only go forward or pull the power. CANCEL is the
    // exit, and it sits on the first page only -- the pager owns that slot on
    // later ones, and cancelling from page 1 is a step away rather than a
    // neighbour of the button that claims the words are copied.
    //
    // Deliberately NOT a "back one step": that would regenerate the seed, and
    // anyone who had already copied page 1 onto paper would be holding words
    // for a wallet that no longer exists, with nothing on screen to say so.
    // Nothing is staged yet at this point (wallet_seed_stage runs after the
    // quiz), so leaving here stores nothing and destroys nothing.
    if (s_wpage == 0)
        mk_pill(tr(STR_C_CANCEL), 48, WT_ACTION_Y, 160, cancel_cb, NULL);
    if (s_wpage < pages - 1)
        mk_pill(tr(STR_R_NEXT), 430, WT_ACTION_Y, 320, words_page_cb,
                (void *)(intptr_t)1);
    else
        mk_pill(tr(STR_W_WROTE), 430, WT_ACTION_Y, 320, words_go_cb, NULL);
    // After the pills, never before: the first pill summons the opaque action
    // bar (action_bar_ensure), which swallowed this counter on page one, where
    // no pill preceded it. Page two only ever looked right because BACK was
    // built first there.
    if (pages > 1) {
        char cnt[40];   // large enough for conservative compiler range analysis
        snprintf(cnt, sizeof cnt, "%d-%d / %d", first + 1, first + on, s_count);
        mk_lbl(cnt, 232, 416, wt_font23(), MUT_COL);
    }
}

// ---- entropy (NEW path) ----
// The verdict the dice judge reached about the rolls behind the NEXT call to
// wallet_setup_entropy. Camera and taps leave it 0, which is what clears a
// previous device's worth of warning when a seed is rebuilt honestly.
static int s_ent_note;
void wallet_setup_entropy_note(int v) { s_ent_note = v; }

void wallet_setup_entropy(const uint8_t *entropy, unsigned len)
{
    if (!s_scr || s_restore || !entropy || (len != 16 && len != 32))
        return;
    char words[WSEED_MAX_MNEMONIC];
    unsigned need = s_count == 24 ? 32 : 16;
    if (len < need)
        return;
    if (wallet_seed_from_entropy(entropy, need, words, sizeof words) != 0)
        return;
    // Every seed creating path funnels through here, so this is the one place
    // that can promise the note describes the seed the owner actually has.
    wallet_seed_set_entropy_note(s_ent_note);
    s_ent_note = 0;
    // split into the word array for the reveal grid + quiz
    s_nw = 0;
    const char *p = words;
    while (*p && s_nw < 24) {
        int n = 0;
        while (p[n] && p[n] != ' ' && n < 11) n++;
        memcpy(s_w[s_nw], p, (size_t)n);
        s_w[s_nw][n] = 0;
        s_nw++;
        p += n;
        while (*p == ' ') p++;
    }
    // wally_bzero, not memset: `words` is dead after this line, so a compiler
    // is free to drop a plain memset and leave a mnemonic on the stack. The rest
    // of this file already reaches for the same primitive.
    wz_bzero(words, sizeof words);
    s_wpage = 0;
    words_screen();
}

// ---- source 3: the tap screen (see docs/specs/tap-entropy.md) ----
// One card, centred, that IS the tap target: nothing to aim at, no missed
// touch. The bar counts events that happened -- one segment per counted tap --
// rather than scoring their quality, because a quality score is a claim the
// code cannot prove and is exactly the false assurance the Coldcard postmortem
// warns about. The camera's two sources were captured on the previous screen
// and wait in the statics below until the last tap folds all three.
#define TAP_CARD_X   100
#define TAP_CARD_Y   130
#define TAP_CARD_W   600
#define TAP_CARD_H   260
#define TAP_SEG_GAP    3

static uint8_t s_cam_chain[32];   // source 1: the camera frame fold, frozen
static uint8_t s_cam_trng[32];    // source 2: the chip read at capture
static bool    s_cam_have;        // false when the camera failed: cam stays 0

static lv_obj_t *s_tap_segs[WTAP_TARGET];
static lv_obj_t *s_tap_count;
static lv_obj_t *s_tap_card;

// The clock the tap timing is read from. On device this is where the entropy
// lives; in the sim it is monotonic by construction so a scripted walk never
// trips the debounce (real timing entropy is the device's job, unit-tested on
// the host by sim/test_tapent.c).
static void tap_clock(uint64_t *us, uint32_t *cyc)
{
#ifdef SIMULATOR
    static uint64_t t;
    t += WTAP_DEBOUNCE_US + 1000;
    *us = t;
    *cyc = ui_rand();
#else
    *us = (uint64_t)esp_timer_get_time();
    *cyc = esp_cpu_get_cycle_count();
#endif
}

static void tap_fill_trng(uint8_t *b, size_t n)
{
#ifdef SIMULATOR
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(ui_rand() & 0xFF);
#else
    esp_fill_random(b, n);
#endif
}

// The seed did not build. All source material is already wiped by the time we
// know, so the only way on is a fresh collection: rebuild the entropy screen.
static void ent_retry_cb(lv_event_t *e) { (void)e; entropy_screen(); }

// The only place a seed comes into existence: three chains in, words out, and
// every intermediate wiped on the way through.
static void tap_done_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    uint8_t cam[32], trng[32], taps[32], seed[32];
    // A dead lens costs a source, not the wallet. What stands in for it has to
    // be a source the camera's absence does not already imply: reading the chip
    // twice would put both halves on one circuit, and that circuit going quiet
    // is the exact failure the three-way fold exists to survive. Timing jitter
    // is the one physical source on this board outside it (wallet_crypto.h),
    // and needs nobody present, which is why the SD device key already uses it.
    // If even that fails, cam stays zero and the seed is chip + taps -- the old
    // behaviour, not a worse one.
    if (s_cam_have) {
        memcpy(cam, s_cam_chain, 32);
        memcpy(trng, s_cam_trng, 32);
    } else {
        if (wallet_jitter(cam) != 0)
            memset(cam, 0, 32);
        tap_fill_trng(trng, 32);
    }
    // Source 2 has to prove where it came from, and this is the only check that
    // can: esp_fill_random hands back bytes and reports success whether or not
    // a noise source is behind it, so quality is unmeasurable and provenance is
    // the whole question (wallet_crypto.h). wallet_seed_sd.c has refused on this
    // since the device key existed; the seed -- the one piece of key material
    // the owner cannot rotate -- was the one path still taking it on trust.
    //
    // One check covers both reads of the chip, the one at capture
    // (camera_spike.c) and tap_fill_trng's above, because the flag is a latch
    // set once at boot and never cleared: false here means false there too. The
    // fills above it are wiped unread, since && stops before mix3 sees them.
    //
    // A refusal, not a warning, and deliberately not softened into "two sources
    // instead of three". A dead lens loses a source the fold was built to
    // survive. A chip whose noise was never switched on is a source that looks
    // exactly like a live one all the way to the words screen, and folding it
    // with the taps would hand back a seed every later check calls valid.
    int ok = wallet_trng_live() &&
             wallet_tapent_take(taps) == 0 &&
             wallet_entropy_mix3(cam, trng, taps, seed) == 0;
    // Every one of these is dead-store territory: last read is the line above,
    // so memset is elidable and wally_bzero is not. Same reasoning as
    // wallet_scan.c's scan_bzero and wallet_seed_sd.c's sd_bzero.
    wz_bzero(cam, sizeof cam);
    wz_bzero(trng, sizeof trng);
    wz_bzero(taps, sizeof taps);
    wz_bzero(s_cam_chain, sizeof s_cam_chain);
    wz_bzero(s_cam_trng, sizeof s_cam_trng);
    s_cam_have = false;
    wallet_tapent_reset();
    if (ok) {
        wallet_setup_entropy(seed, 32);
    } else {
        // Reachable one way now: the chip's noise source is not running. The
        // other two legs still cannot fail after a full 64-tap gate (take
        // succeeds, mix3 only fails on NULL). Either way the owner is told
        // plainly rather than left on a full bar that does nothing.
        //
        // TRY AGAIN restarts collection, which will not revive a chip that
        // never came up -- and that is the honest outcome. A device that cannot
        // prove where its randomness came from has no business minting a seed,
        // and no wording on this screen should imply otherwise. It stays one
        // screen rather than two because boot switches the source on before any
        // screen the owner can reach (main.c), so this is a guard against a
        // future reorder, and a guard does not earn 21 locales of its own copy.
        mk_screen(tr(STR_W_ENT_FAIL_T), NULL);
        mk_body(tr(STR_W_ENT_FAIL_B), 48, 118, 704, 260, INK_COL);
        mk_pill(tr(STR_C_TRY_AGAIN), WT_BACK_X, WT_ACTION_Y, 160, ent_retry_cb, NULL);
    }
    wz_bzero(seed, sizeof seed);
}

static void tap_hit_cb(lv_event_t *e)
{
    (void)e;
    lv_point_t p = {0, 0};
    lv_indev_t *indev = lv_indev_active();
    if (indev) lv_indev_get_point(indev, &p);

    uint64_t us; uint32_t cyc;
    tap_clock(&us, &cyc);
    if (!wallet_tapent_tap(us, cyc, (int16_t)p.x, (int16_t)p.y))
        return;                          // debounced: no light, no count

    unsigned n = wallet_tapent_count();
    if (n >= 1 && n <= WTAP_TARGET && s_tap_segs[n - 1])
        lv_obj_set_style_bg_color(s_tap_segs[n - 1], OK_COL, 0);
    if (s_tap_count) {
        char buf[16];
        snprintf(buf, sizeof buf, "%u / %u", n, (unsigned)WTAP_TARGET);
        lv_label_set_text(s_tap_count, buf);
    }
    if (n >= WTAP_TARGET) {
        lv_obj_remove_flag(s_tap_card, LV_OBJ_FLAG_CLICKABLE);
        // Hold the full bar so completion is seen, not inferred.
        lv_timer_create(tap_done_cb, 400, NULL);
    }
}

static void tap_screen(void)
{
    wallet_tapent_reset();
    memset(s_tap_segs, 0, sizeof s_tap_segs);
    s_tap_count = NULL;
    mk_screen2(tr(STR_W_ENT_TAP_T), tr(STR_W_ENT_TAP_S));

    s_tap_card = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_tap_card);
    lv_obj_set_pos(s_tap_card, TAP_CARD_X, TAP_CARD_Y);
    lv_obj_set_size(s_tap_card, TAP_CARD_W, TAP_CARD_H);
    lv_obj_set_style_radius(s_tap_card, 10, 0);
    lv_obj_set_style_border_width(s_tap_card, 1, 0);
    lv_obj_set_style_border_color(s_tap_card, WT_EDGE, 0);
    lv_obj_set_style_bg_color(s_tap_card, WT_PANEL, 0);
    lv_obj_set_style_bg_opa(s_tap_card, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_tap_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_tap_card, LV_OBJ_FLAG_CLICKABLE);
    // CLICKED, not PRESSED: one count per completed down-up, the same event the
    // rest of the app uses. A press dragged off the card never clicks, so a
    // drag counts once, not once per move.
    lv_obj_add_event_cb(s_tap_card, tap_hit_cb, LV_EVENT_CLICKED, NULL);

    wt_lbl(s_tap_card, tr(STR_W_ENT_SRC3_CAP), 18, 16, wt_font14(), MUT_COL);

    // 64 segments, not a smooth fill: a segment is a countable event, while a
    // continuous bar would imply a measurement of quality we refuse to claim.
    int inner = TAP_CARD_W - 36;
    int sw = (inner - (WTAP_TARGET - 1) * TAP_SEG_GAP) / WTAP_TARGET;
    for (int i = 0; i < WTAP_TARGET; i++) {
        lv_obj_t *seg = lv_obj_create(s_tap_card);
        lv_obj_remove_style_all(seg);
        lv_obj_set_pos(seg, 18 + i * (sw + TAP_SEG_GAP), 62);
        lv_obj_set_size(seg, sw, 26);
        lv_obj_set_style_radius(seg, 2, 0);
        lv_obj_set_style_bg_color(seg, WT_EDGE, 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_CLICKABLE);   // presses hit the card
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        s_tap_segs[i] = seg;
    }

    char buf[16];
    snprintf(buf, sizeof buf, "0 / %u", (unsigned)WTAP_TARGET);
    s_tap_count = wt_lbl(s_tap_card, buf, 18, 108, wt_font_mono28(), INK_COL);
    lv_obj_remove_flag(s_tap_count, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *note = wt_lbl(s_tap_card, tr(STR_W_ENT_TAP_NOTE), 18, 168,
                            wt_font14(), MUT_COL);
    lv_obj_set_width(note, TAP_CARD_W - 36);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_remove_flag(note, LV_OBJ_FLAG_CLICKABLE);

    // CANCEL only. Same rule the words screen documents: no screen without an
    // exit. Nothing is staged here, because the seed does not exist yet.
    mk_pill(tr(STR_C_CANCEL), WT_BACK_X, WT_ACTION_Y, 160, cancel_cb, NULL);
}

#ifdef SIMULATOR
// camera_spike.h is device only, and the sim builds the same screen. It will
// never produce a reason code, but ent_ui_sync's signature is shared.
enum { ENT_R_OK = 0, ENT_R_DARK, ENT_R_STILL };

static void sim_entropy_cb(lv_event_t *e)
{
    (void)e;
    // The sim has no camera: stand in for sources 1 and 2 so the tap screen,
    // which is the same on both builds, can be walked and shot for the docs.
    for (int i = 0; i < 32; i++) {
        s_cam_chain[i] = (uint8_t)(ui_rand() & 0xFF);
        s_cam_trng[i]  = (uint8_t)(ui_rand() & 0xFF);
    }
    s_cam_have = true;
    tap_screen();
}
#else
// The capture happens on the camera task; an LVGL timer collects the hash.
static lv_timer_t *s_ent_tmr;
static void ent_ui_sync(int pct, int reason);   // below; the poll drives it live

static void ent_poll_cb(lv_timer_t *t)
{
    // Reflect live accrual every tick: the SOURCE-1 fill, the readiness dot,
    // the state line, the chips and the action gate all read from this. Without
    // it the meter climbs invisibly and the screen looks frozen until a lucky
    // tap. The reason rides along so a bar that is NOT climbing can say why,
    // which since the light and novelty gates landed is a state a holder can
    // sit in indefinitely.
    ent_ui_sync(camera_entropy_progress(), camera_entropy_reason());

    // Capture freezes sources 1 and 2 into the statics and hands them here.
    // They wait through the tap screen; the mnemonic is not made until the
    // last tap folds all three in tap_done_cb.
    if (camera_entropy_sources(s_cam_chain, s_cam_trng)) {
        lv_timer_delete(t);
        s_ent_tmr = NULL;
        camera_entropy_stop();
        s_cam_have = true;
        tap_screen();
    }
}

static void ent_tap_cb(lv_event_t *e) { (void)e; camera_entropy_tap(); }

// The camera-failure route: no frame fold to carry, so the seed will come from
// the chip TRNG and the taps. s_cam_have stays false, which tap_done_cb reads
// as "cam is all-zero, read a fresh TRNG".
static void tap_only_cb(lv_event_t *e)
{
    (void)e;
    if (s_ent_tmr) { lv_timer_delete(s_ent_tmr); s_ent_tmr = NULL; }
    camera_entropy_stop();
    s_cam_have = false;
    tap_screen();
}

static void ent_back_cb(lv_event_t *e)
{
    (void)e;
    if (s_ent_tmr) { lv_timer_delete(s_ent_tmr); s_ent_tmr = NULL; }
    camera_entropy_stop();
    choose_screen();
}

// The explainer overlay is being torn down: put the camera screen back, with a
// fresh meter. Deferred by one tick, because entropy_screen() replaces the
// screen whose delete event is running right now.
static void ent_mix_reopen_cb(lv_timer_t *t) { lv_timer_delete(t); entropy_screen(); }
static void ent_mix_closed_cb(lv_event_t *e)
{
    (void)e;
    lv_timer_t *t = lv_timer_create(ent_mix_reopen_cb, 1, NULL);
    if (t) lv_timer_set_repeat_count(t, 1);
}
#endif

// PROVE IT leaves through the overlay's pill, not through its close. The
// overlay's DELETE handler schedules an entropy_screen rebuild one tick out
// (ent_mix_closed_cb), which would stomp the proof screen the moment it
// appeared -- so the handler comes off before the navigation tears it down.
static void ent_prove_cb(lv_event_t *e)
{
#ifndef SIMULATOR
    lv_obj_t *ovl = lv_event_get_user_data(e);
    if (ovl) lv_obj_remove_event_cb(ovl, ent_mix_closed_cb);
#else
    (void)e;
#endif
    proof_screen();
}

// The same detour from the entropy screen's own action row, where the camera
// is still running: hand it over the way BACK does, or the proof screen opens
// a second stream onto a framebuffer this one is still writing.
static void ent_audit_cb(lv_event_t *e)
{
    (void)e;
#ifndef SIMULATOR
    if (s_ent_tmr) { lv_timer_delete(s_ent_tmr); s_ent_tmr = NULL; }
    camera_entropy_stop();
#endif
    proof_screen();
}

// Between CAPTURE (48..348) and BACK (610..750). The mark is the frame the
// audit is about, the same glyph the recipe diagram and the camera method row
// already carry.
static void ent_audit_pill(void)
{
    wt_pill_icon(s_scr, LV_SYMBOL_IMAGE, tr(STR_W_PROOF_BTN),
                 368, WT_ACTION_Y, 224, WT_ACTION_H, ent_audit_cb, NULL);
}

// One glyph per body line, in order: the lens, the chip, the hand's tap, and
// the dice the fourth line sends an unconvinced reader to (the same LIST glyph
// method_screen puts on the DICE row, so the two marks agree).
static const char *const ENT_MIX_ICONS[] = {
    LV_SYMBOL_IMAGE,
    LV_SYMBOL_CHARGE,
    LV_SYMBOL_OK,
    LV_SYMBOL_LIST,
};

// The "?" on the equation card. This was a hand-built page -- three wt_row_x
// rows at hardcoded y, a takeaway label placed by eye -- and it looked like a
// different device to every other "?" on this one. wt_explain_open IS the
// explainer: title and badge where wt_screen puts them, a body font measured
// against the active locale rather than assumed, a dismiss pill, close on a tap
// anywhere. WT_GRID_ICONS takes the body as one `term: definition` per line and
// deals it into a badge grid, which is exactly the shape three named sources
// want and needs no string this file would otherwise have invented.
//
// The camera stops first: the card covers the panel and the preview owns a
// column of it, so a live stream would paint video across the text. BACK
// rebuilds the entropy screen, which restarts the camera on a fresh meter, and
// a fresh meter per visit is what the existing rule already wants.
static void ent_mix_help_cb(lv_event_t *e)
{
    (void)e;
#ifndef SIMULATOR
    if (s_ent_tmr) { lv_timer_delete(s_ent_tmr); s_ent_tmr = NULL; }
    camera_entropy_stop();
#endif
    // The subtitle is the claim, and it is a WEAKER claim than the one it
    // replaces. "an attacker must beat all three" is true of a bad RNG and
    // false of bad firmware: all three sources are produced by this device, so
    // firmware that lies produces all three lies together. What is honestly
    // true is that no ONE of them decides the wallet -- and the reader who
    // wants more than that is handed the dice path on the fourth line, because
    // dice is the only source here that can be checked off the device.
    wt_explain_t x = {
        .title  = tr(STR_W_ENT_MIX_T),
        .sub    = tr(STR_W_ENT_MIX_S),
        .icon   = LV_SYMBOL_SHUFFLE,
        .body   = tr(STR_W_ENT_MIX_B),
        .ok_txt = tr(STR_C_OK),
        .mode   = WT_GRID_ICONS,
        .icons  = ENT_MIX_ICONS,
    };
    lv_obj_t *ovl = wt_explain_open(s_scr, &x);
    // The doubt this card names is the doubt the proof answers: the pill sits
    // exactly where the reader has just been told the camera cannot be
    // checked. A tap on the pill stays on the pill (nothing here bubbles), so
    // the overlay's close-on-tap-anywhere is not in play.
    if (ovl)
        wt_pill(ovl, tr(STR_W_PROOF_BTN), 48, WT_ACTION_Y, 220, ent_prove_cb,
                ovl);
#ifndef SIMULATOR
    // The card is an OVERLAY, not a replacement screen, so the entropy screen
    // is still underneath with a stopped camera and no poll timer. Rebuild it
    // when the overlay goes: there is no BACK button to hang this on, because
    // an explainer closes on a tap anywhere.
    if (ovl) lv_obj_add_event_cb(ovl, ent_mix_closed_cb, LV_EVENT_DELETE, NULL);
    else     entropy_screen();
#endif
}

// ---- ADDENDUM-01 section 2: the entropy screen's right column ----
// The old screen had a title, a subtitle and nothing else, because the camera
// wrote all 480x800 every frame with LVGL suppressed, so anything drawn here
// was covered before it could be read.
//
// What that cost: the second source was invisible. The subtitle claims two
// sources and says neither one decides, and a reader had no way to check that
// claim at the exact moment their keys were being made. Worse, a reader who
// believed only the camera mattered would think a dim room produces a weak
// wallet, which is false.
//
// So the second source gets a bar that is already FULL when you arrive. That is
// the whole argument, made visually: the chip has been contributing since boot
// and there is nothing to aim. camera_spike.c now confines the preview to the
// left column (camera_spike_set_preview_rect) and main.c lets LVGL paint
// outside it, so this column is on screen while the video runs.
// Vertical budget, measured rather than guessed. mk_screen2 draws its own two
// line subtitle from y=66, which runs to about 120, so content starts at 128.
// The floor is WT_CONTENT_BOTTOM (398). That leaves 270: a 200 tall preview with
// its readiness line under it on the left, and two 96 tall cards plus the
// equation on the right, both landing clear of the bar.
#define ENT_CAM_X   48                  // the preview column, landscape UI space
#define ENT_CAM_Y   128
#define ENT_CAM_W   300
#define ENT_CAM_H   200
#define ENT_CARD_H  96
#define ENT_COL_X   372                 // the source cards
#define ENT_COL_W   380

static lv_obj_t *s_ent_bar1, *s_ent_bar2, *s_ent_state, *s_ent_dot;
static lv_obj_t *s_ent_capture;
// The equation's chips are STATE, not decoration, so they are held and driven.
static lv_obj_t *s_ent_c1, *s_ent_c2, *s_ent_c3, *s_ent_cr;

// One source card: caption, bit count right aligned, a bar, and a note. Returns
// the bar so the caller can drive it, and hands back the card itself through
// out_card for the one caller that has to strike the whole card through.
static lv_obj_t *ent_card(int y, int cap, int note, bool full, lv_obj_t **out_card)
{
    lv_obj_t *card = lv_obj_create(s_scr);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, ENT_COL_X, y);
    lv_obj_set_size(card, ENT_COL_W, ENT_CARD_H);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, WT_HAIR, 0);
    lv_obj_set_style_bg_color(card, WT_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *c = wt_lbl(card, tr(cap), 14, 12, wt_font14(), MUT_COL);
    lv_obj_set_style_text_letter_space(c, 1, 0);
    // The bit count is the number W_12_NOTE already taught ("128 bits of
    // entropy"), put next to the thing it describes. WT_OK on both cards: this
    // is a quantity that is present, not a status that varies.
    lv_obj_t *b = wt_lbl(card, tr(STR_W_ENT_BITS), 0, 12, wt_font14(), OK_COL);
    lv_obj_update_layout(b);
    lv_obj_set_pos(b, ENT_COL_W - 14 - lv_obj_get_width(b), 12);

    // Track, then the fill on top of it. Two plain objects rather than a slider:
    // nothing here is draggable and a slider brings knob styling to suppress.
    lv_obj_t *track = lv_obj_create(card);
    lv_obj_remove_style_all(track);
    lv_obj_set_pos(track, 14, 42);
    lv_obj_set_size(track, ENT_COL_W - 28, 5);
    lv_obj_set_style_radius(track, 100, 0);
    lv_obj_set_style_bg_color(track, WT_DIV, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *fill = lv_obj_create(track);
    lv_obj_remove_style_all(fill);
    lv_obj_set_pos(fill, 0, 0);
    lv_obj_set_size(fill, full ? ENT_COL_W - 28 : 0, 5);
    lv_obj_set_style_radius(fill, 100, 0);
    lv_obj_set_style_bg_color(fill, OK_COL, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_SCROLLABLE);

    // 52, not 58: the bar ends at 47 and two font14 lines are 38, so 52 lands
    // the second line on 90 inside a 96 tall card. At 58 the longest English
    // note wrapped to exactly 96 and lost its last row of pixels.
    lv_obj_t *n = wt_lbl(card, tr(note), 14, 52, wt_font14(), MUT_COL);
    lv_obj_set_width(n, ENT_COL_W - 28);
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    if (out_card) *out_card = card;
    return fill;
}

// Light one of the equation's chips, or leave it as an unmet promise. A chip is
// two objects -- the box and the label inside it -- so both move or the change
// reads as a rendering fault.
static void ent_chip_lit(lv_obj_t *c, bool lit)
{
    if (!c) return;
    lv_color_t col = lit ? OK_COL : WT_DIM;
    lv_obj_set_style_border_color(c, col, 0);
    lv_obj_t *l = lv_obj_get_child(c, 0);
    if (l) lv_obj_set_style_text_color(l, col, 0);
}

// What this function has already PAINTED. -1 is "nothing yet", so the first
// call after a screen build always draws every part.
//
// These exist because of the camera, not to save cycles. With a preview rect
// set, camera_spike stops flipping framebuffers and writes the video into the
// SAME buffer LVGL paints this column into (camera_spike.c, show_frame), and
// rot_flush in main.c accepts that an LVGL repaint lands on top of the video for
// one frame -- explicitly "only while something is actually being repainted".
//
// This sync runs on an 80ms poll, and LVGL has no early-out to lean on:
// lv_obj_set_local_style_prop ends in lv_obj_refresh_style on EVERY call, same
// value or not. So an unguarded sync invalidates a dozen objects 12.5 times a
// second for as long as the camera is live, "something is being repainted"
// becomes permanently true, and the owner watches a black box crawl over their
// own video. Comparing first is what keeps the steady state silent.
static int s_ent_last_pct   = -1;
static int s_ent_last_key   = -1;
static int s_ent_last_ready = -1;    // tri-state, so the first paint is forced

// Drive source one's bar, the state line and the equation from the camera's
// meter. Runs on the same LVGL timer that polls for the capture result.
static void ent_ui_sync(int pct, int reason)
{
    if (s_ent_bar1 && pct != s_ent_last_pct) {
        lv_obj_set_width(s_ent_bar1, (ENT_COL_W - 28) * pct / 100);
        s_ent_last_pct = pct;
    }
    bool ready = pct >= 100;

    // Four states, and each one is an INSTRUCTION. It used to be two lines, and
    // the not-ready one ("point at something with more detail") was a guess
    // dressed as advice: the screen had no idea whether the view was too dark
    // or simply not arriving, so it said the same thing either way. Now that a
    // frame can score zero for two different reasons, a holder watching a bar
    // that will never move on its own has to be told which one they are in.
    //
    // Keyed on the STRING rather than on `reason`, because two reasons can pick
    // the same line and re-setting a label to the text it already holds is a
    // full invalidation for no visible change.
    int k = ready       ? STR_W_ENT_DONE1
          : reason == ENT_R_DARK  ? STR_W_ENT_LOW
          : reason == ENT_R_STILL ? STR_W_ENT_STILL
          : STR_W_ENT_GOING;
    if (s_ent_state && k != s_ent_last_key) {
        lv_label_set_text(s_ent_state, tr(k));
        lv_obj_set_style_text_color(s_ent_state,
                                    ready ? OK_COL
                                    : k == STR_W_ENT_GOING ? MUT_COL : WARN_COL, 0);
        s_ent_last_key = k;
    }

    // Everything below moves on the SAME edge -- the moment source one fills --
    // so it is one comparison and not five. Chips 2, 3 and the result never
    // change at all after the first paint; they were being rewritten twelve
    // times a second to say exactly what they already said.
    //
    // The equation reads as state, not as a picture. Chip 2 is lit from the
    // moment the screen opens because the chip's own noise has been running
    // since boot and its bar is already full; chip 1 lights when the camera
    // fills; chip 3 stays dark because it is collected on the NEXT screen, and
    // so does the result, which is what the three of them ADD UP TO rather than
    // something already in hand. Drawing "12 WORDS" in the accent while a
    // source was still missing was the screen claiming to be finished.
    if ((int)ready != s_ent_last_ready) {
        if (s_ent_dot)
            lv_obj_set_style_bg_color(s_ent_dot, ready ? OK_COL : WT_EDGE, 0);
        ent_chip_lit(s_ent_c1, ready);
        ent_chip_lit(s_ent_c2, true);
        ent_chip_lit(s_ent_c3, false);
        ent_chip_lit(s_ent_cr, false);

        // The action pill is the discoverable form of "tap anywhere", which
        // still works. Disabled until source one is full, because a capture
        // below the gate is refused by camera_spike anyway and a button that
        // silently does nothing reads as a missed touch.
        if (s_ent_capture) {
            lv_obj_set_style_opa(s_ent_capture, ready ? LV_OPA_COVER : LV_OPA_40, 0);
            if (ready) lv_obj_add_flag(s_ent_capture, LV_OBJ_FLAG_CLICKABLE);
            else       lv_obj_remove_flag(s_ent_capture, LV_OBJ_FLAG_CLICKABLE);
        }
        s_ent_last_ready = ready;
    }
}

// The camera and dice paths both end at wallet_setup_entropy(); the cards path
// ends at the same words_screen from its own picker. This screen is the only
// fork between the three. Camera is convenient and multi-source; dice is
// single-source but recomputable off-device; cards is the owner's own words
// with no machine randomness at all.
//
// Camera restores the creation default (12 words) because a BACK out of a
// count screen can arrive here carrying s_count = 24. Its three source story
// and its result chip are both built around one length, and nothing in that
// path can be recomputed off the device anyway, so the extra 128 bits would be
// margin the owner has to take on trust.
//
// Dice asks. It is the one creation path whose entropy the owner supplies and
// can recompute on any computer, which makes it the path where somebody wants
// 256 bits badly enough to roll for them -- and the floor for that was written,
// reasoned and tested from the start (DICE_FLOOR_256 = 99, 99 x log2 6 ~ 256)
// while no screen could reach it. 12 is still what most owners should pick and
// still what the count screen lists first; the choice is no longer made for
// them by a callback.
static void method_cam_cb(lv_event_t *e)  { (void)e; s_cards = false; s_dice = false; s_count = 12; entropy_screen(); }
static void method_dice_cb(lv_event_t *e) { (void)e; s_cards = false; s_dice = true;  count_screen(); }
static void method_cards_cb(lv_event_t *e) { (void)e; s_cards = true; s_dice = false; count_screen(); }

static void method_screen(void)
{
    // NOT STR_W_HOWMANY: this screen forks camera against dice, and it wore the
    // word-count screen's subtitle asking how many words. Nothing on it answers
    // that question.
    mk_screen(tr(STR_W_NEW_T), tr(STR_W_METHOD_S));
    wt_row_x(s_scr, LV_SYMBOL_IMAGE, tr(STR_W_CHOOSE_NEW), tr(STR_W_NEW_NOTE), NULL,
             NULL, NULL, WT_INK, false, WT_CHOICE_X, WT_CHOICE_Y(0),
             WT_CHOICE_W, WT_CHOICE_H, method_cam_cb, NULL);
    wt_row_x(s_scr, LV_SYMBOL_LIST, tr(STR_W_CHOOSE_DICE), tr(STR_W_DICE_NOTE),
             NULL, NULL, NULL, WT_INK, false, WT_CHOICE_X, WT_CHOICE_Y(1),
             WT_CHOICE_W, WT_CHOICE_H, method_dice_cb, NULL);
    // KEYBOARD, not SHUFFLE. This row's whole subject is a word LIST the owner
    // cuts up and picks from, and a shuffle mark is the last thing on this
    // screen that reads as a deck of playing cards -- which is exactly how the
    // mode kept being misread. The glyph now says what the owner does here.
    wt_row_x(s_scr, LV_SYMBOL_KEYBOARD, tr(STR_W_CHOOSE_CARDS), tr(STR_W_CARDS_NOTE),
             NULL, NULL, NULL, WT_INK, false, WT_CHOICE_X, WT_CHOICE_Y(2),
             WT_CHOICE_W, WT_CHOICE_H, method_cards_cb, NULL);
    mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, goto_choose_cb, NULL);
}

// ---- PROVE IT (docs/specs/prove-it.md) ----
// The WHY overlay concedes that all three sources are made by this device and
// hands doubters the dice. This is the camera's answer: one frame becomes a
// file on the card, a SHA256 and 24 words, all checkable on any computer. The
// words are a real seed sitting on the card in cleartext, so they live in
// their own buffers -- never s_w -- and every exit wipes them. No path from
// here reaches the quiz or store_and_finish.
static char s_pf_w[24][12];       // the burned words; structurally not s_w
static uint8_t s_pf_hash[32];     // sha256 of the frame, shown lowercase like shasum
static int s_pf_page;
static lv_obj_t *s_pf_state;      // line under the viewfinder; SAVING paints here
static lv_obj_t *s_pf_shot, *s_pf_backp;
#ifndef SIMULATOR
static lv_timer_t *s_pf_tmr;
#endif

static void pf_wipe(void)
{
    wz_bzero(s_pf_w, sizeof s_pf_w);
    wz_bzero(s_pf_hash, sizeof s_pf_hash);
    s_pf_page = 0;
}

// Same splitter as wallet_setup_entropy, into the proof's own grid.
static void pf_split(const char *words)
{
    memset(s_pf_w, 0, sizeof s_pf_w);
    int nw = 0;
    const char *p = words;
    while (*p && nw < 24) {
        int n = 0;
        while (p[n] && p[n] != ' ' && n < 11) n++;
        memcpy(s_pf_w[nw], p, (size_t)n);
        s_pf_w[nw][n] = 0;
        nw++;
        p += n;
        while (*p == ' ') p++;
    }
}

// Every proof exit lands back on the entropy screen, which restarts the
// wizard's camera on a fresh meter -- the state BACK from the WHY overlay
// would have produced anyway.
static void pf_exit(void)
{
    pf_wipe();
    entropy_screen();
}

static void pf_back_cb(lv_event_t *e)
{
    (void)e;
#ifndef SIMULATOR
    if (s_pf_tmr) { lv_timer_delete(s_pf_tmr); s_pf_tmr = NULL; }
    camera_proof_stop();
    camera_proof_end();
#endif
    pf_exit();
}

static void pf_retry_cb(lv_event_t *e) { (void)e; proof_screen(); }
static void pf_done_cb(lv_event_t *e) { (void)e; pf_exit(); }

// The no-card gate and the failed-write screen are one shape: a framed SD row
// that says what is missing or what went wrong, TRY AGAIN, and a way out.
static void pf_gate_screen(const char *label, int body_key)
{
    mk_screen(tr(STR_W_PROOF_T), NULL);
    wt_row_x(s_scr, WT_ICON_SD, label, tr(body_key), NULL, NULL, NULL, WT_INK,
             false, WT_CHOICE_X, WT_CHOICE_Y(0), WT_CHOICE_W, WT_CHOICE_H,
             NULL, NULL);
    lv_obj_t *p = mk_pill(tr(STR_C_TRY_AGAIN), 48, WT_ACTION_Y, 240,
                          pf_retry_cb, NULL);
    wt_pill_primary(p);
    mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, pf_back_cb, NULL);
}

// Frame in hand: hash it, write it, derive the words. Runs one LVGL tick after
// the SAVING line paints (the do_sign_cb defer pattern), because the atomic
// write of 1.9MB blocks for seconds and a screen that freezes silently reads
// as a crash.
static void pf_finish(void)
{
    char words[WSEED_MAX_MNEMONIC];
    int rc;
#ifdef SIMULATOR
    rc = wallet_proof_run(NULL, 0, s_pf_hash, words, sizeof words);
#else
    camera_proof_stop();               // stream off BEFORE SDMMC gets touched
    size_t n = 0;
    const uint8_t *fr = camera_proof_data(&n);
    rc = fr ? wallet_proof_run(fr, n, s_pf_hash, words, sizeof words)
            : WPROOF_ERR_ARG;
    camera_proof_end();
#endif
    if (rc == WPROOF_OK) {
        pf_split(words);
        wz_bzero(words, sizeof words);
        s_pf_page = 0;
        proof_result_screen();
    } else {
        pf_wipe();
        pf_gate_screen(WPROOF_NAME, STR_W_PROOF_FAIL_B);
    }
}

#ifdef SIMULATOR
// The sim has no camera; the stubbed wallet_proof_run in sim_main.c writes a
// small real file and derives the fixed SIM_WORDS, so the walk exercises the
// same screens the device shows.
static void pf_sim_capture_cb(lv_event_t *e) { (void)e; pf_finish(); }
#else
static void pf_capture_cb(lv_event_t *e) { (void)e; camera_proof_capture(); }

static void pf_write_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    pf_finish();
}

static void pf_poll_cb(lv_timer_t *t)
{
    if (!camera_proof_done()) return;
    lv_timer_delete(t);
    s_pf_tmr = NULL;
    // The preview is frozen on exactly the captured frame. Say what happens
    // next and take both pills away: the write is not interruptible, and a
    // BACK that silently lost the race with it would read as a missed touch.
    if (s_pf_state) lv_label_set_text(s_pf_state, tr(STR_W_PROOF_SAVING));
    if (s_pf_shot) {
        lv_obj_set_style_opa(s_pf_shot, LV_OPA_40, 0);
        lv_obj_remove_flag(s_pf_shot, LV_OBJ_FLAG_CLICKABLE);
    }
    if (s_pf_backp) {
        lv_obj_set_style_opa(s_pf_backp, LV_OPA_40, 0);
        lv_obj_remove_flag(s_pf_backp, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_timer_create(pf_write_cb, 50, NULL);
}
#endif

static void proof_screen(void)
{
    pf_wipe();
    s_pf_state = s_pf_shot = s_pf_backp = NULL;
    if (platform_sd_probe() != 1) {
        pf_gate_screen(tr(STR_W_PROOF_SD_T), STR_W_PROOF_SD_B);
        return;
    }
    mk_screen2(tr(STR_W_PROOF_T), tr(STR_W_PROOF_S));

    // Left: the same viewfinder geometry as the entropy screen, so the proof
    // reads as the same camera being put to a different question.
    wt_viewfinder(s_scr, ENT_CAM_X, ENT_CAM_Y, ENT_CAM_W, ENT_CAM_H);
    s_pf_state = wt_lbl(s_scr, "", ENT_CAM_X, ENT_CAM_Y + ENT_CAM_H + 8,
                        wt_font14(), WARN_COL);
    lv_obj_set_width(s_pf_state, ENT_CAM_W);
    lv_label_set_long_mode(s_pf_state, LV_LABEL_LONG_WRAP);

    // Right: the recipe as a diagram -- frame to SHA256 to words -- in the
    // equation card's shape, then the file the card will receive.
    lv_obj_t *pc = wt_card(s_scr, ENT_COL_X, ENT_CAM_Y, ENT_COL_W, 56);
    lv_obj_t *pq = lv_obj_create(pc);
    lv_obj_remove_style_all(pq);
    lv_obj_set_pos(pq, 0, 0);
    lv_obj_set_size(pq, ENT_COL_W, 56);
    lv_obj_set_flex_flow(pq, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(pq, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(pq, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(pq, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *row = wt_diagram_row(pq);
    wt_chip(row, LV_SYMBOL_IMAGE, false);
    wt_diagram_op(row, LV_SYMBOL_RIGHT);
    wt_chip(row, "SHA256", false);
    wt_diagram_op(row, LV_SYMBOL_RIGHT);
    wt_chip(row, tr(STR_W_24), true);

    // The filename is a C literal, never translated: the owner types it into a
    // shell, so the screen shows exactly what the card will hold. 132 tall,
    // not the chooser's 96: the note wraps to three lines in this narrower
    // column and a 96 row cuts the third.
    wt_row_x(s_scr, WT_ICON_SD, WPROOF_NAME, tr(STR_W_PROOF_FILE_NOTE), NULL,
             NULL, NULL, WT_INK, false, ENT_COL_X, ENT_CAM_Y + 64, ENT_COL_W,
             132, NULL, NULL);

#ifdef SIMULATOR
    s_pf_shot = mk_pill(tr(STR_W_PROOF_SHOT), 48, WT_ACTION_Y, 300,
                        pf_sim_capture_cb, NULL);
    wt_pill_primary(s_pf_shot);
#else
    // Rect BEFORE start, same as the entropy screen: the video and LVGL share
    // one framebuffer from the first frame.
    camera_spike_set_preview_rect(ENT_CAM_X, ENT_CAM_Y, ENT_CAM_W, ENT_CAM_H);
    if (camera_proof_start()) {
        s_pf_shot = mk_pill(tr(STR_W_PROOF_SHOT), 48, WT_ACTION_Y, 300,
                            pf_capture_cb, NULL);
        wt_pill_primary(s_pf_shot);
        if (!s_pf_tmr) s_pf_tmr = lv_timer_create(pf_poll_cb, 80, NULL);
    } else {
        // No camera, no proof: unlike the wizard there is no source to fall
        // back to, so the column carries the error and BACK is the only way.
        mk_lbl(tr(STR_C_CAM_UNAVAIL), ENT_CAM_X + 14, ENT_CAM_Y + 100,
               wt_font23(), STOP_COL);
        mk_lbl(camera_spike_status(), ENT_CAM_X + 14, ENT_CAM_Y + 134,
               wt_font14(), MUT_COL);
    }
#endif
    s_pf_backp = mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140,
                         pf_back_cb, NULL);
}

static void pf_words_cb(lv_event_t *e)
{
    (void)e;
    s_pf_page = 0;
    proof_words_screen();
}

static void proof_result_screen(void)
{
    mk_screen(tr(STR_W_PROOF_R_T), tr(STR_W_PROOF_R_S));

    // The hash, framed, with the filename it belongs to in the card's corner.
    // Lowercase, like the dice fingerprint and like shasum's own output, so
    // the owner compares character by character with no case translation.
    // Hand-built rather than wt_value_card because the 79 grouped characters
    // must wrap, and the value card pins its value to one line.
    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", s_pf_hash[i]);
    char grp[96];
    wt_group4(hex, grp, sizeof grp);

    lv_obj_t *card = wt_card(s_scr, 48, 96, 704, 0);
    lv_obj_t *cap = wt_lbl(card, tr(STR_W_PROOF_HASH_CAP), 14, 12, wt_font14(),
                           MUT_COL);
    lv_obj_set_style_text_letter_space(cap, 1, 0);
    lv_obj_t *fn = wt_lbl(card, WPROOF_NAME, 0, 12, wt_font_mono14(), OK_COL);
    lv_obj_update_layout(fn);
    lv_obj_set_pos(fn, 704 - 14 - lv_obj_get_width(fn), 12);
    lv_obj_set_width(cap, 704 - 28 - lv_obj_get_width(fn) - 12);
    lv_label_set_long_mode(cap, LV_LABEL_LONG_WRAP);
    lv_obj_update_layout(cap);
    int vy = 12 + lv_obj_get_height(cap) + 8;
    lv_obj_t *v = wt_lbl(card, grp, 14, vy, wt_font_mono23(), INK_COL);
    lv_obj_set_width(v, 676);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_update_layout(v);
    lv_obj_set_size(card, 704, vy + lv_obj_get_height(v) + 14);

    // No QR here. It carried the hash to the hosted page, but the check needs
    // the FILE, and the file is 1.9MB on the card -- so the machine that reads
    // the card is the machine that checks, and a phone scanning a code could
    // never finish. The card's own copy of the page carries the claim instead.
    //
    // Accent on how the check works, WARN on where the words go wrong: the
    // proven pair geometry, same call shape as the dice verdict screen.
    const lv_font_t *f = wt_body_font2(tr(STR_W_PROOF_CHECK_B),
                                       tr(STR_W_PROOF_BURN_B), 330, 112);
    wt_why_block(s_scr, tr(STR_W_PROOF_CHECK_H), tr(STR_W_PROOF_CHECK_B),
                 48, 232, 344, WT_CONTENT_BOTTOM - 232, f, wt_accent());
    wt_why_block(s_scr, tr(STR_W_PROOF_BURN_H), tr(STR_W_PROOF_BURN_B),
                 408, 232, 344, WT_CONTENT_BOTTOM - 232, f, WT_WARN);

    lv_obj_t *sw = mk_pill(tr(STR_W_PROOF_WORDS_BTN), 48, WT_ACTION_Y, 300,
                           pf_words_cb, NULL);
    wt_pill_primary(sw);
    mk_pill(tr(STR_C_DONE), WT_BACK_X, WT_ACTION_Y, 140, pf_done_cb, NULL);
}

static void pf_words_page_cb(lv_event_t *e)
{
    s_pf_page += (int)(intptr_t)lv_event_get_user_data(e);
    proof_words_screen();
}

static void pf_words_back_cb(lv_event_t *e) { (void)e; proof_result_screen(); }

// The words screen's grid, reading the proof's own buffers. Always 24 words
// over two pages, and the loud line is the opposite claim: these are NOT for
// paper, they are on the card in the open.
static void proof_words_screen(void)
{
    if (s_pf_page < 0) s_pf_page = 0;
    if (s_pf_page > 1) s_pf_page = 1;
    mk_screen(tr(STR_W_24), NULL);

    const int first = s_pf_page * WORDS_PER_PAGE;
    const int rows = 6;
    for (int k = 0; k < WORDS_PER_PAGE; k++) {
        char buf[32];
        snprintf(buf, sizeof buf, "%2d. %.11s", first + k + 1,
                 s_pf_w[first + k]);
        mk_lbl(buf, 48 + (k / rows) * 352, 104 + (k % rows) * 40, wt_font28(),
               INK_COL);
    }
    lv_obj_t *po = mk_lbl(tr(STR_W_PROOF_BURNED), 48, 352,
                          wt_body_font(tr(STR_W_PROOF_BURNED), 700, 40),
                          STOP_COL);
    lv_obj_set_width(po, 700);
    lv_label_set_long_mode(po, LV_LABEL_LONG_WRAP);

    if (s_pf_page == 0) {
        mk_pill(tr(STR_C_BACK), 48, WT_ACTION_Y, 160, pf_words_back_cb, NULL);
        mk_pill(tr(STR_R_NEXT), 430, WT_ACTION_Y, 320, pf_words_page_cb,
                (void *)(intptr_t)1);
    } else {
        mk_pill(tr(STR_C_BACK), 48, WT_ACTION_Y, 160, pf_words_page_cb,
                (void *)(intptr_t)-1);
        mk_pill(tr(STR_C_DONE), 430, WT_ACTION_Y, 320, pf_done_cb, NULL);
    }
    // After the pills, never before: the first pill summons the opaque action
    // bar (action_bar_ensure), and anything drawn into the band before that
    // moment is painted under it.
    char cnt[40];
    snprintf(cnt, sizeof cnt, "%d-%d / 24", first + 1, first + WORDS_PER_PAGE);
    mk_lbl(cnt, 232, 416, wt_font23(), MUT_COL);
}

// ---- dice screen ----
// The card sits on the y=96 content line and runs to 394, four clear of
// WT_CONTENT_BOTTOM: the histogram needs the height, and the only direction
// with any was up.
#define DICE_CARD_X   100
#define DICE_CARD_Y    96
#define DICE_CARD_W   600
#define DICE_CARD_H   298
#define DICE_KEY_W     84
#define DICE_KEY_H     60
#define DICE_KEY_GAP   10
#define DICE_KEY_Y     20   // keys 20..80, card relative
#define DICE_BAR_TOP   92   // tracks 92..136
#define DICE_BAR_H     44   // full height = TWICE the fair share, so the fair
#define DICE_BAR_W     40   //   share tick always sits at exactly half height
#define DICE_BAR_BASE (DICE_BAR_TOP + DICE_BAR_H)
#define DICE_TICK_Y   (DICE_BAR_TOP + DICE_BAR_H / 2)
#define DICE_CNT_Y    140   // per face counts under the columns
#define DICE_TALLY_Y  168
#define DICE_NOTE_Y   210
#define DICE_FP_Y     250

static lv_obj_t *s_dice_card;
static lv_obj_t *s_dice_tally;
static lv_obj_t *s_dice_done;
static lv_obj_t *s_dice_fp;        // live SHA256 fingerprint, for the owner to check
static bool     s_dice_fp_full;    // tap the fingerprint to reveal all 64 hex
static lv_obj_t *s_dice_fill[6];   // histogram fills, grown up from the base
static lv_obj_t *s_dice_cnt[6];    // exact count under each column
static lv_obj_t *s_dice_chip;      // the one status coloured element on the screen
static int      s_dice_last_verdict;

// The floor is not derived here any more: wallet_dice_judge computes it from
// the byte need, so the screen reads it off the verdict struct and cannot
// disagree with the module about where DONE unlocks.
static unsigned dice_need(void) { return s_count == 24 ? 32 : 16; }

// The "?" beside the verdict chip. Same canonical explainer as every other "?"
// on the device; unlike the entropy screen's there is no camera to stop and
// restart, so it is a plain overlay with no teardown. First glyph is the LIST
// mark method_screen already puts on the DICE row, so the marks agree.
// A fourth row, and it is the one a newcomer actually needed: the card used to
// explain the CHECKER and never the point. Dice exist on a signer so the owner
// does not have to take this device's word for its own randomness, and nothing
// on the screen said so -- the SHA256 sat under the tally with no reason
// attached. That line cannot live on the note itself: DICE_NOTE_Y 210 to
// DICE_FP_Y 250 is 40px, two font14 lines, and a longer locale would land on
// the fingerprint. The card has the room, so the reason goes here.
static const char *const DICE_HELP_ICONS[] = {
    LV_SYMBOL_LIST,
    LV_SYMBOL_LOOP,
    LV_SYMBOL_WARNING,
    LV_SYMBOL_OK,
};

// The arrow the dice screen never drew. YOURS TO CHECK tells the owner to
// recompute the SHA256 offline, and stops there -- so a reader who does it is
// left holding a number with nothing said about what it is FOR. It is the
// words: feed that hash to any BIP39 tool and the same list comes back. Two
// chips say so in the space a sentence would need, and unlike a sentence they
// are the same two marks the reader already met on the screen behind the card.
//
// Deliberately costs no translation. SHA256 is a literal everywhere, and the
// word count reuses the strings the count screen already ships in 21 locales,
// so this row can never be the thing that fails a fit check.
static int aside_dice_flow(lv_obj_t *p, int x, int y, int w)
{
    lv_obj_t *col = aside_col(p, x, y, w);

    char buf[WT_ICON_TEXT_MAX];
    lv_obj_t *row = wt_diagram_row(col);
    snprintf(buf, sizeof buf, "%s SHA256", LV_SYMBOL_SHUFFLE);
    wt_chip(row, buf, false);
    wt_diagram_op(row, LV_SYMBOL_RIGHT);
    // s_count is pinned to 12 on the way in (method_dice_cb), but dice_need
    // already refuses to assume that, and a chip that disagrees with the words
    // the owner is about to be shown is worse than the branch costs.
    snprintf(buf, sizeof buf, "%s %s", LV_SYMBOL_LIST,
             tr(s_count == 24 ? STR_W_24 : STR_W_12));
    wt_chip(row, buf, true);

    lv_obj_update_layout(col);
    return lv_obj_get_height(col);
}

static void dice_help_cb(lv_event_t *e)
{
    (void)e;
    wt_explain_t x = {
        .title  = tr(STR_W_DICE_HELP_T),
        .icon   = LV_SYMBOL_LIST,
        .body   = tr(STR_W_DICE_HELP_B),
        .ok_txt = tr(STR_C_OK),
        .mode   = WT_GRID_ICONS,
        .icons  = DICE_HELP_ICONS,
        .aside  = aside_dice_flow,
    };
    wt_explain_open(s_scr, &x);
}

// The live histogram: six columns welded positionally to the six keys that
// feed them, with a hairline at the level a fair die homes in on. Krux ships
// the same information as a bar graph behind a "stats for nerds" menu plus a
// bit count; here the distribution IS the screen, and no bit count appears
// anywhere in the product, because the plug-in estimate reads ~3.6 bits under
// the promised 128 at 50 rolls and a number below the promise invites exactly
// the panic it was meant to prevent. The bits stay in wallet_dice_q.c and its
// tests, where they gate instead of alarm.
//
// On the tap screen's rule that a continuous bar would claim a measurement of
// quality: each column here is a COUNT, printed in figures directly under it.
// It claims nothing the number does not already state.
static void dice_bars_make(lv_obj_t *par, int x0, int pitch, int y)
{
    for (int i = 0; i < 6; i++) {
        lv_obj_t *tr = lv_obj_create(par);
        lv_obj_remove_style_all(tr);
        lv_obj_set_pos(tr, x0 + i * pitch, y);
        lv_obj_set_size(tr, DICE_BAR_W, DICE_BAR_H);
        lv_obj_set_style_bg_color(tr, WT_DIV, 0);
        lv_obj_set_style_bg_opa(tr, LV_OPA_COVER, 0);
        lv_obj_remove_flag(tr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(tr, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *f = lv_obj_create(tr);
        lv_obj_remove_style_all(f);
        lv_obj_set_pos(f, 0, DICE_BAR_H);
        lv_obj_set_size(f, DICE_BAR_W, 0);
        lv_obj_set_style_bg_color(f, wt_accent(), 0);
        lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
        s_dice_fill[i] = f;
    }
    // The fair share line, drawn LAST so it crosses over the fills: a level
    // skyline against it says "a die did this" with no words in any locale.
    lv_obj_t *tick = lv_obj_create(par);
    lv_obj_remove_style_all(tick);
    lv_obj_set_pos(tick, x0 - DICE_KEY_GAP - 12, y + DICE_BAR_H / 2);
    lv_obj_set_size(tick, 5 * pitch + DICE_BAR_W + 2 * (DICE_KEY_GAP + 12), 1);
    lv_obj_set_style_bg_color(tick, WT_DIV, 0);
    lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
}

static void dice_bars_set(const wallet_dice_q_t *q)
{
    // Full height is twice the fair share: count = n/6 lands on the tick at
    // half, count = n/3 tops out. Nothing references the floor, so the scale
    // is honest at roll 7 and at roll 180 alike.
    for (int i = 0; i < 6; i++) {
        if (!s_dice_fill[i]) continue;
        int h = 0;
        if (q->n) {
            h = (int)(q->face[i] * 3 * DICE_BAR_H / q->n);
            if (h > DICE_BAR_H) h = DICE_BAR_H;
        }
        lv_obj_set_pos(s_dice_fill[i], 0, DICE_BAR_H - h);
        lv_obj_set_size(s_dice_fill[i], DICE_BAR_W, h);
        if (s_dice_cnt[i]) {
            char b[8];
            snprintf(b, sizeof b, "%u", q->face[i]);
            lv_label_set_text(s_dice_cnt[i], b);
        }
    }
}

static void dice_refresh(void)
{
    unsigned n = wallet_dice_count();
    wallet_dice_q_t q;
    wallet_dice_judge(wallet_dice_digits(), n, dice_need(), &q);
    dice_bars_set(&q);

    if (s_dice_tally) {
        // Literal, not a translatable format. Past the floor the denominator
        // goes: there is no target left, and "60 / 50" reads as a fault.
        char buf[16];
        if (n <= q.floor) snprintf(buf, sizeof buf, "%u / %u", n, q.floor);
        else              snprintf(buf, sizeof buf, "%u", n);
        lv_label_set_text(s_dice_tally, buf);
    }
    if (s_dice_fp) {
        // first 8 bytes by default (enough to spot a mismatch), all 32 on tap.
        char fp[65] = "";
        uint8_t e[32];
        if (n > 0 && wallet_dice_peek(e) == 0) {
            int bytes = s_dice_fp_full ? 32 : 8;
            for (int i = 0; i < bytes; i++) snprintf(fp + i * 2, 3, "%02x", e[i]);
        }
        lv_label_set_text(s_dice_fp, fp);
        memset(e, 0, sizeof e);
    }
    if (s_dice_done) {
        // Disabled, not hidden: a control that pops into existence at roll 50
        // reads as a rendering fault, where a greyed one says "not yet".
        bool ready = n >= q.floor;
        lv_obj_set_style_opa(s_dice_done, ready ? LV_OPA_COVER : LV_OPA_40, 0);
        if (ready) lv_obj_add_flag(s_dice_done, LV_OBJ_FLAG_CLICKABLE);
        else       lv_obj_remove_flag(s_dice_done, LV_OBJ_FLAG_CLICKABLE);
    }
    if (s_dice_chip && q.verdict != s_dice_last_verdict) {
        // Guarded because wt_state_chip_set re-measures; the bars are not,
        // because the 132/n scale genuinely moves them on every press. Hidden
        // below the floor rather than disabled: a verdict that does not exist
        // yet is not a control, and "23 / 50" already says keep rolling.
        s_dice_last_verdict = q.verdict;
        if (q.verdict == WD_Q_SHORT) {
            lv_obj_add_flag(s_dice_chip, LV_OBJ_FLAG_HIDDEN);
        } else {
            char b[64];
            if (q.verdict == WD_Q_OK)
                snprintf(b, sizeof b, "%s", LV_SYMBOL_OK);
            else
                snprintf(b, sizeof b, "%s %s", LV_SYMBOL_WARNING,
                         tr(q.verdict == WD_Q_UNEVEN ? STR_W_DICE_UNEVEN
                                                     : STR_W_DICE_PATTERN));
            wt_state_chip_set(s_dice_chip, b,
                              q.verdict == WD_Q_OK ? OK_COL : WARN_COL);
            lv_obj_align(s_dice_chip, LV_ALIGN_TOP_RIGHT, -(DICE_CARD_W - 540),
                         DICE_TALLY_Y + 2);
            lv_obj_remove_flag(s_dice_chip, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void dice_fp_cb(lv_event_t *e) { (void)e; s_dice_fp_full = !s_dice_fp_full; dice_refresh(); }

static void dice_key_cb(lv_event_t *e)
{
    int face = (int)(intptr_t)lv_event_get_user_data(e);
    wallet_dice_roll(face);
    dice_refresh();
}

static void dice_undo_cb(lv_event_t *e) { (void)e; wallet_dice_undo(); dice_refresh(); }

// Build the seed from the banked rolls. Same wipe discipline as tap_done_cb:
// entropy is seed material. The rolls stay in the module until take() consumes
// them, so this can be reached straight from the "same-y rolls" nudge without
// losing them.
static void dice_commit(void)
{
    unsigned need = s_count == 24 ? 32 : 16;
    uint8_t entropy[32];
    if (wallet_dice_take(entropy, need) == 0) {
        // Judge the digits one last time, BEFORE reset() drops them. This is
        // the only moment the raw rolls and the decision to keep them exist
        // together, and after the hash nothing can ever tell.
        // A flagged run can no longer reach here: DONE refuses it and ROLL MORE
        // is the way through. So the note this path writes is always "nothing
        // to say", and writing it is still what CLEARS a previous device's
        // verdict when a seed is rebuilt.
        wallet_setup_entropy_note(0);
        wallet_dice_reset();
        wallet_setup_entropy(entropy, need);
    } else {
        // Cannot happen once the floor is met, but never leave a dead button.
        mk_screen(tr(STR_W_ENT_FAIL_T), NULL);
        mk_body(tr(STR_W_ENT_FAIL_B), 48, 118, 704, 260, INK_COL);
        mk_pill(tr(STR_C_TRY_AGAIN), WT_BACK_X, WT_ACTION_Y, 160, ent_retry_cb, NULL);
    }
    wz_bzero(entropy, sizeof entropy);
}

static void dice_screen_build(void);
// ROLL MORE: back to the keypad WITH the rolls banked. This pill is the whole
// point of the warning — the old samey screen's only way back went through
// dice_screen(), which reset the module and silently threw away fifty rolls.
static void dice_keep_cb(lv_event_t *e) { (void)e; dice_screen_build(); }

// The verdict screen. Replaces the samey nudge, which was a bare title,
// subtitle and two pills — the exact shape the BARE gate exists to refuse, and
// it survived only because no walk ever rendered it. The evidence leads: the
// same six columns from the keypad, so the owner is shown the shape being
// questioned rather than told about it.
static void dice_warn_screen(int verdict)
{
    mk_screen(tr(STR_W_DICE_WARN_T),
              tr(verdict == WD_Q_UNEVEN ? STR_W_DICE_UNEVEN_S
                                        : STR_W_DICE_PATTERN_S));
    lv_obj_t *card = wt_card(s_scr, 48, 104, 704, 92);
    for (int i = 0; i < 6; i++) s_dice_cnt[i] = NULL;
    dice_bars_make(card, 58, 117, 24);
    wallet_dice_q_t q;
    wallet_dice_judge(wallet_dice_digits(), wallet_dice_count(), dice_need(), &q);
    dice_bars_set(&q);

    const lv_font_t *f = wt_body_font2(tr(STR_W_DICE_W1_B), tr(STR_W_DICE_W2_B),
                                       330, 112);
    wt_why_block(s_scr, tr(STR_W_DICE_W1_H), tr(STR_W_DICE_W1_B),
                 48, 232, 344, WT_CONTENT_BOTTOM - 232, f, WT_WARN);
    wt_why_block(s_scr, tr(STR_W_DICE_W2_H), tr(STR_W_DICE_W2_B),
                 408, 232, 344, WT_CONTENT_BOTTOM - 232, f, wt_accent());

    // No USE ANYWAY. ROLL MORE keeps the right hand slot it already had, so the
    // muscle memory survives the pill count dropping to two, and it is primary
    // because it is the way through: the rolls are all still banked, which is
    // the whole reason DICE_MAX is 180.
    lv_obj_t *p[2];
    p[0] = wt_pillh(s_scr, tr(STR_W_START_OVER), 48, WT_ACTION_Y_TALL, 330, 66,
                    method_dice_cb, NULL);
    p[1] = wt_pillh(s_scr, tr(STR_W_DICE_MORE), 422, WT_ACTION_Y_TALL, 330, 66,
                    dice_keep_cb, NULL);
    wt_pill_row(p, 2);
    wt_pill_primary(p[1]);
}

static void dice_done_cb(lv_event_t *e)
{
    (void)e;
    // A run this judge does not believe was rolled does not become a seed.
    //
    // This used to warn and then honour a USE ANYWAY, on the argument that dice
    // entropy is the owner's and a device overriding it takes back the trust
    // root the path exists to hand over. The owner still owns it — ROLL MORE is
    // how they exercise that, and it keeps every roll already banked, so a
    // refusal is never a dead end for anyone actually rolling a die. What the
    // old shape really offered was one tap between a shape nobody rolled and a
    // wallet, at the one moment in the ritual an owner is least inclined to
    // read.
    //
    // The line stays at WD_RATE = 2050 and must not move to the promised 128
    // bits: the plug-in estimator reads ~3.6 bits low at fifty rolls, so a
    // 126 bit gate refuses about half of honest sessions. See wallet_dice_q.c.
    wallet_dice_q_t q;
    wallet_dice_judge(wallet_dice_digits(), wallet_dice_count(), dice_need(), &q);
    if (wallet_dice_blocked(q.verdict)) {
        dice_warn_screen(q.verdict);
        return;
    }
    dice_commit();
}

static void dice_cancel_cb(lv_event_t *e) { wallet_dice_reset(); cancel_cb(e); }

static void dice_screen_build(void)
{
    s_dice_tally = NULL; s_dice_done = NULL; s_dice_fp = NULL; s_dice_fp_full = false;
    s_dice_chip = NULL; s_dice_last_verdict = -1;
    for (int i = 0; i < 6; i++) { s_dice_fill[i] = NULL; s_dice_cnt[i] = NULL; }
    mk_screen(tr(STR_W_DICE_T), tr(STR_W_DICE_S));

    s_dice_card = wt_card(s_scr, DICE_CARD_X, DICE_CARD_Y, DICE_CARD_W, DICE_CARD_H);

    // six d6 keys, 1..6, in a row, each directly over the column it feeds
    for (int i = 0; i < 6; i++) {
        lv_obj_t *k = lv_button_create(s_dice_card);
        lv_obj_set_pos(k, 18 + i * (DICE_KEY_W + DICE_KEY_GAP), DICE_KEY_Y);
        lv_obj_set_size(k, DICE_KEY_W, DICE_KEY_H);
        lv_obj_add_event_cb(k, dice_key_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(i + 1));
        lv_obj_t *lbl = lv_label_create(k);
        char d[2] = { (char)('1' + i), 0 };
        lv_label_set_text(lbl, d);
        lv_obj_center(lbl);
    }

    // the live histogram under the keys, one exact count under each column
    dice_bars_make(s_dice_card, 18 + (DICE_KEY_W - DICE_BAR_W) / 2,
                   DICE_KEY_W + DICE_KEY_GAP, DICE_BAR_TOP);
    for (int i = 0; i < 6; i++) {
        s_dice_cnt[i] = wt_lbl(s_dice_card, "0", 18 + i * (DICE_KEY_W + DICE_KEY_GAP),
                               DICE_CNT_Y, wt_font14(), MUT_COL);
        lv_obj_set_width(s_dice_cnt[i], DICE_KEY_W);
        lv_obj_set_style_text_align(s_dice_cnt[i], LV_TEXT_ALIGN_CENTER, 0);
    }

    s_dice_tally = wt_lbl(s_dice_card, "", 18, DICE_TALLY_Y, wt_font_mono28(), INK_COL);

    // the verdict chip (hidden until the floor) and the "?" that explains it
    s_dice_chip = wt_state_chip(s_dice_card, "", WARN_COL);
    lv_obj_add_flag(s_dice_chip, LV_OBJ_FLAG_HIDDEN);
    wt_help_chip(s_dice_card, DICE_CARD_W - 44, DICE_TALLY_Y - 2, MUT_COL,
                 dice_help_cb, NULL);

    lv_obj_t *note = wt_lbl(s_dice_card, tr(STR_W_DICE_VERIFY_NOTE), 18, DICE_NOTE_Y,
                            wt_font14(), MUT_COL);
    lv_obj_set_width(note, DICE_CARD_W - 36);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);

    // live SHA256 fingerprint: first 8 bytes, tap to reveal all 64 hex. The
    // value the owner can reproduce on any offline machine to check the device.
    s_dice_fp = wt_lbl(s_dice_card, "", 18, DICE_FP_Y, wt_font14(), INK_COL);
    lv_obj_set_width(s_dice_fp, DICE_CARD_W - 36);
    lv_label_set_long_mode(s_dice_fp, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(s_dice_fp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_dice_fp, dice_fp_cb, LV_EVENT_CLICKED, NULL);

    // action row (WT_ACTION_Y): UNDO left, DONE middle (disabled until the
    // floor is met), CANCEL right. Same row the rest of the wizard uses.
    // UNDO is 200, not the 140 it wore unmeasured: DESHACER, DESFAZER and
    // HOÀN TÁC all fell to font14 at 140, and the row has the slack.
    mk_pill(tr(STR_W_DICE_UNDO), 48, WT_ACTION_Y, 200, dice_undo_cb, NULL);
    s_dice_done = mk_pill(tr(STR_C_DONE), 330, WT_ACTION_Y, 200, dice_done_cb, NULL);
    mk_pill(tr(STR_C_CANCEL), WT_BACK_X, WT_ACTION_Y, 160, dice_cancel_cb, NULL);
    dice_refresh();
}

static void dice_screen(void)
{
    wallet_dice_reset();
    dice_screen_build();
}

static void entropy_screen(void)
{
    mk_screen2(tr(STR_W_RAND_T), tr(STR_W_RAND_S));
    s_ent_bar1 = s_ent_bar2 = s_ent_state = s_ent_dot = s_ent_capture = NULL;
    s_ent_c1 = s_ent_c2 = s_ent_c3 = s_ent_cr = NULL;
    // The widgets above are gone, so what ent_ui_sync last painted is gone with
    // them. Forgetting this is how a "nothing changed" guard turns into a meter
    // that never fills on the second visit: the screen is rebuilt on retry and
    // on every capture, and a surviving cache would match the new empty bar and
    // decline to draw anything.
    s_ent_last_pct = s_ent_last_key = s_ent_last_ready = -1;

    // Left: the frame the preview lands in. The same viewfinder the scan screen
    // uses, so the two camera screens are one object to the eye: a filled panel
    // rather than an empty outline, with the bracket corners outside the rect
    // where the video cannot cover them.
    wt_viewfinder(s_scr, ENT_CAM_X, ENT_CAM_Y, ENT_CAM_W, ENT_CAM_H);

    // Readiness, under the preview: a 9px dot and one line. This replaces the
    // on-video bar as the cue a holder waits on.
    s_ent_dot = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_ent_dot);
    lv_obj_set_pos(s_ent_dot, ENT_CAM_X, ENT_CAM_Y + ENT_CAM_H + 14);
    lv_obj_set_size(s_ent_dot, 9, 9);
    lv_obj_set_style_radius(s_ent_dot, 100, 0);
    lv_obj_set_style_bg_color(s_ent_dot, WT_EDGE, 0);
    lv_obj_set_style_bg_opa(s_ent_dot, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_ent_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_ent_dot, LV_OBJ_FLAG_SCROLLABLE);
    s_ent_state = wt_lbl(s_scr, tr(STR_W_ENT_LOW), ENT_CAM_X + 18,
                         ENT_CAM_Y + ENT_CAM_H + 8, wt_font14(), MUT_COL);
    lv_obj_set_width(s_ent_state, ENT_CAM_W - 18);
    lv_label_set_long_mode(s_ent_state, LV_LABEL_LONG_WRAP);

    // Right: the two sources, then the equation.
    lv_obj_t *card1 = NULL;
    s_ent_bar1 = ent_card(ENT_CAM_Y, STR_W_ENT_SRC1_CAP, STR_W_ENT_SRC1_NOTE,
                          false, &card1);
    s_ent_bar2 = ent_card(ENT_CAM_Y + ENT_CARD_H + 8, STR_W_ENT_SRC2_CAP,
                          STR_W_ENT_SRC2_NOTE, true, NULL);

    // 1 + 2 + 3 -> 12 WORDS, in a CARD, with the "?" in that card's own top
    // right corner. Both halves of that matter. The chips used to float on the
    // page and the "?" sat further right again at (752, 66) with the word WHY
    // beside it, which is the arrangement wallet_scan.c already learned not to
    // ship: a help affordance in the gutter belongs to nothing, so a reader has
    // to guess what it explains. Boxed with the diagram, it is that diagram's
    // footnote and nothing else, and it is the shape the PSBT and glossary "?"
    // marks already wear.
    //
    // 336..392, under the two 96-tall source cards (which end at 328) and clear
    // of WT_CONTENT_BOTTOM at 398.
    lv_obj_t *eqc = wt_card(s_scr, ENT_COL_X, ENT_CAM_Y + 2 * ENT_CARD_H + 16,
                            ENT_COL_W, 56);
    wt_help_chip(eqc, ENT_COL_W - 42, 13, MUT_COL, ent_mix_help_cb, NULL);

    // A flex column parent, because wt_diagram_row takes its y from the layout.
    // Width stops short of the chip so a long translated result chip cannot
    // centre itself underneath it.
    lv_obj_t *eq = lv_obj_create(eqc);
    lv_obj_remove_style_all(eq);
    lv_obj_set_pos(eq, 0, 0);
    lv_obj_set_size(eq, ENT_COL_W - 46, 56);
    lv_obj_set_flex_flow(eq, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(eq, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(eq, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(eq, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *row = wt_diagram_row(eq);
    // Every chip is built inert and lit by ent_ui_sync, so no state is painted
    // here that the screen has not actually reached.
    s_ent_c1 = wt_chip(row, "1", false);
    lv_obj_t *op1 = wt_diagram_op(row, "+");
    s_ent_c2 = wt_chip(row, "2", false);
    wt_diagram_op(row, "+");
    s_ent_c3 = wt_chip(row, "3", false);
    wt_diagram_op(row, LV_SYMBOL_RIGHT);
    s_ent_cr = wt_chip(row, tr(STR_W_ENT_RESULT), false);

    // The audit used to be reachable only through the "?" overlay, which meant
    // the owner had to already doubt the camera to find out they could test
    // it. It sits in the action row instead, between the action and the way
    // out; the overlay keeps its pill, where the doubt is actually named.
#ifdef SIMULATOR
    (void)card1; (void)op1;   // the sim has no camera-failure branch to strike
    s_ent_capture = mk_pill(tr(STR_W_ENT_CAPTURE), 48, WT_ACTION_Y, 300,
                            sim_entropy_cb, NULL);
    wt_pill_primary(s_ent_capture);
    ent_audit_pill();
    mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, goto_choose_cb, NULL);
    // The sim has no camera and no meter, so the walk would see a permanently
    // disabled action pill. Show the ready state: it is the one the scripted
    // tap exercises, and the frame the docs publish.
    ent_ui_sync(100, ENT_R_OK);
#else
    // Rect BEFORE start: set_preview_rect pins and blanks the framebuffer both
    // the video and LVGL will share, so it has to happen before the first frame
    // arrives rather than after.
    camera_spike_set_preview_rect(ENT_CAM_X, ENT_CAM_Y, ENT_CAM_W, ENT_CAM_H);
    if (camera_entropy_start()) {
        lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);   // any tap = capture try
        lv_obj_add_event_cb(s_scr, ent_tap_cb, LV_EVENT_CLICKED, NULL);
        if (!s_ent_tmr) s_ent_tmr = lv_timer_create(ent_poll_cb, 80, NULL);
        s_ent_capture = mk_pill(tr(STR_W_ENT_CAPTURE), 48, WT_ACTION_Y, 300,
                                ent_tap_cb, NULL);
        wt_pill_primary(s_ent_capture);
        ent_audit_pill();
        ent_ui_sync(0, camera_entropy_reason());
    } else {
        // The camera failed. The preview column carries the error.
        mk_lbl(tr(STR_C_CAM_UNAVAIL), ENT_CAM_X + 14, ENT_CAM_Y + 100,
               wt_font23(), STOP_COL);
        mk_lbl(camera_spike_status(), ENT_CAM_X + 14, ENT_CAM_Y + 134,
               wt_font14(), MUT_COL);

        // And the rest of the screen stops promising a source it will not
        // deliver. This screen used to leave all of it standing: SOURCE 1 still
        // offering "leaves, gravel, a shuffled deck" over a bar that could
        // never fill, the equation still reading 1 + 2 + 3, and -- worst of the
        // three -- the readiness line still telling the owner to point at
        // something busier, directly under the words CAMERA UNAVAILABLE. An
        // owner who had just been shown WHY THREE SOURCES was left to work out
        // on their own that they were down to two.
        //
        // Said with marks rather than a sentence, which is also what keeps it
        // free: striking the card and dropping the chip needs no new string, so
        // no locale gains a glyph over it. The equation reads 2 + 3 -> 12 WORDS,
        // which is the whole message and is already the screen's own idiom.
        if (card1) lv_obj_set_style_opa(card1, LV_OPA_40, 0);
        if (s_ent_c1) { lv_obj_delete(s_ent_c1); s_ent_c1 = NULL; }
        if (op1) lv_obj_delete(op1);
        // Readiness belongs to a meter that will never move. The dot and its
        // line go together: half of a cue is a rendering fault, not a cue.
        if (s_ent_state) { lv_obj_delete(s_ent_state); s_ent_state = NULL; }
        if (s_ent_dot)   { lv_obj_delete(s_ent_dot);   s_ent_dot   = NULL; }

        // A dead camera must not be a dead device: sources 2 and 3 are still
        // there, so the seed loses a source rather than the device losing its
        // only path to a wallet. CAPTURE goes straight to the taps.
        s_ent_capture = mk_pill(tr(STR_W_ENT_CAPTURE), 48, WT_ACTION_Y, 300,
                                tap_only_cb, NULL);
        wt_pill_primary(s_ent_capture);
    }
    mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, ent_back_cb, NULL);
#endif
}

// ---- restore: letter keyboard + wordlist autocomplete ----
static const char *RESTORE_MAP[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    "CANCEL", "",
};

static void restore_refresh(void)
{
    char buf[96];   // ru "слово %d из %d" + typed prefix overflowed 48
    snprintf(buf, sizeof buf, tr(STR_W_WORD_N_FMT), s_nw + 1, entry_target(), s_prefix);
    lv_label_set_text(s_word_lbl, buf);
    const char *sug[3] = {0};
    int n = s_prefix[0] ? wallet_seed_suggest(s_prefix, sug, 3) : 0;
    for (int i = 0; i < 3; i++) {
        lv_obj_t *lbl = lv_obj_get_child(s_sug[i], 0);
        if (i < n) {
            lv_label_set_text(lbl, sug[i]);
            lv_obj_clear_flag(s_sug[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_sug[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void restore_accept_cb(lv_event_t *e)
{
    lv_obj_t *pill = lv_event_get_current_target(e);
    const char *w = lv_label_get_text(lv_obj_get_child(pill, 0));
    snprintf(s_w[s_nw], sizeof s_w[s_nw], "%s", w);
    s_nw++;
    s_prefix[0] = 0;
    if (s_nw >= entry_target()) {
        if (s_cards) { cards_cksum_open(); return; }   // the picker owns the last word
        if (s_verify) verify_finish();     // check the paper, don't stage a seed
        else store_and_finish();
        return;
    }
    restore_refresh();
}

static void restore_kb_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    const char *txt = lv_buttonmatrix_get_button_text(kb, id);
    if (!txt) return;
    if (strcmp(txt, tr(STR_C_CANCEL)) == 0) {
        if (s_verify) { verify_finish_exit(); return; }   // verify: back to the wallet
        wipe_state();
        // Cancelling word entry drops the staged storage mode too, not only the
        // words. wipe_state() clears the word buffer; the mode answer lives in
        // wallet_seed.c and needs its own discard, exactly as cancel_cb does.
        // Without this, RESTORE -> pick a non-default mode -> type -> CANCEL
        // leaves that mode staged for the next setup or login to inherit -- the
        // uncommitted-mode leak the storage layer is built to prevent.
        wallet_seed_discard();
        choose_screen();
        return;
    }
    size_t pl = strlen(s_prefix);
    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        if (pl) s_prefix[pl - 1] = 0;
    } else if (strlen(txt) == 1 && pl + 1 < sizeof s_prefix) {
        s_prefix[pl] = txt[0];
        s_prefix[pl + 1] = 0;
    }
    restore_refresh();
}

static void restore_screen(void)
{
    s_nw = 0;
    s_prefix[0] = 0;
    RESTORE_MAP[30] = tr(STR_C_CANCEL);   // slot 30 = the CANCEL key (localized)
    mk_screen(s_cards  ? tr(STR_W_CARDS_T)
            : s_verify ? tr(STR_W_VERIFY_T) : tr(STR_W_RESTORE_T),
              s_verify ? tr(STR_W_VERIFY_S)
                       : tr(STR_W_RESTORE_S));

    s_word_lbl = mk_lbl("", 48, 108, wt_font28(), INK_COL);

    for (int i = 0; i < 3; i++) {
        s_sug[i] = mk_pill("", 48 + i * 250, 156, 230, restore_accept_cb, NULL);
        wt_pill_primary(s_sug[i]);
        lv_obj_add_flag(s_sug[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *kb = lv_buttonmatrix_create(s_scr);
    lv_buttonmatrix_set_map(kb, RESTORE_MAP);
    lv_obj_set_size(kb, 800, 250);
    lv_obj_set_pos(kb, 0, 224);
    lv_obj_set_style_bg_color(kb, BG_COL, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 6, 0);
    lv_obj_set_style_pad_gap(kb, 6, 0);
    lv_obj_set_style_bg_color(kb, KEY_COL, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, INK_COL, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, wt_font28(), LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 8, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(kb, restore_kb_cb, LV_EVENT_VALUE_CHANGED, NULL);
    restore_refresh();
}

// ---- cards (BLIND DRAW): the owner's words, the device's checksum ----
// The creation mode with no machine randomness in the seed: 11 or 23 words
// drawn blind from the cut up word list, typed on the restore keyboard above,
// then a last word picked from the checksum valid candidates. That word joins s_w
// and the flow rejoins words_screen -> quiz -> store like every other mode.
// See docs/superpowers/specs/2026-08-04-cards-lastword-design.md

static void cards_cancel_cb(lv_event_t *e)
{
    (void)e;
    wipe_state();
    // Same discard the restore keyboard's CANCEL does: the staged storage
    // mode must not outlive the words it was staged for.
    wallet_seed_discard();
    choose_screen();
}

static void cards_start_cb(lv_event_t *e)   { (void)e; restore_screen(); }
static void cards_pick_go_cb(lv_event_t *e) { (void)e; cards_pick_screen(); }

// What the owner is actually being asked to do, in three marks. None of it can
// be read off the equation card above: that the list is public and the same
// 2048 words in every wallet, that the secret is WHICH ones a blind pick lands
// on, and that the last word is arithmetic rather than a choice. The SHUFFLE
// mark earns its place on this line and not on the method row, because here it
// labels one step -- mixing the pieces -- instead of the whole mode.
//
// Index paired with the three lines of STR_W_CARDS_HELP_B: explain_grid counts
// the newlines and reads this array in step, so a locale shipping four lines
// would read past the end. Every locale ships three.
static const char *const CARDS_HELP_ICONS[] = {
    LV_SYMBOL_LIST,
    LV_SYMBOL_SHUFFLE,
    LV_SYMBOL_OK,
};

static void cards_help_cb(lv_event_t *e)
{
    (void)e;
    wt_explain_t x = {
        .title  = tr(STR_W_CARDS_HELP_T),
        .icon   = LV_SYMBOL_LIST,
        .body   = tr(STR_W_CARDS_HELP_B),
        .ok_txt = tr(STR_C_OK),
        .mode   = WT_GRID_ICONS,
        .icons  = CARDS_HELP_ICONS,
    };
    wt_explain_open(s_scr, &x);
}

static void cards_intro_screen(void)
{
    mk_screen(tr(STR_W_CARDS_T), tr(STR_W_CARDS_S));

    // The draw as an equation: 11 + 1 -> 12. Numerals, so the card reads in
    // every locale; the accent sits on the 1 the device contributes. Same
    // 128..212 band as the backup check and passphrase intros.
    lv_obj_t *card = wt_card(s_scr, 48, 128, 704, 84);
    lv_obj_t *col = lv_obj_create(card);
    lv_obj_remove_style_all(col);
    lv_obj_set_pos(col, 0, 0);
    // 46 narrower than the card so a locale with wider numerals cannot centre
    // the equation underneath the "?" in the corner. Same reservation the
    // entropy screen's equation card makes for the same chip.
    lv_obj_set_size(col, 704 - 46, 84);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *row = wt_diagram_row(col);
    char n1[16], n2[16];   // 16: device gcc sizes %d for a full int
    snprintf(n1, sizeof n1, "%d", s_count - 1);
    snprintf(n2, sizeof n2, "%d", s_count);
    wt_chip(row, n1, false);
    wt_diagram_op(row, "+");
    wt_chip(row, "1", true);
    wt_diagram_op(row, LV_SYMBOL_RIGHT);
    wt_chip(row, n2, false);

    // The mark sits in the corner of the thing it answers, which is what the
    // rest of the device does. The equation says 11 + 1 -> 12 without saying
    // where the 11 come from or why the device gets the 1; that is what opens
    // from here.
    wt_help_chip(card, 704 - 44, 12, MUT_COL, cards_help_cb, NULL);

    {
        const char *b1 = tr(STR_W_CARDS_W1_B), *b2 = tr(STR_W_CARDS_W2_B);
        const int BW = 344, BY = 232, BH = WT_CONTENT_BOTTOM - BY;
        const int HEAD_ROOM = 46;
        const lv_font_t *f = wt_body_font2(b1, b2, BW - 14, BH - HEAD_ROOM - 8);
        wt_why_block(s_scr, tr(STR_W_CARDS_W1_H), b1,  48, BY, BW, BH, f, wt_accent());
        wt_why_block(s_scr, tr(STR_W_CARDS_W2_H), b2, 408, BY, BW, BH, f, WARN_COL);
    }

    lv_obj_t *p = mk_pill(tr(STR_W_TYPE_MY_WORDS), 48, WT_ACTION_Y, 300,
                          cards_start_cb, NULL);
    wt_pill_primary(p);
    mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, goto_count_cb, NULL);
}

// ---- the draw, drawn ----
// One bar per typed word, height from where that word sits in the 2048 word
// list. A blind draw is a jagged skyline, a sorted draw climbs, and a deck
// nobody shuffled draws a flat line. Same argument as the dice histogram: show
// the shape being questioned rather than describing it, which is also what
// clears the BARE gate on both screens below.
//
// No text at all, so it is identical in all 21 locales. A photograph of it
// narrows each index to about six bits, which is strictly less than the same
// observer gets three taps later on words_screen, where every word is spelled
// out — so this is not a new exposure class and carries no mitigation.
#define CARDS_BAR_W  14
#define CARDS_BAR_H  64
static void cards_bars_make(lv_obj_t *card, lv_color_t col)
{
    if (s_nw < 2) return;
    int usable = 704 - 36 - CARDS_BAR_W;
    int pitch = usable / (s_nw - 1);
    for (int i = 0; i < s_nw; i++) {
        lv_obj_t *b = lv_obj_create(card);
        lv_obj_remove_style_all(b);
        // +4 so index 0 still draws a visible stub rather than nothing.
        int h = 4 + (int)((uint32_t)s_cidx[i] * (CARDS_BAR_H - 4) / 2047);
        lv_obj_set_pos(b, 18 + i * pitch, 14 + (CARDS_BAR_H - h));
        lv_obj_set_size(b, CARDS_BAR_W, h);
        lv_obj_set_style_bg_color(b, col, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    }
    // The midline, drawn last so it crosses the bars: half the list. A draw
    // that never crosses it came off one end of the deck.
    lv_obj_t *tick = lv_obj_create(card);
    lv_obj_remove_style_all(tick);
    lv_obj_set_pos(tick, 18, 14 + CARDS_BAR_H / 2);
    lv_obj_set_size(tick, usable + CARDS_BAR_W, 1);
    lv_obj_set_style_bg_color(tick, WT_DIV, 0);
    lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
}

// The subtitle and the "why it matters" body both turn on which rule fired.
static int cards_sub_key(void)
{
    switch (s_cq.verdict) {
        case WC_Q_SAME:    return STR_W_CARDS_SAME_S;
        case WC_Q_PERIOD:  return STR_W_CARDS_PERIOD_S;
        case WC_Q_CLUSTER: return STR_W_CARDS_CLUST_S;
        case WC_Q_SORTED:  return STR_W_CARDS_SORTED_S;
        default:           return STR_W_CARDS_DUP_S;
    }
}

static int cards_why_key(void)
{
    switch (s_cq.verdict) {
        case WC_Q_CLUSTER: return STR_W_CARDS_CLUST_B;
        case WC_Q_SORTED:  return STR_W_CARDS_SORTED_B;
        case WC_Q_DUP:     return STR_W_CARDS_DUP_B;
        default:           return STR_W_CARDS_BLOCK_B;   // SAME and PERIOD
    }
}

// USE ANYWAY on a warn: straight to the picker, verdict intact so the chip on
// the checksum card can still say what it said.
static void cards_force_cb(lv_event_t *e) { (void)e; cards_cksum_screen(); }

// A retype is a fresh draw, not an edit, so the old words go before the
// keyboard opens and a half finished retype cannot leave a stale word behind
// s_nw. wipe_state() is the wrong tool here: it clears s_cards and would drop
// the flow out of cards mode entirely.
static void cards_retype_cb(lv_event_t *e)
{
    (void)e;
    memset(s_w, 0, sizeof s_w);
    memset(s_cidx, 0, sizeof s_cidx);
    memset(s_prefix, 0, sizeof s_prefix);
    s_nw = 0;
    s_cq = (wallet_cards_q_t){0};
    restore_screen();
}

// The two verdict screens. Same geometry, one difference that is the whole
// point: the warn has a way past and the block does not.
static void cards_verdict_screen(int title, lv_color_t col, bool blocked)
{
    mk_screen(tr(title), tr(cards_sub_key()));

    lv_obj_t *card = wt_card(s_scr, 48, 104, 704, 92);
    cards_bars_make(card, col);

    const char *b1 = tr(cards_why_key()), *b2 = tr(STR_W_CARDS_FIX_B);
    const int BW = 344, BY = 232, BH = WT_CONTENT_BOTTOM - BY;
    const lv_font_t *f = wt_body_font2(b1, b2, BW - 14, BH - 46 - 8);
    // Reusing the dice pair's headings: already parallel, already translated,
    // and wallet_info.c reuses a dice title off the dice path for the same
    // reason. The rule colour is the verdict's, the fix is always the accent.
    wt_why_block(s_scr, tr(STR_W_DICE_W1_H), b1,  48, BY, BW, BH, f, col);
    wt_why_block(s_scr, tr(STR_W_DICE_W2_H), b2, 408, BY, BW, BH, f, wt_accent());

    // Two pills, 48/422 at 330 wide: margins 48 and 48, gap 44. Symmetric,
    // unlike the three pill row this replaces. The way forward is on the right,
    // farthest from nothing and nearest the thumb.
    lv_obj_t *p[2];
    p[0] = wt_pillh(s_scr, tr(blocked ? STR_C_CANCEL : STR_L_USE_ANYWAY),
                    48, WT_ACTION_Y_TALL, 330, 66,
                    blocked ? cards_cancel_cb : cards_force_cb, NULL);
    p[1] = wt_pillh(s_scr, tr(STR_W_START_OVER), 422, WT_ACTION_Y_TALL, 330, 66,
                    cards_retype_cb, NULL);
    wt_pill_row(p, 2);
    wt_pill_primary(p[1]);
}

static void cards_warn_screen(void)
{
    cards_verdict_screen(STR_W_CARDS_WARN_T, WARN_COL, false);
}

static void cards_block_screen(void)
{
    cards_verdict_screen(STR_W_CARDS_BLOCK_T, STOP_COL, true);
}

// The checksum explainer: why the last word is picked from a list. Two
// equations: a word IS a number, and the numbers have to land right. Symbols
// first, so the blocks below are confirming something already shown.
static void cards_cksum_screen(void)
{
    mk_screen(tr(STR_W_CKSUM_T), tr(STR_W_CKSUM_S));

    lv_obj_t *card = wt_card(s_scr, 48, 104, 704, 100);
    lv_obj_t *col = lv_obj_create(card);
    lv_obj_remove_style_all(col);
    lv_obj_set_pos(col, 0, 0);
    lv_obj_set_size(col, 704, 100);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 8, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    // Both blocks below say "the numbers behind your words", and the diagram is
    // where one gets shown. It moved out to cksum_diagram when the words screen
    // grew a "?" carrying the same lesson: the equation is the lesson, so the
    // two places that teach it draw from one builder or they drift.
    cksum_diagram(col);

    // The verdict, kept where the flow can still see it. This screen is reached
    // clean or through USE ANYWAY, and a warning that vanishes on the next tap
    // is a warning the product forgot on the owner's behalf. Same place and
    // shape as the dice keypad's chip: TOP_RIGHT of the card, clear of the two
    // rows, which are numerals and glyphs and so the same width in every
    // locale. It cannot go in the row at y=210 -- the card bottoms at 204 and
    // the why blocks start at the mandated 232, leaving 28px for a 30px chip.
    //
    // A block never reaches this screen, so only the three warn verdicts have a
    // word here. OK is the bare mark: the green accent is byte identical to
    // WT_OK, so a word beside it would be saying the colour twice.
    {
        char b[64];
        bool clean = s_cq.verdict == WC_Q_OK || s_cq.verdict == WC_Q_SHORT;
        if (clean) {
            snprintf(b, sizeof b, "%s", LV_SYMBOL_OK);
        } else {
            int k = s_cq.verdict == WC_Q_CLUSTER ? STR_W_CARDS_Q_CLUSTER
                  : s_cq.verdict == WC_Q_SORTED  ? STR_W_CARDS_Q_SORTED
                                                 : STR_W_CARDS_Q_DUP;
            snprintf(b, sizeof b, "%s %s", LV_SYMBOL_WARNING, tr(k));
        }
        lv_obj_t *vchip = wt_state_chip(card, b, clean ? OK_COL : WARN_COL);
        lv_obj_align(vchip, LV_ALIGN_TOP_RIGHT, -14, 10);
    }

    // The one concrete number: how many of the 2048 list words fit these.
    char fit[96];
    snprintf(fit, sizeof fit, tr(STR_W_CKSUM_FIT_FMT), s_ncand);
    lv_obj_t *fl = mk_lbl(fit, 48, 210, wt_font14(), wt_accent());
    lv_obj_set_width(fl, 704);
    lv_obj_set_style_text_align(fl, LV_TEXT_ALIGN_CENTER, 0);

    {
        const char *b1 = tr(STR_W_CKSUM_W1_B), *b2 = tr(STR_W_CKSUM_W2_B);
        const int BW = 344, BY = 232, BH = WT_CONTENT_BOTTOM - BY;
        const int HEAD_ROOM = 46;
        const lv_font_t *f = wt_body_font2(b1, b2, BW - 14, BH - HEAD_ROOM - 8);
        wt_why_block(s_scr, tr(STR_W_CKSUM_W1_H), b1,  48, BY, BW, BH, f, wt_accent());
        wt_why_block(s_scr, tr(STR_W_CKSUM_W2_H), b2, 408, BY, BW, BH, f, WARN_COL);
    }

    lv_obj_t *p = mk_pill(tr(STR_W_CKSUM_GO), 48, WT_ACTION_Y, 300,
                          cards_pick_go_cb, NULL);
    wt_pill_primary(p);
    mk_pill(tr(STR_C_CANCEL), WT_BACK_X, WT_ACTION_Y, 140, cards_cancel_cb, NULL);
}

static void cards_cksum_open(void)
{
    char partial[WSEED_MAX_MNEMONIC];
    join_words(partial, sizeof partial);
    s_ncand = wallet_lastword_candidates(partial, s_cand);
    wz_bzero(partial, sizeof partial);
    s_cpage = 0;
    if (s_ncand <= 0) {
        // Unreachable by construction: every typed word came off the suggest
        // pills, so the prefix is wordlist words and the count is 128 or 8.
        // Still never a dead branch on a seed path.
        mk_screen(tr(STR_W_CHECK_T), tr(STR_W_CHECK_S));
        mk_body(tr(STR_W_CHECK_B), 48, 140, 704, 256, STOP_COL);
        mk_pill(tr(STR_W_START_OVER), 48, WT_ACTION_Y, 240, goto_restore_cb, NULL);
        return;
    }

    // Judge the owner's own words HERE, before the checksum word joins them and
    // makes every set look finished. This is the cards analogue of judging the
    // raw dice digits before SHA256 whitens them.
    //
    // This function is the cards path's alone -- restore_accept_cb reaches it
    // under if (s_cards) and nothing else calls it -- which is why the check
    // lives here and not on the keyboard the restore, verify and amnesic load
    // flows all share. A pre-existing wallet whose words happen to repeat one
    // must still restore, and by construction it cannot reach this line.
    {
        unsigned n = 0;
        for (int i = 0; i < s_nw && n < 24; i++) {
            int k = wallet_lastword_index(s_w[i]);
            if (k < 0) { n = 0; break; }   // unreachable: every word came off a pill
            s_cidx[n++] = (uint16_t)k;
        }
        wallet_cards_judge(s_cidx, n, &s_cq);
    }
    if (wallet_cards_blocked(&s_cq)) { cards_block_screen(); return; }
    if (s_cq.verdict != WC_Q_OK && s_cq.verdict != WC_Q_SHORT) {
        cards_warn_screen();
        return;
    }
    cards_cksum_screen();
}

// ---- cards: pick the last word ----
// 4 x 4 pill pages: the 24 word draw fits its 8 candidates on one page, the
// 12 word draw pages its 128 in 8. Every candidate is a real English BIP39
// word, untranslated on purpose, exactly as the reveal grid shows them.
#define CARDS_PER_PAGE 16

static void cards_page_cb(lv_event_t *e)
{
    s_cpage += (int)(intptr_t)lv_event_get_user_data(e);
    cards_pick_screen();
}

static void cards_pick_cb(lv_event_t *e)
{
    int pos = (int)(intptr_t)lv_event_get_user_data(e);
    const char *w = wallet_lastword_word(s_cand[pos]);
    if (!w) return;
    snprintf(s_w[s_count - 1], sizeof s_w[0], "%s", w);
    // The set is checksum valid by construction, so store_and_finish cannot
    // refuse it; the reveal and quiz see a full seed like any other mode.
    s_nw = s_count;
    s_wpage = 0;
    words_screen();
}

static void cards_pick_screen(void)
{
    const int pages = (s_ncand + CARDS_PER_PAGE - 1) / CARDS_PER_PAGE;
    if (s_cpage < 0) s_cpage = 0;
    if (s_cpage >= pages) s_cpage = pages - 1;

    mk_screen(tr(STR_W_CARDS_PICK_T), tr(STR_W_CARDS_PICK_S));

    const int first = s_cpage * CARDS_PER_PAGE;
    int on = s_ncand - first;
    if (on > CARDS_PER_PAGE) on = CARDS_PER_PAGE;
    const int rows = (on + 3) / 4;

    // The grid wears the chooser frame: 14px side margins + 16px gaps fill
    // WT_CHOICE_W exactly (14 + 4*160 + 3*16 + 14 = 716). 56px pills keep the
    // 52px touch floor; 4 rows bottom at 104 + 286 = 390, under the 398 line.
    lv_obj_t *card = wt_card(s_scr, WT_CHOICE_X, 104, WT_CHOICE_W, rows * 66 + 22);
    for (int k = 0; k < on; k++) {
        wt_pillh(card, wallet_lastword_word(s_cand[first + k]),
                 14 + (k % 4) * 176, 16 + (k / 4) * 66, 160, 56,
                 cards_pick_cb, (void *)(intptr_t)(first + k));
    }

    // Same action row contract as the reveal pager: CANCEL only on page one,
    // BACK owns that slot on later pages, NEXT while there is more to see.
    if (s_cpage == 0)
        mk_pill(tr(STR_C_CANCEL), 48, WT_ACTION_Y, 160, cards_cancel_cb, NULL);
    else
        mk_pill(tr(STR_C_BACK), 48, WT_ACTION_Y, 160, cards_page_cb,
                (void *)(intptr_t)-1);
    if (s_cpage < pages - 1)
        mk_pill(tr(STR_R_NEXT), 430, WT_ACTION_Y, 320, cards_page_cb,
                (void *)(intptr_t)1);
    if (pages > 1) {
        char cnt[40];
        snprintf(cnt, sizeof cnt, "%d-%d / %d", first + 1, first + on, s_ncand);
        mk_lbl(cnt, 232, 416, wt_font23(), MUT_COL);
    }
}

// ---- word count ----
static void count_pick_cb(lv_event_t *e)
{
    s_count = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_cards) { cards_intro_screen(); return; }
    // The keypad reads the count back through dice_need(), so the floor under
    // the tally becomes 99 here without this callback saying so.
    if (s_dice)  { dice_screen(); return; }
    if (s_restore) restore_screen();
    else entropy_screen();
}

static void goto_method_cb(lv_event_t *e) { (void)e; method_screen(); }

static void count_screen(void)
{
    // Reached while RESTORING, or from the two creation paths whose cost is
    // paid by the owner rather than the device: cards, where the count decides
    // how many cards get drawn, and dice, where it decides whether the floor
    // under the tally is 50 rolls or 99. Camera never arrives here -- it makes
    // 12 and its screens are built around that.
    mk_screen(s_restore ? tr(STR_W_RESTORE_T) : tr(STR_W_NEW_T), tr(STR_W_HOWMANY));
    // Rows, on the chooser grid the storage and create-or-restore screens use.
    // Three pills each trailing a note in a column 380px away was the last
    // place on this device where a control and its explanation were separate
    // objects that happened to share a y.
    //
    // LIST for a count of words, and WT_ICON_QR for the path that reads them off
    // a code instead. No tick on any of them: the paper decides how many words
    // there are, so the device has no current answer to mark.
    wt_row_x(s_scr, LV_SYMBOL_LIST, tr(STR_W_12), tr(STR_W_12_NOTE), NULL,
             NULL, NULL, WT_INK, false, WT_CHOICE_X, WT_CHOICE_Y(0),
             WT_CHOICE_W, WT_CHOICE_H, count_pick_cb, (void *)(intptr_t)12);
    wt_row_x(s_scr, LV_SYMBOL_LIST, tr(STR_W_24), tr(STR_W_24_NOTE), NULL,
             NULL, NULL, WT_INK, false, WT_CHOICE_X, WT_CHOICE_Y(1),
             WT_CHOICE_W, WT_CHOICE_H, count_pick_cb, (void *)(intptr_t)24);
    // A SeedQR carries its own length, so it sits beside the count rather than
    // after it.
    if (s_restore)
        wt_row_x(s_scr, WT_ICON_QR, tr(STR_W_SCAN_SEED_QR),
                 tr(STR_W_LOAD_SCAN_NOTE), NULL, NULL, NULL, WT_INK, false,
                 WT_CHOICE_X, WT_CHOICE_Y(2), WT_CHOICE_W, WT_CHOICE_H,
                 restore_scan_cb, NULL);
    mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140,
            (s_cards || s_dice) ? goto_method_cb : goto_choose_cb, NULL);
}

// ---- storage mode: the one question that decides what this device holds ----
// STAGE the answer, never apply it. This screen is step one of the wizard, so
// "AMNESIC" here used to erase the wallet the user already had before a
// single new word existed -- and BACK, or a power cut, then left them with
// neither. wallet_seed_commit applies it once the whole ritual is done.
static void storage_pick_cb(lv_event_t *e)
{
    wallet_seed_stage_mode((int)(intptr_t)lv_event_get_user_data(e));
    if (s_restore) {
        count_screen();          // restoring: the paper decides, 12 or 24
        return;
    }
    // CREATING: 12 is the DEFAULT, and it is the right one. 128 bits is not
    // brute-forceable by anything, so the extra 128 buys margin against nothing
    // that can happen, while 24 words doubles the length of the ONE step where
    // a real mistake is likely -- copying them onto paper by hand and reading
    // them back. Camera keeps it outright; cards and dice offer the choice and
    // list 12 first, because on those two paths the owner is the one paying for
    // the extra length and can see what it costs. RESTORE takes whatever length
    // arrives, since seeds made on other signers are not ours to argue with.
    s_count = 12;
    method_screen();
}

static void storage_screen(void)
{
    mk_screen(tr(STR_W_STORE_T), tr(STR_W_STORE_S));

    // Three explicit storage names, always in the same order used by
    // Settings. The note is inside its control instead of hidden behind a
    // help card: this choice decides what an attacker or a border search can
    // recover after power-off.
    // Geometry and OBJECT from WT_CHOICE_* and wt_row_x, matching
    // storage_chooser_screen() in wallet_settings.c row for row. The two screens
    // present the identical choice and must not drift apart again, which is why
    // the numbers live in wallet_theme.h and not in either file -- and now the
    // shape does too, which is the drift that actually happened last time.
    //
    // Nothing is selected here. In Settings one of the three IS the current
    // mode and wears the tick; this is first boot, there is no current mode yet,
    // and a tick on FLASH would be the device answering its own question.
    static const int MODE[3] = {
        WSEED_MODE_KEEP, WSEED_MODE_SD, WSEED_MODE_AMNESIC
    };
    static const char *const ICON[3] = {
        LV_SYMBOL_SAVE, WT_ICON_SD, WT_ICON_SECRET
    };
    const int BTN[3] = { STR_W_KEEP_BTN, STR_W_SD_BTN, STR_W_AMNESIC_BTN };
    // The FLASH note tells the truth about what a chip dump would find, which
    // is the encryption state. Per HANDOFF-04's storage residual: when the
    // chip reports encryption OFF, this note is a caution not a footnote, so
    // it renders in WT_WARN not the default WT_MUT.
    bool enc = wallet_seed_flash_encrypted();
    const int NOTE[3] = {
        enc ? STR_W_FLASH_ENC_NOTE : STR_W_KEEP_NOTE,
        STR_W_SD_NOTE, STR_W_AMNESIC_NOTE
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *row = wt_row_x(s_scr, ICON[i], tr(BTN[i]), tr(NOTE[i]), NULL,
                                 NULL, NULL, WT_INK, false,
                                 WT_CHOICE_X, WT_CHOICE_Y(i), WT_CHOICE_W,
                                 WT_CHOICE_H, storage_pick_cb,
                                 (void *)(intptr_t)MODE[i]);
        if (i == 0 && !enc) wt_row_sub_color(row, WT_WARN);
    }
    mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, goto_choose_cb, NULL);
}

// ---- entry ----
static void new_cb(lv_event_t *e)     { (void)e; s_restore = false; storage_screen(); }
static void restore_cb(lv_event_t *e) { (void)e; s_restore = true;  storage_screen(); }
static void cancel_cb(lv_event_t *e)
{
    (void)e;
    // The storage answer is staged before the mnemonic exists. Cancelling the
    // wizard must drop that answer as well as any staged words, otherwise the
    // next setup/login can inherit a mode the owner never committed.
    wallet_seed_discard();
    // Captured entropy is seed material too: a cancel from the tap screen must
    // not leave the camera's two chains sitting in RAM for the next flow.
    memset(s_cam_chain, 0, sizeof s_cam_chain);
    memset(s_cam_trng, 0, sizeof s_cam_trng);
    s_cam_have = false;
    wallet_tapent_reset();
    close_all();
}

static void setup_lang_picked(void)
{
    choose_screen();                   // mk_screen replaces s_scr (overlay dies with it)
}

static void setup_lang_cb(lv_event_t *e)
{
    (void)e;
    wallet_lang_picker_open(s_scr, setup_lang_picked);
}

// Both buttons on the choice screen talk about the SEED, and nothing on the
// device said what one is. This screen does: the ordered words are a BIP39
// mnemonic, they plus the passphrase are the wallet, and any compatible BIP39
// signer can rebuild it from them. It is reachable before either choice is
// made, because that is when the question is actually being asked.
static void whatseed_back_cb(lv_event_t *e) { (void)e; choose_screen(); }

static void whatseed_cb(lv_event_t *e)
{
    (void)e;
    mk_screen(tr(STR_W_WHATSEED_T), tr(STR_W_WHATSEED_S));
    mk_body(tr(STR_W_WHATSEED_B), 48, 118, 704, 260, INK_COL);
    mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, whatseed_back_cb, NULL);
}

static void choose_screen(void)
{
    mk_screen(tr(STR_W_SETUP_T), tr(STR_W_SETUP_S));
    // The language pill starts at x=560 on the title's row, so the title gets
    // 496. French, Italian and Portuguese titles reached into it at font34.
    wt_title_fit(s_scr, 496);
    // A whole sentence on each pill, not the bare word "seed": nobody arrives
    // knowing what a seed is, and this is the first screen a new owner ever
    // reaches. STR_W_CREATE_NEW and STR_W_RESTORE_FROM_WORDS keep their short
    // labels for the Settings-side pills at 190 and 280 wide that also reuse
    // them; they never lead a new owner in cold, so the shorthand still reads.
    // A ROW per choice. This is the first screen a new owner ever sees, and it
    // was two buttons with two paragraphs floating beside them; which paragraph
    // belonged to which button was left to the reader's eye. A row settles that
    // by construction, and it is the shape the rest of the device now uses for
    // every list of options.
    //
    // The primary fill is gone with the pills. "New wallet first" is said by
    // being the first row, which is how every list on this device says what to
    // reach for first, and it is said better: a filled button beside a hollow
    // one on the very first screen looks like one choice is disabled.
    //
    // PLUS for making one, LOOP for bringing one back. Both are in the baked
    // SYMS set, and this is the one screen where a reader may not have the
    // language yet -- the picker sits in the corner beside them.
    wt_row_x(s_scr, LV_SYMBOL_PLUS, tr(STR_W_CHOOSE_NEW), tr(STR_W_NEW_NOTE),
             NULL, NULL, NULL, WT_INK, false, WT_CHOICE_X, WT_CHOICE_Y(0),
             WT_CHOICE_W, WT_CHOICE_H, new_cb, NULL);
    wt_row_x(s_scr, LV_SYMBOL_LOOP, tr(STR_W_CHOOSE_RESTORE),
             tr(STR_W_RESTORE_NOTE), NULL, NULL, NULL, WT_INK, false,
             WT_CHOICE_X, WT_CHOICE_Y(1), WT_CHOICE_W, WT_CHOICE_H,
             restore_cb, NULL);
    // "WHAT IS A SEED?" was a third pill, then a bare "?" chip parked at
    // (752, 66) in the header lane. Both were wrong in opposite directions. The
    // pill ranked a question equal to the two decisions; the naked chip ranked
    // it as nothing at all, floating in the gutter beside a subtitle it did not
    // belong to, so the first screen a new owner ever sees offered a control
    // with no clue what it opened.
    //
    // A short note card in the same 716 lane, with the "?" in ITS top right
    // corner, is what the rest of the device already does (wallet_scan.c, and
    // the export note in wallet_info.c). Now the mark is attached to the words
    // that say what it answers. It is not a third choice: it is 76 tall where
    // the rows are 96, carries no icon badge and no chevron, and sits below the
    // pair rather than in their rhythm.
    //
    // 306..382, under the second row (which ends at 294) and clear of
    // WT_CONTENT_BOTTOM at 398.
    {
        // One line's worth of height for the note, vertically centred with the
        // chip. wt_note_fit takes the biggest font that fits the box it is
        // given, so a taller box does NOT mean bigger type here -- it means a
        // short sentence stranded at the top of a half empty card.
        lv_obj_t *hc = wt_card(s_scr, WT_CHOICE_X, 306, WT_CHOICE_W, 76);
        wt_note(hc, tr(STR_W_SEED_HELP), 16, 22, WT_CHOICE_W - 32 - 34, 34);
        wt_help_chip(hc, WT_CHOICE_W - 44, 23, MUT_COL, whatseed_cb, NULL);
        // The whole card opens it, not just the 30px mark. Someone who does not
        // know what a seed is will reach for the sentence, not the punctuation.
        lv_obj_add_flag(hc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(hc, whatseed_cb, LV_EVENT_CLICKED, NULL);
    }
    mk_pill(tr(STR_C_CANCEL), 610, WT_ACTION_Y, 140, cancel_cb, NULL);

    // first boot happens BEFORE Settings is reachable: a fresh device must not
    // trap its owner in English, so the language picker lives here too
    {
        int li = i18n_get_lang();
        const char *nat = i18n_lang_info(li)->native;
        const char *par = strstr(nat, " (");
        char sn[24];
        size_t n = par ? (size_t)(par - nat) : strlen(nat);
        if (n >= sizeof sn) n = sizeof sn - 1;
        memcpy(sn, nat, n);
        sn[n] = 0;
        lv_obj_t *lp = wt_pillh(s_scr, sn, 560, 30, 190, 44, setup_lang_cb, NULL);
        if (img_lang_flags[li]) {
            lv_obj_t *name = lv_obj_get_child(lp, 0);
            lv_obj_set_style_text_letter_space(name, 0, 0);
            lv_obj_align(name, LV_ALIGN_CENTER, 14, 0);
            lv_obj_t *fl = lv_image_create(lp);
            lv_image_set_src(fl, img_lang_flags[li]);
            lv_obj_align(fl, LV_ALIGN_LEFT_MID, 16, 0);
            lv_obj_remove_flag(fl, LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

// ---- AMNESIC per-session load: type the words, or scan a seed QR ----
// KISS never writes a seed QR. It reads one the owner already made on a
// SeedSigner / Krux, which is what makes "power on, load, sign, power off"
// bearable. Everything here stages into RAM; nothing can reach flash because
// wallet_seed_commit is a no-op in this mode.
static void load_screen(void);
static void load_screen_fwd(void) { load_screen(); }

static void load_type_cb(lv_event_t *e)
{
    (void)e;
    s_restore = true;
    count_screen();                 // word count -> the usual restore keypad
}

static void load_new_cb(lv_event_t *e)
{
    (void)e;
    s_load = false;                 // a fresh wallet needs the whole ritual
    choose_screen();
}

static void load_back_cb(lv_event_t *e) { (void)e; load_screen(); }

static void qr_bad_screen(void)
{
    mk_screen(tr(STR_W_QRBAD_T), tr(STR_W_QRBAD_S));
    wt_why_body(s_scr, tr(STR_W_QRBAD_B), 140, STOP_COL, true);
    lv_obj_t *p = mk_pill(tr(STR_C_TRY_AGAIN), 48, WT_ACTION_Y, 300,
                          s_qr_from_restore ? goto_count_cb : load_back_cb, NULL);
    wt_pill_primary(p);
}

// The scan screen owns the camera; it hands us the first decoded payload.
static void qr_text_cb(const char *txt, size_t len)
{
    char words[WSEED_MAX_MNEMONIC];
    int rc = wallet_seed_from_qr(txt, len, words, sizeof words);
    if (rc == 0)
        rc = wallet_seed_stage(words);
    wz_bzero(words, sizeof words);           // a scanned mnemonic must not linger
    if (rc != 0) { qr_bad_screen(); return; }
    void (*cb)(void) = s_done;      // straight to the passphrase, same as typing
    s_load = false;
    close_all();
    if (cb) cb();
}

static void qr_cancel_cb(void) { load_screen(); }

// Same scan, reached from the wizard's word-count screen instead. Cancel and a
// bad scan go back there rather than to the amnesic load screen.
static void goto_count_cb(lv_event_t *e) { (void)e; count_screen(); }
static void restore_qr_cancel_cb(void)   { count_screen(); }

static void restore_scan_cb(lv_event_t *e)
{
    (void)e;
    s_qr_from_restore = true;
    wallet_scan_open_raw(s_parent, qr_text_cb, restore_qr_cancel_cb);
}

static void load_scan_cb(lv_event_t *e)
{
    (void)e;
    s_qr_from_restore = false;   // amnesic load: a bad scan goes back to ITS screen
    wallet_scan_open_raw(s_parent, qr_text_cb, qr_cancel_cb);
}

static void load_screen(void)
{
    mk_screen(tr(STR_W_LOAD_T), tr(STR_W_LOAD_S));
    lv_obj_t *p = mk_pill(tr(STR_W_TYPE_MY_WORDS), 48, 150, 340, load_type_cb, NULL);
    wt_pill_primary(p);
    mk_pill(tr(STR_W_SCAN_SEED_QR), 48, 264, 340, load_scan_cb, NULL);
    wt_wraph(s_scr, tr(STR_W_LOAD_TYPE_NOTE), 430, 150, 340, 110);
    wt_wraph(s_scr, tr(STR_W_LOAD_SCAN_NOTE), 430, 266, 340, 130);
    mk_pill(tr(STR_W_CREATE_NEW), 560, WT_ACTION_Y, 190, load_new_cb, NULL);
}

// ---- configured SD wallet: card/file gate before passphrase entry ----
// A missing card is not "no wallet". This screen is intentionally separate
// from the first-boot chooser so the user can retry hot-plugging the card or
// explicitly start recovery, but can never create over the wallet by accident.
static const char *sd_problem_body(int rc)
{
    switch (rc) {
    case WSEED_ERR_SD_CORRUPT:     return tr(STR_W_SD_CORRUPT_B);
    case WSEED_ERR_SD_IO:          return tr(STR_W_SD_IO_B);
    default:                       return tr(STR_W_SD_MISSING_B);
    }
}

static void sd_problem_screen(int rc);

static void sd_retry_cb(lv_event_t *e)
{
    (void)e;
    int rc = wallet_setup_sd_status();
    if (rc != WSEED_OK) {
        sd_problem_screen(rc);       // redraws with the exact current failure
        return;
    }
    void (*cb)(void) = s_done;
    close_all();
    if (cb) cb();                    // ordinary one-passphrase login
}

static void sd_recover_cb(lv_event_t *e)
{
    (void)e;
    close_all();
    wallet_begin_setup();            // full restore + type-twice setup ritual
}

static void sd_problem_back_cb(lv_event_t *e)
{
    (void)e;
    close_all();                     // underlying game remains available
}

static void sd_problem_screen(int rc)
{
    s_sd_problem = rc;
    mk_screen(tr(STR_W_SD_MISSING_T), tr(STR_W_SD_MISSING_S));
    mk_body(sd_problem_body(s_sd_problem), 48, 132, 704, 226,
            s_sd_problem == WSEED_ERR_SD_MISSING ? MUT_COL : WARN_COL);

    lv_obj_t *retry = mk_pill(tr(STR_C_TRY_AGAIN), 48, WT_ACTION_Y, 240,
                              sd_retry_cb, NULL);
    wt_pill_primary(retry);
    lv_obj_t *recover = mk_pill(tr(STR_W_RESTORE_FROM_WORDS), 304, WT_ACTION_Y, 280,
                                sd_recover_cb, NULL);
    lv_obj_t *back = mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140,
                             sd_problem_back_cb, NULL);
    lv_obj_t *row[3] = { retry, recover, back };
    wt_pill_row(row, 3);
}

void wallet_setup_open_load(lv_obj_t *parent, void (*done_cb)(void))
{
    if (s_scr) return;
    wallet_ui_ensure_indev();
    s_parent = parent;
    s_done = done_cb;
    s_restore = true;
    s_verify = false;
    s_load = true;
    wipe_state();
    load_screen();
}

void wallet_setup_open_sd_missing(lv_obj_t *parent, int reason,
                                  void (*done_cb)(void))
{
    if (s_scr) return;
    wallet_ui_ensure_indev();
    s_parent = parent;
    s_done = done_cb;
    s_restore = false;
    s_verify = false;
    s_load = false;
    wipe_state();
    sd_problem_screen(reason);
}

void wallet_setup_open(lv_obj_t *parent, void (*done_cb)(void))
{
    if (s_scr) return;
    wallet_ui_ensure_indev();          // wizard can be the first touch UI ever
    s_parent = parent;
    s_done = done_cb;
    s_restore = false;
    s_verify = false;
    wipe_state();
    choose_screen();
}

void wallet_setup_open_verify(lv_obj_t *parent, void (*done_cb)(void))
{
    if (s_scr) return;
    wallet_ui_ensure_indev();
    s_parent = parent;
    s_done = done_cb;
    s_restore = true;                  // reuse the restore word-entry keypad
    s_verify = true;
    s_verify_ok = false;
    wipe_state();
    char words[WSEED_MAX_MNEMONIC];
    if (wallet_seed_load(words, sizeof words) != 0) {   // no seed: nothing to check
        s_verify = false;
        if (done_cb) done_cb();
        return;
    }
    int n = 1;                                          // fix the entry length to the
    for (char *p = words; *p; p++) if (*p == ' ') n++;  // stored seed's word count
    wz_bzero(words, sizeof words);
    s_count = n;
    verify_intro_screen();
}
