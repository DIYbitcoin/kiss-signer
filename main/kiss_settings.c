// Settings: network selection. Style matches the other wallet screens.
// The network flips instantly (the master key is network-free) and persists
// across power cycles via NVS on the device.
#include "kiss_settings.h"

#include <stdio.h>
#include <string.h>

#include "flag_imgs.h"   // language-picker flags (Twemoji, CC-BY; en has none)
#include "i18n.h"
#include "kiss_crypto.h"
#include "kiss_info.h"
#include "kiss_fw_ui.h"   // the firmware pill opens it
#include "kiss_seed.h"
#include "kiss_seed_sd.h"   // SDSEED_FILENAME: the sealed row on CARD INFO
#include "kiss_setup.h"
#include "platform_sd.h"
#include "kiss_rngaudit.h"   // the AUDIT chooser's second row opens it
#include "kiss_duress.h"
#include "kiss_duress_ui.h"
#include "kiss_word_ui.h"
#include "kiss_gword.h"
#include "kiss_fw_ui.h"   // SD firmware update: the screens this page opens
#include "kiss_theme.h"
#include "kiss_ui.h"   // kiss_build_id_apply: the shared build-identity line
#include "kiss_usage.h"   // clear the receive-index history on wipe
#include "kiss_payee.h"   // ...and who this wallet has paid

#ifndef SIMULATOR
#include "nvs.h"
#include "nvs_flash.h"
#endif

// main.c: closes any wallet screen and runs the seed wizard, then the
// type-twice login. Lets Settings reach CREATE NEW / RESTORE after first boot.
void kiss_begin_setup(void);
// main.c: re-sync the home TESTNET badge after the network is changed here.
void kiss_home_refresh(void);
// main.c: after a wipe, lock straight back to the game (the wallet no longer
// exists; the next KISS unlock lands in first-boot setup).
void kiss_wiped_lock(void);

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
static lv_obj_t *s_theme_dot;   // the header pill's accent swatch; restyle repaints it
static lv_obj_t *s_theme_menu;  // theme dropdown scrim; NULL whenever it is closed
static lv_obj_t *s_build_id;
static lv_obj_t *s_wipe_pill;
static lv_obj_t *s_lang_pill;   // paired with BACK so the bottom row matches
static lv_obj_t *s_fw_pill;     // header row beside it: the device's own controls
static lv_obj_t *s_type_pill, *s_type_pfx, *s_type_expl, *s_type_name;
static lv_obj_t *s_storage_pill;  // STORAGE over the explicit current mode
static lv_obj_t *s_parent;      // language change rebuilds the screen here

static int s_load_error_code;
#ifdef SIMULATOR
static kiss_settings_load_status_t s_sim_load_status = WSETTINGS_LOAD_OK;
static int s_sim_load_error_code;

void kiss_settings_sim_set_load_result(kiss_settings_load_status_t status,
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

bool kiss_settings_active(void) { return s_scr != NULL; }

static void store_u8(const char *key, uint8_t v);

// The unit amounts are shown in. Changed by tapping the total on the sign
// screen -- the amount IS the control, which is how every wallet that offers
// this does it -- and persisted here beside the accent, because it is the same
// kind of preference and survives a power cycle the same way.
void kiss_settings_set_denom(int d)
{
    wt_denom_set(d);
    store_u8("denom", (uint8_t)wt_denom());
}

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

const char *kiss_settings_load_status_name(kiss_settings_load_status_t status)
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

int kiss_settings_load_error_code(void)
{
    return s_load_error_code;
}

#ifndef SIMULATOR
static kiss_settings_load_status_t init_failure(esp_err_t err)
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

kiss_settings_load_status_t kiss_settings_load(void)
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
    uint8_t tn = KISS_NET_DEFAULT_TESTNET, sc = 0, ac = 0, lg = 0;
    uint8_t dn = WT_DENOM_SATS;   // sats unless a previous run said otherwise
    uint8_t hs = 1;               // history remembered unless the owner said not
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
                  get_optional_u8(h, "denom", &dn) &&
                  get_optional_u8(h, "lang", &lg) &&
                  get_optional_u8(h, "hist", &hs);
        nvs_close(h);
        if (!ok)
            return WSETTINGS_LOAD_NVS_READ_FAILED;
    }

    // Apply nothing until the complete settings read is known-good. Defaults
    // are deliberate only for an absent namespace/key, never for an I/O/type
    // failure that could otherwise make a configured wallet look factory-new.
    kiss_set_network(tn);
    kiss_set_script(sc);
    wt_accent_set(ac);
    wt_denom_set(dn);
    i18n_set_lang(lg);
    kiss_history_set_enabled(hs);   // raw setter: a load is not the switch
    return WSETTINGS_LOAD_OK;
#endif
}

