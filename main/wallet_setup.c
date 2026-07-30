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
#include "wallet_scan.h"     // wallet_scan_open_raw: seed-QR import (amnesic load)
#include "wallet_seed.h"
#include "wallet_settings.h"   // wallet_lang_picker_open: first-boot language switch
#include "wallet_theme.h"
#include "wallet_ui.h"

#ifndef SIMULATOR
#include "camera_spike.h"
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
static void words_screen(void);
static void quiz_screen(void);
static void restore_screen(void);
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
    memset(words, 0, sizeof words);
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
        mk_body(tr(STR_W_VBAD_B), 48, 206, 704, 190, MUT_COL);
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
    mk_body(tr(STR_W_VINTRO_B), 48, 122, 704, 274, MUT_COL);
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
    // not. "spot check X of Y" is not gone from the code; it stays available
    // to OSDs and screen readers via STR_W_QUIZ_N_FMT, it is just no longer
    // the only progress cue.
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

static void words_screen(void)
{
    const int pages = (s_count + WORDS_PER_PAGE - 1) / WORDS_PER_PAGE;
    if (s_wpage < 0) s_wpage = 0;
    if (s_wpage >= pages) s_wpage = pages - 1;

    mk_screen(tr(STR_W_WRITE_T), tr(STR_W_WRITE_S));

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

    if (pages > 1) {
        char cnt[40];   // large enough for conservative compiler range analysis
        snprintf(cnt, sizeof cnt, "%d-%d / %d", first + 1, first + on, s_count);
        if (s_wpage > 0)
            mk_pill(tr(STR_C_BACK), 48, WT_ACTION_Y, 160, words_page_cb,
                    (void *)(intptr_t)-1);
        mk_lbl(cnt, 232, 416, wt_font23(), MUT_COL);
    }
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
}

// ---- entropy (NEW path) ----
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
    memset(words, 0, sizeof words);
    s_wpage = 0;
    words_screen();
}

#ifdef SIMULATOR
static void sim_entropy_cb(lv_event_t *e)
{
    (void)e;
    uint8_t ent[32];
    for (int i = 0; i < 32; i++) ent[i] = (uint8_t)(ui_rand() & 0xFF);
    wallet_setup_entropy(ent, 32);
}
#else
// The capture happens on the camera task; an LVGL timer collects the hash.
static lv_timer_t *s_ent_tmr;

static void ent_poll_cb(lv_timer_t *t)
{
    uint8_t h[32];
    if (camera_entropy_result(h)) {
        lv_timer_delete(t);
        s_ent_tmr = NULL;
        camera_entropy_stop();
        wallet_setup_entropy(h, 32);
        memset(h, 0, sizeof h);
    }
}

static void ent_tap_cb(lv_event_t *e) { (void)e; camera_entropy_tap(); }

static void ent_back_cb(lv_event_t *e)
{
    (void)e;
    if (s_ent_tmr) { lv_timer_delete(s_ent_tmr); s_ent_tmr = NULL; }
    camera_entropy_stop();
    choose_screen();
}
#endif

static void ent_mix_back_cb(lv_event_t *e) { (void)e; entropy_screen(); }

// The "?" beside the equation. Stops the preview before replacing the screen:
// the card is a full screen of prose and the camera owns a column of the panel,
// so leaving the stream live would paint video across the paragraph. BACK
// rebuilds the entropy screen, which restarts the camera on a fresh meter, and
// a fresh meter is what the existing per session rule already wants.
static void ent_mix_help_cb(lv_event_t *e)
{
    (void)e;
#ifndef SIMULATOR
    if (s_ent_tmr) { lv_timer_delete(s_ent_tmr); s_ent_tmr = NULL; }
    camera_entropy_stop();
#endif
    mk_screen(tr(STR_W_ENT_MIX_T), NULL);
    mk_body(tr(STR_W_ENT_MIX_B), 48, 118, 704, 260, INK_COL);
    mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, ent_mix_back_cb, NULL);
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

// One source card: caption, bit count right aligned, a bar, and a note. Returns
// the bar so the caller can drive it.
static lv_obj_t *ent_card(int y, int cap, int note, bool full)
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
    return fill;
}

