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

// Handed in by the build; "dev" is what a bare compile of this file gets, the
// same fallback kiss_ui.c takes for the identity line behind this row.
#ifndef KISS_VERSION_STR
#define KISS_VERSION_STR "dev"
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

// The five groups, in strip order. The page shows ONE of them at a time, which
// is what buys a group the full 752px lane and four rows instead of nine rows
// fighting over two 365px columns.
enum { TAB_SIGNER = 0, TAB_SECURITY, TAB_BACKUP, TAB_DEVICE, TAB_NOUNDO,
       TAB_N };

static lv_obj_t *s_scr;
static lv_obj_t *s_parent;      // language change rebuilds the screen here
// Which group is open. The one piece of state this page did not have before,
// and it has to SURVIVE settings_reopen(): every pick rebuilds the screen, so
// without it a network change would throw the owner back to tab one.
static int s_tab;
// The address type help card, or NULL. It is a scrim plus a box, and this is
// the SCRIM: it is what has to be deleted, and deleting the box alone would
// leave a full-screen catcher eating every tap on the page. The only overlay
// left on this page -- it explains, it does not choose.
static lv_obj_t *s_help;

// ---- the page moves: frame 5a ------------------------------------------
// Three jobs, in the order they matter. If any of this ever has to be cut,
// cut from the bottom:
//
//   1. the caution on a flagged row is pointed AT, once, after the row lands.
//      This is the reason the page animates rather than decorating it.
//   2. NO UNDO arrives unlike its four neighbours -- it rises rather than
//      sliding, slower, without the overshoot, and the pane reddens. The owner
//      knows which group they are in before reading a word.
//   3. the value lands a beat after its label, so the eye reads the setting's
//      NAME and then what it is set to, instead of a grid arriving at once.
//
// Everything here is confined to a TAB CHANGE. Walking in from home, and
// coming back from any screen a row opens, paint settled: a value chip
// rebuilds the whole page on every tap, and three taps to reach SIGNET
// replaying the entry under the owner's finger is not a design, it is a
// flicker.
static lv_obj_t *s_pane;       // the group on screen
static lv_obj_t *s_pane_out;   // the group leaving, alive for its own 200ms
static lv_obj_t *s_tabs;       // the strip's highlight: the one part that slides
static bool      s_entering;   // the new group has not settled yet

// A child of the pane that is scenery rather than a row -- the NO UNDO wash.
// It fades on its own schedule and must not be dealt a row's slide.
static const char SET_SKIP_TAG[] = "set_skip";

#define MO_IN_MS      260   // a row arriving
#define MO_IN_DX       56
#define MO_IN_STEP     38   // and the beat between rows down the group
#define MO_UP_MS      320   // ...except in NO UNDO, which rises
#define MO_UP_DY       14
#define MO_UP_STEP     44
#define MO_OUT_MS     200   // a row leaving
#define MO_OUT_DX      44
#define MO_OUT_STEP    26
#define MO_FADE_IN     200
#define MO_FADE_UP     240
#define MO_FADE_OUT    160
#define MO_CTRL_MS    220   // the value, a beat behind its label
#define MO_CTRL_DX      7
#define MO_CTRL_LAG    90
#define MO_FLARE_UP   220   // the caution, pointed at once and let go
#define MO_FLARE_DOWN 200   // 200 and not the 420 the handoff drew: at 420 the
                            // page is still moving at 976ms, past the 800 the
                            // kit allows a page change
#define MO_FLARE_LAG  260
#define MO_WASH_MS    280

static void an_tx(void *v, int32_t x)  { lv_obj_set_style_translate_x(v, x, 0); }
static void an_ty(void *v, int32_t y)  { lv_obj_set_style_translate_y(v, y, 0); }
static void an_opa(void *v, int32_t o) { lv_obj_set_style_opa(v, (lv_opa_t)o, 0); }

// The prototype's ease, cubic-bezier(.17,.84,.32,1.05), typed in as itself.
//
// The handoff calls this "about 5% past the mark, then back" and spends a
// paragraph on how to reproduce the overshoot without LVGL's stock one, which
// is far stronger and reads as bouncy on a page of settings. There is no
// overshoot to reproduce: 1.05 is a CONTROL POINT, not the curve's maximum,
// and the curve it controls peaks at 1.0069 -- four tenths of a pixel on a
// 56px travel, in the browser as much as here. Measured off the frames, the
// row arrives at 25 and stays at 25.
//
// The curve is still not ease_out. It is front loaded: most of the distance is
// gone in the first third, so a row reads as arriving rather than as being
// slid. That is what it is here for, and the overshoot never existed.
static void an_path_settle(lv_anim_t *a)
{
    lv_anim_set_path_cb(a, lv_anim_path_custom_bezier3);
    lv_anim_set_bezier3_param(a, LV_BEZIER_VAL_FLOAT(0.17),
                                 LV_BEZIER_VAL_FLOAT(0.84),
                                 LV_BEZIER_VAL_FLOAT(0.32),
                                 LV_BEZIER_VAL_FLOAT(1.05));
}

// The caution, and it is a RING rather than the dot the prototype draws. The
// rows on this page already carry a warning glyph and an amber rim; a 7px dot
// growing to 14 beside them is a detail nobody at the bench would see, and it
// would be a fifth mark on a row that has four. The ring is the row's own edge,
// brightened once. It is a separate object so an interrupted pulse is deleted
// rather than unwound -- there is no half-restored border colour to put back.
static void flare_del(lv_anim_t *a) { lv_obj_delete(a->var); }

static void flare_down(lv_anim_t *a)
{
    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, a->var);
    lv_anim_set_exec_cb(&b, an_opa);
    lv_anim_set_values(&b, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&b, MO_FLARE_DOWN);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&b, flare_del);
    lv_anim_start(&b);
}

static void flare(lv_obj_t *row, int delay)
{
    lv_obj_t *ring = lv_obj_create(row);
    lv_obj_remove_style_all(ring);
    lv_obj_set_pos(ring, 0, 0);
    lv_obj_set_size(ring, WT_WIDE_W, WT_WIDE_H);
    lv_obj_set_style_radius(ring, 10, 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_style_border_color(ring, WT_WARN, 0);
    lv_obj_set_style_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ring);
    lv_anim_set_exec_cb(&a, an_opa);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, MO_FLARE_UP);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, flare_down);
    lv_anim_start(&a);
}

static void enter_done(lv_anim_t *a) { (void)a; s_entering = false; }

// Asked of the ROW rather than of the page's state, so the pulse and the amber
// card can never disagree about which row it is: WT_SEV_WARN is the page's own
// answer to "does this want reading", and this reads the answer back off the
// object it was written on.
static bool row_wants_reading(lv_obj_t *c)
{
    return lv_obj_get_style_border_opa(c, LV_PART_MAIN) == 77
        && lv_color_eq(lv_obj_get_style_border_color(c, LV_PART_MAIN), WT_WARN);
}

// Point at the cautions in a group already on the glass, without moving it.
// The chip's second tap wants this and not a whole entry: the group is not
// arriving, it is being indicated.
static void pane_point(lv_obj_t *pane)
{
    uint32_t n = lv_obj_get_child_count(pane);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(pane, i);
        if (row_wants_reading(c)) flare(c, 0);
    }
}