// ---- screen ----
static void restyle(void)
{
    int tn = kiss_testnet();
    // accent follows the picked theme everywhere it appears on this screen
    lv_obj_set_style_text_color(wt_screen_title(s_scr), wt_accent(), 0);
    // Eyebrows and chevrons, wherever they were built. They wear the accent
    // now, and they are made by shared helpers rather than held in statics
    // here, so the flag walk is what finds them.
    wt_accent_restyle(s_scr);
    kiss_build_id_restyle(s_build_id);
    // The theme control is a header pill now: a swatch of the current accent
    // and a chevron. The swatch IS the value readout, so a pick repaints it and
    // nothing else here -- the dropdown that offered the choice closes on the
    // same tap, and its rows are built fresh each open.
    if (s_theme_dot) lv_obj_set_style_bg_color(s_theme_dot, wt_accent(), 0);
    // Nothing to restyle for the network: picking one rebuilds Settings, so
    // the row is built holding the live name and never has to be corrected in
    // place. Its colours are WT_WARN and WT_INK, neither of which follows the
    // accent.

    // The address type row carries the address PREFIX as its right-hand value
    // and the type name as its sub-line. Both are rebuilt by walking the row's
    // children rather than by index, because wt_row adds the chevron first and a
    // row without a sub-line has one child fewer.
    if (s_type_pill) {
        int sc = kiss_script();
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


static void settings_reopen(void)
{
    lv_obj_t *parent = s_parent;
    s_type_pill = s_type_pfx = s_type_expl = s_storage_pill = NULL;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    kiss_settings_open(parent);
}

// The firmware screens own the display while they are up and hand it back the
// same way the duress screens do, by rebuilding Settings underneath.
static void fw_open_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *parent = s_parent;
    // Same reset settings_reopen does. The row pointers outlive the screen they
    // point into otherwise, and restyle() walks them.
    s_type_pill = s_type_pfx = s_type_expl = s_storage_pill = NULL;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    kiss_fw_ui_open(parent, settings_reopen);
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
        return tr(kiss_seed_flash_encrypted() ? STR_W_FLASH_ENC_NOTE
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
    // 552..752: a lone acknowledge is still the way off the screen.
    lv_obj_t *ok = wt_pill(s_scr, tr(STR_C_OK), 552, WT_ACTION_Y, 200,
                           storage_result_ack_cb, NULL);
    if (rc == WSEED_OK) wt_pill_primary(ok);
}

static void storage_apply(void *ud)
{
    int target = (int)(intptr_t)ud;
    int rc = kiss_seed_move_to(target);
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
    if (target == kiss_seed_mode()) return;     // already selected and named
    storage_confirm_screen(target);
}

static void storage_chooser_back_cb(lv_event_t *e)
{
    (void)e;
    settings_reopen();
}

// ---- the card itself: capacity, free space, and what is on it ----
// Reached from the storage chooser's bar, the one screen where the owner is
// already thinking about the card. Facts as rows on the list grid, the
// firmware screen's shape; the framed subject is used-of-total.
static void sdinfo_back_cb(lv_event_t *e)
{
    (void)e;
    storage_chooser_screen();
}

static void sdinfo_screen(void)
{
    platform_sd_info_t inf;
    int rc = platform_sd_info(&inf);

    s_type_pill = s_type_pfx = s_type_expl = s_storage_pill = NULL;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }

    if (rc != 0) {
        // The slot is empty (or the card unreadable). The firmware screen's
        // no-card pair already says what to do in 21 locales; the right block
        // says what this screen would have shown.
        s_scr = wt_screen(s_parent, tr(STR_W_SD_BTN), NULL);
        const char *lh = tr(STR_G_FW_NOCARD_H), *lb = tr(STR_G_FW_NOCARD_B);
        const char *rh = tr(STR_G_SD_ABOUT_H), *rb = tr(STR_G_SD_ABOUT_B);
        const lv_font_t *f = wt_body_font2_head(lh, lb, rh, rb, 344 - 14,
                                                WT_CONTENT_BOTTOM - 232);
        wt_why_block(s_scr, lh, lb, 48, 232, 344, WT_CONTENT_BOTTOM - 232,
                     f, WT_WARN);
        wt_why_block(s_scr, rh, rb, 408, 232, 344, WT_CONTENT_BOTTOM - 232,
                     f, wt_accent());
        wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140,
                sdinfo_back_cb, NULL);
        return;
    }

    // The CID product name is the card introducing itself; it is the subtitle
    // so the title stays the word the chooser's pill promised.
    s_scr = wt_screen(s_parent, tr(STR_W_SD_BTN), inf.name);

    uint64_t used = inf.total_bytes - inf.free_bytes;
    char a[24], b[24], val[52];
    wt_fmt_bytes(used, a, sizeof a);
    wt_fmt_bytes(inf.total_bytes, b, sizeof b);
    snprintf(val, sizeof val, "%s / %s", a, b);
    wt_value_card(s_scr, tr(STR_G_FW_ON_CARD), val,
                  WT_LIST_L_X, WT_LIST_Y(0), WT_LIST_W, true);

    wt_fmt_bytes(inf.free_bytes, a, sizeof a);
    wt_row_x(s_scr, LV_SYMBOL_DRIVE, tr(STR_G_SD_ROW_FREE), NULL, NULL,
             a, wt_font_mono23(), WT_INK, false,
             WT_LIST_L_X, WT_LIST_Y(2), WT_LIST_W, WT_ROW_H, NULL, NULL);

    // Counts from the directory, not from a kept window: every lister returns
    // the real total, so a card holding more files than any screen shows still
    // counts them all here.
    char one[1][SD_NAME_LEN];
    char cnt[16], sub[32];
    int total = 0;
    platform_sd_list_psbt(one, 1, &total);
    int nsigned = platform_sd_signed_scan(NULL, NULL, 0, 0);
    snprintf(cnt, sizeof cnt, "%d", total);
    snprintf(sub, sizeof sub, tr(STR_G_SD_ROW_SIGNED_FMT),
             nsigned > 0 ? nsigned : 0);
    wt_row_x(s_scr, LV_SYMBOL_FILE, tr(STR_G_SD_ROW_PSBT), sub, NULL,
             cnt, wt_font_mono23(), WT_INK, false,
             WT_LIST_R_X, WT_LIST_Y(0), WT_LIST_W, WT_ROW_H, NULL, NULL);

    total = 0;
    platform_sd_list_firmware(one, 1, &total);
    snprintf(cnt, sizeof cnt, "%d", total);
    wt_row_x(s_scr, LV_SYMBOL_DOWNLOAD, tr(STR_G_SD_ROW_FW), NULL, NULL,
             cnt, wt_font_mono23(), WT_INK, false,
             WT_LIST_R_X, WT_LIST_Y(1), WT_LIST_W, WT_ROW_H, NULL, NULL);

    total = 0;
    platform_sd_list_kef(one, 1, &total);
    snprintf(cnt, sizeof cnt, "%d", total);
    wt_row_x(s_scr, WT_ICON_LOCK, tr(STR_G_SD_ROW_KEF), NULL, NULL,
             cnt, wt_font_mono23(), WT_INK, false,
             WT_LIST_R_X, WT_LIST_Y(2), WT_LIST_W, WT_ROW_H, NULL, NULL);

    // Only in SD storage mode: the sealed words file, present or not. The
    // filename is the label -- it is a filename, not a phrase to translate --
    // and the sub is W_SD_MISSING_S's short sentence: the chooser's full
    // W_SD_NOTE ellipsised against the tick in this 365px lane.
    if (kiss_seed_mode() == WSEED_MODE_SD) {
        size_t len = 0;
        platform_sd_file *f = platform_sd_open(SDSEED_FILENAME, &len);
        bool present = f != NULL;
        if (f) platform_sd_close(f);
        lv_obj_t *row = wt_row_x(s_scr, WT_ICON_KEY, SDSEED_FILENAME,
                                 tr(present ? STR_W_SD_MISSING_S
                                            : STR_G_SD_ROW_WORDS_MISSING),
                                 NULL,
                                 present ? LV_SYMBOL_OK : LV_SYMBOL_WARNING,
                                 NULL, present ? WT_OK : WT_WARN, false,
                                 WT_LIST_R_X, WT_LIST_Y(3), WT_LIST_W,
                                 WT_ROW_H, NULL, NULL);
        wt_row_sev(row, present ? WT_SEV_OK : WT_SEV_WARN);
        if (!present) wt_row_sub_color(row, WT_WARN);
    }

    wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140,
            sdinfo_back_cb, NULL);
}

static void sdinfo_open_cb(lv_event_t *e)
{
    (void)e;
    sdinfo_screen();
}

static void storage_chooser_screen(void)
{
    int current = kiss_seed_mode();
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
        if (mode == WSEED_MODE_KEEP && !kiss_seed_flash_encrypted())
            wt_row_sub_color(row, WT_WARN);
    }
    lv_obj_t *back = wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140,
                             storage_chooser_back_cb, NULL);
    lv_obj_set_ext_click_area(back, 10);
    // The card itself, from the one screen already about it. The label avoids
    // the words SD CARD, which name the mode row two inches above the pill.
    wt_pill_icon(s_scr, WT_ICON_SD, tr(STR_G_SD_INFO_PILL),
                 WT_ACT_X, WT_ACTION_Y, 240, WT_ACTION_H, sdinfo_open_cb, NULL);
}

#ifdef SIMULATOR
// Rebuild the chooser in place, so the walk can photograph it with flash
// encryption both off and on. The KEEP row's amber subline is the only
// difference and it is applied at build time, so nothing but a rebuild shows
// the other state -- which is why the sim's hardcoded 0 meant one of the two
// renders had never existed.
void kiss_settings_sim_reopen_storage(void) { storage_chooser_screen(); }
#endif

static void storage_open_cb(lv_event_t *e)
{
    (void)e;
    storage_chooser_screen();
}

// ---- history: what the wallet remembers between sessions ----
// Two rows for one switch, on the same chooser shape as the network and the
// address type: the marks this covers are the receive high-water guard
// (kiss_usage.h) and the paid-before memory (kiss_payee.h). Instant apply,
// no hold: this is a teaching guard, not funds, the marks rebuild through
// ordinary use, and the OFF row's own sub-line states the erase before the
// tap. Picking the row the tick is on is a no-op, so a double tap cannot
// erase anything.
static void hist_chooser_screen(void);

