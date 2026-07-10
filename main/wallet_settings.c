// Settings: network selection. Style matches the other wallet screens.
// The network flips instantly (the master key is network-free) and persists
// across power cycles via NVS on the device.
#include "wallet_settings.h"

#include <stdio.h>

#include "wallet_crypto.h"
#include "wallet_seed.h"
#include "wallet_setup.h"
#include "wallet_ui.h"   // wallet_build_id_apply: the shared build-identity line

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

#define BG_COL   lv_color_hex(0x070A10)
#define INK_COL  lv_color_hex(0xE8EEF7)
#define MUT_COL  lv_color_hex(0x7A869C)
#define KEY_COL  lv_color_hex(0x10141D)
#define OK_COL   lv_color_hex(0x35D07F)
#define WARN_COL lv_color_hex(0xF2B84B)

#define STOP_COL lv_color_hex(0xFF4D5E)

static lv_obj_t *s_scr;
static lv_obj_t *s_main_pill, *s_test_pill, *s_state_lbl;
static lv_obj_t *s_replace_pill;
static lv_obj_t *s_wipe_pill;
static bool s_wipe_arm;         // "wipe wallet" needs a confirming 2nd tap too
static lv_obj_t *s_type_seg[3], *s_type_pfx[3], *s_type_expl;   // NATIVE/NESTED/LEGACY chooser + example prefix

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
    uint8_t tn = 0, sc = 0;
    if (nvs_open("kiss", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "testnet", &tn);
        nvs_get_u8(h, "script", &sc);
        nvs_close(h);
    }
    wallet_set_network(tn);
    wallet_set_script(sc);
#endif
}