// `dir` is sign(new tab - old tab), so the group arrives from the side of the
// strip the finger moved towards and the motion carries which way you went.
static void pane_enter(lv_obj_t *pane, int dir, bool rise)
{
    uint32_t n = lv_obj_get_child_count(pane);
    int k = 0;
    lv_obj_t *last = NULL;       // the last row DEALT, which the latch hangs on
    s_entering = true;

    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(pane, i);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, c);

        // The wash is not a row. It has no lane to come in from: it is the
        // colour of the page changing, so it only deepens.
        if (lv_obj_get_user_data(c) == (void *)SET_SKIP_TAG) {
            lv_obj_set_style_opa(c, LV_OPA_TRANSP, 0);
            lv_anim_set_exec_cb(&a, an_opa);
            lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
            lv_anim_set_duration(&a, MO_WASH_MS);
            lv_anim_set_path_cb(&a, lv_anim_path_linear);
            lv_anim_start(&a);
            continue;
        }

        const int delay = k * (rise ? MO_UP_STEP : MO_IN_STEP);
        lv_anim_set_delay(&a, delay);
        if (rise) {
            lv_anim_set_exec_cb(&a, an_ty);
            lv_anim_set_values(&a, MO_UP_DY, 0);
            lv_anim_set_duration(&a, MO_UP_MS);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);   // never overshoot
        } else {
            lv_anim_set_exec_cb(&a, an_tx);
            lv_anim_set_values(&a, dir > 0 ? MO_IN_DX : -MO_IN_DX, 0);
            lv_anim_set_duration(&a, MO_IN_MS);
            an_path_settle(&a);
        }
        // The flag comes off the LAST row to be dealt, which is the last one
        // to settle. A tap arriving before it is a tap on a page still moving.
        // The flag comes off the LAST ROW DEALT, which is not the same as the
        // last child: the wash `continue`s above without an entry animation,
        // and a group whose scenery happened to be built last would leave
        // s_entering true for ever. The symptom of that is silent -- every
        // later tab change would drop its outgoing group instead of sliding
        // it -- so it is guarded here rather than by remembering the order
        // tab_noundo builds in.
        lv_anim_set_completed_cb(&a, NULL);
        lv_anim_start(&a);
        last = c;

        lv_obj_set_style_opa(c, LV_OPA_TRANSP, 0);
        lv_anim_set_completed_cb(&a, NULL);
        lv_anim_set_exec_cb(&a, an_opa);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&a, rise ? MO_FADE_UP : MO_FADE_IN);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_start(&a);

        // The value, 90ms behind the label it belongs to. Its own opacity, on
        // top of the row's, so it is still climbing after the row has arrived.
        lv_obj_t *ctrl = wt_row_wide_ctrl(c);
        if (ctrl) {
            lv_obj_set_style_opa(ctrl, LV_OPA_TRANSP, 0);
            lv_anim_set_var(&a, ctrl);
            lv_anim_set_delay(&a, delay + MO_CTRL_LAG);
            lv_anim_set_duration(&a, MO_CTRL_MS);
            lv_anim_start(&a);
            lv_anim_set_exec_cb(&a, an_tx);
            lv_anim_set_values(&a, MO_CTRL_DX, 0);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_start(&a);
        }

        if (row_wants_reading(c)) flare(c, delay + MO_FLARE_LAG);

        k++;
    }

    // A group of nothing but scenery never settles, so it is already settled.
    if (!last) { s_entering = false; return; }

    // Re-armed on the row that actually finishes last, over its own travel:
    // restarting the same (var, exec_cb) pair replaces the animation LVGL is
    // already running for it rather than adding a second one.
    lv_anim_t z;
    lv_anim_init(&z);
    lv_anim_set_var(&z, last);
    lv_anim_set_exec_cb(&z, rise ? an_ty : an_tx);
    lv_anim_set_values(&z, rise ? MO_UP_DY : (dir > 0 ? MO_IN_DX : -MO_IN_DX), 0);
    lv_anim_set_duration(&z, rise ? MO_UP_MS : MO_IN_MS);
    lv_anim_set_delay(&z, (k - 1) * (rise ? MO_UP_STEP : MO_IN_STEP));
    if (rise) lv_anim_set_path_cb(&z, lv_anim_path_ease_out);
    else      an_path_settle(&z);
    lv_anim_set_completed_cb(&z, enter_done);
    lv_anim_start(&z);
}

static void pane_out_done(lv_anim_t *a)
{
    (void)a;
    // The one callback on this page that reaches past its own object, so it
    // goes through the static and never a captured pointer: by the time it
    // fires the pane it meant may already have been deleted, by a second tab
    // tap or by the screen closing over it.
    if (s_pane_out) { lv_obj_delete(s_pane_out); s_pane_out = NULL; }
}

static void pane_exit(lv_obj_t *pane, int dir)
{
    uint32_t n = lv_obj_get_child_count(pane);
    if (!n) { pane_out_done(NULL); return; }

    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(pane, i);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, c);
        lv_anim_set_delay(&a, i * MO_OUT_STEP);
        lv_anim_set_exec_cb(&a, an_tx);
        lv_anim_set_values(&a, 0, dir > 0 ? -MO_OUT_DX : MO_OUT_DX);
        lv_anim_set_duration(&a, MO_OUT_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
        // The slide outlasts the fade, so the pane goes when the LAST row has
        // finished travelling and not when it stopped being visible.
        if (i + 1 == n) lv_anim_set_completed_cb(&a, pane_out_done);
        lv_anim_start(&a);

        lv_anim_set_completed_cb(&a, NULL);
        lv_anim_set_exec_cb(&a, an_opa);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_duration(&a, MO_FADE_OUT);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_start(&a);
    }
}

// Everything moving, stopped, and both lanes accounted for. Deleting a pane
// takes its animations with it -- lv_obj's destructor calls lv_anim_delete --
// which is what makes the callback that would have deleted it never fire.
static void motion_stop(void)
{
    if (s_pane_out) { lv_obj_delete(s_pane_out); s_pane_out = NULL; }
    s_entering = false;
}

static void build_tab(void);   // the group the strip points at, into s_pane

// Every route off this page drops the screen, and a dozen of them do it
// without a word to the statics: the exit, the idle lock, and every row that
// opens a screen of its own -- sdinfo_screen(), device_screen(), the storage
// chooser, the language picker, the firmware page. Each one deletes s_scr and
// builds its own into the same parent, and the pane went with it while the
// static still named it. The next reopen then deleted a freed object.
//
// So the OBJECT says when it is gone, rather than ten call sites remembering
// to. Comparing against the static is what makes it safe when a page has
// already been replaced: an older pane's delete arrives after the new one has
// been named, matches nothing, and does nothing.
static void pane_gone(lv_event_t *e)
{
    lv_obj_t *p = lv_event_get_target(e);
    if (p == s_pane)     s_pane = NULL;
    if (p == s_pane_out) s_pane_out = NULL;
}

static void tabs_gone(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_tabs) s_tabs = NULL;
}

static lv_obj_t *pane_new(void)
{
    lv_obj_t *p = lv_obj_create(s_scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, 0, 0);
    // The whole page, and never clipped: a row leaving travels 44px past the
    // lane, and a container sized to the rows would cut it in half. It takes
    // no taps of its own, so the strip and the exit under it stay reachable --
    // LVGL only ever hands a press to a CLICKABLE object and walks past this
    // one to the siblings beneath.
    lv_obj_set_size(p, 800, 480);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(p, pane_gone, LV_EVENT_DELETE, NULL);
    return p;
}

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
// Every settings byte comes through here, and PERSIST is the gate on it: with
// the switch off, a change applies to the running session and is never written,
// so the next boot comes up on whatever was last stored. "prst" itself is the
// one exception -- a switch that could not record its own position would turn
// itself back on at the next power up.
static void store_u8(const char *key, uint8_t v)
{
#ifndef SIMULATOR
    if (!kiss_persist_enabled() && strcmp(key, "prst") != 0)
        return;
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
    uint8_t ps = 1;               // the signer saves what it saves, unless told not
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
                  get_optional_u8(h, "prst", &ps);
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
    kiss_persist_set_enabled(ps);   // raw setter: a load is not the switch
    return WSETTINGS_LOAD_OK;
#endif
}

// ---- screen ----
// Everything on this page that wears the accent is built by a shared helper and
// carries the flag, so the walk is what finds it. Nothing is held in a static
// here any more: a pick REBUILDS the page rather than correcting labels in
// place, which is what let the old address-type row keep two captured pointers
// and a loop that had to tell them apart from the chevron.
static void restyle(void)
{
    lv_obj_set_style_text_color(wt_screen_title(s_scr), wt_accent(), 0);
    wt_accent_restyle(s_scr);
}

