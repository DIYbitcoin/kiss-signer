// Settings: network selection. Style matches the other wallet screens.
// The network flips instantly (the master key is network-free) and persists
// across power cycles via NVS on the device.
#include "wallet_settings.h"

#include <stdio.h>
#include <string.h>

#include "flag_imgs.h"   // language-picker flags (Twemoji, CC-BY; en has none)
#include "i18n.h"
#include "wallet_crypto.h"
#include "wallet_info.h"
#include "wallet_seed.h"
#include "wallet_setup.h"
#include "wallet_duress.h"
#include "wallet_duress_ui.h"
#include "wallet_theme.h"
#include "wallet_ui.h"   // wallet_build_id_apply: the shared build-identity line
#include "wallet_usage.h"   // clear the receive-index history on wipe

#ifndef SIMULATOR
#include "nvs.h"
#include "nvs_flash.h"
#endif

// main.c: closes any wallet screen and runs the seed wizard, then the
// type-twice login. Lets Settings reach CREATE NEW / RESTORE after first boot.
void wallet_begin_setup(void);
// main.c: re-sync the home TESTNET badge after the network is changed here.
void wallet_home_refresh(void);
// main.c: after a wipe, lock straight back to the game (the wallet no longer
// exists; the next KISS unlock lands in first-boot setup).
void wallet_wiped_lock(void);

#define BG_COL   WT_BG
#define INK_COL  WT_INK
#define MUT_COL  WT_MUT
#define KEY_COL  WT_KEY
#define OK_COL   WT_OK
#define WARN_COL WT_WARN

#define STOP_COL WT_STOP

// Vertical budget for the two notes whose text is swapped by restyle(): they
// have to be re-fitted on every tap, so the gap lives here rather than being
// written twice and drifting apart.
//
// NETWORK and the selected ADDRESS TYPE both read at font23. The three type
// choices moved to their own full-width screen; squeezing them into 110px
// segments made the choice names and their explanations the smallest text on
// Settings.
#define NET_NOTE_H  58   // two lines at font23; "TESTNET: practice coins
                         // with no value." needs both and used to lose them
#define TYPE_NOTE_H 29

static lv_obj_t *s_scr;
static lv_obj_t *s_acc_dot[WT_ACC_N];   // theme dots, top-right
static lv_obj_t *s_acc_name;            // live name under the dots
static lv_obj_t *s_main_pill, *s_test_pill, *s_state_lbl;
static lv_obj_t *s_replace_pill;
static lv_obj_t *s_build_id;
static lv_obj_t *s_wipe_pill;
static lv_obj_t *s_lang_pill;   // paired with BACK so the bottom row matches
static lv_obj_t *s_type_pill, *s_type_pfx, *s_type_expl;  // selected type row
static lv_obj_t *s_storage_pill;  // STORAGE over the explicit current mode
static lv_obj_t *s_parent;      // language change rebuilds the screen here

static int s_load_error_code;
#ifdef SIMULATOR
static wallet_settings_load_status_t s_sim_load_status = WSETTINGS_LOAD_OK;
static int s_sim_load_error_code;

void wallet_settings_sim_set_load_result(wallet_settings_load_status_t status,
                                         int error_code)
{
    s_sim_load_status = status;
    s_sim_load_error_code = error_code;
}
#endif

// example address prefix per type, following the current network so it never
// lies (bc1 on mainnet, tb1 on testnet).
static const char *type_prefix(int sc, int tn)
{
    switch (sc) {
    case WSCRIPT_LEGACY: return tn ? "m/n..." : "1...";
    case WSCRIPT_NESTED: return tn ? "2..."   : "3...";
    default:             return tn ? "tb1..." : "bc1...";
    }
}

static const char *type_name(int sc)
{
    switch (sc) {
    case WSCRIPT_LEGACY: return tr(STR_S_TY_LEGACY);
    case WSCRIPT_NESTED: return tr(STR_S_TY_NESTED);
    default:             return tr(STR_S_TY_NATIVE);
    }
}

static const char *type_note(int sc)
{
    switch (sc) {
    case WSCRIPT_LEGACY: return tr(STR_G_TY_LEGACY_NOTE);
    case WSCRIPT_NESTED: return tr(STR_G_TY_NESTED_NOTE);
    default:             return tr(STR_G_TY_NATIVE_NOTE);
    }
}

bool wallet_settings_active(void) { return s_scr != NULL; }

// ---- persistence ----
static void store_u8(const char *key, uint8_t v)
{
#ifndef SIMULATOR
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
#else
    (void)key; (void)v;
#endif
}

const char *wallet_settings_load_status_name(wallet_settings_load_status_t status)
{
    switch (status) {
    case WSETTINGS_LOAD_OK:                return "OK";
    case WSETTINGS_LOAD_NVS_NO_FREE_PAGES: return "NVS_NO_FREE_PAGES";
    case WSETTINGS_LOAD_NVS_NEW_VERSION:   return "NVS_NEW_VERSION_FOUND";
    case WSETTINGS_LOAD_NVS_INIT_FAILED:   return "NVS_INIT_FAILED";
    case WSETTINGS_LOAD_NVS_OPEN_FAILED:   return "NVS_OPEN_FAILED";
    case WSETTINGS_LOAD_NVS_READ_FAILED:   return "NVS_READ_FAILED";
    default:                               return "NVS_UNKNOWN_FAILURE";
    }
}

int wallet_settings_load_error_code(void)
{
    return s_load_error_code;
}

#ifndef SIMULATOR
static wallet_settings_load_status_t init_failure(esp_err_t err)
{
    s_load_error_code = (int)err;
    if (err == ESP_ERR_NVS_NO_FREE_PAGES)
        return WSETTINGS_LOAD_NVS_NO_FREE_PAGES;
    if (err == ESP_ERR_NVS_NEW_VERSION_FOUND)
        return WSETTINGS_LOAD_NVS_NEW_VERSION;
    return WSETTINGS_LOAD_NVS_INIT_FAILED;
}

static bool get_optional_u8(nvs_handle_t h, const char *key, uint8_t *value)
{
    esp_err_t err = nvs_get_u8(h, key, value);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND)
        return true;
    s_load_error_code = (int)err;
    return false;
}
#endif