static void hist_pick_cb(lv_event_t *e)
{
    int on = (int)(intptr_t)lv_event_get_user_data(e);
    if (on == kiss_history_enabled()) return;   // already selected and ticked
    kiss_history_apply(on);                     // OFF also erases both stores
    store_u8("hist", (uint8_t)on);
    settings_reopen();
}

static void hist_back_cb(lv_event_t *e) { (void)e; settings_reopen(); }

static void hist_open_cb(lv_event_t *e) { (void)e; hist_chooser_screen(); }

static void hist_chooser_screen(void)
{
    s_type_pill = s_type_pfx = s_type_expl = s_storage_pill = NULL;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    // The subtitle names what the switch covers; the tick below says which
    // way it is set, so a "current: X" line would say it twice.
    s_scr = wt_screen(s_parent, tr(STR_I_SEC_HISTORY),
                      tr(STR_I_ROW_HISTORY_SUB));

    // The ON row's sub tells the truth the way the storage chooser's KEEP row
    // does: may_persist() (kiss_usage.c) needs encrypted flash AND a non
    // amnesic mode, so on the plaintext lane ENABLED only lasts the session
    // and the sub says so, in amber. Lying here would be the exact fault the
    // KEEP row's amber note exists to avoid.
    bool cross_boot = kiss_seed_flash_encrypted() &&
                      kiss_seed_mode() != WSEED_MODE_AMNESIC;
    lv_obj_t *on_row = wt_row_x(s_scr, LV_SYMBOL_SAVE, tr(STR_G_HIST_ON_BTN),
                                tr(cross_boot ? STR_G_HIST_ON_NOTE
                                              : STR_G_HIST_ON_NOTE_PLAIN),
                                NULL, NULL, NULL, WT_INK,
                                kiss_history_enabled() != 0,
                                WT_CHOICE_X, WT_CHOICE_Y(0), WT_CHOICE_W,
                                WT_CHOICE_H, hist_pick_cb, (void *)(intptr_t)1);
    if (!cross_boot)
        wt_row_sub_color(on_row, WT_WARN);
    wt_row_x(s_scr, WT_ICON_SECRET, tr(STR_G_HIST_OFF_BTN),
             tr(STR_G_HIST_OFF_NOTE), NULL, NULL, NULL, WT_INK,
             kiss_history_enabled() == 0,
             WT_CHOICE_X, WT_CHOICE_Y(1), WT_CHOICE_W, WT_CHOICE_H,
             hist_pick_cb, (void *)(intptr_t)0);
    lv_obj_set_ext_click_area(
        wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140,
                hist_back_cb, NULL), 10);
}

#ifdef SIMULATOR
// Rebuild the chooser in place, so the walk photographs the ON row's sub in
// both encryption states -- the same two-render problem the storage chooser's
// reopen hook exists for.
void kiss_settings_sim_reopen_history(void) { hist_chooser_screen(); }
#endif

// ---- network ----
// Three of them now, and the third one is a LABEL. Signet, testnet3 and
// testnet4 share coin type 1h, the tb hrp and the tsp prefix, so kiss_testnet()
// stays the boolean every derivation asks and this screen is the only thing
// that knows which of the two a reader is looking at.
static const char *net_note(int net)
{
    return net == KISS_NET_MAIN   ? tr(STR_I_NET_MAIN_NOTE)
         : net == KISS_NET_SIGNET ? tr(STR_I_NET_SIGNET_NOTE)
                                  : tr(STR_I_NET_TEST_NOTE);
}

static void net_pick_cb(lv_event_t *e)
{
    int net = (int)(intptr_t)lv_event_get_user_data(e);
    kiss_set_network(net);
    // The NVS key is still "testnet" and still a u8; it holds KISS_NET_* now.
    // Widening it beats a second key: a device that stored 0 or 1 under the
    // two-network build reads back as exactly the network it had.
    store_u8("testnet", (uint8_t)net);
    settings_reopen();                     // return with the selected row updated
}

static void net_back_cb(lv_event_t *e)
{
    (void)e;
    settings_reopen();
}

static void net_chooser_screen(void)
{
    s_type_pill = s_type_pfx = s_type_expl = s_storage_pill = NULL;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_scr = wt_screen(s_parent, tr(STR_I_SEC_NET), NULL);

    // Enum order, which is also real money first and the two practice chains
    // under it. The VALUE is the address prefix, and it is the whole reason
    // this screen shows a value at all: TESTNET and SIGNET both read tb1...,
    // side by side, one row apart. That is the trap this device cannot catch
    // for you -- the addresses are identical and the coins are not.
    //
    // DIRECTORY on every row, the same mark the address types wear, because
    // what a network selects here is the same kind of thing: a coin type at
    // the top of the derivation path.
    for (int net = 0; net < 3; net++) {
        // font23 named, not auto-fitted. Auto-fit sizes each row on its own
        // string, so the shortest note came back a rung LARGER than the two
        // above it and the list read as three unrelated screens. Three options
        // of one question are one size.
        wt_row_x(s_scr, LV_SYMBOL_DIRECTORY, kiss_net_name_of(net), net_note(net),
                 wt_font23(), type_prefix(kiss_script(), net != KISS_NET_MAIN),
                 wt_font_mono23(), WT_MUT, kiss_network() == net,
                 WT_CHOICE_X, WT_CHOICE_Y(net), WT_CHOICE_W, WT_CHOICE_H,
                 net_pick_cb, (void *)(intptr_t)net);
    }
    lv_obj_set_ext_click_area(
        wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, net_back_cb, NULL), 10);
}

static void net_open_cb(lv_event_t *e)
{
    (void)e;
    net_chooser_screen();
}

#ifdef SIMULATOR
void kiss_settings_sim_reopen_network(void) { net_chooser_screen(); }
#endif

static void type_pick_cb(lv_event_t *e)
{
    int sc = (int)(intptr_t)lv_event_get_user_data(e);
    kiss_set_script(sc);
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
    int tn = kiss_testnet();
    for (int i = 0; i < 3; i++) {
        int sc = scripts[i];
        wt_row_x(s_scr, LV_SYMBOL_DIRECTORY, type_name(sc), type_note(sc), NULL,
                 type_prefix(sc, tn), wt_font_mono23(), WT_MUT,
                 kiss_script() == sc, WT_CHOICE_X, WT_CHOICE_Y(i),
                 WT_CHOICE_W, WT_CHOICE_H, type_pick_cb, (void *)(intptr_t)sc);
    }
    lv_obj_set_ext_click_area(
        wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, type_back_cb, NULL), 10);
}

// The stroke chooser takes over the screen and hands control back here.
static void duress_open(void)
{
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    // No passphrase on this session means this wallet IS the spare: both ways
    // in reach it, so configuring a stroke would only let someone believe
    // otherwise. Say that instead of opening the chooser.
    if (kiss_session_decoy())
        kiss_duress_ui_open_nopass(s_parent, settings_reopen);
    else
        kiss_duress_ui_open(s_parent, settings_reopen);
}

// Two destinations behind one row, because they are two halves of one question.
// The stroke decides WHICH signer a draw opens; the word decides what has to be
// drawn at all. Splitting them into two Settings rows would put the rarer and
// more consequential of the two -- the one that can make a device unopenable --
// in the same list as the address type.
static void waysin_stroke_cb(lv_event_t *e) { (void)e; duress_open(); }

static void waysin_word_cb(lv_event_t *e)
{
    (void)e;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    kiss_word_ui_open(s_parent, settings_reopen);
}

static void waysin_back_cb(lv_event_t *e) { (void)e; settings_reopen(); }