static void settings_reopen(void)
{
    lv_obj_t *parent = s_parent;
    // The screen goes async, so for one handler pass the OLD page is still up
    // while the new one is being built over it. Anything still moving on it
    // would be moving objects the statics no longer name.
    motion_stop();
    if (s_pane) { lv_obj_delete(s_pane); s_pane = NULL; }
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    kiss_settings_open(parent);      // s_tab survives, deliberately
}

// The firmware screens own the display while they are up and hand it back the
// same way the duress screens do, by rebuilding Settings underneath.
static void fw_open_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *parent = s_parent;
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

// CANCEL goes back to the page the dropdown was opened from, which is where
// the pick was made. The chooser screen that used to sit between them is gone:
// a three item list with a tick on the live one is what a dropdown IS.
static void storage_chooser_screen(void);

static void storage_confirm_cancel_cb(lv_event_t *e)
{
    (void)e;
    storage_chooser_screen();   // back to the list it was picked from
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

// ---- the card itself: capacity, free space, and what is on it ----
// Reached from the storage chooser's bar, the one screen where the owner is
// already thinking about the card. Facts as rows on the list grid, the
// firmware screen's shape; the framed subject is used-of-total.
static void device_screen(void);

static void sdinfo_back_cb(lv_event_t *e)
{
    (void)e;
    device_screen();
}

static void sdinfo_screen(void)
{
    platform_sd_info_t inf;
    int rc = platform_sd_info(&inf);

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

// ---- the picks that resolve where they stand ----
// None of these opens anything. The row IS the control: tap it and the value
// advances to the next one in its set, the sub line under the label changes to
// say what that one means, and the page redraws in place.
//
// They were popovers for one commit. Two things were wrong with a popover here
// and the first is arithmetic: the sets are two, three and four long, so the
// list costs a tap to open, a tap to pick and a scrim over the page to choose
// between as few as TWO things -- which is what the denomination row below has
// always said, in a comment, while four rows beside it did the opposite.
//
// The second is that a floating box has to fit its options into itself, and it
// does not fit them. The storage list shipped reading "SD C...": the note
// beside it, "on the card you carry", took the lane at font14 and the ellipsis
// landed on the one word that says WHICH option the row is. A page is 800px
// wide. The row is 752 of them. Nothing here needed a box.
//
// So the mark says which kind of tap a row takes -- LV_SYMBOL_LOOP advances in
// place, LV_SYMBOL_RIGHT opens a screen -- and the two are never mixed.
//
// Every one of them is instant, free and reversible by tapping again. The one
// pick in SETTINGS that is none of those is STORAGE, which MOVES the recovery
// words, and it is the one that still opens a screen and still holds.

// ---- network ----
// Three of them, and the third one is a LABEL. Signet, testnet3 and testnet4
// share coin type 1h, the tb hrp and the tsp prefix, so kiss_testnet() stays
// the boolean every derivation asks and this row is the only thing that knows
// which of the two a reader is looking at.
//
// MAINNET is first, so the cycle from it goes straight to a test network and
// the row turns amber on the very next tap. Coming back is two taps, and the
// amber is on screen for both of them.
static void net_cb(lv_event_t *e)
{
    (void)e;
    int net = (kiss_network() + 1) % 3;
    kiss_set_network(net);
    // The NVS key is still "testnet" and still a u8; it holds KISS_NET_* now.
    // Widening it beats a second key: a device that stored 0 or 1 under the
    // two-network build reads back as exactly the network it had.
    store_u8("testnet", (uint8_t)net);
    settings_reopen();
}

// ---- address type ----
// Oldest to newest, so the tradeoff advances as a progression rather than
// jumping about, and the "?" beside the label opens the card that names all
// three at once -- which is the thing a list of three was really for.
static const int TYPE_ORDER[3] = {
    WSCRIPT_LEGACY, WSCRIPT_NESTED, WSCRIPT_NATIVE
};

static void type_cb(lv_event_t *e)
{
    (void)e;
    int cur = 0;
    for (int i = 0; i < 3; i++) if (TYPE_ORDER[i] == kiss_script()) cur = i;
    int sc = TYPE_ORDER[(cur + 1) % 3];
    kiss_set_script(sc);
    store_u8("script", (uint8_t)sc);
    settings_reopen();
}

// ---- the address type help card ----
// What the three names mean, as three lines and not a paragraph: a BIP number,
// the prefix it produces, and what it costs. The prefixes follow the live
// network, exactly as the row's own value does, so the card cannot claim bc1
// on a device set to testnet.
static void help_close(void)
{
    if (s_help) { lv_obj_delete_async(s_help); s_help = NULL; }
}

static void help_close_cb(lv_event_t *e) { (void)e; help_close(); }

static void help_open_cb(lv_event_t *e)
{
    (void)e;
    if (s_help) { help_close(); return; }

    static const int SC[3]   = { WSCRIPT_LEGACY, WSCRIPT_NESTED, WSCRIPT_NATIVE };
    static const char *BIP[3] = { "BIP44", "BIP49", "BIP84" };
    const int NOTE[3] = { STR_I_BIP_44_NOTE, STR_I_BIP_49_NOTE,
                          STR_I_BIP_84_NOTE };

    const lv_font_t *hf = wt_font23(), *nf = wt_font14(),
                    *pf = wt_font_mono23();
    const int hh = lv_font_get_line_height(hf);
    const int rh = lv_font_get_line_height(pf);
    const int h  = 16 + hh + 12 + 3 * rh + 2 * 10 + 16;

    // Sized to its content and then floated so its FEET land clear of the
    // action row, rather than pinned to the drawing's y. The kit's type is
    // bigger than the prototype's, so the same three rows need a taller box,
    // and a box measured downward from a fixed top would cross 398.
    lv_obj_t *box = wt_overlay_box(s_scr, &s_help, 250, WT_CONTENT_BOTTOM - 6 - h,
                                   480, h, 12, help_close_cb);

    wt_lbl(box, tr(STR_I_BIP_T), 20, 16, hf, WT_INK);
    lv_obj_t *cl = wt_lbl(box, tr(STR_I_BIP_CLOSE), 0, 0, nf, WT_MUT);
    lv_obj_set_style_text_letter_space(cl, 1, 0);
    lv_obj_align(cl, LV_ALIGN_TOP_RIGHT, -20, 16 + (hh - lv_font_get_line_height(nf)) / 2);

    int y = 16 + hh + 12;
    for (int i = 0; i < 3; i++) {
        // The recommended row in full ink and the other two muted: this card
        // answers "which one" as well as "what are they".
        bool best = SC[i] == WSCRIPT_NATIVE;
        lv_color_t c = best ? WT_INK : WT_MUT;
        wt_lbl(box, BIP[i], 20, y, nf, c);
        // 96, not the drawing's 74. Its prefixes are one glyph and an ellipsis
        // ("1…"); ours are the strings the address type row and the network
        // list already show, and "m/n..." at mono23 is 84px wide -- so a 74px
        // lane put the legacy prefix straight through the note beside it.
        wt_lbl(box, type_prefix(SC[i], kiss_testnet()), 20 + 76, y, pf,
               best ? WT_INK : WT_MUT);
        lv_obj_t *nt = wt_lbl(box, tr(NOTE[i]), 20 + 76 + 96, y, nf, c);
        lv_obj_set_width(nt, 480 - (20 + 76 + 96) - 20);
        lv_obj_set_height(nt, lv_font_get_line_height(nf));
        lv_label_set_long_mode(nt, LV_LABEL_LONG_DOT);
        y += rh + 10;
    }
}

// ---- storage ----
// The one pick on this page that does NOT resolve where it stands, and the
// mark on its row says so before the finger lands: a chevron, not a loop.
//
// Moving where the recovery words live is not a preference. It reads the seed
// out of one place and writes it to another, so it gets a screen that names
// all three destinations with what each one costs, and then the confirmation
// and the 1500ms hold it has always had. A tap that cycled it would step the
// owner through two moves to get back where they started.
static const int STORE_MODE[3] = {
    WSEED_MODE_KEEP, WSEED_MODE_SD, WSEED_MODE_AMNESIC
};

static void store_pick_cb(lv_event_t *e)
{
    int target = STORE_MODE[(int)(intptr_t)lv_event_get_user_data(e)];
    if (target == kiss_seed_mode()) return;    // already the one it is on
    storage_confirm_screen(target);
}

static void store_back_cb(lv_event_t *e) { (void)e; settings_reopen(); }

static void storage_chooser_screen(void)
{
    const int SUB[3] = { STR_I_STORE_FLASH_SUB, STR_I_STORE_SD_SUB,
                         STR_I_STORE_AMN_SUB };
    char current[128];
    snprintf(current, sizeof current, tr(STR_G_STORAGE_CURRENT_FMT),
             storage_mode_name(kiss_seed_mode()));

    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_scr = wt_screen(s_parent, tr(STR_G_STORAGE_SEC), current);

    for (int i = 0; i < 3; i++) {
        bool on = kiss_seed_mode() == STORE_MODE[i];
        // Encrypted flash is a different sentence from bare flash, and this
        // pair of strings is the only place the distinction is ever stated.
        bool bare = STORE_MODE[i] == WSEED_MODE_KEEP
                    && !kiss_seed_flash_encrypted();
        int sub = SUB[i];
        if (STORE_MODE[i] == WSEED_MODE_KEEP && !bare)
            sub = STR_I_STORE_FLASH_ENC_SUB;
        wt_row_wide(s_scr, WT_WIDE_Y(i), &(wt_wide_t){
            .label = storage_mode_name(STORE_MODE[i]),
            .sub   = tr(sub),
            // Amber on the SUB and not on the row. wt_row_sev would wash the
            // whole card, and a permanently amber option in a list of three
            // reads as broken rather than as cautioned.
            .sub_col = bare ? WT_WARN : (lv_color_t){0},
            // The one it is already on takes an accent tick and NO callback,
            // which leaves it un-tappable without drawing it as WT_WIDE_INERT.
            // Inert means dead, and it dims the whole row -- including the sub
            // line, which on bare flash is the amber saying the words are
            // sitting there unencrypted. Selected is not unavailable, and the
            // caution that matters most is the one on the mode you are ON.
            .kind  = WT_WIDE_OPEN,
            .val   = on ? LV_SYMBOL_OK : NULL,
            .vcol  = on ? wt_accent() : (lv_color_t){0},
            .cb    = on ? NULL : store_pick_cb,
            .ud    = (void *)(intptr_t)i,
        });
    }
    wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140,
            store_back_cb, NULL);
}

static void store_open_cb(lv_event_t *e) { (void)e; storage_chooser_screen(); }

// ---- persist: what this signer keeps between sessions ----
// The marks this covers are the receive high-water guard (kiss_usage.h) and
// the paid-before memory (kiss_payee.h). A flip, applied instantly, with no
// hold and no confirmation: this is a teaching guard rather than funds, the
// marks rebuild through ordinary use, and turning it OFF is the direction that
// erases -- which is the SAFE direction for a thing whose whole content is a
// record of what the owner has done. The sub line under the label states which
// way it is set and what that costs.
static void persist_cb(lv_event_t *e)
{
    (void)e;
    int on = kiss_persist_enabled() == 0;
    kiss_persist_apply(on);             // OFF also erases both stores
    store_u8("prst", (uint8_t)on);
    settings_reopen();
}

// ---- denomination ----
static void denom_cb(lv_event_t *e)
{
    (void)e;
    // Two values, so the chip IS the control. A dropdown offering exactly one
    // alternative is a switch wearing a list's clothes.
    kiss_settings_set_denom(wt_denom() == WT_DENOM_SATS ? WT_DENOM_BTC
                                                        : WT_DENOM_SATS);
    settings_reopen();
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

// The audit owns the display while it runs and hands back the same way the
// firmware screens do, by rebuilding Settings underneath.
static void audit_rng_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *parent = s_parent;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    kiss_rngaudit_open(parent, settings_reopen);
}

static void audit_back_cb(lv_event_t *e) { (void)e; settings_reopen(); }
static void audit_open_cb(lv_event_t *e);
static void made_back_cb(lv_event_t *e) { (void)e; audit_open_cb(NULL); }

// The label for the path that produced this seed, and the note under it. Every
// one of these already ships in 21 locales on the screen that OFFERED the
// choice, which is the right place to take them from: an owner reading this
// should see the words they picked, not a second vocabulary for the same act.
static void made_labels(int src, const char **label, const char **note)
{
    switch (src) {
        case WSEED_SRC_MIX:     *label = tr(STR_W_CHOOSE_MIX);
                                *note  = tr(STR_W_MIX_NOTE);      break;
        case WSEED_SRC_DICE:    *label = tr(STR_W_CHOOSE_DICE);
                                *note  = tr(STR_W_DICE_NOTE);     break;
        case WSEED_SRC_CARDS:   *label = tr(STR_W_CHOOSE_CARDS);
                                *note  = tr(STR_W_CARDS_NOTE);    break;
        // The caption reads MADE WITH, so an imported path names the THING it
        // came in as, not the button that fetched it: "MADE WITH / SCAN" is
        // not a sentence, and the note underneath already says it was made
        // somewhere else.
        case WSEED_SRC_RESTORE: *label = tr(STR_D_WORDS);
                                *note  = tr(STR_W_MADE_ELSE);     break;
        case WSEED_SRC_KEF:     *label = tr(STR_I_ROW_KEF);
                                *note  = tr(STR_W_MADE_ELSE);     break;
        // Includes the retired seed-QR source (kiss_seed.h): a device upgraded
        // across its removal still has that number in flash, and the honest
        // answer for a path this firmware no longer has is that it did not
        // record one.
        default:                *label = tr(STR_W_MADE_NOREC);
                                *note  = tr(STR_W_MADE_NONE);     break;
    }
}

// What went into the seed this device is holding. The one fact about a wallet
// an owner cannot recover by looking at the words, and until now the device
// knew it for the length of one screen and then forgot it on their behalf.
//
// The sources are drawn rather than listed: this is a fold, and the equation
// says so in the same shape the creation screen used, so an owner who saw it
// once recognises it here.
static void made_open_cb(lv_event_t *e)
{
    (void)e;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_scr = wt_screen(s_parent, tr(STR_W_MADE_T), tr(STR_W_MADE_S));

    int src = kiss_seed_source();
    const char *label = NULL, *note = NULL;
    made_labels(src, &label, &note);
    wt_value_card(s_scr, tr(STR_W_MADE_CAP), label, 48, 104, 704, false);

    // The legs, for the paths this device folded itself. An import has none to
    // show: the fold happened on somebody else's device and claiming otherwise
    // would be the screen inventing a provenance it does not have.
    int by = 232;
    if (src == WSEED_SRC_MIX || src == WSEED_SRC_DICE) {
        lv_obj_t *card = wt_card(s_scr, 48, 208, 704, 96);
        lv_obj_t *col = lv_obj_create(card);
        lv_obj_remove_style_all(col);
        lv_obj_set_pos(col, 0, 0);
        lv_obj_set_size(col, 704, 96);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *row = wt_diagram_row(col);
        if (src == WSEED_SRC_MIX) {
            // The four marks the WHY FOUR SOURCES card teaches, in its order.
            // Marks rather than words because the owner met them there and a
            // chip is not the place to re-explain a source (house rule 4).
            wt_chip(row, LV_SYMBOL_IMAGE, false);
            wt_diagram_op(row, "+");
            wt_chip(row, LV_SYMBOL_SETTINGS, false);
            wt_diagram_op(row, "+");
            wt_chip(row, LV_SYMBOL_OK, false);
            wt_diagram_op(row, "+");
            wt_chip(row, LV_SYMBOL_REFRESH, false);
        } else {
            wt_chip(row, tr(STR_D_ROLLS), false);
        }
        wt_diagram_op(row, LV_SYMBOL_RIGHT);
        wt_chip(row, tr(STR_D_KEYS), true);
        by = 320;
    }

    wt_why_body(s_scr, note, by, wt_accent(), true);
    lv_obj_set_ext_click_area(
        wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140,
                made_back_cb, NULL), 10);
}

