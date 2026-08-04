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
static lv_obj_t *s_main_pill, *s_test_pill;   // the two network segments
static lv_obj_t *s_state_lbl;                // the network row's sub-line
static lv_obj_t *s_replace_pill;
static lv_obj_t *s_build_id;
static lv_obj_t *s_wipe_pill;
static lv_obj_t *s_lang_pill;   // paired with BACK so the bottom row matches
static lv_obj_t *s_type_pill, *s_type_pfx, *s_type_expl, *s_type_name;
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
    lv_obj_set_style_text_color(wt_screen_title(s_scr), wt_accent(), 0);
    wallet_build_id_restyle(s_build_id);
    for (int i = 0; i < WT_ACC_N; i++)
        if (s_acc_dot[i]) {
            bool on = (i == wt_accent_get());
            lv_obj_set_style_border_color(s_acc_dot[i], on ? wt_accent() : KEY_COL, 0);
            // Scaled for the 18px dot in the action bar. At the old 36px these
            // were 3/12/3; kept at that size on an 18px dot the halo is wider
            // than the dot and the four of them merge into one bright smear.
            lv_obj_set_style_border_width(s_acc_dot[i], on ? 2 : 1, 0);
            lv_obj_set_style_shadow_width(s_acc_dot[i], on ? 7 : 0, 0);
            lv_obj_set_style_shadow_color(s_acc_dot[i], wt_accent(), 0);
            lv_obj_set_style_shadow_opa(s_acc_dot[i], on ? 90 : 0, 0);
            // detached ink halo: the gap reads even when the dot is white (MONO)
            lv_obj_set_style_outline_width(s_acc_dot[i], on ? 2 : 0, 0);
            lv_obj_set_style_outline_pad(s_acc_dot[i], 2, 0);
            lv_obj_set_style_outline_color(s_acc_dot[i], INK_COL, 0);
        }
    if (s_acc_name) lv_label_set_text(s_acc_name, wt_accent_name());
    // The network row's value: the live network name, amber on testnet. That
    // colour is the whole warning now that the two pills are gone, and it is
    // paired with the sub-line below so the state never rests on colour alone.
    // The segmented control: the live side is a filled lozenge, the other is
    // just text on the track. MAINNET fills WT_INK, as drawn; TESTNET fills
    // WT_WARN instead, because amber is what this app has always used to say
    // "these coins are not real" and the fill is now the loudest place to say
    // it. Both take dark ink on the fill -- WT_INK text on WT_WARN is the one
    // pairing here with no contrast.
    for (int i = 0; i < 2; i++) {
        lv_obj_t *s = i ? s_test_pill : s_main_pill;
        if (!s) continue;
        bool on = (i == 1) == (tn != 0);
        lv_obj_set_style_bg_color(s, i ? WARN_COL : INK_COL, 0);
        lv_obj_set_style_bg_opa(s, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(s, on ? WT_BAR : MUT_COL, 0);
    }
    // The network note is a plain row sub-line now, not an auto-fitting note
    // block: the row owns the width and the type size, so this only sets text
    // and colour.
    if (s_state_lbl) {
        lv_label_set_text(s_state_lbl, tn ? tr(STR_G_TESTNET_NOTE)
                                          : tr(STR_G_MAINNET_NOTE));
        lv_obj_set_style_text_color(s_state_lbl, tn ? WARN_COL : MUT_COL, 0);
    }

    // The address type row carries the address PREFIX as its right-hand value
    // and the type name as its sub-line. Both are rebuilt by walking the row's
    // children rather than by index, because wt_row adds the chevron first and a
    // row without a sub-line has one child fewer.
    if (s_type_pill) {
        int sc = wallet_script();
        // The sub-line is left aligned, so it only needs its text.
        if (s_type_name) lv_label_set_text(s_type_name, type_name(sc));
        // The value is right aligned and wt_row placed it by its old width, so
        // re-pin its right edge after the text changes length.
        if (s_type_pfx) {
            int oldw = lv_obj_get_width(s_type_pfx);
            int oldx = lv_obj_get_x(s_type_pfx);
            lv_label_set_text(s_type_pfx, type_prefix(sc, tn));
            lv_obj_update_layout(s_type_pfx);
            lv_obj_set_x(s_type_pfx, oldx + oldw - lv_obj_get_width(s_type_pfx));
        }
    }
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
    lv_obj_set_style_text_color(wt_screen_title(s_scr), title_col, 0);
    // The rule colour carries the outcome, so the block agrees with the title
    // above it instead of being grey under a green or red heading.
    wt_why_body(s_scr, body, 136, title_col, true);
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
    wt_why_body(s_scr, body, 126,
                target == WSEED_MODE_AMNESIC ? WARN_COL : wt_accent(), true);

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
    // One ROW per mode, on the grid the cards used to sit on. This was a card
    // with a pill floating inside it and the mode's explanation orbiting to the
    // right: three buttons for a question that is not "press one of these" but
    // "which of these is on". A row answers that with a tick and no word at all,
    // in every locale at once, and it puts the explanation INSIDE the option it
    // belongs to instead of beside it.
    //
    // 96 tall, which is the same box the note had before, so its three line
    // budget at font23 survives intact -- Turkish, Portuguese and Russian all
    // need those three lines and none of them re-wrap.
    //
    // The icons say the same three things the words do, one glance sooner: a
    // floppy for the copy that stays on the chip, a card for the copy that
    // leaves with you, and the incognito hat for the mode that keeps nothing.
    static const char *const ICON[3] = {
        LV_SYMBOL_SAVE, WT_ICON_SD, WT_ICON_SECRET
    };
    for (int i = 0; i < 3; i++) {
        int mode = modes[i];
        lv_obj_t *row = wt_row_x(s_scr, ICON[i], storage_mode_name(mode),
                                 storage_mode_note(mode), NULL, NULL, NULL,
                                 WT_INK, current == mode,
                                 WT_CHOICE_X, WT_CHOICE_Y(i), WT_CHOICE_W,
                                 WT_CHOICE_H, storage_pick_cb,
                                 (void *)(intptr_t)mode);
        // Per ADDENDUM-02 style rule and HANDOFF-04's storage residual: the
        // FLASH note is a caution when the chip reports encryption OFF, not a
        // footnote. WT_WARN, not the default WT_MUT.  "your recovery words are
        // saved here unencrypted" needs to READ as a warning; drawing it in
        // muted grey was the review's original complaint on this row.
        //
        // It is the SUB that is tinted, not the row: wt_row_sev would wash the
        // whole card amber, and a permanently amber option in a list of three
        // reads as broken rather than as cautioned.
        if (mode == WSEED_MODE_KEEP && !wallet_seed_flash_encrypted())
            wt_row_sub_color(row, WT_WARN);
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
    // One row per type, on the same WT_CHOICE grid the storage chooser uses.
    // These two screens are reached from adjacent rows of Settings and ask the
    // same shape of question, so they are drawn the same way, and now they are
    // literally the same object: a tick on the live one, the tradeoff on the
    // sub-line, the address PREFIX as the row's value.
    //
    // The prefix moves out of the label and into the value slot, which is what
    // it always was. "Native SegWit   bc1..." was one string doing two jobs and
    // it ellipsised to "Native SegWit   bc1..." losing the prefix -- the half a
    // reader actually matches against what their coordinator shows.
    //
    // DIRECTORY on every row, because what a type really selects is a derivation
    // branch. One mark repeated says "these three are the same kind of thing",
    // which is exactly true here and is not true of the storage modes.
    int tn = wallet_testnet();
    for (int i = 0; i < 3; i++) {
        int sc = scripts[i];
        wt_row_x(s_scr, LV_SYMBOL_DIRECTORY, type_name(sc), type_note(sc), NULL,
                 type_prefix(sc, tn), wt_font_mono23(), WT_MUT,
                 wallet_script() == sc, WT_CHOICE_X, WT_CHOICE_Y(i),
                 WT_CHOICE_W, WT_CHOICE_H, type_pick_cb, (void *)(intptr_t)sc);
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
        lv_obj_set_style_radius(ok, 10, 0);   // wt_pillh's radius: this is a button
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
    lv_obj_set_style_radius(ok, 10, 0);   // wt_pillh's radius: this is a button
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

    // The top right belongs to the LANGUAGE pill now; see the block that builds
    // it below. It held four 36px theme dots once, which was the largest,
    // brightest, most saturated thing on the page handed to the one control that
    // changes nothing about the wallet, and then briefly the wallet fingerprint,
    // which moved to the home screen with every other copy of it.

    // THEME, in the action bar: an 11px eyebrow and four 18px dots. Same four
    // accents, same picker callback, a third the diameter of the originals.
    // The whole block sits 40px left of where the drawing put it, because BACK
    // has taken the right corner: the fourth dot's 15px ext click area used to
    // reach 624 and BACK's pill starts at 610, so they would have been fighting
    // over the same taps. Ending the dots at 557 leaves 53px of daylight.
    s_acc_name = lv_label_create(s_scr);       // names the dressed colour
    lv_obj_set_style_text_color(s_acc_name, MUT_COL, 0);
    lv_obj_set_style_text_font(s_acc_name, wt_font14(), 0);
    lv_obj_set_style_text_letter_space(s_acc_name, 2, 0);
    // RIGHT aligned, ending 10px short of the first dot at x=470. Not placed by
    // its left edge: this label is the live accent NAME, so it is "MONO" in one
    // theme and "CYPHERPINK" in another, and a fixed left edge put the long one
    // straight through the dots. The text overlap gate cannot catch that -- a
    // dot is not text -- so the geometry has to make it impossible instead.
    lv_obj_set_width(s_acc_name, 120);
    lv_obj_set_style_text_align(s_acc_name, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_acc_name, 340, WT_ACTION_Y + 8);
    for (int i = 0; i < WT_ACC_N; i++) {
        int save = wt_accent_get();
        wt_accent_set(i);                     // borrow the accent table for the dot fill
        lv_color_t c = wt_accent();
        wt_accent_set(save);
        lv_obj_t *d = lv_obj_create(s_scr);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 18, 18);
        lv_obj_set_pos(d, 470 + i * 27, WT_ACTION_Y + 13);
        lv_obj_set_style_radius(d, 9, 0);
        lv_obj_set_style_bg_color(d, c, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(d, 1, 0);
        lv_obj_set_style_border_color(d, KEY_COL, 0);
        lv_obj_add_flag(d, LV_OBJ_FLAG_CLICKABLE);
        // The dot shrank from 36 to 18, so the TOUCH target has to grow to keep
        // it tappable: 15px of ext area gives each one a 48x48 region, and the
        // 27px pitch means those regions tile without overlapping a neighbour.
        lv_obj_set_ext_click_area(d, 15);
        lv_obj_add_event_cb(d, theme_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_acc_dot[i] = d;
    }

    // Redraw 05: two columns of CARDS under section eyebrows, not a grid of
    // pills. Every number here is read off the drawing's own DOM rather than
    // eyeballed: both columns are 365 wide, the left at x=25 and the right at
    // x=412, cards 56..60 tall on a 64..67 pitch, first card at y=95 under an
    // eyebrow at y=73.
    //
    // Ours are 64 tall on a 71 pitch, which is the same shape carrying the type
    // this device actually has. The drawing's 56 fits a 15px label over a 12px
    // sub; at the 23 and 14 the wallet uses, the same two lines need 64. Keeping
    // the drawn card height instead would have meant shrinking the type, which
    // was tried, flashed, and rejected on glass for being unreadable.
    //
    // The pitch is what has to be checked against WT_CONTENT_BOTTOM, not the
    // height: the right column is the tall one, because it carries two cards, a
    // second eyebrow and two more cards. 95 + 64 + 7 + 64 + 8 + 22 + 64 + 7 + 64
    // lands its last card's bottom edge on 395, three pixels clear of 398.
    //
    // Why this shape rather than the pills it replaces. Eleven pills gave every
    // control the same visual weight and the same answer to "what is this
    // setting currently?", which was a second line of small type inside a
    // button. A row states the setting on the left, its value on the right and
    // the consequence underneath, so the page can be read down the value column
    // alone. The groups are what make the destructive pair legible as a class
    // instead of two more buttons in a stack.
    //
    // MAINNET and TESTNET stay a real pair of pills: they are two values of one
    // setting and the only control here where the choice itself is the widget.
#define SG_L_X    25
#define SG_L_W   365
#define SG_R_X   412
#define SG_R_W   365
#define SG_TOP    72
#define SG_HEAD  23    // eyebrow at SG_TOP -> first card at 95, as drawn
#define SG_PITCH 71    // 64 tall card + 7 gap
    wt_row_head(s_scr, tr(STR_I_SEC_THIS_WALLET), SG_L_X, SG_TOP, SG_L_W);

    // Network: the one row on this page whose control IS the choice, so redraw 05
    // draws a SEGMENTED control instead of a value plus a chevron -- a bordered
    // track holding two lozenges, the live one filled. There is no chevron on
    // this row in the drawing either, and that is the point: you do not navigate
    // into a two-state setting, you set it where it is stated.
    //
    // The pair of full pills this replaces failed for a measurable reason: at
    // font23 each needed 165px, which inside a row left the label 20px, and
    // squeezed to 88 both words wrapped mid-syllable. At font14 a segment sets
    // in 84, so the whole track is 176. That is the only thing that changed --
    // the type, not the idea.
    //
    // 177 rather than the drawing's 162, which costs 15px of fidelity nobody can
    // see and buys the label 155px instead of 140. The overlap gate compares
    // BOXES: wt_row sizes the label against the chevron, this row has none, so
    // the label's box ran the whole width of the card and CONTAINED both
    // lozenges -- two findings in all 21 locales. Both text lines have to be
    // capped short of the track, and 155 is what keeps the longest translations
    // of "Network" off the ellipsis.
#define SG_SEG_X 177
#define SG_SEG_TEXT_W (SG_SEG_X - 14 - 8)
    {
        int y = SG_TOP + SG_HEAD;
        int tn0 = wallet_testnet();
        // No value and no callback: the segments carry both. wt_row still lays
        // out the label and the sub-line, and only reserves the sub-line when it
        // is given one, so the note text goes in here rather than being added
        // afterwards (doing that put the note on top of the label).
        lv_obj_t *row = wt_row(s_scr, tr(STR_I_ROW_NETWORK),
                               tn0 ? tr(STR_G_TESTNET_NOTE) : tr(STR_G_MAINNET_NOTE),
                               NULL, WT_INK, SG_L_X, y, SG_L_W, NULL, NULL);
        s_state_lbl = NULL;

        // The track. 162 from the card's left edge puts it at x=187 absolute,
        // which is where the drawing has it, and 34 tall centred in a 64 card.
        lv_obj_t *seg = lv_obj_create(row);
        lv_obj_remove_style_all(seg);
        lv_obj_set_pos(seg, SG_SEG_X, (WT_ROW_H - 34) / 2);
        lv_obj_set_size(seg, 176, 34);
        lv_obj_set_style_radius(seg, 100, 0);
        lv_obj_set_style_border_width(seg, 1, 0);
        lv_obj_set_style_border_color(seg, WT_EDGE, 0);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_CLICKABLE);

        // Two lozenges inset 3 inside it, 90 wide each. Each one is a label
        // carrying its own box, the same trick wt_state_chip uses: a container
        // plus a child needs two layout passes to measure, and LVGL labels take
        // background and radius styles perfectly well on their own.
        static const char *const seg_txt[2] = { "MAINNET", "TESTNET" };
        lv_obj_t **slot[2] = { &s_main_pill, &s_test_pill };
        for (int i = 0; i < 2; i++) {
            lv_obj_t *s = lv_label_create(seg);
            lv_label_set_text(s, seg_txt[i]);
            lv_obj_set_pos(s, 3 + i * 86, 3);
            lv_obj_set_size(s, 84, 28);
            lv_obj_set_style_radius(s, 100, 0);
            lv_obj_set_style_text_font(s, wt_font14(), 0);
            lv_obj_set_style_text_letter_space(s, 1, 0);
            lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
            // The glyphs are 14px in a 28px box, so the text has to be pushed
            // down to sit on the lozenge's centre line rather than its top.
            lv_obj_set_style_pad_top(s, 5, 0);
            lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
            // 10px reach in every direction: an 84x28 lozenge is a 104x48
            // target, and the two cannot steal from each other because the reach
            // is smaller than half the 86px pitch between them.
            lv_obj_set_ext_click_area(s, 10);
            lv_obj_add_event_cb(s, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            *slot[i] = s;
        }

        // BOTH text lines stop short of the track, the label as well as the note.
        // wt_row sizes them against the chevron, and this row has none, so each
        // box ran the full width of the card and CONTAINED both lozenges. The
        // lozenges are text, so the overlap gate called it -- correctly -- in
        // every locale. Only the row's own two labels are touched here; the
        // segments live inside `seg`, not in this child list.
        uint32_t n = lv_obj_get_child_count(row);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t *c = lv_obj_get_child(row, i);
            if (!lv_obj_check_type(c, &lv_label_class)) continue;
            lv_obj_set_width(c, SG_SEG_TEXT_W);
            lv_label_set_long_mode(c, LV_LABEL_LONG_DOT);
            if (lv_obj_get_style_text_font(c, 0) == wt_font14()) s_state_lbl = c;
        }
    }

    // Address type: the ADDRESS PREFIX is the value, the type name is the
    // sub-line. That is the way round it has to be, not a preference.
    //
    // wt_row_x sizes the label to the width the value leaves it, and the label
    // is pinned to one ellipsised line. With the translated "Native SegWit" on
    // the right, "Address type" ran out of room in ENGLISH -- it drew as
    // "Address t..." -- and every locale's budget moved with its own
    // translation of the type name. "bc1..." is never translated and is the
    // width of "FLASH" next door, so the label now gets ~260px in all 21
    // locales at once. It is also the half an owner recognises: bc1 is what
    // they see in their wallet, Native SegWit is the name for it.
    s_type_pill = wt_row(s_scr, tr(STR_I_ROW_TYPE),
                         type_name(wallet_script()),
                         type_prefix(wallet_script(), wallet_testnet()), WT_INK,
                         SG_L_X, SG_TOP + SG_HEAD + SG_PITCH, SG_L_W,
                         type_open_cb, NULL);
    s_type_expl = NULL;                // the explanation lives on the chooser
    // Capture the two labels restyle has to rewrite, rather than letting it
    // guess by x. Guessing matched the CHEVRON too, so the arrow's glyph was
    // replaced by the type name and the row grew a ghost second value.
    s_type_pfx = NULL;
    s_type_name = NULL;
    {
        uint32_t n = lv_obj_get_child_count(s_type_pill);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t *c = lv_obj_get_child(s_type_pill, i);
            if (!lv_obj_check_type(c, &lv_label_class)) continue;
            const char *t = lv_label_get_text(c);
            if (!t) continue;
            if (!strcmp(t, type_prefix(wallet_script(), wallet_testnet())))
                s_type_pfx = c;
            else if (!strcmp(t, type_name(wallet_script())))
                s_type_name = c;
        }
    }
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
    // Storage joins the left column, third row: it belongs with the facts about
    // THIS WALLET rather than opening the right column, and the drawing puts it
    // there. Its sub-line is the honest flash-encryption state.
    s_storage_pill = wt_row(s_scr, tr(STR_I_ROW_STORAGE),
                            wallet_seed_mode() == WSEED_MODE_KEEP &&
                            !wallet_seed_flash_encrypted()
                                ? tr(STR_W_FLASH_PLAIN_NOTE_SHORT) : "",
                            storage_mode_name(wallet_seed_mode()), WT_INK,
                            SG_L_X, SG_TOP + SG_HEAD + 2 * SG_PITCH, SG_L_W,
                            storage_open_cb, NULL);
    // Amber CARD, not just an amber note. The drawing tints this whole box when
    // the words sit in a flash this build does not encrypt, which is the one
    // fact on the page a holder should catch without reading anything.
    if (wallet_seed_mode() == WSEED_MODE_KEEP && !wallet_seed_flash_encrypted())
        wt_row_sev(s_storage_pill, WT_SEV_WARN);

    const int g = wallet_duress_real();
    if (!(wallet_session_decoy() && g != WDG_NONE)) {
        wt_row(s_scr, tr(STR_I_ROW_DURESS), tr(STR_GD_SET_SUB),
               g == WDG_NONE ? tr(STR_GD_OFF) : tr(wallet_duress_label_key(g)),
               WT_INK, SG_L_X, SG_TOP + SG_HEAD + 3 * SG_PITCH, SG_L_W,
               duress_cb, NULL);
    }

    // RIGHT COLUMN, group one: the backup. Redraw 05 gives this its own eyebrow
    // rather than leaving the words row adrift among the destructive buttons,
    // and that separation is the point: reading your words and destroying them
    // are opposite intentions that used to sit in one stack.
    wt_row_head(s_scr, tr(STR_I_SEC_YOUR_BACKUP), SG_R_X, SG_TOP, SG_R_W);
    wt_row(s_scr, tr(STR_I_ROW_WORDS), tr(STR_I_WORDS_SUB), NULL, WT_INK,
           SG_R_X, SG_TOP + SG_HEAD, SG_R_W, words_cb, NULL);
    {
        // Paper checked: its own row, because whether the paper was ever proven
        // against this device is a fact about the backup, not a footnote on the
        // button that shows the words.
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
        lv_obj_t *r = wt_row(s_scr, tr(STR_I_ROW_PAPER), buf, NULL,
                             WT_INK, SG_R_X, SG_TOP + SG_HEAD + SG_PITCH, SG_R_W,
                             words_cb, NULL);
        // A colour cue AND a glyph, per ADDENDUM-02: in GREEN theme the accent
        // is byte identical to WT_OK, so colour alone stops carrying meaning.
        // The sub-line is the row's third child (label, sub, chevron order
        // varies, so find it by walking rather than by index).
        uint32_t n = lv_obj_get_child_count(r);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t *c = lv_obj_get_child(r, i);
            if (lv_obj_get_style_text_color(c, 0).blue == WT_MUT.blue &&
                lv_obj_get_y(c) > 24)
                lv_obj_set_style_text_color(c, ok ? WT_OK : WT_WARN, 0);
        }
        // and the card itself, which is how the drawing states it: green once
        // the paper has been proven against this device, amber until it has.
        wt_row_sev(r, ok ? WT_SEV_OK : WT_SEV_WARN);
    }

    // Group two: the destructive pair, under a WT_STOP eyebrow with a WT_STOP
    // rule running out to the right of it. The rule was taken out once, on the
    // argument that a horizontal line immediately above a row's own separator
    // reads as a rendering fault. That reasoning died with the separators: the
    // rows are cards now, so the only line in this region is the rule itself,
    // and the drawing has it -- 284x1 at 25 percent, starting after the label.
    {
        // No gap above the eyebrow. 72 + 23 + 2*71 is already 237, which clears
        // the card above it (bottom edge 230) and still leaves the second card
        // of THIS group ending on 395, inside WT_CONTENT_BOTTOM.
        int y = SG_TOP + SG_HEAD + 2 * SG_PITCH;
        lv_obj_t *h = wt_row_head(s_scr, tr(STR_I_SEC_NO_UNDO), SG_R_X, y, SG_R_W);
        lv_obj_set_style_text_color(h, STOP_COL, 0);
        // The rule starts one em past the WORDS and runs to the column's right
        // edge, so a longer translation simply shortens it instead of striking
        // through itself. Below 40px it is not a rule any more, so it goes.
        //
        // Measure the TEXT, not the label. wt_row_head sets the eyebrow's width
        // to the whole column so a long translation ellipsises, so asking the
        // object how wide it is answers 365 and puts the rule off the screen --
        // which is exactly how the first attempt drew no rule at all.
        lv_point_t hs;
        lv_text_get_size(&hs, tr(STR_I_SEC_NO_UNDO), wt_font14(), 2, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int rx = SG_R_X + hs.x + 10;
        lv_obj_t *rule = lv_obj_create(s_scr);
        lv_obj_remove_style_all(rule);
        lv_obj_set_pos(rule, rx, y + 6);
        lv_obj_set_size(rule, SG_R_X + SG_R_W - rx, 1);
        lv_obj_set_style_bg_color(rule, STOP_COL, 0);
        lv_obj_set_style_bg_opa(rule, 64, 0);      // the drawing's 25 percent
        lv_obj_remove_flag(rule, LV_OBJ_FLAG_CLICKABLE);
        if (SG_R_X + SG_R_W - rx < 40) lv_obj_delete(rule);

        s_replace_pill = wt_row(s_scr, tr(STR_I_ROW_REPLACE),
                                tr(STR_I_ROW_REPLACE_SUB), NULL, WT_INK,
                                SG_R_X, y + SG_HEAD, SG_R_W, replace_cb, NULL);
        s_wipe_pill = wt_row(s_scr, tr(STR_I_ROW_ERASE), tr(STR_I_ROW_ERASE_SUB), NULL,
                             WT_INK, SG_R_X, y + SG_HEAD + SG_PITCH, SG_R_W,
                             wipe_cb, NULL);
        // Red CARDS and red LABELS, so both read as destructive before either is
        // tapped; a hold on the confirmation is what actually erases. The label
        // takes WT_STOP_INK rather than WT_STOP: full stop red on a stop-tinted
        // card is the one pairing on this page that vibrates.
        lv_obj_t *pair[2] = { s_replace_pill, s_wipe_pill };
        for (int p = 0; p < 2; p++) {
            wt_row_sev(pair[p], WT_SEV_STOP);
            uint32_t n = lv_obj_get_child_count(pair[p]);
            for (uint32_t i = 0; i < n; i++) {
                lv_obj_t *c = lv_obj_get_child(pair[p], i);
                if (lv_obj_get_y(c) < 24 && lv_obj_get_x(c) < 20)
                    lv_obj_set_style_text_color(c, WT_STOP_INK, 0);
            }
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
        // flag's width used to come straight out of the name's.
#define LANG_PILL_W 170
        // TOP right, on the page's own 752 margin rather than the action bar's
        // 779. It sat bottom right until BACK came for that corner, and up here
        // it is the better neighbour anyway: it is the only control on this
        // screen that changes how everything else READS, which is a header job.
        //
        // y=18 matters twice. It lines the pill up with the title's box, and it
        // is above WT_CONTENT_BOTTOM, so wt_pillh does NOT build an action bar
        // for it -- the bar is BACK's now.
        s_lang_pill = mk_pillh(shortname, 752 - LANG_PILL_W, 18,
                               LANG_PILL_W, 44, lang_open_cb, NULL);
        wt_pill_row(&s_lang_pill, 1);
        // The title had the whole 704 lane and now shares it with a 170px pill.
        // Nothing else would catch this: the overlap gate measures text against
        // text, a pill is not text, and a long locale's title would simply run
        // underneath it. 518 = 704 - 170 - 16 of gap.
        wt_title_fit(s_scr, 752 - LANG_PILL_W - 16 - 48);
    }

    {
        // BACK takes the bottom RIGHT corner, at WT_BACK_X, in the standard
        // 140x52 pill every other lone-BACK screen uses. It spent a while in the
        // left corner on the reasoning that redraws 01, 02, 03 and 05 put the
        // escape leftmost -- but that rule is about a bar that holds SEVERAL
        // pills, where the far right is reserved for the control doing the
        // screen's work. Sign and Receive are those screens and keep it. This
        // bar has one pill in it, so there is no consequential control for the
        // corner to protect, and matching WALLET is worth more than matching a
        // rule whose condition is absent.
        wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140,
                close_cb, NULL);

        // Build identity AFTER the pill, and that order is load bearing. The
        // action bar is built lazily by the first wt_pill on the screen; the
        // language pill used to be that pill and used to come first, so this
        // line landed on top of the bar by accident of sequence. The pill went
        // to the top of the screen, BACK became the bar's builder, and the
        // version vanished behind it. Raising it afterwards is not enough
        // either: wallet_build_id_make creates THREE sibling labels and hands
        // back only the first, so a move_foreground on the return value lifts
        // the version and leaves encryption and radio buried. Building it last
        // is the fix that cannot half work.
        //
        // x=48, the page margin. It sat at 150 only because BACK held the left
        // corner and 48 was inside it. Three rows from 48 run to about 275, and
        // the THEME eyebrow does not start until 340.
        //
        // y=404, the top of the bar, up from 418: the block is three rows now
        // (see wallet_build_id_make) and 418 put the last one at 462..480, on
        // the screen edge and below the bar's own fill, which stops at 471.
        // From 404 the rows are 404, 426 and 448, all of them inside it.
        s_build_id = wallet_build_id_make(s_scr, 48, 404, true, true);

        // The THEME control has to be raised above the action bar's floor for
        // the same reason: the label and all four dots are created before any
        // pill exists, so the bar drew straight over the top of them. Nothing
        // errored, because they were still there and still tappable, just
        // hidden.
        lv_obj_move_foreground(s_acc_name);
        for (int i = 0; i < WT_ACC_N; i++)
            if (s_acc_dot[i]) lv_obj_move_foreground(s_acc_dot[i]);
    }

    // NO FLAG ON THIS PILL. It used to carry the active language's flag, which
    // meant SETTINGS displayed a foreign country's flag permanently, for twenty
    // of the twenty-one locales -- a national flag as fixed furniture on a
    // Bitcoin signer, standing in for nothing the user needs.
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