wallet_settings_load_status_t wallet_settings_load(void)
{
#ifdef SIMULATOR
    s_load_error_code = s_sim_load_error_code;
    return s_sim_load_status;
#else
    s_load_error_code = 0;
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK)
        return init_failure(err);

    nvs_handle_t h;
    uint8_t tn = 0, sc = 0, ac = 0, lg = 0;
    err = nvs_open("kiss", NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // A genuinely blank partition has no namespace yet. That is the one
        // open failure that means "fresh", not "storage became unreadable".
    } else if (err != ESP_OK) {
        s_load_error_code = (int)err;
        return WSETTINGS_LOAD_NVS_OPEN_FAILED;
    } else {
        bool ok = get_optional_u8(h, "testnet", &tn) &&
                  get_optional_u8(h, "script", &sc) &&
                  get_optional_u8(h, "accent", &ac) &&
                  get_optional_u8(h, "lang", &lg);
        nvs_close(h);
        if (!ok)
            return WSETTINGS_LOAD_NVS_READ_FAILED;
    }

    // Apply nothing until the complete settings read is known-good. Defaults
    // are deliberate only for an absent namespace/key, never for an I/O/type
    // failure that could otherwise make a configured wallet look factory-new.
    wallet_set_network(tn);
    wallet_set_script(sc);
    wt_accent_set(ac);
    i18n_set_lang(lg);
    return WSETTINGS_LOAD_OK;
#endif
}

// ---- screen ----
static void restyle(void)
{
    int tn = wallet_testnet();
    // accent follows the picked theme everywhere it appears on this screen
    lv_obj_set_style_text_color(lv_obj_get_child(s_scr, 0), wt_accent(), 0);  // title
    wallet_build_id_restyle(s_build_id);
    for (int i = 0; i < WT_ACC_N; i++)
        if (s_acc_dot[i]) {
            bool on = (i == wt_accent_get());
            lv_obj_set_style_border_color(s_acc_dot[i], on ? wt_accent() : KEY_COL, 0);
            lv_obj_set_style_border_width(s_acc_dot[i], on ? 3 : 1, 0);
            lv_obj_set_style_shadow_width(s_acc_dot[i], on ? 12 : 0, 0);
            lv_obj_set_style_shadow_color(s_acc_dot[i], wt_accent(), 0);
            lv_obj_set_style_shadow_opa(s_acc_dot[i], on ? 90 : 0, 0);
            // detached ink halo: the gap reads even when the dot is white (MONO)
            lv_obj_set_style_outline_width(s_acc_dot[i], on ? 2 : 0, 0);
            lv_obj_set_style_outline_pad(s_acc_dot[i], 3, 0);
            lv_obj_set_style_outline_color(s_acc_dot[i], INK_COL, 0);
        }
    if (s_acc_name) lv_label_set_text(s_acc_name, wt_accent_name());
    lv_obj_set_style_bg_color(s_main_pill, tn ? KEY_COL : wt_accent_bg(), 0);
    lv_obj_set_style_border_color(s_main_pill, tn ? MUT_COL : wt_primary(), 0);
    lv_obj_set_style_border_width(s_main_pill, tn ? 1 : 2, 0);
    lv_obj_set_style_bg_color(s_test_pill, tn ? lv_color_hex(0x2A2113) : KEY_COL, 0);
    lv_obj_set_style_border_color(s_test_pill, tn ? WARN_COL : MUT_COL, 0);
    lv_obj_set_style_border_width(s_test_pill, tn ? 2 : 1, 0);
    wt_note_fit(s_state_lbl, tn ? tr(STR_G_TESTNET_NOTE) : tr(STR_G_MAINNET_NOTE),
                340, NET_NOTE_H);
    lv_obj_set_style_text_color(s_state_lbl, tn ? WARN_COL : MUT_COL, 0);

    if (s_type_pill) {
        int sc = wallet_script();
        lv_label_set_text(lv_obj_get_child(s_type_pill, 0), type_name(sc));
        lv_label_set_text(s_type_pfx, type_prefix(sc, tn));
        lv_obj_set_style_text_color(s_type_pfx, wt_accent(), 0);
        wt_pill_select(s_type_pill, true);
        wt_note_fit(s_type_expl, type_note(sc), 340, TYPE_NOTE_H);
    }

    // The storage pill is selected, so wt_pill_select paints it in the accent,
    // and it was painted once at build time and never again. Picking a new
    // theme on this very screen left it wearing the old one until something
    // else rebuilt the screen, which made the theme look like it had only half
    // applied. Every other selected control on this screen is re-asserted
    // above; this one was simply missed.
    if (s_storage_pill) wt_pill_select(s_storage_pill, true);
}

static void pick_cb(lv_event_t *e)
{
    int tn = (int)(intptr_t)lv_event_get_user_data(e);
    wallet_set_network(tn);
    store_u8("testnet", tn ? 1 : 0);
    restyle();
}

static void settings_reopen(void)
{
    lv_obj_t *parent = s_parent;
    s_type_pill = s_type_pfx = s_type_expl = s_storage_pill = NULL;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    wallet_settings_open(parent);
}

// ---- wallet storage: explicit current mode + transactional migration ----
static const char *storage_mode_name(int mode)
{
    switch (mode) {
    case WSEED_MODE_SD:      return tr(STR_W_SD_BTN);
    case WSEED_MODE_AMNESIC: return tr(STR_W_AMNESIC_BTN);
    default:                 return tr(STR_W_KEEP_BTN);
    }
}

static const char *storage_mode_note(int mode)
{
    switch (mode) {
    case WSEED_MODE_SD:
        return tr(STR_W_SD_NOTE);
    case WSEED_MODE_AMNESIC: return tr(STR_W_AMNESIC_NOTE);
    default:
        // Encryption state, not SD availability -- see storage_screen.
        return tr(wallet_seed_flash_encrypted() ? STR_W_FLASH_ENC_NOTE
                                                : STR_W_KEEP_NOTE);
    }
}

static void storage_chooser_screen(void);

static void storage_result_ack_cb(lv_event_t *e)
{
    (void)e;
    // AMNESIC keeps the current unlocked mnemonic in RAM by contract. Return
    // to Settings so it can even be moved back to persistent storage before
    // the owner explicitly locks; the next lock/power-off is what forgets it.
    settings_reopen();
}

