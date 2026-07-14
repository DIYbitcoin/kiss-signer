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

#include "wallet_seed.h"
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
        mk_screen("CHECK YOUR WORDS", "those aren't a valid set of backup words");
        mk_lbl("one or more words are wrong. check them\nagainst your paper and try again.",
               48, 140, &lv_font_montserrat_14, STOP_COL);
        mk_pill("START OVER", 48, 404, 240, goto_restore_cb, NULL);
        return;
    }
    void (*cb)(void) = s_done;
    close_all();
    if (cb) cb();
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
    char buf[64];
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
    mk_screen("PROVE IT", "no peeking. these words are your only way back\n"
                          "into this wallet if you ever lose the device.");
    snprintf(buf, sizeof buf, "which is word #%d?", s_quiz_pos + 1);
    mk_lbl(buf, 48, 120, &lv_font_montserrat_28, INK_COL);

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
    snprintf(buf, sizeof buf, "%d of %d", s_quiz_round + 1, QUIZ_ROUNDS);
    mk_lbl(buf, 680, 30, &lv_font_montserrat_14, MUT_COL);
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
    mk_screen("WRITE THESE DOWN", "these words ARE your wallet. copy them onto\n"
                                  "paper in order. never a photo or a computer file.");
    int cols = s_count == 24 ? 4 : 2;
    int rows = s_count / cols;
    for (int i = 0; i < s_count; i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "%2d. %.11s", i + 1, s_w[i]);
        int c = i / rows, r = i % rows;
        mk_lbl(buf, 48 + c * (cols == 4 ? 184 : 300), 108 + r * 42,
               &lv_font_montserrat_14, INK_COL);
    }
    mk_pill("I WROTE THEM DOWN", 430, 404, 320, words_go_cb, NULL);
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
    mk_screen("ADD RANDOMNESS", "a wallet is only as safe as how randomly it\n"
                                "was created. the camera provides that.");
#ifdef SIMULATOR
    mk_lbl("(simulator: camera entropy is scripted)", 48, 140,
           &lv_font_montserrat_14, MUT_COL);
    mk_pill("CAPTURE", 48, 404, 240, sim_entropy_cb, NULL);
    mk_pill("BACK", 610, 404, 140, goto_choose_cb, NULL);
#else
    mk_lbl("point the camera at anything messy, like\n"
           "leaves, gravel, or a shuffled deck of cards.\n"
           "tap the screen once the bar turns green.", 48, 122,
           &lv_font_montserrat_14, MUT_COL);
    if (camera_entropy_start()) {
        lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);   // any tap = capture try
        lv_obj_add_event_cb(s_scr, ent_tap_cb, LV_EVENT_CLICKED, NULL);
        if (!s_ent_tmr) s_ent_tmr = lv_timer_create(ent_poll_cb, 80, NULL);
    } else {
        mk_lbl("camera unavailable", 48, 240, &lv_font_montserrat_28, STOP_COL);
        mk_lbl(camera_spike_status(), 48, 284, &lv_font_montserrat_14, MUT_COL);
    }
    mk_pill("BACK", 610, 404, 140, ent_back_cb, NULL);
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
    char buf[48];
    snprintf(buf, sizeof buf, "word %d of %d:  %s_", s_nw + 1, s_count, s_prefix);
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
        store_and_finish();
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
    if (strcmp(txt, "CANCEL") == 0) {
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
    mk_screen("RESTORE", "type each word, then tap it when it appears");

    s_word_lbl = mk_lbl("", 48, 108, &lv_font_montserrat_28, INK_COL);

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
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_28, LV_PART_ITEMS);
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
    mk_screen(s_restore ? "RESTORE" : "NEW WALLET", "how many words?");
    lv_obj_t *p = mk_pill("12 WORDS", 48, 150, 340, count_pick_cb, (void *)(intptr_t)12);
    wt_pill_primary(p);
    mk_pill("24 WORDS", 48, 230, 340, count_pick_cb, (void *)(intptr_t)24);
    mk_lbl("recommended. your passphrase\n"
           "does the heavy lifting anyway.", 430, 150,
           &lv_font_montserrat_14, MUT_COL);
    mk_lbl("not safer, just longer to write down.\n"
           "pick this only to match an old backup.", 430, 230,
           &lv_font_montserrat_14, MUT_COL);
    mk_pill("BACK", 610, 404, 140, goto_choose_cb, NULL);
}

// ---- entry ----
static void new_cb(lv_event_t *e)     { (void)e; s_restore = false; count_screen(); }
static void restore_cb(lv_event_t *e) { (void)e; s_restore = true;  count_screen(); }
static void cancel_cb(lv_event_t *e)  { (void)e; close_all(); }

static void choose_screen(void)
{
    mk_screen("SET UP YOUR WALLET", "there is no wallet on this device yet");
    lv_obj_t *p = mk_pill("CREATE NEW", 48, 150, 340, new_cb, NULL);
    wt_pill_primary(p);
    mk_pill("RESTORE FROM WORDS", 48, 230, 340, restore_cb, NULL);
    mk_lbl("a brand-new wallet made on this device", 430, 158, &lv_font_montserrat_14, MUT_COL);
    mk_lbl("bring back a wallet from its backup words", 430, 238, &lv_font_montserrat_14, MUT_COL);
    mk_pill("CANCEL", 610, 404, 140, cancel_cb, NULL);
}

void wallet_setup_open(lv_obj_t *parent, void (*done_cb)(void))
{
    if (s_scr) return;
    wallet_ui_ensure_indev();          // wizard can be the first touch UI ever
    s_parent = parent;
    s_done = done_cb;
    s_restore = false;
    wipe_state();
    choose_screen();
}
