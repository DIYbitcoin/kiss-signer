// Settings: network selection. Style matches the other wallet screens.
// The network flips instantly (the master key is network-free) and persists
// across power cycles via NVS on the device.
#include "wallet_settings.h"

#include <stdio.h>
#include <string.h>

#include "flag_imgs.h"   // language-picker flags (Twemoji, CC-BY; en has none)
#include "i18n.h"
#include "wallet_crypto.h"
#include "wallet_seed.h"
#include "wallet_setup.h"
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

static lv_obj_t *s_scr;
static lv_obj_t *s_acc_dot[WT_ACC_N];   // theme dots, top-right
static lv_obj_t *s_acc_name;            // live name under the dots
static lv_obj_t *s_main_pill, *s_test_pill, *s_state_lbl;
static lv_obj_t *s_replace_pill;
static lv_obj_t *s_build_id;
static lv_obj_t *s_wipe_pill;
static bool s_wipe_arm;         // "wipe wallet" needs a confirming 2nd tap too
static lv_obj_t *s_type_seg[3], *s_type_pfx[3], *s_type_expl;   // NATIVE/NESTED/LEGACY chooser + example prefix
static lv_obj_t *s_parent;      // language change rebuilds the screen here

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
static bool s_replace_arm;      // "replace wallet" needs a confirming 2nd tap

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

void wallet_settings_load(void)
{
#ifndef SIMULATOR
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK)
        return;
    nvs_handle_t h;
    uint8_t tn = 0, sc = 0, ac = 0, lg = 0;
    if (nvs_open("kiss", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "testnet", &tn);
        nvs_get_u8(h, "script", &sc);
        nvs_get_u8(h, "accent", &ac);
        nvs_get_u8(h, "lang", &lg);
        nvs_close(h);
    }
    wallet_set_network(tn);
    wallet_set_script(sc);
    wt_accent_set(ac);
    i18n_set_lang(lg);
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
    lv_label_set_text(s_state_lbl, tn ? tr(STR_G_TESTNET_NOTE) : tr(STR_G_MAINNET_NOTE));
    lv_obj_set_style_text_color(s_state_lbl, tn ? WARN_COL : MUT_COL, 0);

    if (s_type_seg[0]) {
        int sc = wallet_script();
        for (int i = 0; i < 3; i++) {              // highlight the active type, dim the rest
            bool on = (i == sc);
            lv_obj_set_style_bg_color(s_type_seg[i], on ? wt_accent_bg() : KEY_COL, 0);
            lv_obj_set_style_border_color(s_type_seg[i], on ? wt_primary() : MUT_COL, 0);
            lv_obj_set_style_border_width(s_type_seg[i], on ? 2 : 1, 0);
            lv_obj_set_style_text_color(lv_obj_get_child(s_type_seg[i], 0),  // name label
                                        on ? INK_COL : MUT_COL, 0);
            lv_label_set_text(s_type_pfx[i], type_prefix(i, tn));            // example prefix
            lv_obj_set_style_text_color(s_type_pfx[i],
                                        on ? wt_accent() : lv_color_hex(0x525C6E), 0);
        }
        lv_label_set_text(s_type_expl,
            sc == WSCRIPT_LEGACY ? tr(STR_G_TY_LEGACY_NOTE)
          : sc == WSCRIPT_NESTED ? tr(STR_G_TY_NESTED_NOTE)
                                 : tr(STR_G_TY_NATIVE_NOTE));
    }
}

static void pick_cb(lv_event_t *e)
{
    int tn = (int)(intptr_t)lv_event_get_user_data(e);
    wallet_set_network(tn);
    store_u8("testnet", tn ? 1 : 0);
    restyle();
}