static void storage_result_screen(int rc, int target)
{
    const char *title;
    const char *body;
    lv_color_t title_col;
    char formatted[512];

    if (rc == WSEED_OK) {
        title = tr(STR_G_STORAGE_OK_T);
        title_col = OK_COL;
        if (target == WSEED_MODE_AMNESIC) {
            body = tr(STR_G_STORAGE_OK_AMNESIC_B);
        } else {
            snprintf(formatted, sizeof formatted, tr(STR_G_STORAGE_OK_FMT),
                     storage_mode_name(target));
            body = formatted;
        }
    } else if (rc == WSEED_ERR_CLEANUP) {
        // The backend contract is precise here: destination committed and
        // verified, old-source cleanup failed. Do not say "not changed" and do
        // not claim one-copy storage.
        title = tr(STR_G_STORAGE_CLEANUP_T);
        title_col = WARN_COL;
        body = tr(STR_G_STORAGE_CLEANUP_B);
    } else {
        title = tr(STR_G_STORAGE_FAIL_T);
        title_col = STOP_COL;
        if (rc == WSEED_ERR_SD_MISSING || rc == WSEED_ERR_SD_IO ||
            rc == WSEED_ERR_SD_CORRUPT)
            body = tr(STR_G_STORAGE_FAIL_CARD_B);
        else if (rc == WSEED_ERR_VERIFY)
            body = tr(STR_G_STORAGE_FAIL_VERIFY_B);
        else
            body = tr(STR_G_STORAGE_FAIL_GENERIC_B);
    }

    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_scr = wt_screen(s_parent, title, NULL);
    lv_obj_set_style_text_color(lv_obj_get_child(s_scr, 0), title_col, 0);
    lv_obj_t *b = wt_lbl(s_scr, body, 48, 136,
                         wt_body_font(body, 704, 230), MUT_COL);
    lv_obj_set_width(b, 704);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *ok = wt_pill(s_scr, tr(STR_C_OK), 300, WT_ACTION_Y, 200,
                           storage_result_ack_cb, NULL);
    if (rc == WSEED_OK) wt_pill_primary(ok);
}

static void storage_apply(void *ud)
{
    int target = (int)(intptr_t)ud;
    int rc = wallet_seed_move_to(target);
    storage_result_screen(rc, target);
}

static void storage_confirm_cancel_cb(lv_event_t *e)
{
    (void)e;
    storage_chooser_screen();
}

static void storage_confirm_screen(int target)
{
    const char *body = target == WSEED_MODE_SD
                     ? tr(STR_G_STORAGE_CONFIRM_SD_B)
                     : target == WSEED_MODE_AMNESIC
                     ? tr(STR_G_STORAGE_CONFIRM_AMNESIC_B)
                     : tr(STR_G_STORAGE_CONFIRM_FLASH_B);
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_scr = wt_screen(s_parent, tr(STR_G_STORAGE_CONFIRM_T), NULL);
    lv_obj_t *b = wt_lbl(s_scr, body, 48, 126,
                         wt_body_font(body, 704, 238),
                         target == WSEED_MODE_AMNESIC ? WARN_COL : MUT_COL);
    lv_obj_set_width(b, 704);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);

    // The tall row: this hold label wraps to two lines in most locales. It used
    // to be a hand typed 392/66, a third convention beside the 404 everywhere
    // else and the 398 on the sign screen, and 392 put its top 6px above the
    // content line where it read as content rather than as a button. CANCEL
    // takes the tall geometry too, because a row whose pills have different
    // heights stops looking like a row.
    wt_hold_pill(s_scr,
                 tr(target == WSEED_MODE_AMNESIC
                    ? STR_G_STORAGE_HOLD_AMNESIC
                    : STR_G_STORAGE_HOLD_MOVE),
                 48, WT_ACTION_Y_TALL, 330, WT_ACTION_H_TALL, 1500, storage_apply,
                 (void *)(intptr_t)target);
    lv_obj_t *cancel = wt_pillh(s_scr, tr(STR_C_CANCEL), 585, WT_ACTION_Y_TALL, 165,
                                WT_ACTION_H_TALL, storage_confirm_cancel_cb, NULL);
    lv_obj_set_ext_click_area(cancel, 10);
}

static void storage_pick_cb(lv_event_t *e)
{
    int target = (int)(intptr_t)lv_event_get_user_data(e);
    if (target == wallet_seed_mode()) return;     // already selected and named
    storage_confirm_screen(target);
}

static void storage_chooser_back_cb(lv_event_t *e)
{
    (void)e;
    settings_reopen();
}

static void storage_chooser_screen(void)
{
    int current = wallet_seed_mode();
    char current_line[128];
    snprintf(current_line, sizeof current_line, tr(STR_G_STORAGE_CURRENT_FMT),
             storage_mode_name(current));

    s_type_pill = s_type_pfx = s_type_expl = s_storage_pill = NULL;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_scr = wt_screen(s_parent, tr(STR_G_STORAGE_SEC), current_line);

    static const int modes[3] = {
        WSEED_MODE_KEEP, WSEED_MODE_SD, WSEED_MODE_AMNESIC
    };
    // 107px apart, not 108, and starting at 106 rather than 110. Each note is
    // given three lines at font23 (87px) and each sits 10px above its pill, so
    // the first note lands exactly on the y=96 content line and the third ends
    // at 396. At 110 the third ran to 402 and the languages that actually need
    // the third line, Turkish, Portuguese and Russian, were the ones that lost
    // it: the mode that keeps nothing on the device explaining itself in two
    // lines instead of three.
    static const int py[3] = {106, 213, 320};
    for (int i = 0; i < 3; i++) {
        int mode = modes[i];
        lv_obj_t *p = wt_pillh(s_scr, storage_mode_name(mode),
                               48, py[i], 252, 52, storage_pick_cb,
                               (void *)(intptr_t)mode);
        wt_pill_select(p, current == mode);
        lv_obj_t *note = wt_wraph(s_scr, storage_mode_note(mode),
                                  330, py[i] - 10, 420, 87);
        // Per ADDENDUM-02 style rule and HANDOFF-04's storage residual: the
        // FLASH note is a caution when the chip reports encryption OFF, not a
        // footnote. WT_WARN, not the default WT_MUT.  "your recovery words are
        // saved here unencrypted" needs to READ as a warning; drawing it in
        // muted grey was the review's original complaint on this row.
        if (mode == WSEED_MODE_KEEP && !wallet_seed_flash_encrypted())
            lv_obj_set_style_text_color(note, WT_WARN, 0);
    }
    lv_obj_t *back = wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140,
                             storage_chooser_back_cb, NULL);
    lv_obj_set_ext_click_area(back, 10);
}