// Drive source one's bar and the readiness line from the camera's meter. Runs
// on the same LVGL timer that polls for the capture result.
static void ent_ui_sync(int pct)
{
    if (s_ent_bar1)
        lv_obj_set_width(s_ent_bar1, (ENT_COL_W - 28) * pct / 100);
    bool ready = pct >= 100;
    if (s_ent_dot)
        lv_obj_set_style_bg_color(s_ent_dot, ready ? OK_COL : WT_EDGE, 0);
    if (s_ent_state) {
        lv_label_set_text(s_ent_state,
                          tr(ready ? STR_W_ENT_READY : STR_W_ENT_NOTREADY));
        lv_obj_set_style_text_color(s_ent_state, ready ? OK_COL : MUT_COL, 0);
    }
    // CAPTURE is the discoverable form of "tap anywhere", which still works.
    // Disabled until source one is full, because a capture below the gate is
    // refused by camera_spike anyway and a button that silently does nothing
    // reads as a missed touch.
    if (s_ent_capture) {
        lv_obj_set_style_opa(s_ent_capture, ready ? LV_OPA_COVER : LV_OPA_40, 0);
        if (ready) lv_obj_add_flag(s_ent_capture, LV_OBJ_FLAG_CLICKABLE);
        else       lv_obj_remove_flag(s_ent_capture, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void entropy_screen(void)
{
    mk_screen2(tr(STR_W_RAND_T), tr(STR_W_RAND_S));
    s_ent_bar1 = s_ent_bar2 = s_ent_state = s_ent_dot = s_ent_capture = NULL;

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
    s_ent_state = wt_lbl(s_scr, tr(STR_W_ENT_NOTREADY), ENT_CAM_X + 18,
                         ENT_CAM_Y + ENT_CAM_H + 8, wt_font14(), MUT_COL);
    lv_obj_set_width(s_ent_state, ENT_CAM_W - 18);
    lv_label_set_long_mode(s_ent_state, LV_LABEL_LONG_WRAP);

    // Right: the two sources, then the equation.
    s_ent_bar1 = ent_card(ENT_CAM_Y, STR_W_ENT_SRC1_CAP, STR_W_ENT_SRC1_NOTE, false);
    s_ent_bar2 = ent_card(ENT_CAM_Y + ENT_CARD_H + 8, STR_W_ENT_SRC2_CAP,
                          STR_W_ENT_SRC2_NOTE, true);

    // 1 + 2 -> 12 WORDS, in the same vocabulary the fingerprint card uses, so
    // it reads as part of one system rather than as new decoration. A flex
    // column parent, because wt_diagram_row takes its y from the layout.
    lv_obj_t *eq = lv_obj_create(s_scr);
    lv_obj_remove_style_all(eq);
    lv_obj_set_pos(eq, ENT_COL_X, ENT_CAM_Y + 2 * ENT_CARD_H + 24);
    lv_obj_set_size(eq, ENT_COL_W, 46);
    lv_obj_set_flex_flow(eq, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(eq, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(eq, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(eq, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *row = wt_diagram_row(eq);
    wt_chip(row, "1", false);
    wt_diagram_op(row, "+");
    wt_chip(row, "2", false);
    wt_diagram_op(row, LV_SYMBOL_RIGHT);
    wt_chip(row, tr(STR_W_ENT_RESULT), true);
    wt_help_chip(s_scr, 752, ENT_CAM_Y + 2 * ENT_CARD_H + 28, MUT_COL,
                 ent_mix_help_cb, NULL);

#ifdef SIMULATOR
    s_ent_capture = mk_pill(tr(STR_W_ENT_CAPTURE), 48, WT_ACTION_Y, 300,
                            sim_entropy_cb, NULL);
    wt_pill_primary(s_ent_capture);
    mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, goto_choose_cb, NULL);
    // The sim has no camera and no meter, so the walk would see a permanently
    // disabled CAPTURE. Show the ready state: it is the one the scripted tap
    // exercises, and the frame the docs publish.
    ent_ui_sync(100);
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
        ent_ui_sync(0);
    } else {
        // The camera failed. The two cards above still tell the truth about the
        // chip, so they stay; the preview column carries the error instead.
        mk_lbl(tr(STR_C_CAM_UNAVAIL), ENT_CAM_X + 14, ENT_CAM_Y + 100,
               wt_font23(), STOP_COL);
        mk_lbl(camera_spike_status(), ENT_CAM_X + 14, ENT_CAM_Y + 134,
               wt_font14(), MUT_COL);
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
    snprintf(buf, sizeof buf, tr(STR_W_WORD_N_FMT), s_nw + 1, s_count, s_prefix);
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
    if (s_nw >= s_count) {
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
    mk_screen(s_verify ? tr(STR_W_VERIFY_T) : tr(STR_W_RESTORE_T),
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

// ---- word count ----
static void count_pick_cb(lv_event_t *e)
{
    s_count = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_restore) restore_screen();
    else entropy_screen();
}

static void count_screen(void)
{
    // Reached only while RESTORING now; creating always makes 12.
    mk_screen(s_restore ? tr(STR_W_RESTORE_T) : tr(STR_W_NEW_T), tr(STR_W_HOWMANY));
    lv_obj_t *p = mk_pill(tr(STR_W_12), 48, 150, 340, count_pick_cb, (void *)(intptr_t)12);
    wt_pill_primary(p);
    mk_pill(tr(STR_W_24), 48, 230, 340, count_pick_cb, (void *)(intptr_t)24);
    wt_wraph(s_scr, tr(STR_W_12_NOTE), 430, 150, 340, 76);
    // was a bare font14 label while its twin above auto-fit: same box, same
    // job, so it gets the same treatment
    wt_wraph(s_scr, tr(STR_W_24_NOTE), 430, 230, 340, 76);
    // A SeedQR carries its own length, so it sits beside the count rather than
    // after it.
    if (s_restore) {
        mk_pill(tr(STR_W_SCAN_SEED_QR), 48, 310, 340, restore_scan_cb, NULL);
        wt_wraph(s_scr, tr(STR_W_LOAD_SCAN_NOTE), 430, 310, 340, 76);
    }
    mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, goto_choose_cb, NULL);
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
    // CREATING: always 12. 128 bits of entropy is not brute-forceable by
    // anything, so the extra 128 buys margin against nothing that can happen,
    // while 24 words doubles the length of the ONE step where a real mistake
    // is likely -- copying them onto paper by hand and reading them back. The
    // 24-word path stays fully supported for RESTORE, because seeds made on
    // other signers arrive at whatever length they arrive.
    s_count = 12;
    entropy_screen();
}

static void storage_screen(void)
{
    mk_screen(tr(STR_W_STORE_T), tr(STR_W_STORE_S));

    // Three explicit storage names, always in the same order used by
    // Settings. The note is beside its control instead of hidden behind a
    // help card: this choice decides what an attacker or a border search can
    // recover after power-off.
    // Geometry from WT_CHOICE_*, matching storage_chooser_screen() in
    // wallet_settings.c row for row: one card per mode with the pill centred in
    // it and the note given three lines at font23 beside it. The two screens
    // present the identical choice and must not drift apart again, which is why
    // the numbers live in wallet_theme.h and not in either file.
    for (int i = 0; i < 3; i++)
        wt_card(s_scr, WT_CHOICE_X, WT_CHOICE_Y(i), WT_CHOICE_W, WT_CHOICE_H);

    lv_obj_t *flash = mk_pill(tr(STR_W_KEEP_BTN), 48, WT_CHOICE_Y(0) + 22, 252,
                              storage_pick_cb,
                              (void *)(intptr_t)WSEED_MODE_KEEP);
    wt_pill_primary(flash);

    // The FLASH note tells the truth about what a chip dump would find, which
    // is the encryption state. Per HANDOFF-04's storage residual: when the
    // chip reports encryption OFF, this note is a caution not a footnote, so
    // it renders in WT_WARN not the default WT_MUT.
    {
        bool enc = wallet_seed_flash_encrypted();
        lv_obj_t *n = wt_wraph(s_scr, tr(enc ? STR_W_FLASH_ENC_NOTE
                                             : STR_W_KEEP_NOTE),
                               322, WT_CHOICE_Y(0) + 4, 420, 87);
        if (!enc) lv_obj_set_style_text_color(n, WT_WARN, 0);
    }
    mk_pill(tr(STR_W_SD_BTN), 48, WT_CHOICE_Y(1) + 22, 252, storage_pick_cb,
            (void *)(intptr_t)WSEED_MODE_SD);
    wt_wraph(s_scr, tr(STR_W_SD_NOTE), 322, WT_CHOICE_Y(1) + 4, 420, 87);

    mk_pill(tr(STR_W_AMNESIC_BTN), 48, WT_CHOICE_Y(2) + 22, 252,
            storage_pick_cb, (void *)(intptr_t)WSEED_MODE_AMNESIC);
    wt_wraph(s_scr, tr(STR_W_AMNESIC_NOTE), 322, WT_CHOICE_Y(2) + 4, 420, 87);
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
    // A card per choice. This is the first screen a new owner ever sees, and it
    // was two buttons with two paragraphs floating beside them; which paragraph
    // belonged to which button was left to the reader's eye. The cards are
    // different heights on purpose: RESTORE's note is a line longer than NEW's in
    // every locale, and forcing them equal would cost the longer one its last
    // line in exactly the languages that need it.
    //
    // The notes moved 16px left, to 444, so their boxes end inside the card.
    // At 460 with a 310 wide box they reached x=770, past the page's own 752
    // margin: harmless while nothing was drawn there, wrong once a card is.
    wt_card(s_scr, WT_CHOICE_X, 140, WT_CHOICE_W, 112);
    wt_card(s_scr, WT_CHOICE_X, 260, WT_CHOICE_W, 136);

    lv_obj_t *p = mk_pill(tr(STR_W_CHOOSE_NEW), 48, 170, 340, new_cb, NULL);
    wt_pill_primary(p);
    // Both pills are 340 and both take one type size, chosen for the PAIR. The
    // restore pill used to be 400 wide because its English label was "RESTORE
    // FROM A SEED PHRASE": two words longer than it needed to be, and the extra
    // 60px hung it out past its neighbour so the two read as different kinds of
    // control. The English label is "RESTORE SEED PHRASE" now; the other twenty
    // locales keep their longer translations and pay for them with a smaller
    // type size, which wt_pill_group_fit applies to BOTH so the row never
    // splits across two rungs.
    // RESTORE FROM A SEED PHRASE is longer than the primary and the plan's own
    // escape hatch is to widen the pill rather than drop a rung. 400 wide takes
    // the pill to x=448, so the note beside it moves to x=460 (w=310) to clear
    // it. Neither label is the primary action, so the secondary sitting a rung
    // BELOW the primary at 23 stays legitimate for locales whose translation
    // still overflows 400.
    lv_obj_t *rp = mk_pill(tr(STR_W_CHOOSE_RESTORE), 48, 302, 340, restore_cb, NULL);
    {
        const char *lbls[2] = { tr(STR_W_CHOOSE_NEW), tr(STR_W_CHOOSE_RESTORE) };
        wt_pill_fit_t f = wt_pill_group_fit(lbls, 2, 340, 52, true);
        wt_pill_apply_fit(p,  f, 340);
        wt_pill_apply_fit(rp, f, 340);
    }
    wt_wraph(s_scr, tr(STR_W_NEW_NOTE),     444, 146, 296, 100);
    wt_wraph(s_scr, tr(STR_W_RESTORE_NOTE), 444, 266, 296, 124);
    // The third pill "WHAT IS A SEED?" is now a "?" chip beside the subtitle.
    // A question does not rank equal to the two decisions, and the pill's
    // bottom edge landed at 396 anyway, two pixels off the content floor.
    // The chip is 30x30 with a 12px hit slop -> 54px effective target, so it
    // is still tappable at arm's length. Placed at (752, 66) it sits on the
    // subtitle's baseline, at the right end of the header lane.
    wt_help_chip(s_scr, 752, 66, MUT_COL, whatseed_cb, NULL);
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
    mk_body(tr(STR_W_QRBAD_B), 48, 140, 704, 240, STOP_COL);
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
    memset(words, 0, sizeof words);          // a scanned mnemonic must not linger
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
    memset(words, 0, sizeof words);
    s_count = n;
    verify_intro_screen();
}
