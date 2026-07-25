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
#include "wallet_seed.h"
#include "wallet_settings.h"   // wallet_lang_picker_open: first-boot language switch
#include "wallet_theme.h"
#include "wallet_ui.h"

#ifndef SIMULATOR
#include "camera_spike.h"
#endif

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
static bool s_restore;
static bool s_verify;           // reuse the restore keypad to CHECK the paper backup

static int s_quiz_round;
static int s_quiz_pos;          // word index being asked this round
static int s_quiz_correct;     // which of the 4 pills is right
static int s_quiz_asked[QUIZ_ROUNDS];   // positions already asked this pass

static char s_prefix[12];       // restore: letters typed for the current word
static lv_obj_t *s_word_lbl, *s_sug[3];

static void choose_screen(void);
static void count_screen(void);
static void entropy_screen(void);
static void words_screen(void);
static void quiz_screen(void);
static void restore_screen(void);
static void verify_finish(void);
static void verify_finish_exit(void);

static void goto_choose_cb(lv_event_t *e)  { (void)e; choose_screen(); }
static void goto_restore_cb(lv_event_t *e) { (void)e; restore_screen(); }

bool wallet_setup_active(void) { return s_scr != NULL; }

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

// ---- shared widgets: thin wrappers over the wallet_theme kit ----
static void mk_screen(const char *title, const char *sub)
{
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_scr = wt_screen(s_parent, title, sub);
}

static lv_obj_t *mk_pill(const char *txt, int x, int y, int w, lv_event_cb_t cb, void *ud)
{
    return wt_pill(s_scr, txt, x, y, w, cb, ud);
}

static lv_obj_t *mk_lbl(const char *txt, int x, int y, const lv_font_t *f, lv_color_t col)
{
    return wt_lbl(s_scr, txt, x, y, f, col);
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
        mk_lbl(tr(STR_W_CHECK_B),
               48, 140, wt_font14(), STOP_COL);
        mk_pill(tr(STR_W_START_OVER), 48, 404, 240, goto_restore_cb, NULL);
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
        mk_screen(tr(STR_W_VOK_T), tr(STR_W_VOK_S));
        mk_lbl(tr_sym(LV_SYMBOL_OK, STR_W_VOK_MATCH), 48, 150,
               wt_font28(), OK_COL);
        mk_lbl(tr(STR_W_VOK_B),
               48, 206, wt_font14(), MUT_COL);
        lv_obj_t *p = mk_pill(tr(STR_C_DONE), 48, 404, 300, verify_exit_cb, NULL);
        wt_pill_primary(p);
    } else {
        char buf[128];   // Cyrillic runs 2 bytes/char: 48 truncated every ru render
        snprintf(buf, sizeof buf, tr(STR_W_VBAD_FMT), mism + 1);
        mk_screen(tr(STR_W_VBAD_T), tr(STR_W_VBAD_S));
        mk_lbl(buf, 48, 150, wt_font28(), STOP_COL);
        mk_lbl(tr(STR_W_VBAD_B),
               48, 206, wt_font14(), MUT_COL);
        lv_obj_t *p = mk_pill(tr(STR_W_TYPE_AGAIN_BTN), 48, 404, 300, verify_retry_cb, NULL);
        wt_pill_primary(p);
        mk_pill(tr(STR_C_DONE), 610, 404, 140, verify_exit_cb, NULL);
    }
}

static void verify_start_cb(lv_event_t *e) { (void)e; restore_screen(); }

static void verify_intro_screen(void)
{
    mk_screen(tr(STR_W_VINTRO_T),
              tr(STR_W_VINTRO_S));
    mk_lbl(tr(STR_W_VINTRO_B),
           48, 122, wt_font14(), MUT_COL);
    lv_obj_t *p = mk_pill(tr(STR_W_TYPE_MY_WORDS), 48, 404, 300, verify_start_cb, NULL);
    wt_pill_primary(p);
    mk_pill(tr(STR_C_BACK), 610, 404, 140, verify_exit_cb, NULL);
}