static void storage_open_cb(lv_event_t *e)
{
    (void)e;
    storage_chooser_screen();
}

static void type_pick_cb(lv_event_t *e)
{
    int sc = (int)(intptr_t)lv_event_get_user_data(e);
    wallet_set_script(sc);
    store_u8("script", (uint8_t)sc);
    settings_reopen();                     // return with the selected row updated
}

static void type_back_cb(lv_event_t *e)
{
    (void)e;
    settings_reopen();
}

static void type_open_cb(lv_event_t *e)
{
    (void)e;
    s_type_pill = s_type_pfx = s_type_expl = NULL;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }

    // No subtitle. It used to say that each network and type pair is its own
    // separate wallet, which is true and which nobody needed told: the three
    // rows below already name the tradeoff each type makes, and anyone who has
    // met testnet knows its coins live somewhere else.
    s_scr = wt_screen(s_parent, tr(STR_I_SEC_TYPE), NULL);

    // Oldest-to-newest makes the tradeoff legible as a progression, and puts
    // the recommended Native SegWit choice last, closest to the action row.
    // Each option gets the full 704px width: a 23px name in the pill and its
    // plain-language explanation at 23px underneath.
    static const int scripts[3] = {
        WSCRIPT_LEGACY, WSCRIPT_NESTED, WSCRIPT_NATIVE
    };
    static const int py[3] = {96, 190, 282};
    static const int ny[3] = {154, 248, 340};
    int tn = wallet_testnet();
    for (int i = 0; i < 3; i++) {
        int sc = scripts[i];
        char label[96];
        snprintf(label, sizeof label, "%s   %s", type_name(sc),
                 type_prefix(sc, tn));
        lv_obj_t *p = wt_pillh(s_scr, label, 48, py[i], 704, 52,
                               type_pick_cb, (void *)(intptr_t)sc);
        wt_pill_select(p, wallet_script() == sc);
        wt_note(s_scr, type_note(sc), 68, ny[i], 664, 29);
    }
    lv_obj_set_ext_click_area(
        wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, type_back_cb, NULL), 10);
}

// The stroke chooser takes over the screen and hands control back here.
static void duress_cb(lv_event_t *e)
{
    (void)e;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    // No passphrase on this session means this wallet IS the spare: both ways
    // in reach it, so configuring a stroke would only let someone believe
    // otherwise. Say that instead of opening the chooser.
    if (wallet_session_decoy())
        wallet_duress_ui_open_nopass(s_parent, settings_reopen);
    else
        wallet_duress_ui_open(s_parent, settings_reopen);
}

static void theme_pick_cb(lv_event_t *e)
{
    wt_accent_set((int)(intptr_t)lv_event_get_user_data(e));
    store_u8("accent", (uint8_t)wt_accent_get());
    restyle();
    wallet_home_refresh();
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    wallet_home_refresh();                // reflect any network change on the home badge
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
}

void wallet_settings_close(void) { close_cb(NULL); }   // idle auto-lock path

static void settings_after_words(void)
{
    wallet_settings_open(s_parent);
}

static void words_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *parent = s_parent;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    wallet_info_open_words(parent, settings_after_words);
}

// Wipe: erase the seed and go back to being just a game. One tap on the pill
// opens a confirm screen; the erase happens on a HOLD there and takes effect
// immediately (a power pull right after must still find the seed gone), then a
// full-screen confirmation says so.
static void wiped_ok_cb(lv_event_t *e)
{
    (void)e;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    wallet_wiped_lock();
}
static void wipe_fail_ok_cb(lv_event_t *e)   // dismiss back to settings, retryable
{
    lv_obj_t *ovl = lv_event_get_user_data(e);
    lv_obj_delete_async(ovl);
}

// The erase itself. Reached only from the confirm screen's hold, never from a
// tap on the Settings pill: two taps in one spot is a gesture a pocket or a
// double tap can produce by accident, and this one is not undoable from here.
#define WIPE_HOLD_MS 2000    // longer than hold-to-sign: this one has no undo

static void wipe_cancel_cb(lv_event_t *e)
{
    lv_obj_delete_async((lv_obj_t *)lv_event_get_user_data(e));
}