static void type_pick_cb(lv_event_t *e)
{
    int sc = (int)(intptr_t)lv_event_get_user_data(e);   // tap the type you want directly
    wallet_set_script(sc);
    store_u8("script", (uint8_t)sc);
    restyle();
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

// two armed red pills at once would be ambiguous — arming one disarms the other
static void disarm_replace(void)
{
    if (!s_replace_arm) return;
    s_replace_arm = false;
    lv_obj_t *l = lv_obj_get_child(s_replace_pill, 0);
    lv_label_set_text(l, tr(STR_G_CREATE_NEW));
    lv_obj_set_style_text_color(l, INK_COL, 0);
    lv_obj_set_style_border_color(s_replace_pill, MUT_COL, 0);
}
static void disarm_wipe(void)
{
    if (!s_wipe_arm) return;
    s_wipe_arm = false;
    lv_obj_t *l = lv_obj_get_child(s_wipe_pill, 0);
    lv_label_set_text(l, tr(STR_G_WIPE));
    lv_obj_set_style_text_color(l, STOP_COL, 0);
    lv_obj_set_style_border_color(s_wipe_pill, MUT_COL, 0);
}

// Wipe: erase the seed and go back to being just a game. Two-tap arm like
// CREATE NEW, but the second tap erases IMMEDIATELY (a power pull right after
// must still find the seed gone), then shows a full-screen confirmation.
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

static void wipe_cb(lv_event_t *e)
{
    lv_obj_t *lbl = lv_obj_get_child(lv_event_get_current_target(e), 0);
    if (!s_wipe_arm) {
        s_wipe_arm = true;
        disarm_replace();
        lv_label_set_text(lbl, tr(STR_G_TAP_WIPE));
        lv_obj_set_style_text_color(lbl, STOP_COL, 0);
        lv_obj_set_style_border_color(s_wipe_pill, STOP_COL, 0);
        return;
    }
    s_wipe_arm = false;
    if (wallet_seed_wipe() != 0) {        // NVS erase/commit CAN fail: never claim
        // "erased" unless it truly is — reset the pill, tell the truth
        lv_label_set_text(lbl, tr(STR_G_WIPE));
        lv_obj_set_style_text_color(lbl, STOP_COL, 0);
        lv_obj_set_style_border_color(s_wipe_pill, MUT_COL, 0);

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
static void replace_cb(lv_event_t *e)
{
    lv_obj_t *lbl = lv_obj_get_child(lv_event_get_current_target(e), 0);
    if (!s_replace_arm) {
        s_replace_arm = true;
        disarm_wipe();
        lv_label_set_text(lbl, tr(STR_G_TAP_ERASE));
        lv_obj_set_style_text_color(lbl, STOP_COL, 0);
        lv_obj_set_style_border_color(s_replace_pill, STOP_COL, 0);
        return;
    }
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_replace_arm = false;
    wallet_begin_setup();
}

// thin wrappers over the wallet_theme kit (call sites keep their signatures)
static lv_obj_t *mk_pillh(const char *txt, int x, int y, int w, int h, lv_event_cb_t cb, void *ud)
{
    return wt_pillh(s_scr, txt, x, y, w, h, cb, ud);
}

static lv_obj_t *mk_pill(const char *txt, int x, int y, int w, lv_event_cb_t cb, void *ud)
{
    return mk_pillh(txt, x, y, w, 52, cb, ud);
}

static lv_obj_t *mk_section(const char *txt, int x, int y)
{
    return wt_section(s_scr, txt, x, y);
}

static lv_obj_t *mk_wrap(int x, int y, int w)
{
    return wt_wrap(s_scr, x, y, w);
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
    s_acc_name = lv_label_create(s_scr);       // names the dressed color
    lv_obj_set_style_text_color(s_acc_name, MUT_COL, 0);
    lv_obj_set_style_text_font(s_acc_name, wt_font14(), 0);
    lv_obj_set_style_text_letter_space(s_acc_name, 2, 0);
    lv_obj_set_pos(s_acc_name, 560, 74);

    // LEFT: network + address type
    mk_section(tr(STR_I_SEC_NET), 48, 78);
    s_main_pill = mk_pillh("MAINNET", 48, 104, 340, 44, pick_cb, (void *)(intptr_t)0);
    s_test_pill = mk_pillh("TESTNET", 48, 154, 340, 44, pick_cb, (void *)(intptr_t)1);
    s_state_lbl = mk_wrap(48, 206, 340);

    mk_section(tr(STR_I_SEC_TYPE), 48, 258);
    // three visible choices (like the network chooser) so it's obvious you pick
    // one — no hidden cycling. Each shows an example address prefix underneath;
    // restyle() highlights the active type and updates the prefixes per network.
    static const char *tn_name[3] = {"NATIVE", "NESTED", "LEGACY"};
    static const int   tn_sc[3]   = {WSCRIPT_NATIVE, WSCRIPT_NESTED, WSCRIPT_LEGACY};
    for (int i = 0; i < 3; i++) {
        int x = 48 + i * 115;
        s_type_seg[i] = mk_pillh(tn_name[i], x, 286, 110, 54, type_pick_cb, (void *)(intptr_t)tn_sc[i]);
        lv_obj_align(lv_obj_get_child(s_type_seg[i], 0), LV_ALIGN_TOP_MID, 0, 8);  // name up top
        s_type_pfx[i] = lv_label_create(s_type_seg[i]);                           // example below
        lv_obj_set_style_text_font(s_type_pfx[i], wt_font14(), 0);
        lv_obj_align(s_type_pfx[i], LV_ALIGN_BOTTOM_MID, 0, -7);
    }
    s_type_expl = mk_wrap(48, 350, 360);
    lv_obj_t *sep_n = mk_wrap(48, 394, 360);   // one line; fits the wrap width
    lv_label_set_text(sep_n, tr(STR_G_SEPARATE));

    // RIGHT: wallet actions, each pill with its own caption. Caption lines are
    // hand-broken well under the wrap width so LVGL never re-wraps them into
    // orphan words (the old copy stacked "separate" / "coins." on own lines).
    mk_section(tr(STR_I_T), 430, 78);
    s_replace_arm = false;
    s_replace_pill = mk_pillh(tr(STR_G_CREATE_NEW), 430, 104, 320, 52, replace_cb, NULL);
    lv_obj_t *wn = mk_wrap(430, 166, 340);
    lv_label_set_text(wn, tr(STR_G_CREATE_NOTE));

    // wipe: seed off the device entirely (back to just a game). Red text so it
    // reads as destructive before it's ever tapped; second tap confirms.
    s_wipe_arm = false;
    s_wipe_pill = mk_pillh(tr(STR_G_WIPE), 430, 268, 320, 52, wipe_cb, NULL);
    lv_obj_set_style_text_color(lv_obj_get_child(s_wipe_pill, 0), STOP_COL, 0);
    lv_obj_t *wipe_n = mk_wrap(430, 330, 340);
    lv_label_set_text(wipe_n, tr(STR_G_WIPE_NOTE));

    // LANGUAGE: the current language on the pill; opens the picker. The pill is
    // narrow, so strip the regional qualifier ("ESPAÑOL (ESPAÑA)" -> "ESPAÑOL")
    // and let the flag carry the variant instead.
    mk_section(tr(STR_G_SEC_LANGUAGE), 430, 384);
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
        lv_obj_t *lp = mk_pill(shortname, 430, 404, 160, lang_open_cb, NULL);
        if (img_lang_flags[li]) {
            lv_obj_t *name = lv_obj_get_child(lp, 0);
            lv_obj_set_style_text_letter_space(name, 0, 0);
            lv_obj_align(name, LV_ALIGN_CENTER, 16, 0);
            lv_obj_t *fl = lv_image_create(lp);
            lv_image_set_src(fl, img_lang_flags[li]);
            lv_obj_align(fl, LV_ALIGN_LEFT_MID, 16, 0);   // clear of the pill's corner radius
            lv_obj_remove_flag(fl, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    // build identity, bottom edge (below the pill row; bottom has no overscan)
    s_build_id = wallet_build_id_make(s_scr, 48, 460);

    mk_pill(tr(STR_C_BACK), 610, 404, 140, close_cb, NULL);
    restyle();
}