// Either audit owns the display while it runs and hands back the same way
// the firmware screens do, by rebuilding Settings underneath.
static void audit_cam_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *parent = s_parent;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    kiss_setup_open_audit(parent, settings_reopen);
}

static void audit_rng_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *parent = s_parent;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    kiss_rngaudit_open(parent, settings_reopen);
}

static void audit_back_cb(lv_event_t *e) { (void)e; settings_reopen(); }

// Two audits behind one word. A chooser rather than a second pill: the row
// beside the ways in card has 140px to give, and a chooser row carries a
// sub line saying what each audit checks BEFORE it is entered -- which a
// pill never could, and which is most of what a newcomer needs from either.
static void audit_open_cb(lv_event_t *e)
{
    (void)e;
    s_type_pill = s_type_pfx = s_type_expl = s_storage_pill = NULL;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_scr = wt_screen(s_parent, tr(STR_W_AUD_T), tr(STR_W_AUD_S));
    wt_row_x(s_scr, LV_SYMBOL_IMAGE, tr(STR_W_PROOF_T), tr(STR_W_AUD_CAM_SUB),
             NULL, NULL, NULL, WT_INK, false,
             WT_CHOICE_X, WT_CHOICE_Y(0), WT_CHOICE_W, WT_CHOICE_H,
             audit_cam_cb, NULL);
    wt_row_x(s_scr, LV_SYMBOL_SHUFFLE, tr(STR_W_RNG_T), tr(STR_W_AUD_RNG_SUB),
             NULL, NULL, NULL, WT_INK, false,
             WT_CHOICE_X, WT_CHOICE_Y(1), WT_CHOICE_W, WT_CHOICE_H,
             audit_rng_cb, NULL);
    lv_obj_set_ext_click_area(
        wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140,
                audit_back_cb, NULL), 10);
}

static void duress_cb(lv_event_t *e)
{
    (void)e;
    s_type_pill = s_type_pfx = s_type_expl = s_storage_pill = NULL;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_scr = wt_screen(s_parent, tr(STR_I_ROW_WAYSIN), tr(STR_I_ROW_WAYSIN_SUB));

    // What is true today, as chips: what opens the spare (KISS, or the
    // owner's drawing), and what has to follow it.
    //
    // The chip names the swipe as YOURS again, because it is: kiss_duress_route
    // compares the drawn stroke against the stored one. Never render WHICH
    // stroke here -- this page is reachable from the spare session, so the
    // shape itself is the one thing on it that a coerced owner must not be able
    // to hand over by opening Settings.
    {
        lv_obj_t *row = wt_diagram_row(s_scr);
        wt_chip(row, gw_stored_any() ? tr(STR_GD_WORD_T) : "KISS",
                gw_stored_any());
        wt_diagram_op(row, "+");
        wt_chip(row, tr(STR_GD_PICK_REAL_T), true);   // ONE SWIPE, the rule
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 128);
    }

    // The rule in words, under the chips that state it as a picture. The chips
    // alone read as a STATUS -- "drawing set, swipe set" -- so an owner who has
    // just set a drawing goes looking for where the swipe is chosen. That
    // control now exists and HOW IT WORKS below reaches it. The left block says
    // what the swipe does (yours opens the real keys, anything else the spare),
    // the right says what it is not (the secret; the passphrase is).
    //
    // No headings. Both are sentences lifted whole from screens that already
    // teach this, and a heading over either would be a new key in 21 locales
    // for decoration. Accent on how it works, WARN on where it goes wrong --
    // the proven pair geometry.
    {
        const char *b1 = tr(STR_GD_PICK_REAL_S);
        const char *b2 = tr(STR_GD_DONE_B);
        const lv_font_t *f = wt_body_font2(b1, b2, 330,
                                           WT_CONTENT_BOTTOM - 232);
        wt_why_block(s_scr, NULL, b1, 48, 232, 344,
                     WT_CONTENT_BOTTOM - 232, f, wt_accent());
        wt_why_block(s_scr, NULL, b2, 408, 232, 344,
                     WT_CONTENT_BOTTOM - 232, f, WT_WARN);
    }

    // BACK leftmost, the two actions right aligned to 752. 140 + 270 + 270 with
    // 12px gaps is exactly the 704 lane, which is why this row runs tighter
    // than the 22px the roomier rows get.
    lv_obj_t *row[3];
    row[0] = wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140,
                     waysin_back_cb, NULL);
    row[1] = wt_pill(s_scr, tr(STR_GD_SET_BTN), WT_ACT_X, WT_ACTION_Y, 270,
                     waysin_stroke_cb, NULL);
    row[2] = wt_pill(s_scr, tr(STR_GD_WORD_PILL), 330, WT_ACTION_Y, 270,
                     waysin_word_cb, NULL);
    // One size across the row. pill_label_fit is per pill, so the longest label
    // drops only its own pill a rung -- USE YOUR OWN DRAWING sat at font14
    // between two pills at 28 and read as a rendering mistake rather than as
    // three choices. That is the exact case wt_pill_row was written for and
    // this row was not calling it.
    wt_pill_row(row, 3);
}

static void theme_pick_cb(lv_event_t *e)
{
    wt_accent_set((int)(intptr_t)lv_event_get_user_data(e));
    store_u8("accent", (uint8_t)wt_accent_get());
    restyle();
    kiss_home_refresh();
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    kiss_home_refresh();                // reflect any network change on the home badge
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
}

void kiss_settings_close(void) { close_cb(NULL); }   // idle auto-lock path

static void settings_after_words(void)
{
    kiss_settings_open(s_parent);
}

static void words_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *parent = s_parent;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    kiss_info_open_words(parent, settings_after_words);
}

// Wipe: erase the seed and go back to being just a game. One tap on the pill
// opens a confirm screen; the erase happens on a HOLD there and takes effect
// immediately (a power pull right after must still find the seed gone), then a
// full-screen confirmation says so.
// Straight into the wizard, on a device that has just been emptied. Its own
// first screen offers a fresh draw or RESTORE, so the owner who erased in
// order to type their paper back in is not sent round by the menu.
static void wiped_new_cb(lv_event_t *e)
{
    (void)e;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    kiss_begin_setup();
}