static void do_wipe(void *ud)
{
    lv_obj_t *confirm = ud;
    if (confirm) lv_obj_delete_async(confirm);
    if (wallet_seed_wipe() != 0) {        // NVS erase/commit CAN fail: never claim
        // "erased" unless it truly is — say so and change nothing
        lv_obj_t *ovl = lv_obj_create(s_scr);
        lv_obj_remove_style_all(ovl);
        lv_obj_set_size(ovl, 800, 480);
        lv_obj_set_pos(ovl, 0, 0);
        lv_obj_set_style_bg_color(ovl, BG_COL, 0);
        lv_obj_set_style_bg_opa(ovl, LV_OPA_COVER, 0);
        lv_obj_add_flag(ovl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(ovl, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *t = lv_label_create(ovl);
        lv_label_set_text(t, tr(STR_G_NOERASE_T));
        lv_obj_set_style_text_color(t, STOP_COL, 0);
        lv_obj_set_style_text_font(t, wt_font28(), 0);
        lv_obj_set_style_text_letter_space(t, 3, 0);
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 150);
        lv_obj_t *st = lv_label_create(ovl);
        lv_label_set_text(st, tr(STR_G_NOERASE_B));
        lv_obj_set_style_text_color(st, MUT_COL, 0);
        lv_obj_set_style_text_font(st, wt_body_font(tr(STR_G_NOERASE_B), 704, 160), 0);
        lv_obj_set_width(st, 704);
        lv_label_set_long_mode(st, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(st, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(st, LV_ALIGN_TOP_MID, 0, 196);
        lv_obj_t *ok = lv_obj_create(ovl);
        lv_obj_remove_style_all(ok);
        lv_obj_set_size(ok, 200, 52);
        lv_obj_align(ok, LV_ALIGN_TOP_MID, 0, 386);
        lv_obj_set_style_radius(ok, 26, 0);
        lv_obj_set_style_bg_color(ok, KEY_COL, 0);
        lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ok, 1, 0);
        lv_obj_set_style_border_color(ok, STOP_COL, 0);
        lv_obj_add_flag(ok, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(ok, wipe_fail_ok_cb, LV_EVENT_CLICKED, ovl);
        lv_obj_t *okl = lv_label_create(ok);
        lv_label_set_text(okl, tr(STR_C_BACK));
        lv_obj_set_style_text_color(okl, INK_COL, 0);
        lv_obj_set_style_text_font(okl, wt_font14(), 0);
        lv_obj_center(okl);
        return;
    }
    wallet_session_close();               // truly gone: session key leaves RAM too
    wallet_usage_wipe();                   // drop the receive-index history too
    // On device the whole-partition erase in wallet_seed.c has already taken
    // these (they are deliberately absent from its KEEP_KEYS). Host builds keep
    // them in RAM, so say it explicitly: an unlock layout that outlived its
    // seed would point at a wallet that no longer exists.
    wallet_duress_forget();

    // full-screen confirmation as an overlay child (never delete the event
    // target's ancestors mid-event)
    lv_obj_t *ovl = lv_obj_create(s_scr);
    lv_obj_remove_style_all(ovl);
    lv_obj_set_size(ovl, 800, 480);
    lv_obj_set_pos(ovl, 0, 0);
    lv_obj_set_style_bg_color(ovl, BG_COL, 0);
    lv_obj_set_style_bg_opa(ovl, LV_OPA_COVER, 0);
    lv_obj_add_flag(ovl, LV_OBJ_FLAG_CLICKABLE);   // swallow stray taps
    lv_obj_clear_flag(ovl, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(ovl);
    lv_label_set_text(t, tr(STR_G_ERASED_T));
    lv_obj_set_style_text_color(t, INK_COL, 0);
    lv_obj_set_style_text_font(t, wt_font28(), 0);
    lv_obj_set_style_text_letter_space(t, 3, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 140);

    lv_obj_t *s = lv_label_create(ovl);
    lv_label_set_text(s, tr(STR_G_ERASED_B));
    lv_obj_set_style_text_color(s, MUT_COL, 0);
    lv_obj_set_style_text_font(s, wt_body_font(tr(STR_G_ERASED_B), 704, 160), 0);
    lv_obj_set_width(s, 704);
    lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 196);

    lv_obj_t *ok = lv_obj_create(ovl);
    lv_obj_remove_style_all(ok);
    lv_obj_set_size(ok, 200, 52);
    lv_obj_align(ok, LV_ALIGN_TOP_MID, 0, 386);
    lv_obj_set_style_radius(ok, 26, 0);
    lv_obj_set_style_bg_color(ok, KEY_COL, 0);
    lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ok, 1, 0);
    lv_obj_set_style_border_color(ok, MUT_COL, 0);
    lv_obj_add_flag(ok, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ok, wiped_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ol = lv_label_create(ok);
    lv_label_set_text(ol, tr(STR_C_OK));
    lv_obj_set_style_text_color(ol, INK_COL, 0);
    lv_obj_set_style_text_font(ol, wt_font14(), 0);
    lv_obj_set_style_text_letter_space(ol, 2, 0);
    lv_obj_center(ol);
}

// Replacing the seed abandons EVERY passphrase-wallet on the old one, so make
// it a deliberate two-tap: first tap arms + turns the pill red, second runs
// the wizard. Completing the wizard overwrites the seed; cancelling keeps it.
// CREATE NEW WALLET opens the wizard and nothing else: the mnemonic is staged
// in RAM and only reaches flash after the whole passphrase ritual, so backing
// out at any point leaves the existing wallet untouched. It used to arm like
// WIPE, which bought no safety and put two identical "tap again" gestures on
// one screen -- the dangerous one then looked routine.
static void replace_go_cb(lv_event_t *e)
{
    (void)e;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    wallet_begin_setup();
}

static void replace_cancel_cb(lv_event_t *e)
{
    lv_obj_t *ovl = lv_event_get_user_data(e);
    if (ovl) lv_obj_delete_async(ovl);
}

// CREATE NEW SEED walked straight into the wizard with nothing said. Finishing
// it replaces the seed on this device, and the owner was never told that before
// starting -- while WIPE, the other control that ends a wallet, gates itself.
//
// A tap-confirm, not a hold: nothing is destroyed here. The staged seed only
// reaches flash at wallet_seed_commit(), right at the end of the wizard, so
// this is a warning about where the next few minutes lead rather than a last
// chance before an erase.
static void replace_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *ovl = lv_obj_create(s_scr);
    lv_obj_remove_style_all(ovl);
    lv_obj_set_size(ovl, 800, 480);
    lv_obj_set_pos(ovl, 0, 0);
    lv_obj_set_style_bg_color(ovl, BG_COL, 0);
    lv_obj_set_style_bg_opa(ovl, LV_OPA_COVER, 0);
    lv_obj_add_flag(ovl, LV_OBJ_FLAG_CLICKABLE);      // swallow stray taps
    lv_obj_clear_flag(ovl, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(ovl);
    lv_label_set_text(t, tr(STR_G_REPLACEC_T));
    lv_obj_set_style_text_color(t, WARN_COL, 0);
    lv_obj_set_style_text_font(t, wt_font28(), 0);
    lv_obj_set_style_text_letter_space(t, 3, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 96);

    lv_obj_t *b = lv_label_create(ovl);
    lv_label_set_text(b, tr(STR_G_REPLACEC_B));
    lv_obj_set_style_text_color(b, MUT_COL, 0);
    lv_obj_set_style_text_font(b, wt_body_font(tr(STR_G_REPLACEC_B), 704, 190), 0);
    lv_obj_set_width(b, 704);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 160);

    wt_pill(ovl, tr(STR_G_REPLACEC_GO), 48, 372, 320, replace_go_cb, NULL);
    wt_pill(ovl, tr(STR_C_CANCEL), 585, 372, 165, replace_cancel_cb, ovl);
}

// thin wrappers over the wallet_theme kit (call sites keep their signatures)
// One tap on WIPE WALLET lands here. The destructive control is a HOLD, and it
// sits centre-screen, nowhere near the pill that was just tapped, so no amount
// of tapping in one place can reach it.
static void wipe_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *ovl = lv_obj_create(s_scr);
    lv_obj_remove_style_all(ovl);
    lv_obj_set_size(ovl, 800, 480);
    lv_obj_set_pos(ovl, 0, 0);
    lv_obj_set_style_bg_color(ovl, BG_COL, 0);
    lv_obj_set_style_bg_opa(ovl, LV_OPA_COVER, 0);
    lv_obj_add_flag(ovl, LV_OBJ_FLAG_CLICKABLE);      // swallow stray taps
    lv_obj_clear_flag(ovl, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(ovl);
    lv_label_set_text(t, tr(STR_G_WIPEC_T));
    lv_obj_set_style_text_color(t, STOP_COL, 0);
    lv_obj_set_style_text_font(t, wt_font28(), 0);
    lv_obj_set_style_text_letter_space(t, 3, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 96);

    lv_obj_t *b = lv_label_create(ovl);
    lv_label_set_text(b, tr(STR_G_WIPEC_B));
    lv_obj_set_style_text_color(b, MUT_COL, 0);
    lv_obj_set_style_text_font(b, wt_body_font(tr(STR_G_WIPEC_B), 704, 190), 0);
    lv_obj_set_width(b, 704);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 160);

    wt_hold_pill(ovl, tr(STR_G_HOLD_WIPE), 48, 372, 320, 52,
                 WIPE_HOLD_MS, do_wipe, ovl);
    // 165 wide, not 140: "ABBRUCH", "ANNULER" and "ANNULLA" are all already
    // the shortest correct word and still overran a 140px pill, so the pill
    // gives up the 25px instead of the copy giving up a letter. Left edge
    // moves to keep the right edge at 750 with every other pill on the row.
    wt_pill(ovl, tr(STR_C_CANCEL), 585, 372, 165, wipe_cancel_cb, ovl);
}

static lv_obj_t *mk_pillh(const char *txt, int x, int y, int w, int h, lv_event_cb_t cb, void *ud)
{
    return wt_pillh(s_scr, txt, x, y, w, h, cb, ud);
}

static lv_obj_t *mk_section(const char *txt, int x, int y)
{
    return wt_section(s_scr, txt, x, y);
}

// ---- language picker: full-screen overlay, every name in its own language
// (a user stuck in a language they can't read must still find the way back).
// Shared with the first-boot setup screen via wallet_lang_picker_open(). ----

int wallet_lang_pick_slot(int lang)
{
    for (int i = 0; i < I18N_LANG_N; i++)
        if (i18n_pick_order[i] == lang) return i;
    return 0;
}

static void (*s_lang_picked_cb)(void);

static void lang_pick_cb(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    i18n_set_lang(id);
    store_u8("lang", (uint8_t)id);
    void (*cb)(void) = s_lang_picked_cb;
    if (cb) cb();          // owner rebuilds its screen; overlay dies with it
}

static void settings_lang_picked(void)
{
    // rebuild the whole screen: unlike the accent, a language change has to
    // re-set every label's TEXT, and settings screens are create-on-open
    lv_obj_t *parent = s_parent;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    wallet_settings_open(parent);
    wallet_home_refresh();
}

static void lang_open_cb(lv_event_t *e)
{
    (void)e;
    wallet_lang_picker_open(s_scr, settings_lang_picked);
}

void wallet_lang_picker_open(lv_obj_t *parent, void (*picked_cb)(void))
{
    s_lang_picked_cb = picked_cb;
    lv_obj_t *ovl = lv_obj_create(parent);
    lv_obj_remove_style_all(ovl);
    lv_obj_set_size(ovl, 800, 480);
    lv_obj_set_pos(ovl, 0, 0);
    lv_obj_set_style_bg_color(ovl, BG_COL, 0);
    lv_obj_set_style_bg_opa(ovl, LV_OPA_COVER, 0);
    lv_obj_add_flag(ovl, LV_OBJ_FLAG_CLICKABLE);   // swallow stray taps
    lv_obj_clear_flag(ovl, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(ovl);
    lv_label_set_text(t, tr(STR_G_SEC_LANGUAGE));
    lv_obj_set_style_text_color(t, wt_accent(), 0);
    lv_obj_set_style_text_font(t, wt_font28(), 0);
    lv_obj_set_style_text_letter_space(t, 3, 0);
    lv_obj_set_pos(t, 48, 30);

    // 21 locales in 3x7. Fixed rows keep every language one tap away without
    // scrolling, while the compact labels still leave room for each flag.
    // i18n_pick_order (generated) = alphabetical display order, decoupled
    // from the append-only enum whose index is the stored NVS value.
    for (int i = 0; i < I18N_LANG_N; i++) {
        int id = i18n_pick_order[i];
        lv_obj_t *p = wt_pillh(ovl, i18n_lang_info(id)->native,
                               16 + (i % 3) * 260, 76 + (i / 3) * 52, 248, 44,
                               lang_pick_cb, (void *)(intptr_t)id);
        // Every row is in its own script. Select its regional font explicitly;
        // the current UI language must not control another locale's glyph form.
        lv_obj_t *name = lv_obj_get_child(p, 0);
        lv_obj_set_style_text_font(name, wt_font14_for_lang(id), 0);
        lv_obj_set_style_text_letter_space(name, 0, 0);
        if (img_lang_flags[id]) {             // en deliberately has no flag
            lv_obj_t *fl = lv_image_create(p);
            lv_image_set_src(fl, img_lang_flags[id]);
            lv_obj_align(fl, LV_ALIGN_LEFT_MID, 12, 0);
            lv_obj_remove_flag(fl, LV_OBJ_FLAG_CLICKABLE);  // the pill takes the tap
            lv_obj_align(name, LV_ALIGN_CENTER, 16, 0);
        }
        if (id == i18n_get_lang()) wt_pill_select(p, true);
    }
}

void wallet_settings_open(lv_obj_t *parent)
{
    if (s_scr) return;
    s_parent = parent;
    s_type_pill = s_type_pfx = s_type_expl = s_storage_pill = NULL;
    s_scr = wt_screen(parent, tr(STR_G_T), NULL);

    // THEME dots, top-right: tap a color, the wallet UI wears it everywhere
    for (int i = 0; i < WT_ACC_N; i++) {
        int save = wt_accent_get();
        wt_accent_set(i);                     // borrow the accent table for the dot fill
        lv_color_t c = wt_accent();
        wt_accent_set(save);
        lv_obj_t *d = lv_obj_create(s_scr);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 36, 36);
        lv_obj_set_pos(d, 560 + i * 48, 30);
        lv_obj_set_style_radius(d, 18, 0);
        lv_obj_set_style_bg_color(d, c, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(d, 1, 0);
        lv_obj_set_style_border_color(d, KEY_COL, 0);
        lv_obj_add_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(d, 6);
        lv_obj_add_event_cb(d, theme_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_acc_dot[i] = d;
    }
    // The name goes BESIDE the dots, not under them. Under them it landed on
    // y=74, the same baseline as the WALLET section header 130px to its left,
    // and two dim letter-spaced words on one line read as two section headers:
    // "MONO" looked like it was titling the wallet-actions column.
    s_acc_name = lv_label_create(s_scr);       // names the dressed color
    lv_obj_set_style_text_color(s_acc_name, MUT_COL, 0);
    lv_obj_set_style_text_font(s_acc_name, wt_font14(), 0);
    lv_obj_set_style_text_letter_space(s_acc_name, 2, 0);
    lv_obj_set_width(s_acc_name, 118);
    lv_obj_set_style_text_align(s_acc_name, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_acc_name, 430, 39);       // centred on the 36px dot row

    // LEFT: network + address type + ways in. The captions are small on
    // purpose; the vertical budget they give back is what lets every note under
    // a chooser render at a readable size instead of falling to font14.
    //
    // Five controls in the 74..WT_CONTENT_BOTTOM band, which is 324px. Stacked
    // the way they were, the last one ran to y=452 and WAYS IN was sliced in
    // half by the action row -- reported by sim/overlapcheck.c in all 21
    // locales, and visible on the device as a caption with its bottom missing.
    // Two things pay for the 54px that buys back:
    //
    // MAINNET and TESTNET share ONE row instead of stacking. They are two
    // values of one setting, and side by side is what that looks like; stacked
    // they read as two separate buttons, which is also why the selected one
    // needed a 2px border to say which was live. Both words are untranslated,
    // so 165px is generous for either at font23 in every locale.
    //
    // And the column starts at 68 rather than 74. This screen passes NULL for
    // the subtitle, so the y=96 content line the others build against does not
    // apply: the title's own box ends at 63.
    mk_section(tr(STR_I_SEC_NET), 48, 68);
    s_main_pill = mk_pillh("MAINNET", 48, 88, 165, 44, pick_cb, (void *)(intptr_t)0);
    s_test_pill = mk_pillh("TESTNET", 223, 88, 165, 44, pick_cb, (void *)(intptr_t)1);
    s_state_lbl = wt_note(s_scr, "", 48, 136, 340, NET_NOTE_H);  // filled by restyle()

    // The main page shows the selected type as one normal-size row. Tapping it
    // opens a dedicated full-width chooser where all three names and their
    // explanations fit at 23px.
    mk_section(tr(STR_I_SEC_TYPE), 48, 198);   // 4px below the note at 136+58
    // 72 tall, not 60: the example address is the second line and it is now a
    // readable 23 rather than a 14px footnote on its own button. 72 is the
    // smallest height that still leaves the type NAME at 23 above it.
    s_type_pill = mk_pillh(type_name(wallet_script()), 48, 218, 340, 72,
                           type_open_cb, NULL);
    wt_pill_two_line_val(s_type_pill,
                         type_prefix(wallet_script(), wallet_testnet()));
    s_type_pfx = lv_obj_get_child(s_type_pill, 1);
    s_type_expl = wt_note(s_scr, type_note(wallet_script()),
                          48, 293, 340, TYPE_NOTE_H);
    // Duress unlock (wallet_duress.h). ABSENT in a decoy session, not greyed
    // out: a disabled "ways in" row would tell whoever is holding the device
    // that a second signer exists, which is the one thing this must never do.
    // A decoy session sees exactly the page that shipped before this feature.
    //
    // Only hidden once something IS configured. Before that there is nothing to
    // hide, and hiding it from an owner whose only wallet has no passphrase
    // would mean they could never find the setting at all.
    //
    // Hidden ONLY when it would leak: a decoy session on a signer that really
    // has a stroke configured. A wallet with no passphrase also reports as the
    // decoy (both are the seed with an empty passphrase, and the device cannot
    // tell them apart) -- but with nothing configured there is nothing to leak,
    // and hiding the row there was a trap: every session on such a signer is
    // the decoy, so the owner could never reach the setting again. That is
    // exactly how a test device ended up stuck with a stroke it could not clear.
    //
    // This row was pulled out for one build while a device reported Settings
    // freezing on arrival, on the theory that a second 72px two-line pill was
    // the culprit. It was not: the screen never failed to RENDER, it failed to
    // hear a touch, because the decoy skipped the login screen and the login
    // screen was the only thing that created the LVGL indev (main.c,
    // wallet_start). Removing this changed nothing, which is what proved it.
    //
    // 326 + 72 lands the bottom edge on 397, one pixel clear of the action row.
    // That is the whole reason the four rows above it moved: this is the last
    // thing in the column and it is a 72px two-line pill, so everything else
    // had to fit in what was left rather than the other way round.
    // The hidden case leaves the slot empty. It used to carry a filler note
    // reading "each network + type is its own separate wallet", which was there
    // to stop the column ending on a gap. It told nobody anything they had not
    // worked out, and a line of text nobody can act on reads as a mistake. An
    // empty slot at the bottom of a column reads as the end of the column,
    // which is what the paragraph above wants: the page that shipped before
    // this feature existed.
    const int g = wallet_duress_real();
    if (!(wallet_session_decoy() && g != WDG_NONE)) {
        lv_obj_t *dp = mk_pillh(tr(STR_GD_SET_BTN), 48, 326, 340, 72, duress_cb, NULL);
        wt_pill_two_line_val(dp, g == WDG_NONE ? tr(STR_GD_OFF)
                                               : tr(wallet_duress_label_key(g)));
    }

    // RIGHT: the current storage mode is first and explicit. This is the only
    // setting that decides whether wallet material remains after power-off, so
    // it must not be hidden in setup or inferred from a note. Tapping opens the
    // three-mode chooser; the second line is the current mode.
    mk_section(tr(STR_I_T), 430, 74);
    s_storage_pill = mk_pillh(tr(STR_G_STORAGE_SEC), 430, 94, 340, 72,
                              storage_open_cb, NULL);
    wt_pill_two_line_val(s_storage_pill,
                         storage_mode_name(wallet_seed_mode()));
    wt_pill_select(s_storage_pill, true);

    // NO UNDO rule: one 1px WT_STOP hair with the label sitting on it, at
    // y=176, before the two destructive actions. Groups CREATE NEW and WIPE
    // together as the same class of thing. The rule is why the wipe note is
    // gone from y=366: the two-word label under a red hairline says the same
    // caution in the space a caption used, and confirmations still explain
    // both actions in full before either runs.
    {
        lv_obj_t *rule = lv_obj_create(s_scr);
        lv_obj_remove_style_all(rule);
        lv_obj_set_pos(rule, 430, 186);
        lv_obj_set_size(rule, 340, 1);
        lv_obj_set_style_bg_color(rule, WT_STOP, 0);
        lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
        lv_obj_t *lbl = lv_label_create(s_scr);
        lv_label_set_text(lbl, tr(STR_I_SEC_NO_UNDO));
        lv_obj_set_style_text_font(lbl, wt_font14(), 0);
        lv_obj_set_style_text_color(lbl, WT_STOP, 0);
        lv_obj_set_style_text_letter_space(lbl, 2, 0);
        lv_obj_set_style_bg_color(lbl, WT_BG, 0);       // knock out the rule
        lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(lbl, 6, 0);
        lv_obj_set_pos(lbl, 436, 176);                  // slight inset from rule end
    }

    // Under the rule: CREATE NEW at 200, WIPE joins it at 256. Both are the
    // destructive actions; the rule above says why they belong together.
    s_replace_pill = mk_pillh(tr(STR_G_CREATE_NEW), 430, 200, 340, 52,
                              replace_cb, NULL);

    // wipe: seed off the device entirely (back to just a game). Red text so it
    // reads as destructive before it's ever tapped; a hold on the next screen
    // is what actually erases.
    s_wipe_pill = mk_pillh(tr(STR_G_WIPE), 430, 256, 340, 52, wipe_cb, NULL);
    lv_obj_set_style_text_color(lv_obj_get_child(s_wipe_pill, 0), STOP_COL, 0);

    // RECOVERY WORDS: the row moves DOWN and grows to 72 tall + two lines, so
    // the second line can carry whether the paper backup was ever rehearsed
    // on this device. The redraw that spawned this asked for a separate
    // "YOUR BACKUP" column; the columns are full to the pixel and the state
    // belongs on the row about the words anyway. 312 + 72 = 384, 14px above
    // the 398 floor.
    {
        lv_obj_t *pill = mk_pillh(tr(STR_I_WORDS_BTN), 430, 312, 340, 72,
                                  words_cb, NULL);
        bool ok = wallet_ui_backup_verified();
        char buf[160];
        if (ok) {
            uint8_t fp[4]; wallet_ui_last_fp(fp);
            char idstr[16];
            snprintf(idstr, sizeof idstr, "%02X%02X%02X%02X",
                     fp[0], fp[1], fp[2], fp[3]);
            int p = snprintf(buf, sizeof buf, "%s  ", LV_SYMBOL_OK);
            snprintf(buf + p, sizeof buf - p,
                     tr(STR_I_WORDS_VERIFIED_FMT), idstr);
        } else {
            snprintf(buf, sizeof buf, "%s  %s",
                     LV_SYMBOL_WARNING, tr(STR_I_WORDS_UNVERIFIED));
        }
        wt_pill_two_line_val(pill, buf);
        // A colour cue AND a glyph, per ADDENDUM-02: in GREEN theme the
        // accent is byte identical to WT_OK, so colour alone stops carrying
        // meaning. The prefix stays regardless: ✓ for verified, ▲ for the
        // never-checked warning.
        lv_obj_t *sub = lv_obj_get_child(pill, 1);
        if (sub) {
            lv_obj_set_style_text_color(sub, ok ? WT_OK : WT_WARN, 0);
        }
    }

    // LANGUAGE: the current language on the pill; opens the picker. The pill is
    // narrow, so strip the regional qualifier ("ESPAÑOL (ESPAÑA)" -> "ESPAÑOL")
    // and let the flag carry the variant instead.
    {
        int li = i18n_get_lang();
        const char *nat = i18n_lang_info(li)->native;
        if (li == I18N_NB) nat = "BOKMÅL";  // flag already identifies Norway
        const char *par = strstr(nat, " (");
        char shortname[24];
        size_t n = par ? (size_t)(par - nat) : strlen(nat);
        if (n >= sizeof shortname) n = sizeof shortname - 1;
        memcpy(shortname, nat, n);
        shortname[n] = 0;
        // 170, up from 160: this pill carries a flag AND a native name, and the
        // flag's width used to come straight out of the name's. BACK keeps its
        // 140 -- the extra comes from the gap between them.
#define LANG_PILL_W 170
        s_lang_pill = mk_pillh(shortname, 430, WT_ACTION_Y, LANG_PILL_W, 44, lang_open_cb, NULL);
    }

    // build identity, bottom edge (below the pill row; bottom has no overscan)
    // 418, not 460. Two rows now, centred in the action bar's empty left
    // half (x 48..263 against ENGLISH starting at 430), instead of a single
    // panel-wide line squeezed into the 24px strip below the buttons.
    s_build_id = wallet_build_id_make(s_scr, 48, 418, true, true);  // radio readback lives here

    {
        // BACK sits in the bottom-right corner, where a thumb arrives at an
        // angle and lands short. 10px of ext click area turns a 140x44 pill
        // into a 160x64 target without moving a pixel of what is drawn. Not
        // more than 10: the language pill's right edge is at x=600, and a
        // wider reach would start eating taps meant for it.
        lv_obj_t *back = mk_pillh(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, 44, close_cb, NULL);
        lv_obj_set_ext_click_area(back, 10);
        lv_obj_t *row[2] = { s_lang_pill, back };
        wt_pill_row(row, 2);
    }

    // NO FLAG ON THIS PILL. It used to carry the active language's flag, which
    // meant SETTINGS displayed a foreign country's flag permanently next to
    // BACK for twenty of the twenty-one locales -- a national flag as fixed
    // furniture on a Bitcoin signer, standing in for nothing the user needs.
    // The native name already says which language is active, and it is the
    // honest label: a language is not a country. Flags stay in the PICKER, one
    // per row, where they genuinely help scan twenty-one options and there is
    // width to spare.
    //
    // Bonus: the name now gets the pill's whole width, so the clipping this
    // block existed to work around ("TIENG VIET" rendering as "ENG VIET")
    // cannot happen at all.
    restyle();
}