// ---- screen ----
static void restyle(void)
{
    int tn = wallet_testnet();
    lv_obj_set_style_border_color(s_main_pill, tn ? MUT_COL : OK_COL, 0);
    lv_obj_set_style_border_width(s_main_pill, tn ? 1 : 2, 0);
    lv_obj_set_style_border_color(s_test_pill, tn ? WARN_COL : MUT_COL, 0);
    lv_obj_set_style_border_width(s_test_pill, tn ? 2 : 1, 0);
    lv_label_set_text(s_state_lbl, tn
        ? "TESTNET: practice coins with no value.\n"
          "free from coinfaucet.eu"
        : "MAINNET: real bitcoin");
    lv_obj_set_style_text_color(s_state_lbl, tn ? WARN_COL : MUT_COL, 0);

    if (s_type_seg[0]) {
        int sc = wallet_script();
        for (int i = 0; i < 3; i++) {              // highlight the active type, dim the rest
            bool on = (i == sc);
            lv_obj_set_style_border_color(s_type_seg[i], on ? OK_COL : MUT_COL, 0);
            lv_obj_set_style_border_width(s_type_seg[i], on ? 2 : 1, 0);
            lv_obj_set_style_text_color(lv_obj_get_child(s_type_seg[i], 0),  // name label
                                        on ? INK_COL : MUT_COL, 0);
            lv_label_set_text(s_type_pfx[i], type_prefix(i, tn));            // example prefix
            lv_obj_set_style_text_color(s_type_pfx[i],
                                        on ? WARN_COL : lv_color_hex(0x525C6E), 0);
        }
        lv_label_set_text(s_type_expl,
            sc == WSCRIPT_LEGACY ? "oldest style, highest fees. only to\nmatch a very old wallet."
          : sc == WSCRIPT_NESTED ? "older segwit. only to match a\nwallet that needs it."
                                 : "modern, lowest fees. use this\nunless an app needs another kind.");
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
    lv_label_set_text(l, "CREATE NEW WALLET");
    lv_obj_set_style_text_color(l, INK_COL, 0);
    lv_obj_set_style_border_color(s_replace_pill, MUT_COL, 0);
}
static void disarm_wipe(void)
{
    if (!s_wipe_arm) return;
    s_wipe_arm = false;
    lv_obj_t *l = lv_obj_get_child(s_wipe_pill, 0);
    lv_label_set_text(l, "WIPE WALLET");
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
        lv_label_set_text(lbl, "TAP AGAIN TO WIPE");
        lv_obj_set_style_text_color(lbl, STOP_COL, 0);
        lv_obj_set_style_border_color(s_wipe_pill, STOP_COL, 0);
        return;
    }
    s_wipe_arm = false;
    if (wallet_seed_wipe() != 0) {        // NVS erase/commit CAN fail: never claim
        // "erased" unless it truly is — reset the pill, tell the truth
        lv_label_set_text(lbl, "WIPE WALLET");
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
        lv_label_set_text(t, "COULD NOT ERASE");
        lv_obj_set_style_text_color(t, STOP_COL, 0);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_letter_space(t, 3, 0);
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 150);
        lv_obj_t *st = lv_label_create(ovl);
        lv_label_set_text(st, "the backup words may STILL be on this device.\n"
                              "do not sell it or give it away. try the wipe again;\n"
                              "if it keeps failing, treat the device as if it\n"
                              "holds your words.");
        lv_obj_set_style_text_color(st, MUT_COL, 0);
        lv_obj_set_style_text_font(st, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(st, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(st, LV_ALIGN_TOP_MID, 0, 206);
        lv_obj_t *ok = lv_obj_create(ovl);
        lv_obj_remove_style_all(ok);
        lv_obj_set_size(ok, 200, 52);
        lv_obj_align(ok, LV_ALIGN_TOP_MID, 0, 330);
        lv_obj_set_style_radius(ok, 26, 0);
        lv_obj_set_style_bg_color(ok, KEY_COL, 0);
        lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ok, 1, 0);
        lv_obj_set_style_border_color(ok, STOP_COL, 0);
        lv_obj_add_flag(ok, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(ok, wipe_fail_ok_cb, LV_EVENT_CLICKED, ovl);
        lv_obj_t *okl = lv_label_create(ok);
        lv_label_set_text(okl, "BACK");
        lv_obj_set_style_text_color(okl, INK_COL, 0);
        lv_obj_set_style_text_font(okl, &lv_font_montserrat_14, 0);
        lv_obj_center(okl);
        return;
    }
    wallet_session_close();               // truly gone: session key leaves RAM too

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
    lv_label_set_text(t, "WALLET ERASED");
    lv_obj_set_style_text_color(t, INK_COL, 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_letter_space(t, 3, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 140);

    lv_obj_t *s = lv_label_create(ovl);
    lv_label_set_text(s, "the backup words are gone from this device.\n"
                         "nothing on here can spend from that wallet anymore.\n\n"
                         "your paper backup still works: restore it any time\n"
                         "from Settings, or create a brand-new wallet.");
    lv_obj_set_style_text_color(s, MUT_COL, 0);
    lv_obj_set_style_text_font(s, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 196);

    lv_obj_t *ok = lv_obj_create(ovl);
    lv_obj_remove_style_all(ok);
    lv_obj_set_size(ok, 200, 52);
    lv_obj_align(ok, LV_ALIGN_TOP_MID, 0, 340);
    lv_obj_set_style_radius(ok, 26, 0);
    lv_obj_set_style_bg_color(ok, KEY_COL, 0);
    lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ok, 1, 0);
    lv_obj_set_style_border_color(ok, MUT_COL, 0);
    lv_obj_add_flag(ok, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ok, wiped_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ol = lv_label_create(ok);
    lv_label_set_text(ol, "OK");
    lv_obj_set_style_text_color(ol, INK_COL, 0);
    lv_obj_set_style_text_font(ol, &lv_font_montserrat_14, 0);
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
        lv_label_set_text(lbl, "TAP AGAIN TO ERASE");
        lv_obj_set_style_text_color(lbl, STOP_COL, 0);
        lv_obj_set_style_border_color(s_replace_pill, STOP_COL, 0);
        return;
    }
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_replace_arm = false;
    wallet_begin_setup();
}

static lv_obj_t *mk_pillh(const char *txt, int x, int y, int w, int h, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *p = lv_obj_create(s_scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, w, h);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_style_radius(p, 26, 0);
    lv_obj_set_style_bg_color(p, KEY_COL, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, MUT_COL, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(p, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, INK_COL, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_center(l);
    return p;
}

static lv_obj_t *mk_pill(const char *txt, int x, int y, int w, lv_event_cb_t cb, void *ud)
{
    return mk_pillh(txt, x, y, w, 52, cb, ud);
}

static lv_obj_t *mk_section(const char *txt, int x, int y)
{
    lv_obj_t *l = lv_label_create(s_scr);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, MUT_COL, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *mk_wrap(int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(s_scr);
    lv_obj_set_style_text_color(l, MUT_COL, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(l, x, y);
    return l;
}

void wallet_settings_open(lv_obj_t *parent)
{
    if (s_scr) return;
    s_scr = lv_obj_create(parent);
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_size(s_scr, 800, 480);
    lv_obj_set_style_bg_color(s_scr, BG_COL, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_scr);

    lv_obj_t *cap = lv_label_create(s_scr);
    lv_label_set_text(cap, "SETTINGS");
    lv_obj_set_style_text_color(cap, INK_COL, 0);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_letter_space(cap, 3, 0);
    lv_obj_set_pos(cap, 48, 26);

    // LEFT: network + address type
    mk_section("NETWORK", 48, 78);
    s_main_pill = mk_pillh("MAINNET", 48, 104, 340, 44, pick_cb, (void *)(intptr_t)0);
    s_test_pill = mk_pillh("TESTNET", 48, 154, 340, 44, pick_cb, (void *)(intptr_t)1);
    s_state_lbl = mk_wrap(48, 206, 340);

    mk_section("ADDRESS TYPE", 48, 258);
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
        lv_obj_set_style_text_font(s_type_pfx[i], &lv_font_montserrat_14, 0);
        lv_obj_align(s_type_pfx[i], LV_ALIGN_BOTTOM_MID, 0, -7);
    }
    s_type_expl = mk_wrap(48, 350, 360);
    lv_obj_t *sep_n = mk_wrap(48, 394, 360);   // one line; fits the wrap width
    lv_label_set_text(sep_n, "each network + type is its own separate wallet");

    // RIGHT: wallet actions, each pill with its own caption. Caption lines are
    // hand-broken well under the wrap width so LVGL never re-wraps them into
    // orphan words (the old copy stacked "separate" / "coins." on own lines).
    mk_section("WALLET", 430, 78);
    s_replace_arm = false;
    s_replace_pill = mk_pillh("CREATE NEW WALLET", 430, 104, 320, 52, replace_cb, NULL);
    lv_obj_t *wn = mk_wrap(430, 166, 340);
    lv_label_set_text(wn, "make a fresh wallet, or restore one from\n"
                          "backup words. this REPLACES the wallet\n"
                          "on here - back up the old words first.");

    // wipe: seed off the device entirely (back to just a game). Red text so it
    // reads as destructive before it's ever tapped; second tap confirms.
    s_wipe_arm = false;
    s_wipe_pill = mk_pillh("WIPE WALLET", 430, 268, 320, 52, wipe_cb, NULL);
    lv_obj_set_style_text_color(lv_obj_get_child(s_wipe_pill, 0), STOP_COL, 0);
    lv_obj_t *wipe_n = mk_wrap(430, 330, 340);
    lv_label_set_text(wipe_n, "removes the wallet from this device.\n"
                              "only its backup words can bring it back.");

    // build identity, bottom-left (shared with the wallet home corner)
    wallet_build_id_make(s_scr, 48, 436);

    mk_pill("BACK", 610, 404, 140, close_cb, NULL);
    restyle();
}