static void wiped_ok_cb(lv_event_t *e)
{
    (void)e;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    kiss_wiped_lock();
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

static void do_wipe(void *ud)
{
    lv_obj_t *confirm = ud;
    if (confirm) lv_obj_delete_async(confirm);
    if (kiss_seed_wipe() != 0) {        // NVS erase/commit CAN fail: never claim
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
    kiss_session_close();               // truly gone: session key leaves RAM too
    kiss_usage_wipe();                   // drop the receive-index history too
    kiss_payee_wipe();                   // ...and the payees it recognised
    // On device the whole-partition erase in kiss_seed.c has already taken
    // these (they are deliberately absent from its KEEP_KEYS). Host builds keep
    // them in RAM, so say it explicitly: an unlock layout that outlived its
    // seed would point at a wallet that no longer exists.
    kiss_duress_forget();

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

    // Two ways off this screen, because there are two reasons to have been
    // here. The erase used to be half of a pair whose other half made new seed
    // words; that pair is gone, so the offer moves to where it is actually
    // true -- after the erase, on a device that now holds nothing. OK still
    // locks, exactly as it did, for the owner who erased to hand the box on.
    wt_pill(ovl, tr(STR_W_CHOOSE_NEW), 140, 386, 260, wiped_new_cb, NULL);
    wt_pill(ovl, tr(STR_C_OK), 440, 386, 200, wiped_ok_cb, NULL);
}

// ---- NO UNDO: one door ------------------------------------------------
// There used to be a chooser here offering MAKE NEW WORDS beside ERASE THE
// WORDS, and the page was arguing a difference the device does not have: both
// ended at the same whole partition erase, replace simply reaching it at the
// end of the wizard instead of the start. One door now, named for what it
// does, and making new seed words is what the screen AFTER the erase offers.
//
// What that costs, said plainly because it is real: the old replace kept the
// existing seed words until the new ones were written and verified
// (kiss_seed_commit), so an abandoned wizard left the wallet untouched. Erase
// first means an owner rotating keys is on their paper if the setup that
// follows does not finish. The screen says so before the hold, which is the
// trade this device makes everywhere else too.
static void erase_back_cb(lv_event_t *e) { (void)e; settings_reopen(); }

static void erase_screen(void)
{
    s_type_pill = s_type_pfx = s_type_expl = s_storage_pill = NULL;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_scr = wt_screen(s_parent, tr(STR_G_WIPEC_T), NULL);

    // The keys this page is about to end, named. Every route into Settings has
    // an open session behind it, so kiss_ui_last_fp is THIS signer's
    // fingerprint rather than a stale one, and the owner can hold it against
    // the card in their hand before touching the hold. That is the one check
    // the confirmation cannot do on their behalf.
    //
    // No measure-and-shrink pass any more: wt_value_card centres its own
    // caption and value now, so the loop that used to size the card to its
    // widest child was both a no-op (the caption is forced to the full lane)
    // and about to fight the centring.
    {
        uint8_t fp[4];
        kiss_ui_last_fp(fp);
        char id[16];
        snprintf(id, sizeof id, "%02X%02X%02X%02X", fp[0], fp[1], fp[2], fp[3]);
        wt_value_card(s_scr, tr(STR_D_FINGERPRINT), id, 228, 108, 344, false);

        // The block below promises the paper still opens these keys. The device
        // knows when that has never been proven -- Settings says so on an amber
        // card three rows up -- so it says so here too rather than letting an
        // unchecked promise carry an erase.
        if (!kiss_ui_backup_checked()) {
            char warn[96];
            snprintf(warn, sizeof warn, "%s  %s", LV_SYMBOL_WARNING,
                     tr(STR_I_WORDS_UNVERIFIED));
            lv_obj_t *l = wt_lbl(s_scr, warn, 0, 0, wt_font14(), WT_WARN);
            lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 200);
        }
    }

    // G_WIPEC_B is already two paragraphs -- what leaves, and what brings it
    // back -- so the pair geometry falls out of wt_why_body rather than being
    // hand placed: STOP on the half that destroys, mut on the half that
    // reassures. That is the same split the chooser drew, minus the door that
    // led to the same room.
    wt_why_body(s_scr, tr(STR_G_WIPEC_B), 232, STOP_COL, true);

    // 400, not 320. The hold pill was never measured while it lived on an
    // overlay -- nothing in fitcheck pointed at it -- and at 320 the Russian,
    // European Portuguese and Norwegian labels ran 357, 342 and 329px into a
    // 292px lane. The row has the room: 48..448 with BACK still at 612.
    wt_hold_pill(s_scr, tr(STR_G_HOLD_WIPE), WT_ACT_X, WT_ACTION_Y, 400,
                 WT_ACTION_H, WIPE_HOLD_MS, do_wipe, NULL);
    wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140,
            erase_back_cb, NULL);
}

static lv_obj_t *mk_pillh(const char *txt, int x, int y, int w, int h, lv_event_cb_t cb, void *ud)
{
    return wt_pillh(s_scr, txt, x, y, w, h, cb, ud);
}

static void endwords_cb(lv_event_t *e) { (void)e; erase_screen(); }

// ---- language picker: full-screen overlay, every name in its own language
// (a user stuck in a language they can't read must still find the way back).
// Shared with the first-boot setup screen via kiss_lang_picker_open(). ----

int kiss_lang_pick_slot(int lang)
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
    kiss_settings_open(parent);
    kiss_home_refresh();
}

static void lang_open_cb(lv_event_t *e)
{
    (void)e;
    kiss_lang_picker_open(s_scr, settings_lang_picked);
}

void kiss_lang_picker_open(lv_obj_t *parent, void (*picked_cb)(void))
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

// The two column grid every card on this page sits on.
#define SG_L_X    25
#define SG_L_W   365
#define SG_R_X   412
#define SG_R_W   365
#define SG_TOP    72
#define SG_HEAD  23    // eyebrow at SG_TOP -> first card at 95, as drawn
#define SG_PITCH 71    // 64 tall card + 7 gap

// ---- theme: a header pill and a dropdown ----
//
// THEME spent a version as a card in the right column, holding the row label,
// the live accent name and four tappable dots in a 365x64 box. The dots were
// the tell that it was the wrong shape: every other card on this page states a
// value and opens a screen, and this one asked for a pick in place, in a
// column about the wallet, for the one setting that is about the glass. It
// also cost the column a whole slot, which pushed the full width row below
// both columns down to clear it. So it joins LANGUAGE and FIRMWARE in the
// header -- the three controls that belong to the DEVICE rather than the
// wallet in it -- as a pill holding a swatch of the current accent and a
// chevron, and the pick happens on a dropdown under it.
//
// The pill is wordless on purpose: the swatch is the value and the chevron
// says it opens, which is the whole message, and a pill with no words costs
// no key in any locale. 44px, not the 90 it was first drawn at: every pixel
// this pill takes comes out of the TITLE's lane, and the settings word in six
// locales (de leads at 269px in font28) needs 276 of it or wt_title_fit drops
// to font23 -- which the fit gate hard-fails as a title that has stopped
// looking like one. 44 holds the 18px dot and a font14 chevron, the same
// chevron size every row on the device wears, with the fit gate's lane pinned
// to this exact arithmetic in sim/fitcheck.c.
#define THEME_PILL_W 44
// LEFT of FIRMWARE, which is left of LANGUAGE -- both 170 wide (LANG_PILL_W,
// defined where they are built) with 16 between them; this chip sits a
// tighter 12 off FIRMWARE, giving the saved pixels to the title. Same y=18 as
// the others, above WT_CONTENT_BOTTOM, so wt_pillh does not build an action
// bar for it.
#define THEME_PILL_X (752 - 170 - 16 - 170 - 12 - THEME_PILL_W)

// The dropdown card hangs 6px under the pill and shares its right edge, so it
// reads as the pill unfolding rather than as a second panel that happens to
// appear. 230 fits the widest name (CYPHERPINK) beside its dot with the OK
// mark still clear of it. Four rows on a 52 pitch inside 8px padding put the
// card's bottom edge at 292, well above WT_CONTENT_BOTTOM.
#define THEME_MENU_W     230
#define THEME_MENU_X     (THEME_PILL_X + THEME_PILL_W - THEME_MENU_W)
#define THEME_MENU_Y     (18 + 44 + 6)
#define THEME_MENU_PITCH 52
#define THEME_MENU_H     (8 + WT_ACC_N * THEME_MENU_PITCH + 8)

static void theme_menu_close_cb(lv_event_t *e)
{
    (void)e;
    // async: this runs from a tap on the scrim or on one of its rows, and an
    // object must not be torn down while its own event is still on the stack.
    if (s_theme_menu) { lv_obj_delete_async(s_theme_menu); s_theme_menu = NULL; }
}

static void theme_menu_pick_cb(lv_event_t *e)
{
    theme_pick_cb(e);          // set + persist + restyle + home refresh
    theme_menu_close_cb(NULL); // the pick is the answer; the menu's job is done
}