// Two things to look at again, so the chooser comes back -- with a different
// second row. It is the room for "check a part of this signer", and how the
// keys were made is exactly that question asked about the past.
static void audit_open_cb(lv_event_t *e)
{
    (void)e;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_scr = wt_screen(s_parent, tr(STR_W_AUD_T), tr(STR_W_AUD_S));
    {
        int src = kiss_seed_source();
        const char *label = NULL, *note = NULL;
        made_labels(src, &label, &note);
        wt_row_x(s_scr, WT_ICON_KEY, tr(STR_W_MADE_T), label,
                 NULL, NULL, NULL, WT_INK, false,
                 WT_CHOICE_X, WT_CHOICE_Y(0), WT_CHOICE_W, WT_CHOICE_H,
                 made_open_cb, NULL);
    }
    wt_row_x(s_scr, LV_SYMBOL_SHUFFLE, tr(STR_W_RNG_T), tr(STR_W_RNG_S),
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

// ---- theme ----
// It spent a version as a wordless chip in the header, beside LANGUAGE and
// FIRMWARE, because those three belong to the DEVICE rather than to the keys
// in it. That grouping was right and it survives: all three are rows on the
// DEVICE tab now, where each one can state its value in words instead of
// standing for it with a swatch in 44px.
// It is also the row that makes the case for tapping in place loudest: the
// result of the pick is the PAGE, so a list floating over the page was hiding
// the only preview there is. Tap, and every mark on every tab is the new
// colour before the finger lifts.
static void theme_cb(lv_event_t *e)
{
    (void)e;
    wt_accent_set((wt_accent_get() + 1) % WT_ACC_N);
    store_u8("accent", (uint8_t)wt_accent_get());
    kiss_home_refresh();
    // The whole page takes the new accent, not one swatch. Every chevron on
    // every tab is accent inked, so a rebuild is both simpler and more honest
    // than repainting the control that was tapped.
    settings_reopen();
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    kiss_home_refresh();                // reflect any network change on the home badge
    // Both panes go NOW, synchronously, and take every animation on them with
    // them. The screen itself is dropped async, and the animation timer runs
    // in the same handler that will eventually free it -- so between this call
    // and the free there is a window with callbacks still pointing at rows,
    // and the idle auto-lock is exactly the thing that lands in the middle of
    // an entry.
    motion_stop();
    if (s_pane) { lv_obj_delete(s_pane); s_pane = NULL; }
    s_tabs = NULL;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
}

void kiss_settings_close(void) { close_cb(NULL); }   // idle auto-lock path

#ifdef SIMULATOR
// Rebuild the page in place, on whatever tab is open. The walk uses it to
// photograph a group in a state only a seam can produce -- flash encryption
// on, for instance, which changes the storage row's sub-line and the DEVICE
// tab's own fact and is applied at BUILD time, so nothing but a rebuild shows
// the other state. It replaced three per chooser hooks, one of which existed
// because the sim's hardcoded 0 meant one of two renders had never existed.
void kiss_settings_sim_reopen(void) { settings_reopen(); }
#endif

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

// ---- the page ----
//
// Five section tabs, one group on screen at a time. What it replaces: nine
// rows, four header controls and a three fact status footer on ONE 800x480
// page, in two 365px columns.
//
// The columns were the ceiling. 365px could not hold a label beside a value in
// twenty one locales, so every row stacked its label over its sub-line and the
// page could not be read down the value column at all -- which is the one way
// a settings page IS read. Tabs trade "everything visible" for "everything
// legible": a group gets the full 752px lane, a label sits beside its value on
// one line, and the row that used to ellipsise to "Duress w..." simply fits.
//
// What tabs cost is a caution hiding inside a collapsed group. That is bought
// back twice: a dot on the tab that holds it, and the attention chip on the
// action bar, which is visible from every tab and jumps to the flagged one.

// The device's own facts, which used to be a three line footer wedged into
// this page's action bar. They are a SCREEN now, because that is what they
// always were: a diagnostic somebody goes and looks at, not chrome every visit
// has to carry. The card slot's facts moved with them -- capacity and what is
// on it belong with the build id and the radio, not behind a storage picker.
static void device_back_cb(lv_event_t *e) { (void)e; settings_reopen(); }

static void device_open_cb(lv_event_t *e) { (void)e; device_screen(); }

static void device_screen(void)
{
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_scr = wt_screen(s_parent, tr(STR_I_DEVICE_T), NULL);

    // Framed, not floating. kiss_build_id_make draws three sibling labels at
    // font14 and nothing else; on an open page that is a bare paragraph, and
    // on a card it is a block of facts.
    wt_card(s_scr, 48, 104, 704, 96);
    kiss_build_id_make(s_scr, 72, 128, true, true);

    wt_row_wide(s_scr, 232, &(wt_wide_t){
        .label = tr(STR_W_SD_BTN),
        .sub   = tr(STR_I_CARD_SUB),
        .kind  = WT_WIDE_OPEN,
        .cb    = sdinfo_open_cb,
    });

    lv_obj_set_ext_click_area(
        wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140,
                device_back_cb, NULL), 10);
}

// ---- what wants reading ----
// Derived on every build, never stored. THREE conditions, and the chip counts
// every one of them: the paper has never been checked against this device, the
// recovery words sit in a flash this build does not encrypt, and no duress
// mark has been set.
//
// Duress used to be excluded, on the argument that a signer with no duress
// mark is the state every signer ships in and a permanent "1 NEEDS ATTENTION"
// teaches an owner to ignore the chip. The argument reads well and was wrong
// on glass: the dot lit anyway, so the page showed TWO dots over a chip saying
// one, and a number that disagrees with the marks beside it teaches an owner
// to ignore the chip far faster than a number that is merely unwelcome. A mark
// and a count are the same claim; they cannot have different rules.
//
// One case still reads oddly and is named rather than hidden: BACKUP holds two
// of the three conditions behind a single dot, so a device with both and no
// duress problem says "2 NEED ATTENTION" under one mark. A tab has one dot to
// give. Counting items and marking tabs is the trade, taken deliberately.
static bool backup_unchecked(void) { return !kiss_ui_backup_checked(); }

static bool words_unencrypted(void)
{
    return kiss_seed_mode() == WSEED_MODE_KEEP && !kiss_seed_flash_encrypted();
}

static bool duress_unset(void) { return !gw_stored_any(); }

static int attention_count(void)
{
    return (duress_unset()      ? 1 : 0)
         + (backup_unchecked()  ? 1 : 0)
         + (words_unencrypted() ? 1 : 0);
}

// The first tab carrying one, in strip order, so the chip lands on the leftmost
// mark and the owner works rightwards. It used to be hard coded to BACKUP on
// the strength of a comment saying both counted conditions lived there; that
// stopped being true the moment duress joined the count, and a chip that jumps
// past a lit dot is worse than one that does not move.
static int attention_tab(void)
{
    if (duress_unset()) return TAB_SECURITY;
    return TAB_BACKUP;
}

static void go_tab(int tab);

// The chip exists to point at the row that wants reading, and the pulse is the
// thing that points. Rebuilding the page paints it settled, so the one control
// on this page whose entire purpose is "show me the caution" was the only way
// of reaching a caution with the caution already sitting still. It takes the
// same road a tab tap takes now: the highlight slides to BACKUP, the group
// arrives, and the amber row flares once under the eye that just followed it.
static void attn_cb(lv_event_t *e)
{
    (void)e;
    const int want = attention_tab();
    if (s_tab == want) {
        // Already there, so nothing arrives: replaying the entry would slide a
        // group in from a side the finger never moved towards, to say
        // something the pulse says on its own. The chip asks WHICH ROW, and
        // the answer is the flare with no travel under it.
        if (!s_entering && s_pane) pane_point(s_pane);
        return;
    }
    go_tab(want);
}

static void go_tab(int tab)
{
    if (tab == s_tab) return;
    const int dir = tab > s_tab ? 1 : -1;
    const int from = s_tab;
    s_tab = tab;

    // A tab change no longer rebuilds the SCREEN, only the group. Nothing on
    // the strip or the action bar depends on which tab is open -- the dots and
    // the chip read the same three conditions -- so the
    // things that would have been rebuilt identically are simply left alone,
    // and the highlight has something continuous to slide along.
    help_close();                // an overlay does not outlive the group under it

    const bool was_moving = s_entering;
    motion_stop();
    if (was_moving && s_pane) {
        // Tapping faster than the page settles: the group that never finished
        // arriving is dropped outright rather than sent back out. Sliding a
        // row that has not appeared yet is a flicker, not a transition.
        lv_obj_delete(s_pane);
        s_pane = NULL;
    }

    s_pane_out = s_pane;
    s_pane = pane_new();
    build_tab();
    // The new rows were built after the page's own restyle() had already run,
    // so the accent flags on them have never been walked.
    wt_accent_restyle(s_pane);

    wt_tabs_select(s_tabs, from, tab, tab == TAB_NOUNDO);
    pane_enter(s_pane, dir, tab == TAB_NOUNDO);
    if (s_pane_out) pane_exit(s_pane_out, dir);
}

static void tab_cb(lv_event_t *e)
{
    go_tab((int)(intptr_t)lv_event_get_user_data(e));
}

// The explainer under a group: ONE line, at font23, in the page's own margin.
// Never two, and never smaller: a translation that does not fit on one line at
// this size is copy to shorten, not a paragraph to wrap. font14 here would be
// the page's only body text at the size the house rules keep for chip labels
// and row sublines.
static void group_note(int rows, int key)
{
    lv_obj_t *l = wt_lbl(s_pane, tr(key), WT_WIDE_X, WT_WIDE_EXPL_Y(rows),
                         wt_font23(), WT_MUT);
    lv_obj_set_width(l, WT_WIDE_W);
    lv_obj_set_height(l, lv_font_get_line_height(wt_font23()));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
}

static void tab_signer(void)
{
    int sc = kiss_script(), tn = kiss_testnet();

    wt_row_wide(s_pane, WT_WIDE_Y(0), &(wt_wide_t){
        .label   = tr(STR_I_ROW_NETWORK),
        .sub     = tr(tn ? STR_G_TESTNET_NOTE : STR_G_MAINNET_NOTE),
        // Amber on both test networks, in the sub AND in the value: the colour
        // says "these coins are not real" and the words say it again, so the
        // state never rests on colour alone.
        .sub_col = tn ? WT_WARN : WT_MUT,
        .kind    = WT_WIDE_CYCLE,
        .val     = kiss_net_name(),
        .vcol    = tn ? WT_WARN : WT_INK,
        .cb      = net_cb,
    });

    // "Native SegWit · BIP84": the name a reader met in their coordinator, and
    // the number the rest of the world calls it by. Composed rather than
    // translated -- a BIP number is not a word, and the middle dot is already
    // in every font this device ships.
    static const char *const BIPNO[3] = { "84", "49", "44" };   // by WSCRIPT_*
    char tsub[64];
    snprintf(tsub, sizeof tsub, "%s \xC2\xB7 BIP%s", type_name(sc),
             BIPNO[sc >= 0 && sc < 3 ? sc : 0]);
    lv_obj_t *trow = wt_row_wide(s_pane, WT_WIDE_Y(1), &(wt_wide_t){
        .label = tr(STR_I_ROW_TYPE),
        .sub   = tsub,
        .kind  = WT_WIDE_CYCLE,
        // The address PREFIX is the value, monospaced. That is the way round
        // it has to be, not a preference: bc1 is what an owner sees in their
        // coordinator, and "Native SegWit" is the name for it.
        .val   = type_prefix(sc, tn),
        .vf    = wt_font_mono23(),
        .cb    = type_cb,
    });
    wt_row_wide_help(trow, help_open_cb, NULL);

    char unit[16];
    const char *u = wt_denom_unit();
    size_t ui = 0;
    for (; u[ui] && ui + 1 < sizeof unit; ui++)
        unit[ui] = (u[ui] >= 'a' && u[ui] <= 'z') ? (char)(u[ui] - 32) : u[ui];
    unit[ui] = 0;
    wt_row_wide(s_pane, WT_WIDE_Y(2), &(wt_wide_t){
        .label = tr(STR_I_ROW_DENOM),
        .sub   = tr(STR_I_DENOM_SUB),
        .kind  = WT_WIDE_CYCLE,
        .val   = unit,
        .cb    = denom_cb,
    });

    group_note(3, STR_I_EXPL_SIGNER);
}

static void tab_security(void)
{
    // Duress unlock (kiss_duress.h), and it is UNCONDITIONAL. It was hidden in
    // a decoy session whenever a stroke was configured, so an attacker who knew
    // where to look could catch a coerced owner handing over the spare: the
    // row's absence was the confession. Showing it is safe only because the
    // unlock no longer forks on kiss_duress_real() either -- there is nothing
    // left for its presence to corroborate.
    //
    // It also has to be reachable. A signer whose only wallet has no
    // passphrase reports as the decoy on every session, so hiding the row
    // there meant the owner could never reach the setting again; that is
    // exactly how a test device ended up stuck with a stroke it could not
    // clear.
    bool set = !duress_unset();
    char dval[64];
    if (set) snprintf(dval, sizeof dval, "%s", tr(STR_GD_ON));
    else     snprintf(dval, sizeof dval, "%s  %s", LV_SYMBOL_WARNING,
                      tr(STR_GD_OFF));
    wt_row_wide(s_pane, WT_WIDE_Y(0), &(wt_wide_t){
        .label = tr(STR_I_ROW_WAYSIN),
        .sub   = tr(STR_I_WAYSIN_SHORT),
        .kind  = WT_WIDE_OPEN,
        .val   = dval,
        .vcol  = set ? WT_INK : WT_WARN,
        .sev   = set ? WT_SEV_PLAIN : WT_SEV_WARN,
        .cb    = duress_cb,
    });

    // Persist depends on storage. AMNESIC keeps nothing by contract, so the
    // switch has nothing to switch -- and it is drawn INERT with the reason
    // stated rather than hidden. A control that vanishes sends the owner
    // hunting for it; this is the one place on the page that deliberately
    // shows a dead one.
    bool amnesic = kiss_seed_mode() == WSEED_MODE_AMNESIC;
    wt_row_wide(s_pane, WT_WIDE_Y(1), amnesic
        ? &(wt_wide_t){
            .label = tr(STR_I_ROW_HISTORY),
            .sub   = tr(STR_I_PERSIST_DEAD_SUB),
            .kind  = WT_WIDE_INERT,
            .val   = tr(STR_I_PERSIST_DEAD_VAL),
          }
        : &(wt_wide_t){
            .label = tr(STR_I_ROW_HISTORY),
            // The sub follows the STATE rather than naming the feature: ON
            // says what is kept, OFF says that nothing is. It is the only
            // warning the flip gets, and it is on screen before the tap.
            .sub   = tr(kiss_persist_enabled() ? STR_I_HIST_SHORT
                                               : STR_I_POP_NOTHING),
            .kind  = WT_WIDE_CYCLE,
            .val   = tr(kiss_persist_enabled() ? STR_G_HIST_ON_BTN
                                               : STR_G_HIST_OFF_BTN),
            .cb    = persist_cb,
          });

    wt_row_wide(s_pane, WT_WIDE_Y(2), &(wt_wide_t){
        // Its own word, sentence case. STR_W_AUD_T is the audit SCREEN's
        // title and every title on this device is uppercase, which between
        // "Duress" and "Persist" reads as a row shouting.
        .label = tr(STR_I_ROW_AUDIT),
        .sub   = tr(STR_I_AUDIT_SUB),
        .kind  = WT_WIDE_OPEN,
        .cb    = audit_open_cb,
    });

    group_note(3, STR_I_EXPL_SECURITY);
}

static void tab_backup(void)
{
    bool ok = kiss_ui_backup_checked();
    char wsub[160], wval[16];
    if (ok) {
        uint8_t fp[4];
        kiss_ui_last_fp(fp);
        char idstr[16];
        snprintf(idstr, sizeof idstr, "%02X%02X%02X%02X",
                 fp[0], fp[1], fp[2], fp[3]);
        snprintf(wsub, sizeof wsub, tr(STR_I_WORDS_VERIFIED_FMT), idstr);
        snprintf(wval, sizeof wval, "%s", LV_SYMBOL_OK);
    } else {
        snprintf(wsub, sizeof wsub, "%s", tr(STR_I_WORDS_UNVERIFIED));
        snprintf(wval, sizeof wval, "%s", LV_SYMBOL_WARNING);
    }
    // A colour cue AND a glyph: in the GREEN theme the accent is byte
    // identical to WT_OK, so colour alone stops carrying meaning.
    wt_row_wide(s_pane, WT_WIDE_Y(0), &(wt_wide_t){
        .label   = tr(STR_I_ROW_WORDS),
        .sub     = wsub,
        .sub_col = ok ? WT_OK : WT_WARN,
        .kind    = WT_WIDE_OPEN,
        .val     = wval,
        .vcol    = ok ? WT_OK : WT_WARN,
        .sev     = ok ? WT_SEV_OK : WT_SEV_WARN,
        .cb      = words_cb,
    });

    int mode = kiss_seed_mode();
    int ssub = mode == WSEED_MODE_SD      ? STR_I_STORE_SD_SUB
             : mode == WSEED_MODE_AMNESIC ? STR_I_STORE_AMN_SUB
             : kiss_seed_flash_encrypted() ? STR_I_STORE_FLASH_ENC_SUB
                                           : STR_I_STORE_FLASH_SUB;
    // Amber CARD, not just an amber note. Words in a flash this build does not
    // encrypt is the one fact on the page a holder should catch without
    // reading anything.
    bool warn = words_unencrypted();
    wt_row_wide(s_pane, WT_WIDE_Y(1), &(wt_wide_t){
        .label   = tr(STR_I_ROW_STORAGE),
        .sub     = tr(ssub),
        .sub_col = warn ? WT_WARN : WT_MUT,
        .kind    = WT_WIDE_CHIP,   // a chevron: this is the pick that LEAVES
        .val     = storage_mode_name(mode),
        .sev     = warn ? WT_SEV_WARN : WT_SEV_PLAIN,
        .cb      = store_open_cb,
    });

    group_note(2, STR_I_EXPL_BACKUP);

    // WHICH keys. This group has two rows where the others have three or four,
    // so it ended at y=290 with 108px of glass doing nothing under it -- the
    // one tab that looked unfinished. What earns that band is not a third
    // setting invented to fill it: it is the subject the page was missing.
    // Every row here is about moving or checking a set of keys and none of
    // them said WHOSE, and the fingerprint is the only thing an owner can hold
    // against the paper already in their hand. The NO UNDO card makes exactly
    // this argument for its own badge.
    //
    // Framed and centred, on the value-card idiom every figure worth reading
    // off the glass already uses, so the same eight characters sit where they
    // sit on the fingerprint reveal and the pairing screen. STR_L_FP_CAP is
    // the caption those screens use and it already ships in 21 locales.
    {
        uint8_t fp[4];
        kiss_ui_last_fp(fp);
        char id[16];
        snprintf(id, sizeof id, "%02X%02X%02X%02X", fp[0], fp[1], fp[2], fp[3]);
        wt_value_card(s_pane, tr(STR_L_FP_CAP), id, 231, 302, 340, true);
    }
}

static void tab_device(void)
{
    // The pill is narrow, so strip the regional qualifier ("ESPAÑOL (ESPAÑA)"
    // -> "ESPAÑOL") and let the picker's flag carry the variant instead.
    int li = i18n_get_lang();
    const char *nat = i18n_lang_info(li)->native;
    if (li == I18N_NB) nat = "BOKMÅL";       // the flag already identifies Norway
    const char *par = strstr(nat, " (");
    char shortname[24];
    size_t n = par ? (size_t)(par - nat) : strlen(nat);
    if (n >= sizeof shortname) n = sizeof shortname - 1;
    memcpy(shortname, nat, n);
    shortname[n] = 0;
    // Twenty one items do not fit a popover, so this row keeps the full screen
    // picker: every name in its own language, with a flag, because somebody
    // stuck in a language they cannot read must still find the way back.
    wt_row_wide(s_pane, WT_WIDE_Y(0), &(wt_wide_t){
        .label = tr(STR_I_ROW_LANG),
        .sub   = tr(STR_I_LANG_SUB),
        .kind  = WT_WIDE_CHIP,   // a chevron: 21 of them need the screen
        // No per-language font here, unlike the picker's rows. This row names
        // the ACTIVE language, so the active locale's own face is already the
        // right script -- and wt_font14_for_lang would have pinned the SIZE
        // too, leaving this the one chip value at font14 in a column of 23s.
        .val   = shortname,
        .cb    = lang_open_cb,
    });

    // The sub used to append the accent's name to it -- "accent colour · MONO"
    // beside a chip already reading MONO, which is the copy rule's own example
    // of a string restating the value sitting next to it.
    wt_row_wide(s_pane, WT_WIDE_Y(1), &(wt_wide_t){
        .label  = tr(STR_I_ROW_THEME),
        .sub    = tr(STR_I_THEME_SUB),
        .kind   = WT_WIDE_CYCLE,
        .val    = wt_accent_name(),
        .swatch = true,
        .cb     = theme_cb,
    });

    // The version is a FACT, in the page's own ink. It was amber, with no
    // predicate behind it, so a device with nothing wrong wore a caution
    // colour on the one row that states what it is -- and amber on this page
    // means a dot and a count, both of which this row has never had.
    wt_row_wide(s_pane, WT_WIDE_Y(2), &(wt_wide_t){
        .label = tr(STR_I_ROW_FW),
        .sub   = tr(STR_I_FW_SUB),
        .kind  = WT_WIDE_OPEN,
        .val   = KISS_VERSION_STR,
        .vf    = wt_font_mono23(),
        .cb    = fw_open_cb,
    });

    // The sub says what the row OPENS. It carried the encryption state and the
    // C6 radio pad instead, two diagnostics in untranslated ASCII, and both are
    // already printed by kiss_build_id_make on the screen this row leads to --
    // so the row spent its whole lane restating the page behind it and never
    // once said that the card is back there. It was reported from the bench in
    // exactly those terms: nothing in the button hints anything about the card.
    //
    // The amber went with them. It hung off !kiss_seed_flash_encrypted() while
    // the attention chip counts words_unencrypted(), which additionally wants
    // WSEED_MODE_KEEP -- so on SD or AMNESIC storage this row went amber with
    // no dot on the strip and nothing in the count, which is the same fault as
    // a chip disagreeing with its dots. The BACKUP tab owns that fact, marks it
    // and counts it. One place.
    wt_row_wide(s_pane, WT_WIDE_Y(3), &(wt_wide_t){
        .label   = tr(STR_I_ROW_DEVICE),
        .sub     = tr(STR_I_ROW_DEVICE_SUB),
        .kind    = WT_WIDE_OPEN,
        .cb      = device_open_cb,
    });

    // No explainer: the fourth row already reaches 384 and the line would land
    // in the action bar.
}

static void tab_noundo(void)
{
    // The page itself reddens under the group, behind everything in it. NO
    // UNDO is not a peer of the other four tabs and this is what says so from
    // across the room, before a word of the card below has been read. Faint on
    // purpose: at opa 13 it is the same weight as a severity tint, so it
    // colours the page without competing with the card that carries the
    // decision.
    lv_obj_t *wash = lv_obj_create(s_pane);
    lv_obj_remove_style_all(wash);
    lv_obj_set_pos(wash, 8, 118);
    lv_obj_set_size(wash, 784, WT_CONTENT_BOTTOM - 118);
    lv_obj_set_style_bg_color(wash, WT_STOP, 0);
    // 30 and not the prototype's 13. That number is a CSS alpha over a CSS
    // background; on this panel, over WT_BG, it lands at (8,12,16) against
    // (0,8,16) -- eight levels in a five bit red channel, which is nothing.
    // Measured off the frame, because looking at the frame is the only check
    // that catches a colour doing no work. At 30 the band around the card is a
    // dark red field and the group reads as different before a word of it does.
    lv_obj_set_style_bg_opa(wash, 30, 0);
    lv_obj_remove_flag(wash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(wash, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(wash, (void *)SET_SKIP_TAG);   // scenery, not a row

    // ONE card, because a destructive action deserves its reason on the same
    // screen as its button. This group holds exactly one thing: storage and
    // duress are not destructive and are not in it.
    lv_obj_t *card = wt_card(s_pane, WT_WIDE_X, WT_WIDE_Y(0), WT_WIDE_W, 258);
    wt_row_sev(card, WT_SEV_STOP);

    // WT_STOP_INK rather than WT_STOP: full stop red on a stop tinted card is
    // the one pairing on this page that vibrates.
    lv_obj_t *head = wt_lbl(card, tr(STR_I_ROW_ENDWORDS), 24, 22, wt_font28(),
                            WT_STOP_INK);

    // WHICH keys, named, on the tab rather than only on the confirmation
    // behind it. Every route into Settings has an open session under it, so
    // this is THIS signer's fingerprint and an owner can hold it against the
    // card in their hand before they ever reach the hold -- the one check the
    // confirmation cannot make on their behalf.
    //
    // A badge and not a value card: it sits BESIDE the heading rather than
    // taking a row of its own, which is what the drawn card has room for.
    {
        uint8_t fp[4];
        kiss_ui_last_fp(fp);
        char id[16];
        snprintf(id, sizeof id, "%02X%02X%02X%02X", fp[0], fp[1], fp[2], fp[3]);
        lv_point_t hs;
        lv_text_get_size(&hs, tr(STR_I_ROW_ENDWORDS), wt_font28(), 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_obj_t *chip = wt_state_chip(card, id, WT_STOP);
        lv_obj_update_layout(chip);
        lv_obj_set_pos(chip, 24 + hs.x + 16,
                       22 + (lv_font_get_line_height(wt_font28())
                             - lv_obj_get_height(chip)) / 2);
    }
    (void)head;

    const int by = 22 + lv_font_get_line_height(wt_font28()) + 12;
    const int bh = 258 - by - 24 - WT_ACTION_H - 20;
    lv_obj_t *b = wt_lbl(card, tr(STR_I_ERASE_B), 24, by,
                         wt_body_font(tr(STR_I_ERASE_B), 600, bh), WT_INK);
    lv_obj_set_width(b, 600);
    lv_obj_set_height(b, bh);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);

    // The button, and NO HOLD on it. The hold stays where it already is, on
    // the confirmation behind it: two gates in a row teaches an owner to grind
    // through both, and the screen that names the fingerprint is the one worth
    // holding on.
    char blab[WT_ICON_TEXT_MAX];
    wt_icon_text(blab, sizeof blab, LV_SYMBOL_TRASH, tr(STR_I_ERASE_BTN));
    lv_point_t bs;
    lv_text_get_size(&bs, blab, wt_font23(), 1, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    const int bw = bs.x + 48;
    const int byy = 258 - 24 - WT_ACTION_H;
    lv_obj_t *btn = lv_obj_create(card);
    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, 24, byy);
    lv_obj_set_size(btn, bw, WT_ACTION_H);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_bg_color(btn, WT_STOP, 0);
    lv_obj_set_style_bg_opa(btn, 26, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, WT_STOP, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    wt_tap_feedback(btn);
    lv_obj_add_event_cb(btn, endwords_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = wt_lbl(btn, blab, 0, 0, wt_font23(), WT_STOP_INK);
    lv_obj_set_style_text_letter_space(bl, 1, 0);
    lv_obj_center(bl);

    lv_obj_t *cap = wt_lbl(card, tr(STR_I_ERASE_CAP), 24 + bw + 14, 0,
                           wt_font14(), WT_DIM);
    lv_obj_set_y(cap, byy + (WT_ACTION_H - lv_font_get_line_height(wt_font14())) / 2);
}

// The group the strip is pointing at, drawn into whatever pane is current.
static void build_tab(void)
{
    switch (s_tab) {
    case TAB_SECURITY: tab_security(); break;
    case TAB_BACKUP:   tab_backup();   break;
    case TAB_DEVICE:   tab_device();   break;
    case TAB_NOUNDO:   tab_noundo();   break;
    default:           tab_signer();   break;
    }
}

void kiss_settings_open(lv_obj_t *parent)
{
    if (s_scr) return;
    s_parent = parent;
    // An overlay open when the screen died was deleted with it; the handles
    // must not survive to block the next open, or to be animated after it.
    s_help = NULL;
    s_pane = s_pane_out = s_tabs = NULL;
    s_entering = false;
    if (s_tab < 0 || s_tab >= TAB_N) s_tab = TAB_SIGNER;
    s_scr = wt_screen(parent, tr(STR_G_T), NULL);

    // Nothing else lives in the header. The FIRMWARE, LANGUAGE and theme pills
    // that used to sit at y=18 are rows on the DEVICE tab, so the title has the
    // whole 704px lane back -- which is what wt_screen already fits it to, and
    // why the narrowed wt_title_fit this page used to make is gone.

    // The dots are what a collapsed group costs, paid back. Every condition
    // the attention chip counts lights the dot on the tab that holds it:
    // SECURITY while no duress mark is set, BACKUP for either of its two. The
    // chip counts conditions and a tab has one dot, so BACKUP holding both is
    // the one state where the number is larger than the marks.
    const wt_tab_t tabs[TAB_N] = {
        { WT_ICON_KEY,        tr(STR_I_TAB_SIGNER),   false,          false },
        { WT_ICON_SHIELD,     tr(STR_I_TAB_SECURITY), duress_unset(), false },
        { LV_SYMBOL_SAVE,     tr(STR_I_TAB_BACKUP),
          backup_unchecked() || words_unencrypted(), false },
        { LV_SYMBOL_SETTINGS, tr(STR_I_TAB_DEVICE),   false,          false },
        { LV_SYMBOL_TRASH,    tr(STR_I_SEC_NO_UNDO),  false,          true  },
    };
    s_tabs = wt_tabs(s_scr, tabs, TAB_N, s_tab, WT_WIDE_X, 68, tab_cb);
    lv_obj_add_event_cb(s_tabs, tabs_gone, LV_EVENT_DELETE, NULL);

    // The group lives in a pane of its own so that a tab change can hold TWO
    // of them for the 200ms the outgoing one takes to leave. Built here and
    // not animated: walking in from home is not a tab change, and neither is
    // the rebuild every value chip does when it is tapped.
    s_pane = pane_new();
    build_tab();

    // BACK takes the bottom RIGHT corner, in the standard 140x52 pill every
    // other lone-exit screen uses, and it is what builds the action bar the
    // attention chip stands on.
    wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, close_cb, NULL);

    // Opposite it, and only when there is something to say. No "all good" chip:
    // a badge that is always there is a badge nobody reads.
    int n = attention_count();
    if (n > 0) {
        char lab[64];
        if (n == 1) snprintf(lab, sizeof lab, "%s", tr(STR_I_ATTN_1));
        else        snprintf(lab, sizeof lab, tr(STR_I_ATTN_N_FMT), n);
        wt_alert_chip(s_scr, lab, attn_cb, NULL);
    }

    restyle();
}