// ---- quiz (prove the backup) ----
static void quiz_pick_cb(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (slot != s_quiz_correct) {           // wrong: look at the paper again
        s_quiz_round = 0;
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
    mk_screen(tr(STR_W_PROVE_T), tr(STR_W_PROVE_S));
    snprintf(buf, sizeof buf, tr(STR_W_WHICH_FMT), s_quiz_pos + 1);
    mk_lbl(buf, 48, 120, wt_font28(), INK_COL);

    for (int i = 0; i < 4; i++) {
        const char *w = s_w[s_quiz_pos];
        if (i != s_quiz_correct) {          // decoy from the wordlist
            const char *d = NULL;
            do {
                wallet_seed_word((int)(ui_rand() % 2048u), &d);
            } while (d && strcmp(d, s_w[s_quiz_pos]) == 0);
            w = d ? d : "static";
        }
        mk_pill(w, 48 + (i % 2) * 380, 200 + (i / 2) * 80, 340,
                quiz_pick_cb, (void *)(intptr_t)i);
    }
    snprintf(buf, sizeof buf, tr(STR_W_QUIZ_N_FMT), s_quiz_round + 1, QUIZ_ROUNDS);
    mk_lbl(buf, 680, 30, wt_font14(), MUT_COL);
}

// ---- words on screen (the backup moment) ----
static void words_go_cb(lv_event_t *e)
{
    (void)e;
    s_quiz_round = 0;
    quiz_screen();
}

static void words_screen(void)
{
    mk_screen(tr(STR_W_WRITE_T), tr(STR_W_WRITE_S));
    int cols = s_count == 24 ? 4 : 2;
    int rows = s_count / cols;
    for (int i = 0; i < s_count; i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "%2d. %.11s", i + 1, s_w[i]);
        int c = i / rows, r = i % rows;
        mk_lbl(buf, 48 + c * (cols == 4 ? 184 : 300), 108 + r * 42,
               wt_font14(), INK_COL);
    }
    // the one rule that matters while they are copying: loud, under the grid,
    // not buried at the end of the subtitle
    lv_obj_t *po = mk_lbl(tr(STR_W_PAPER_ONLY), 48, 352,
                          wt_body_font(tr(STR_W_PAPER_ONLY), 700, 40), WARN_COL);
    lv_obj_set_width(po, 700);
    lv_label_set_long_mode(po, LV_LABEL_LONG_WRAP);
    mk_pill(tr(STR_W_WROTE), 430, 404, 320, words_go_cb, NULL);
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

static void entropy_screen(void)
{
    mk_screen(tr(STR_W_RAND_T), tr(STR_W_RAND_S));
#ifdef SIMULATOR
    mk_lbl("(simulator: camera entropy is scripted)\n\n"
           "on the device, the camera shot is MIXED with\n"
           "the chip's own hardware randomness - neither\n"
           "source alone decides your words.", 48, 140,
           wt_font14(), MUT_COL);
    mk_pill("CAPTURE", 48, 404, 240, sim_entropy_cb, NULL);
    mk_pill(tr(STR_C_BACK), 610, 404, 140, goto_choose_cb, NULL);
#else
    mk_lbl(tr(STR_W_RAND_B), 48, 122,
           wt_font14(), MUT_COL);
    if (camera_entropy_start()) {
        lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);   // any tap = capture try
        lv_obj_add_event_cb(s_scr, ent_tap_cb, LV_EVENT_CLICKED, NULL);
        if (!s_ent_tmr) s_ent_tmr = lv_timer_create(ent_poll_cb, 80, NULL);
    } else {
        mk_lbl(tr(STR_C_CAM_UNAVAIL), 48, 240, wt_font28(), STOP_COL);
        mk_lbl(camera_spike_status(), 48, 284, wt_font14(), MUT_COL);
    }
    mk_pill(tr(STR_C_BACK), 610, 404, 140, ent_back_cb, NULL);
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
    mk_screen(s_restore ? tr(STR_W_RESTORE_T) : tr(STR_W_NEW_T), tr(STR_W_HOWMANY));
    lv_obj_t *p = mk_pill(tr(STR_W_12), 48, 150, 340, count_pick_cb, (void *)(intptr_t)12);
    wt_pill_primary(p);
    mk_pill(tr(STR_W_24), 48, 230, 340, count_pick_cb, (void *)(intptr_t)24);
    mk_lbl(tr(STR_W_12_NOTE), 430, 150,
           wt_font14(), MUT_COL);
    mk_lbl(tr(STR_W_24_NOTE), 430, 230,
           wt_font14(), MUT_COL);
    mk_pill(tr(STR_C_BACK), 610, 404, 140, goto_choose_cb, NULL);
}

// ---- entry ----
static void new_cb(lv_event_t *e)     { (void)e; s_restore = false; count_screen(); }
static void restore_cb(lv_event_t *e) { (void)e; s_restore = true;  count_screen(); }
static void cancel_cb(lv_event_t *e)  { (void)e; close_all(); }

static void setup_lang_picked(void)
{
    choose_screen();                   // mk_screen replaces s_scr (overlay dies with it)
}

static void setup_lang_cb(lv_event_t *e)
{
    (void)e;
    wallet_lang_picker_open(s_scr, setup_lang_picked);
}

static void choose_screen(void)
{
    mk_screen(tr(STR_W_SETUP_T), tr(STR_W_SETUP_S));
    lv_obj_t *p = mk_pill(tr(STR_W_CREATE_NEW), 48, 150, 340, new_cb, NULL);
    wt_pill_primary(p);
    mk_pill(tr(STR_W_RESTORE_FROM_WORDS), 48, 230, 340, restore_cb, NULL);
    lv_obj_t *note = wt_wrap(s_scr, 430, 158, 340);
    lv_label_set_text(note, tr(STR_W_NEW_NOTE));
    note = wt_wrap(s_scr, 430, 238, 340);
    lv_label_set_text(note, tr(STR_W_RESTORE_NOTE));
    mk_pill(tr(STR_C_CANCEL), 610, 404, 140, cancel_cb, NULL);

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