static void theme_menu_open_cb(lv_event_t *e)
{
    (void)e;
    if (s_theme_menu) return;

    // Full screen scrim, black at 70 percent: the page stays visible but
    // plainly out of reach, and a tap anywhere on it closes the menu. The
    // opacity is load bearing beyond the look -- the overlap gate only treats
    // content as buried under a backdrop at LV_OPA_50 or more, exactly as the
    // language picker's overlay is treated.
    lv_obj_t *scrim = lv_obj_create(s_scr);
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, 800, 480);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_set_style_bg_color(scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_70, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scrim, theme_menu_close_cb, LV_EVENT_CLICKED, NULL);
    s_theme_menu = scrim;

    lv_obj_t *card = wt_card(scrim, THEME_MENU_X, THEME_MENU_Y,
                             THEME_MENU_W, THEME_MENU_H);
    // Clickable with no callback: a tap on the card's own padding is aimed at
    // the menu, not past it, so it must not fall through to the scrim's close.
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < WT_ACC_N; i++) {
        // borrow the accent table for this row's colour and name, exactly as
        // the old card's dots did: set, read, restore
        int save = wt_accent_get();
        wt_accent_set(i);
        lv_color_t c = wt_accent();
        const char *nm = wt_accent_name();
        wt_accent_set(save);

        lv_obj_t *row = lv_obj_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 8, 8 + i * THEME_MENU_PITCH);
        lv_obj_set_size(row, THEME_MENU_W - 16, THEME_MENU_PITCH);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        // the rows already abut on the 52 pitch; the ext area is tolerance at
        // the card's edges, not the thing making the targets big enough
        lv_obj_set_ext_click_area(row, 8);
        lv_obj_add_event_cb(row, theme_menu_pick_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        lv_obj_t *d = lv_obj_create(row);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 18, 18);
        lv_obj_set_pos(d, 6, (THEME_MENU_PITCH - 18) / 2);
        lv_obj_set_style_radius(d, 9, 0);
        lv_obj_set_style_bg_color(d, c, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(d, 1, 0);
        lv_obj_set_style_border_color(d, KEY_COL, 0);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);   // the row takes the tap

        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text(l, nm);
        lv_obj_set_style_text_color(l, WT_INK, 0);
        lv_obj_set_style_text_font(l, wt_font23(), 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 6 + 18 + 12, 0);

        // the current theme's row carries the check, in its own colour --
        // which IS wt_accent() on this row, so the mark and the swatch agree
        if (i == save) {
            lv_obj_t *ok = lv_label_create(row);
            lv_label_set_text(ok, LV_SYMBOL_OK);
            lv_obj_set_style_text_color(ok, c, 0);
            lv_obj_set_style_text_font(ok, wt_font23(), 0);
            lv_obj_align(ok, LV_ALIGN_RIGHT_MID, -6, 0);
        }
    }
}

void kiss_settings_open(lv_obj_t *parent)
{
    if (s_scr) return;
    s_parent = parent;
    s_type_pill = s_type_pfx = s_type_expl = s_storage_pill = NULL;
    s_fw_pill = NULL;
    s_theme_dot = NULL;
    // A menu open when the screen died was deleted with it; the handle must
    // not survive to block the next open.
    s_theme_menu = NULL;
    s_scr = wt_screen(parent, tr(STR_G_T), NULL);

    // The top right belongs to the header pills; see the block that builds
    // them below. THEME is one of them now -- see the dropdown section above
    // kiss_settings_open for the whole story of where it has lived.

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
    // height: the deepest thing on the page is the full width row under both
    // columns. The left column's three cards end at 301 (95 + 64 + 7 + 64 + 7
    // + 64), the right's shorter stack at 253, and the row at SG_FULL_Y = 308
    // puts its own bottom edge on 372, well clear of 398.
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
// The ways in row shares its line with the CAMERA AUDIT pill, so the pair has
// to fill the full 752 page width (25..777) between them. At a single-column
// SG_L_W the label ellipsised to "Duress w..." while the value took the rest
// -- which reads as a struck through label, not as a narrow row.
#define SG_FULL_W (SG_R_X + SG_R_W - SG_L_X)
// 605 + 7 gap + 140 pill = 752: the row keeps almost its whole width (the pill
// is the same 140px width it always had beside BACK, now at the row's right
// end) and the pill keeps its own geometry, so nothing about either object
// moves from what the drawing already proved.
#define SG_AUDIT_W 140
#define SG_WAYS_W  (SG_FULL_W - 7 - SG_AUDIT_W)
#define SG_AUDIT_X (SG_L_X + SG_WAYS_W + 7)
// 331: the standard 7px gap under the RIGHT column, which ends at 324 again.
// This number moves with that column's fortunes: it was 331 while the theme
// card closed the column at 324, dropped to 308 when the theme moved into the
// header dropdown and the column ended at 253, and is 331 once more now that
// the HISTORY row has the theme card's old slot (260..324). Bottom edge 395,
// three clear of WT_CONTENT_BOTTOM -- the exact geometry the theme card
// already shipped.
#define SG_FULL_Y 331
    wt_row_head(s_scr, tr(STR_I_SEC_THIS_WALLET), SG_L_X, SG_TOP, SG_L_W);

    // Network: a value, a chevron, and the choice on a screen of its own.
    //
    // It was a SEGMENTED control while there were two networks, for a good
    // reason -- you do not navigate into a two-state setting, you set it where
    // it is stated -- and that reason expired the moment a third arrived.
    // Three lozenges in the same 176px track set at 56px each, and "MAINNET"
    // does not fit 56px at any size a reader would call type; widening the
    // track takes the room the label needs. So the row joins the two rows
    // under it, which ask the same shape of question and already have a
    // screen each.
    //
    // Amber on both test networks: the colour says "these coins are not real",
    // and the sub-line says it again in words so the state never rests on
    // colour alone. WHICH test network it is only the value can say.
    wt_row(s_scr, tr(STR_I_ROW_NETWORK),
           kiss_testnet() ? tr(STR_G_TESTNET_NOTE) : tr(STR_G_MAINNET_NOTE),
           kiss_net_name(), kiss_testnet() ? WT_WARN : WT_INK,
           SG_L_X, SG_TOP + SG_HEAD, SG_L_W, net_open_cb, NULL);

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
                         type_name(kiss_script()),
                         type_prefix(kiss_script(), kiss_testnet()), WT_INK,
                         SG_L_X, SG_TOP + SG_HEAD + SG_PITCH, SG_L_W,
                         type_open_cb, NULL);
    // Ordinary: no status rides on the address type, so it takes the rim.
    lv_obj_add_flag(s_type_pill, WT_FLAG_ACCENT_BORDER);
    lv_obj_set_style_border_color(s_type_pill, wt_accent(), 0);
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
            if (!strcmp(t, type_prefix(kiss_script(), kiss_testnet())))
                s_type_pfx = c;
            else if (!strcmp(t, type_name(kiss_script())))
                s_type_name = c;
        }
    }
    // Duress unlock (kiss_duress.h). ABSENT in a decoy session, not greyed
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
    // kiss_start). Removing this changed nothing, which is what proved it.
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
                            kiss_seed_mode() == WSEED_MODE_KEEP &&
                            !kiss_seed_flash_encrypted()
                                ? tr(STR_W_FLASH_PLAIN_NOTE_SHORT) : "",
                            storage_mode_name(kiss_seed_mode()), WT_INK,
                            SG_L_X, SG_TOP + SG_HEAD + 2 * SG_PITCH, SG_L_W,
                            storage_open_cb, NULL);
    // Amber CARD, not just an amber note. The drawing tints this whole box when
    // the words sit in a flash this build does not encrypt, which is the one
    // fact on the page a holder should catch without reading anything.
    if (kiss_seed_mode() == WSEED_MODE_KEEP && !kiss_seed_flash_encrypted())
        wt_row_sev(s_storage_pill, WT_SEV_WARN);

    // LEFT-HALF, under both columns. This row was the fourth card in the left
    // column, and it did not belong there twice over: it is the only thing on
    // the page that states a mapping rather than a setting, and 365px could not
    // hold "Duress wallet" beside a value as long as LINE THROUGH.
    //
    // 605px fits it now: the value is SET / NOT SET (short) rather than the
    // long name that burst the 365px column, and the audit pill takes the room
    // it freed beside it (SG_AUDIT_X .. 777) rather than the action bar.
    //
    // The sub-line is GD_SET_NOTE, not GD_SET_SUB. The value on this row is the
    // stroke that opens the REAL wallet -- kiss_duress_real(), set by
    // kiss_duress_set() -- so "a spare you can show" described the other
    // wallet entirely, which is the reading that sent the owner looking for a
    // bug. "which stroke opens which wallet" is what the row actually answers.
    // Unconditional, and that is the point. This row was hidden in a decoy
    // session whenever a stroke was configured, so an attacker who knew where
    // to look could catch a coerced owner handing over the spare: the row's
    // absence was the confession. Showing it is safe only because the unlock no
    // longer forks on kiss_duress_real() either -- there is nothing left for
    // its presence to corroborate.
    // "Duress wallet ... LINE THROUGH" read as "the line through opens the
    // duress wallet". It is the opposite: the value is kiss_duress_real(),
    // the mark that reaches the REAL signer. Reported off the bench, and the
    // owner had it backwards for exactly as long as the row existed.
    //
    // The shape is gone from the value too, because the row was implying a
    // check the device does not make. kiss_duress_route asks whether there
    // was a recognised mark, never WHICH -- any of the six opens the real
    // login, on any device, configured or not. The six exist to tell a
    // deliberate mark from a slip, not to be a secret; the passphrase is the
    // secret. So the row reports whether a way in has been rehearsed, and the
    // screen behind it is where the shape is chosen and taught.
    // SET / NOT SET, not the button's name. With the label back to "Duress" the
    // value has to be a STATE or the row reads as a struck through label again,
    // which is the exact fault 324ef09 renamed the row to escape. It reports
    // whether a way in has been rehearsed and nothing else -- never WHICH mark,
    // which is the reason the label could come back at all.
    // SET means "a custom drawing replaced KISS" -- the one configurable
    // fact left on this page now that the swipe is a rule, not a choice.
    lv_obj_t *wr = wt_row(s_scr, tr(STR_I_ROW_WAYSIN), tr(STR_I_ROW_WAYSIN_SUB),
           gw_stored_any() ? tr(STR_GD_ON) : tr(STR_GD_OFF),
           WT_INK, SG_L_X, SG_FULL_Y, SG_WAYS_W,
           duress_cb, NULL);
    // SET / NOT SET is a setting, not a warning: nothing is wrong either way.
    lv_obj_add_flag(wr, WT_FLAG_ACCENT_BORDER);
    lv_obj_set_style_border_color(wr, wt_accent(), 0);

    // AUDIT, beside the ways in row rather than on the action bar. The bar
    // pill looked like a third action next to BACK; this is a page question
    // -- "prove a part of this signer" sits with the other facts about THIS
    // SIGNER -- and the pill keeps the exact geometry it shipped with
    // (140x52), seated on the row's line (the row card is WT_ROW_H tall, so
    // the pill centres on it). It opens a chooser now that there are two
    // audits behind it, so the label is the word for the class and the mark
    // is an eye, not a camera: the camera is one of the two things to look at.
    wt_pill_icon(s_scr, LV_SYMBOL_EYE_OPEN, tr(STR_W_AUD_T),
                 SG_AUDIT_X, SG_FULL_Y + (WT_ROW_H - WT_ACTION_H) / 2,
                 SG_AUDIT_W, WT_ACTION_H, audit_open_cb, NULL);

    // RIGHT COLUMN, group one: the backup. Redraw 05 gives this its own eyebrow
    // rather than leaving the words row adrift among the destructive buttons,
    // and that separation is the point: reading your words and destroying them
    // are opposite intentions that used to sit in one stack.
    wt_row_head(s_scr, tr(STR_I_SEC_YOUR_BACKUP), SG_R_X, SG_TOP, SG_R_W);
    {
        // ONE row, not two. "Recovery words" and "Paper checked" were separate
        // cards that called the same callback and opened the same screen -- one
        // destination drawn twice, on the column the owner said reads heavy.
        // The state belongs on the row that goes and deals with it, and the
        // page it opens is where there is room to say more.
        bool ok = kiss_ui_backup_checked();
        char buf[160];
        if (ok) {
            uint8_t fp[4]; kiss_ui_last_fp(fp);
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
        lv_obj_t *r = wt_row(s_scr, tr(STR_I_ROW_WORDS), buf, NULL, WT_INK,
                             SG_R_X, SG_TOP + SG_HEAD, SG_R_W, words_cb, NULL);
        // A colour cue AND a glyph, per ADDENDUM-02: in GREEN theme the accent
        // is byte identical to WT_OK, so colour alone stops carrying meaning.
        wt_row_sub_color(r, ok ? WT_OK : WT_WARN);
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
        // 166, one pitch under the backup row rather than two. The 78px band
        // that used to sit here was deliberate -- reading your words and
        // destroying them are opposite intentions, and distance said so -- and
        // the argument for keeping it was that the column did not need the
        // space. It does now: the ways in row moved out of the left column and
        // spans the page at SG_FULL_Y, so this column has to end above 308.
        //
        // The separation survives without the band. These two rows are STOP
        // tinted with red labels under their own red eyebrow and rule; the
        // backup row above is a green or amber card under a different eyebrow.
        // Nothing about the pair reads as continuous with it.
        //
        // The old note also warned this would put ERASE nearer the thumb. It
        // does the opposite: ERASE moves 331 -> 260, a row further from the
        // action bar, with a benign row between it and the bottom of the page.
        int y = SG_TOP + SG_HEAD + SG_PITCH;
        lv_obj_t *h = wt_row_head(s_scr, tr(STR_I_SEC_NO_UNDO), SG_R_X, y, SG_R_W);
        lv_obj_set_style_text_color(h, STOP_COL, 0);
        // ...and it must STAY stop red through a theme change. The flag is
        // what wt_accent_restyle repaints, so this eyebrow gives it up: it
        // names a consequence, not a group, and that is never the accent's.
        lv_obj_remove_flag(h, WT_FLAG_ACCENT);
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

        // ONE row where there were two. Both of the old ones ended at the same
        // erase (see erase_screen), so a second row bought the page nothing
        // but a chance to tap the wrong one. The row that is left says what they
        // share; the screen behind it asks which way.
        //
        // The freed SG_PITCH slot is empty again -- the theme card that filled
        // it for a version moved into the header dropdown -- so NO UNDO closes
        // the column, and an empty slot at the bottom of a column reads as the
        // end of the column.
        // No sub-line. "these words go either way" was true of a chooser with
        // two doors in it; there is one door now and its own screen says what
        // goes and what brings it back, in stronger words and beside the
        // fingerprint it is about.
        s_wipe_pill = wt_row(s_scr, tr(STR_I_ROW_ENDWORDS), NULL, NULL, WT_INK,
                             SG_R_X, y + SG_HEAD, SG_R_W, endwords_cb, NULL);
        // Red CARD and red LABEL, so it reads as destructive before it is
        // tapped; a hold on the confirmation is what actually erases. The label
        // takes WT_STOP_INK rather than WT_STOP: full stop red on a stop-tinted
        // card is the one pairing on this page that vibrates.
        wt_row_sev(s_wipe_pill, WT_SEV_STOP);
        uint32_t n = lv_obj_get_child_count(s_wipe_pill);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t *c = lv_obj_get_child(s_wipe_pill, i);
            if (lv_obj_get_y(c) < 24 && lv_obj_get_x(c) < 20)
                lv_obj_set_style_text_color(c, WT_STOP_INK, 0);
        }

        // HISTORY closes the column, in the slot the theme card used to hold
        // (260..324; SG_FULL_Y answers to it again -- see the define). What it
        // switches is the two stores of marks the wallet keeps between
        // sessions: used receive addresses (kiss_usage.h) and who was paid
        // (kiss_payee.h). The accent rim is what detaches an ordinary setting
        // from the stop-tinted erase card directly above it -- the same rim
        // the TYPE and WAYS IN rows wear for the same reason: nothing is
        // wrong either way.
        // No sub-line: beside the value the 365px row clipped it to "used
        // addresses a...", and an ellipsised promise is worse than none. What
        // the switch covers is the chooser's subtitle instead.
        lv_obj_t *hr = wt_row(s_scr, tr(STR_I_ROW_HISTORY), NULL,
                              tr(kiss_history_enabled() ? STR_G_HIST_ON_BTN
                                                        : STR_G_HIST_OFF_BTN),
                              WT_INK, SG_R_X, y + SG_HEAD + SG_PITCH, SG_R_W,
                              hist_open_cb, NULL);
        lv_obj_add_flag(hr, WT_FLAG_ACCENT_BORDER);
        lv_obj_set_style_border_color(hr, wt_accent(), 0);
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

        // FIRMWARE, beside LANGUAGE, because they are the same kind of thing:
        // the two controls on this screen that belong to the DEVICE rather than
        // the wallet in it. It spent a version in the action bar on the
        // reasoning that it should sit next to the build identity it acts on,
        // and that bar turned out to be full -- build identity runs to about
        // 275, the theme name occupies 340..460, the dots 470..557 and BACK
        // 610..750, so a 240px pill at 300 landed straight through the theme
        // block. The overlap gate caught it as 120x15 px of shared pixels
        // against the accent NAME, in every locale, which is what a bar with no
        // room left looks like from the outside.
        //
        // Same width as LANGUAGE, 16px of gap, and above WT_CONTENT_BOTTOM so
        // wt_pill_icon does not build a second action bar -- the bar is BACK's.
        wt_pill_icon(s_scr, WT_ICON_SD, tr(STR_G_FW_PILL),
                     752 - LANG_PILL_W - 16 - LANG_PILL_W, 18,
                     LANG_PILL_W, 44, fw_open_cb, NULL);

        // THEME, left of FIRMWARE: the third device control in the header.
        // The pill's label is the chevron; the swatch is added beside it and
        // the pair is centred by measuring the glyph, since the pair's width
        // is the only unknown in the 90px lane. Geometry and the story of why
        // it is wordless live with the defines above kiss_settings_open.
        //
        // Same y=18 as its neighbours, above WT_CONTENT_BOTTOM, so wt_pillh
        // does not build an action bar for it -- the bar is BACK's.
        lv_obj_t *tp = mk_pillh(LV_SYMBOL_DOWN, THEME_PILL_X, 18,
                                THEME_PILL_W, 44, theme_menu_open_cb, NULL);
        lv_obj_t *gl = lv_obj_get_child(tp, 0);
        // font14, the size every row's chevron already is: a mark, not text,
        // and the small glyph is what lets the whole chip live in 44px.
        lv_obj_set_style_text_font(gl, wt_font14(), 0);
        lv_point_t gs;
        lv_text_get_size(&gs, LV_SYMBOL_DOWN, wt_font14(), 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int px = (THEME_PILL_W - (18 + 6 + gs.x)) / 2;
        s_theme_dot = lv_obj_create(tp);
        lv_obj_remove_style_all(s_theme_dot);
        lv_obj_set_size(s_theme_dot, 18, 18);
        lv_obj_set_pos(s_theme_dot, px, (44 - 18) / 2);
        lv_obj_set_style_radius(s_theme_dot, 9, 0);
        lv_obj_set_style_bg_color(s_theme_dot, wt_accent(), 0);
        lv_obj_set_style_bg_opa(s_theme_dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_theme_dot, 1, 0);
        lv_obj_set_style_border_color(s_theme_dot, KEY_COL, 0);
        lv_obj_remove_flag(s_theme_dot, LV_OBJ_FLAG_CLICKABLE); // the pill takes the tap
        lv_obj_align(gl, LV_ALIGN_LEFT_MID, px + 18 + 6, 0);

        // The title had the whole 704 lane, then shared it with one 170px
        // pill, two, and now the theme chip as well. Nothing else would catch
        // this: the overlap gate measures text against text, a pill is not
        // text, and a long locale's title would simply run underneath them.
        // 276 = what is left of 704 after both pills, the chip and the gaps
        // -- and 276 is not decoration: de's title needs 269 to hold font28,
        // and sim/fitcheck.c pins this same lane and fails the build if a
        // locale's title is squeezed to font23.
        wt_title_fit(s_scr, 752 - LANG_PILL_W - 16 - LANG_PILL_W - 12
                            - THEME_PILL_W - 16 - 48);
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

        // FIRMWARE is not here. It is device chrome, so it went up beside the
        // language pill; this bar had no room for it. See the header block.

        // CAMERA AUDIT is not here either, though it belongs to this bar: it
        // is built after the build identity, below, because it has to be
        // measured against it.

        // Build identity AFTER the pill, and that order is load bearing. The
        // action bar is built lazily by the first wt_pill on the screen; the
        // language pill used to be that pill and used to come first, so this
        // line landed on top of the bar by accident of sequence. The pill went
        // to the top of the screen, BACK became the bar's builder, and the
        // version vanished behind it. Raising it afterwards is not enough
        // either: kiss_build_id_make creates THREE sibling labels and hands
        // back only the first, so a move_foreground on the return value lifts
        // the version and leaves encryption and radio buried. Building it last
        // is the fix that cannot half work.
        //
        // x=48, the page margin. It sat at 150 only because BACK held the left
        // corner and 48 was inside it.
        //
        // y=404, the top of the bar. TWO rows -- the version alone, then
        // encryption, radio and randomness sharing the second (see
        // kiss_build_id_make, which measured three-in-a-row at x=443 and put
        // the facts side by side instead). Rows land at 404 and 426, both
        // inside the bar's fill, which stops at 471.
        s_build_id = kiss_build_id_make(s_scr, 48, 404, true, true);

        // CAMERA AUDIT. It spent a version on the duress page, which was the
        // wrong room by a mile: an owner looking for the ways in found a
        // camera drill, and an owner wanting to check the camera had to go
        // through the screen about hiding coins to reach it. Those two share
        // nothing except that both were once the only page with space.
        //
        // For one build it sat on the action bar next to BACK, which read as a
        // control like BACK instead of the question it answers. It now shares
        // the ways in row's line (SG_AUDIT_X), the page's one row about THIS
        // SIGNER's identity, which is the question the audit asks too: the
        // camera is a camera, and the words on the glass came out of it.
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
