// Settings: network selection. Style matches the other wallet screens.
// The network flips instantly (the master key is network-free) and persists
// across power cycles via NVS on the device.
#include "kiss_settings.h"
#include "kiss_terms.h"

#include <stdio.h>
#include <string.h>

#include "flag_imgs.h"   // language-picker flags (Twemoji, CC-BY; en has none)
#include "i18n.h"
#include "kiss_crypto.h"
#include "kiss_info.h"
#include "kiss_fw_ui.h"   // the firmware row opens it
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
// The address type help card, or NULL. It is a scrim plus a box, and this is
// the SCRIM: it is what has to be deleted, and deleting the box alone would
// leave a full-screen catcher eating every tap on the page. The only overlay
// left on this page -- it explains, it does not choose.
static lv_obj_t *s_help;

// ---- the page moves ----------------------------------------------------
// The machinery is wt_pane_* in kiss_theme.c: it was five statics and a tag in
// this file until RECOVERY WORDS wanted the same chrome, and every line of it
// was already page-agnostic. What stayed here is the part that is about
// SETTINGS -- which groups exist, and which one is destructive.
static wt_pane_t s_pane_ctx;
// Short names for the four fields this file reads on nearly every screen. The
// context is the thing that moved; the names are the ones the page has always
// used, and forty call sites reading s_pane_ctx.pane would say less.
//
// s_tab has to SURVIVE settings_reopen(): every pick rebuilds the screen, so
// without it a network change would throw the owner back to tab one. It lives
// in the context, and the context is a static, so it does.
#define s_pane      s_pane_ctx.pane
#define s_tabs      s_pane_ctx.tabs
#define s_tab       s_pane_ctx.tab
// The [ ? ] tab, open: the content lane replaced by the page's explainer.
// Cleared by any real tab tap, so the strip is also the way back.
static bool s_what_open;
#define s_entering  s_pane_ctx.entering

static void build_tab(void);   // the group the strip points at, into s_pane

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
// One ellipsis GLYPH, not three dots: two glyphs of mono28 the value does
// not spend are two the type sub keeps, and "Native SegWit · BIP84" fits
// its lane by exactly that margin. KEYS' folded address uses the same glyph.
static const char *type_prefix(int sc, int tn)
{
    switch (sc) {
    case WSCRIPT_LEGACY: return tn ? "m/n\xE2\x80\xA6" : "1\xE2\x80\xA6";
    case WSCRIPT_NESTED: return tn ? "2\xE2\x80\xA6"   : "3\xE2\x80\xA6";
    default:             return tn ? "tb1\xE2\x80\xA6" : "bc1\xE2\x80\xA6";
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

// The home's next-step hint names the one job this signer has not done, and
// checking a paper copy lives on the BACKUP tab. A named entry rather than an
// exported tab enum: one caller needs one destination, and a magic int in a
// header is a thing to get wrong later.
void kiss_settings_open_backup(lv_obj_t *parent)
{
    s_tab = TAB_BACKUP;
    kiss_settings_open(parent);
}

bool kiss_settings_active(void) { return s_scr != NULL; }

static void store_u8(const char *key, uint8_t v);
// The [ ? ] first-run hint's one byte: written the first time the tab is
// opened, anywhere, so the breathing stops for good. The theme owns the RAM
// bool and this hook is how the moment reaches the store without the kit
// including nvs.h -- registered once, at load, beside the accent it mirrors.
// Device only: the sim's load returns before the hook would register, which
// is also what keeps every walk starting from the first-run state.
#ifndef SIMULATOR
static void help_seen_persist(void) { store_u8("hlps", 1); }
static void row_seen_persist(void)  { store_u8("rows", 1); }
#endif

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

// The decoy's high score. Deliberately NOT part of kiss_settings_load's
// gate: a game score that will not read is a cosmetic loss and must never
// be the reason a boot lands on STORAGE LOCKED. Read opportunistically,
// default 0. Respects the persist switch like every other byte here.
uint16_t kiss_game_best_load(void)
{
#ifndef SIMULATOR
    nvs_handle_t h;
    uint16_t v = 0;
    if (nvs_open("kiss", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u16(h, "gbst", &v) != ESP_OK) v = 0;
        nvs_close(h);
    }
    return v;
#else
    return 0;
#endif
}

void kiss_game_best_store(uint16_t best)
{
#ifndef SIMULATOR
    if (!kiss_persist_enabled()) return;
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, "gbst", best);
        nvs_commit(h);
        nvs_close(h);
    }
#else
    (void)best;
#endif
}

// ---- the terms an owner has read: the STORE half --------------------------
// The mask lives in kiss_terms.c; this is the u16 it comes back from. Read
// opportunistically and outside the boot gate, like the game score: a mask
// that will not read costs an owner a second reading of a word, and must
// never be the reason a boot lands on STORAGE LOCKED.
uint16_t kiss_terms_read_load(void)
{
#ifndef SIMULATOR
    nvs_handle_t h;
    uint16_t v = 0;
    if (nvs_open("kiss", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u16(h, "trms", &v) != ESP_OK) v = 0;
        nvs_close(h);
    }
    return v;
#else
    return 0;
#endif
}

void kiss_terms_read_store(uint16_t mask)
{
#ifndef SIMULATOR
    if (!kiss_persist_enabled()) return;
    nvs_handle_t h;
    if (nvs_open("kiss", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, "trms", mask);
        nvs_commit(h);
        nvs_close(h);
    }
#else
    (void)mask;
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
    uint8_t hs = 0;               // [ ? ] never opened until a byte says it was
    uint8_t rs = 0;               // ...and the same for a row that grows
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
                  get_optional_u8(h, "prst", &ps) &&
                  get_optional_u8(h, "hlps", &hs) &&
                  get_optional_u8(h, "rows", &rs);
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
    wt_help_seen_set(hs != 0);
    wt_help_seen_hook(help_seen_persist);
    wt_row_seen_set(rs != 0);
    wt_row_seen_hook(row_seen_persist);
    kiss_terms_set_mask(kiss_terms_read_load());
    kiss_terms_persist_hook(kiss_terms_read_store);
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
    // The chrome title is INK by contract -- the cursor is the accent's one
    // appearance in the header -- so the walk is the whole job now.
    wt_accent_restyle(s_scr);
}

// The next build is a rebuild the FINGER caused -- a value cycle repainting
// the pane it stands on -- so the def list arrives settled instead of
// replaying its welcome. Set around kiss_settings_open by settings_reopen
// only; a walk in from home stays a real entry.
static bool s_still;

static void settings_reopen(void)
{
    lv_obj_t *parent = s_parent;
    // The screen goes async, so for one handler pass the OLD page is still up
    // while the new one is being built over it. Anything still moving on it
    // would be moving objects the statics no longer name.
    wt_pane_stop(&s_pane_ctx);
    if (s_pane) { lv_obj_delete(s_pane); s_pane = NULL; }
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_still = true;
    kiss_settings_open(parent);      // s_tab survives, deliberately
    s_still = false;
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
    // The outcome shape: the lamp carries the verdict and the headline names
    // the move, so the title stays in the page's own ink. A failure here is
    // RETRYABLE -- nothing moved -- so it is amber, never the stop red the
    // old title wore: red is the irreversible, and this is its opposite.
    const char *title, *head, *body;
    bool ok = false;
    char formatted[512];

    if (rc == WSEED_OK) {
        ok = true;
        title = tr(STR_G_STORAGE_OK_T);
        if (target == WSEED_MODE_AMNESIC) {
            head = tr(STR_G_STORAGE_OK_NEXT_AMN);
            body = tr(STR_G_STORAGE_OK_AMNESIC_B);
        } else {
            head = tr(STR_G_STORAGE_OK_NEXT);
            snprintf(formatted, sizeof formatted, tr(STR_G_STORAGE_OK_FMT),
                     storage_mode_name(target));
            body = formatted;
        }
    } else if (rc == WSEED_ERR_CLEANUP) {
        // The backend contract is precise here: destination committed and
        // verified, old-source cleanup failed. Do not say "not changed" and do
        // not claim one-copy storage.
        title = tr(STR_G_STORAGE_CLEANUP_T);
        head  = tr(STR_G_STORAGE_CLEANUP_NEXT);
        body  = tr(STR_G_STORAGE_CLEANUP_B);
    } else {
        title = tr(STR_G_STORAGE_FAIL_T);
        head  = tr(STR_G_STORAGE_FAIL_NEXT);
        if (rc == WSEED_ERR_SD_MISSING || rc == WSEED_ERR_SD_IO ||
            rc == WSEED_ERR_SD_CORRUPT)
            body = tr(STR_G_STORAGE_FAIL_CARD_B);
        else if (rc == WSEED_ERR_VERIFY)
            body = tr(STR_G_STORAGE_FAIL_VERIFY_B);
        else
            body = tr(STR_G_STORAGE_FAIL_GENERIC_B);
    }

    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_scr = wt_chrome(s_parent, title);
    wt_outcome_t o = { .headline = head, .para = body, .ok = ok };
    wt_outcome(s_scr, &o);
    // A lone acknowledge is still the way off the screen, and it LEAVES, so
    // its arrow leads.
    wt_arrow_action(s_scr, tr(STR_C_OK), true, false, 592, WT_ACTION_Y, 160,
                    true, storage_result_ack_cb, NULL);
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
    // The gate shape, amber for all three targets: a storage move is a
    // caution the owner can walk back right up to the hold, never the erase's
    // red. The shipped body's first clause is the paragraph; its caution
    // clause rides the warn slot where it has one; and the shape's two
    // captions carry what the paragraphs used to say at length -- the paper
    // words survive everything, the old stored copy does not.
    const bool amn = target == WSEED_MODE_AMNESIC;
    const char *body = target == WSEED_MODE_SD
                     ? tr(STR_G_STORAGE_CONFIRM_SD_B)
                     : amn ? tr(STR_G_STORAGE_CONFIRM_AMNESIC_B)
                           : tr(STR_G_STORAGE_CONFIRM_FLASH_B);
    char para[384];
    snprintf(para, sizeof para, "%s", body);
    char *cut = strstr(para, "\n\n");
    const char *note = NULL;
    if (cut) {
        *cut = '\0';
        if (target != WSEED_MODE_KEEP) {
            note = cut + 2;
            char *cut2 = strstr(cut + 2, "\n\n");
            if (cut2) *cut2 = '\0';
        }
    }

    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_scr = wt_chrome(s_parent, tr(STR_G_STORAGE_CONFIRM_T));
    char trail[96];
    snprintf(trail, sizeof trail, "%s / %s", tr(STR_G_T),
             tr(STR_I_TAB_BACKUP));
    wt_trail(s_scr, LV_SYMBOL_SAVE, trail, false);

    wt_gate_t g = {
        .mark     = target == WSEED_MODE_SD ? WT_ICON_SD : LV_SYMBOL_SAVE,
        .sentence = tr(target == WSEED_MODE_SD ? STR_G_STOGATE_SENT_SD
                       : amn ? STR_G_STOGATE_SENT_AMN
                             : STR_G_STOGATE_SENT_FLASH),
        .para     = para,
        .warn     = note,
        .surv = tr(STR_G_STOGATE_SURV),
        .goes     = tr(amn ? STR_G_STOGATE_GOES_AMN : STR_G_STOGATE_GOES),
        .stop     = false,
    };
    wt_gate(s_scr, &g);

    wt_slide_rule_c(s_scr,
                    tr(amn ? STR_G_STORAGE_HOLD_AMNESIC
                           : STR_G_STORAGE_HOLD_MOVE),
                    tr(STR_G_FW_KEEP_HOLDING), WT_ACT_X, WT_ACTION_Y_SLIDE, 330,
                    wt_accent(), wt_accent(), storage_apply,
                    (void *)(intptr_t)target);
    wt_arrow_action(s_scr, tr(STR_C_CANCEL), true, false, 592, WT_ACTION_Y,
                    160, true, storage_confirm_cancel_cb, NULL);
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
        wt_chrome_head(s_scr);
        wt_trail(s_scr, WT_ICON_SD, tr(STR_I_DEVICE_T), false);
        // An EMPTY STATE, not a pair of claims: the firmware page's six of
        // these are a mark, a headline and a line of what to do, and an
        // empty slot on this page is the same thing. The pair that was here
        // put "no card" in a coloured column beside a column describing a
        // screen that is not on the glass.
        lv_obj_t *g = wt_lbl(s_scr, LV_SYMBOL_WARNING, WT_LANE_X, 124,
                             wt_font23(), WT_WARN);
        lv_obj_update_layout(g);
        lv_obj_t *h = wt_lbl(s_scr, tr(STR_G_FW_NOCARD_H),
                             WT_LANE_X + lv_obj_get_width(g) + 14, 118,
                             wt_font28(), wt_ink_for(WT_WARN));
        lv_obj_add_flag(h, WT_FLAG_ACCENT);
        lv_obj_t *b = wt_note(s_scr, tr(STR_G_FW_NOCARD_B), WT_LANE_X, 176,
                              WT_LANE_W, 60);
        lv_obj_set_style_text_color(b, WT_MUT, 0);
        // ...and one row saying what the screen behind the empty slot is for,
        // so an owner who opened it by mistake knows what they were after.
        wt_fact_t facts[1] = {
            { .cap = tr(STR_G_SD_ABOUT_H), .val = tr(STR_G_SD_ABOUT_B),
              .icon = WT_ICON_SD },
        };
        wt_facts(s_scr, 260, facts, 1);
        wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160, true, sdinfo_back_cb, NULL);
        return;
    }

    // The CID product name is the card introducing itself; it rides the trail
    // as the path's last element -- the same shape the word grid gives the
    // fingerprint -- so the title stays the word the chooser promised.
    s_scr = wt_screen(s_parent, tr(STR_W_SD_BTN), NULL);
    wt_chrome_head(s_scr);
    {
        char trail[96];
        snprintf(trail, sizeof trail, "%s / %s", tr(STR_I_DEVICE_T),
                 inf.name);
        wt_trail(s_scr, WT_ICON_SD, trail, false);
    }

    // The unit ONCE when both numbers carry the same one, which is how a person
    // says it and what keeps the pair inside the card. mono28 with its letter
    // spacing is 18.8px per character in a 333px box: seventeen characters to a
    // line. "1.2 GB / 29.7 GB" is sixteen and fits; "123.4 GB / 256.0 GB" is
    // nineteen and wraps to two lines on any card of 128 GB or more, which no
    // simulated card is -- so it has never been seen here.
    //
    // The caption is this screen's own, not the firmware screen's. G_FW_ON_CARD
    // means the VERSION on the card and is shared with kiss_fw_ui.c; over a
    // byte pair it read "ON THE CARD" and said neither "used" nor "of".
    uint64_t used = inf.total_bytes - inf.free_bytes;
    char a[24], b[24], val[52];
    wt_fmt_bytes(used, a, sizeof a);
    wt_fmt_bytes(inf.total_bytes, b, sizeof b);
    char *ua = strrchr(a, ' '), *ub = strrchr(b, ' ');
    if (ua && ub && strcmp(ua, ub) == 0) {
        *ua = 0;                                  // "123.4" / "256.0 GB"
        snprintf(val, sizeof val, "%s / %s", a, b);
    } else {
        snprintf(val, sizeof val, "%s / %s", a, b);
    }
    wt_value_card(s_scr, tr(STR_G_SD_SPACE_CAP), val,
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
    // The PRESENT branch used to borrow W_SD_MISSING_S -- "your words are kept
    // on the SD card" -- which is a sentence from the MISSING family stating
    // the storage mode, under a row whose tick already says the file is here.
    // It also says bare "words", which the glossary calls a house term a reader
    // has to unlearn. It says what is true of the file instead: sealed to this
    // signer, and no other.
    if (kiss_seed_mode() == WSEED_MODE_SD) {
        size_t len = 0;
        platform_sd_file *f = platform_sd_open(SDSEED_FILENAME, &len);
        bool present = f != NULL;
        if (f) platform_sd_close(f);
        lv_obj_t *row = wt_row_x(s_scr, WT_ICON_KEY, SDSEED_FILENAME,
                                 tr(present ? STR_G_SD_ROW_WORDS_HERE
                                            : STR_G_SD_ROW_WORDS_MISSING),
                                 NULL,
                                 present ? LV_SYMBOL_OK : LV_SYMBOL_WARNING,
                                 NULL, present ? WT_OK : WT_WARN, false,
                                 WT_LIST_R_X, WT_LIST_Y(3), WT_LIST_W,
                                 WT_ROW_H, NULL, NULL);
        // The badge stays MUTED on this one row. The card already carries a
        // severity tint and the value slot already carries a tick or a warning
        // sign, and on GREEN the accent is WT_OK to the byte -- so an accented
        // key here would be a third mark saying the same thing in the same
        // colour, on the row that reports whether the sealed file is present.
        wt_row_icon_mute(row);
        wt_row_sev(row, present ? WT_SEV_OK : WT_SEV_WARN);
        if (!present) wt_row_sub_color(row, WT_WARN);
    }

    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160, true, sdinfo_back_cb, NULL);
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

    // nf is the note beside each type, and the note is the whole answer this
    // card exists to give -- "which one" as much as "what are they". It was
    // font14, which is the size this page keeps for marks.
    const lv_font_t *hf = wt_font23(), *nf = wt_font23(),
                    *pf = wt_font_mono23();
    const int hh = lv_font_get_line_height(hf);
    const int rh = lv_font_get_line_height(pf);
    const int h  = 16 + hh + 12 + 3 * rh + 2 * 10 + 16;

    // CENTRED, both ways, and WIDER. It was 480 wide starting at x=250, which
    // put it 250..730 on an 800px screen -- off centre by 90px -- and left the
    // note beside each type a 268px lane, so "older apps accept it" shipped as
    // "older apps accep...". Both came back from the bench in one breath: "it
    // is not centered and not all text can be read its cutoff".
    //
    // 640 wide at x=80 is centred by construction, and the extra 160px all
    // goes to the note lane, which is the half of the card that answers WHICH
    // ONE. Vertically it sits in the middle of the content area rather than
    // hanging by its feet off the action row.
    const int W = 640, X = (800 - W) / 2;
    const int Y = WT_LANE_Y + (WT_CONTENT_BOTTOM - WT_LANE_Y - h) / 2;
    lv_obj_t *box = wt_overlay_box(s_scr, &s_help, X, Y, W, h, 12,
                                   help_close_cb);

    wt_lbl(box, tr(STR_I_BIP_T), 20, 16, hf, WT_INK);
    // CLOSE IS A CONTROL. It was a bare label with no handler at all, so the
    // one word on the card that says how to leave did nothing when pressed --
    // "CLOSE does not actually work and one has to tap away from popup". A
    // word action, mark trailing, on the same callback the scrim carries.
    lv_obj_t *cl = wt_word_action(box, NULL, tr(STR_I_BIP_CLOSE), false,
                                  WT_MUT, false, help_close_cb, NULL);
    lv_obj_update_layout(cl);
    lv_obj_align(cl, LV_ALIGN_TOP_RIGHT, -20,
                 16 + (hh - lv_obj_get_height(cl)) / 2);
    lv_obj_set_ext_click_area(cl, 10);

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
        lv_obj_set_width(nt, W - (20 + 76 + 96) - 20);
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

    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_scr = wt_screen(s_parent, tr(STR_G_STORAGE_SEC), NULL);
    wt_chrome_head(s_scr);
    // The trail matches the confirm gate one level deeper. "current: X" is
    // gone from the header: the accent tick on the row already says which
    // mode this signer is on, and a subtitle restating a value beside it is
    // the copy rule's first cut.
    {
        char trail[96];
        snprintf(trail, sizeof trail, "%s / %s", tr(STR_G_T),
                 tr(STR_I_TAB_BACKUP));
        wt_trail(s_scr, LV_SYMBOL_SAVE, trail, false);
    }

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
    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160, true, store_back_cb, NULL);
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
        case WSEED_SRC_COIN:    *label = tr(STR_W_COIN);
                                *note  = tr(STR_W_COIN_NOTE);     break;
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
    // No subtitle: "what went into the keys this signer holds" restated the
    // title one line under it, which is the copy rule's first cut.
    s_scr = wt_screen(s_parent, tr(STR_W_MADE_T), NULL);
    wt_chrome_head(s_scr);
    {
        char trail[96];
        snprintf(trail, sizeof trail, "%s / %s", tr(STR_G_T),
                 tr(STR_W_AUD_T));
        wt_trail(s_scr, WT_ICON_KEY, trail, false);
    }

    int src = kiss_seed_source();
    const char *label = NULL, *note = NULL;
    made_labels(src, &label, &note);
    wt_value_card(s_scr, tr(STR_W_MADE_CAP), label, 48, 104, 704, false);

    // The legs, for the paths this device folded itself. An import has none to
    // show: the fold happened on somebody else's device and claiming otherwise
    // would be the screen inventing a provenance it does not have.
    int by = 232;
    if (src == WSEED_SRC_MIX || src == WSEED_SRC_DICE || src == WSEED_SRC_COIN) {
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

    wt_body_para(s_scr, note, by);
    lv_obj_set_ext_click_area(
        wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160, true, made_back_cb, NULL), 10);
}

// Two things to look at again, so the chooser comes back -- with a different
// second row. It is the room for "check a part of this signer", and how the
// keys were made is exactly that question asked about the past.
static void audit_open_cb(lv_event_t *e)
{
    (void)e;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    // The dropped subtitle's first clause restated the two rows below it; its
    // second clause is the page's real claim and stands in the band now.
    s_scr = wt_screen(s_parent, tr(STR_W_AUD_T), NULL);
    wt_chrome_head(s_scr);
    {
        char trail[96];
        // The tab it was opened FROM, not a fixed one: the row sits on BACKUP
        // as well now, and a trail naming the other tab sends an owner back
        // through a page they were never on.
        snprintf(trail, sizeof trail, "%s / %s", tr(STR_G_T),
                 tr(s_tab == TAB_BACKUP ? STR_I_TAB_BACKUP
                                        : STR_I_TAB_SECURITY));
        wt_trail(s_scr, WT_ICON_SHIELD, trail, false);
    }
    // The standing claim in the ACCENT, not WT_DIM. It is the one thing this
    // page exists to promise -- nothing an audit touches leaves the device --
    // and at WT_DIM the bench could not see the dot at all: "shouldn't the
    // fucking dot next to NOTHING LEAVES THIS DEVICE have the theme color
    // because it's barely noticeable". A claim is not furniture.
    wt_standing(s_scr, tr(STR_W_AUD_STAND), wt_accent(), false);
    {
        // THE SAME SHAPE AS THE ROW BELOW IT. These two had opposite
        // hierarchies: this one put its SUBJECT in the small caption and the
        // ANSWER ("DICE", "CAMERA AND TAPS") in the big value, while the one
        // under it did the reverse -- so the bench read the answer as the more
        // important of the two and asked why. "why the fuck is the CAMERA AND
        // TAPS text larger than the HOW YOUR KEYS WERE MADE text??!!"
        //
        // Subject in the label at font23, answer in the VALUE lane on the
        // right, sub-line at font23 under it. Both rows read the same way
        // down the page now.
        int src = kiss_seed_source();
        const char *label = NULL, *note = NULL;
        made_labels(src, &label, &note);
        (void)note;
        wt_row_x(s_scr, WT_ICON_KEY, tr(STR_W_MADE_T), NULL, NULL,
                 label, wt_font23(), WT_INK, false,
                 WT_CHOICE_X, WT_CHOICE_Y(0), WT_CHOICE_W, WT_CHOICE_H,
                 made_open_cb, NULL);
    }
    // font23 on the sub, stated. NULL takes the fit ladder, and a 96px row
    // affords one line under its label -- which is how this shipped at font14
    // and came back as "text below RANDOMNESS AUDIT is fucking tiny and
    // should be!!! Who the fuck can read that???". The copy is one line now,
    // so the rung is a choice rather than a surrender.
    wt_row_x(s_scr, LV_SYMBOL_SHUFFLE, tr(STR_W_RNG_T), tr(STR_W_RNG_S),
             wt_font23(), NULL, NULL, WT_INK, false,
             WT_CHOICE_X, WT_CHOICE_Y(1), WT_CHOICE_W, WT_CHOICE_H,
             audit_rng_cb, NULL);
    lv_obj_set_ext_click_area(
        wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160, true, audit_back_cb, NULL), 10);
}

static void duress_cb(lv_event_t *e)
{
    (void)e;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    // No subtitle: the chips and the two blocks below teach exactly what
    // "what opens the spare, and what opens your real keys" was saying.
    s_scr = wt_screen(s_parent, tr(STR_I_ROW_WAYSIN), NULL);
    wt_chrome_head(s_scr);
    {
        char trail[96];
        snprintf(trail, sizeof trail, "%s / %s", tr(STR_G_T),
                 tr(STR_I_TAB_SECURITY));
        wt_trail(s_scr, WT_ICON_SHIELD, trail, false);
    }

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
    // just set a drawing goes looking for where the swipe is chosen.
    //
    // Three rows, every value already shipping: what the spare is for, what
    // the swipe does, and the thing neither of them is. The swipe PICKS a
    // signer; the passphrase is what OPENS one, and an owner who reads the
    // chips as a lock has to meet that here.
    {
        wt_fact_t facts[3] = {
            { .cap = tr(STR_D_SPARE),          .val = tr(STR_GD_SET_SUB),
              .icon = WT_ICON_SECRET },
            { .cap = tr(STR_GD_PICK_REAL_T),   .val = tr(STR_I_WAYSIN_SHORT),
              .icon = LV_SYMBOL_EDIT },
            { .cap = tr(STR_L_PASSPHRASE_CAP), .val = tr(STR_T_PASS_VAL),
              .icon = WT_ICON_LOCK },
        };
        // 200: the chips end at 155 and the usual 232 left 75px of nothing
        // between the picture and the words about it.
        wt_facts(s_scr, 200, facts, 3);
    }

    // BACK leftmost, the two actions right aligned to 752. 140 + 270 + 270 with
    // 12px gaps is exactly the 704 lane, which is why this row runs tighter
    // than the 22px the roomier rows get.
    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160, true, waysin_back_cb, NULL);
    wt_arrow_action(s_scr, tr(STR_GD_SET_BTN), false, false, WT_ACT_X, WT_ACTION_Y, 0, false, waysin_stroke_cb, NULL);
    wt_arrow_action(s_scr, tr(STR_GD_WORD_PILL), false, false, 330, WT_ACTION_Y, 0, false, waysin_word_cb, NULL);
    // Arrow actions never re-font, so the row shares one size by
    // construction -- the fault this comment used to guard against (one label
    // dropping a rung between two at 28) is a shape the kit can no longer
    // draw.
}

// ---- theme ----
// It spent a version as a wordless chip in the header, then one as a row on
// the DEVICE tab stating its value in words. It is the breathing dot on the
// action band now, wordless again but on the band every tab shares -- because
// it is the control that makes the case for tapping in place loudest: the
// result of the pick is the PAGE, so the page is the only honest preview.
// Tap, and every mark on every tab is the new colour before the finger lifts.
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
    wt_pane_stop(&s_pane_ctx);
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

// Wipe: erase the seed and go back to being just a game. One tap on the row
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
static void erase_screen(void);
static void wipe_fail_ok_cb(lv_event_t *e)   // dismiss back to settings, retryable
{
    lv_obj_t *ovl = lv_event_get_user_data(e);
    lv_obj_delete_async(ovl);
    // AND REBUILD THE GATE. The refusal is an overlay on the confirm screen,
    // so dismissing it uncovers the same slide -- and that slide has already
    // spent its first leg. One stroke then finished the second and erased the
    // seed, on the one action in the product that cannot be undone. The two
    // legs are the whole safety mechanism and they only mean anything if a
    // refused attempt costs both of them again.
    erase_screen();
}

// The erase itself. Reached only from the confirm screen's slide, never from
// a tap on the Settings row: two taps in one spot is a gesture a pocket or a
// double tap can produce by accident, and this one is not undoable from here.

static void do_wipe(void *ud)
{
    lv_obj_t *confirm = ud;
    if (confirm) lv_obj_delete_async(confirm);
    if (kiss_seed_wipe() != 0) {        // NVS erase/commit CAN fail: never claim
        // "erased" unless it truly is — say so and change nothing. The
        // outcome shape with the WT_WARN lamp: a retryable failure, in the
        // same geometry success uses, so nothing jumps when it goes badly.
        // The headline names the move; the shipped body keeps its "do not
        // sell or give it away" whole.
        lv_obj_t *ovl = wt_chrome(s_scr, tr(STR_G_NOERASE_T));
        wt_outcome_t o = {
            .headline = tr(STR_G_NOERASE_NEXT),
            .para     = tr(STR_G_NOERASE_B),
            .ok       = false,
        };
        wt_outcome(ovl, &o);
        wt_arrow_action(ovl, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y,
                        160, true, wipe_fail_ok_cb, ovl);
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

    // Full-screen confirmation as an overlay child (never delete the event
    // target's ancestors mid-event) -- wt_chrome makes one, since a screen
    // it builds IS a full-screen child of its parent. The outcome shape: the
    // green lamp, a headline naming the two moves, and WHAT SURVIVES carrying
    // the clause G_ERASED_B always ended on. Never a full-page tick.
    lv_obj_t *ovl = wt_chrome(s_scr, tr(STR_G_ERASED_T));
    char para[192], surv[96];
    snprintf(para, sizeof para, "%s", tr(STR_G_ERASED_B));
    char *cut = strstr(para, "\n\n");
    if (cut) {
        *cut = '\0';
        snprintf(surv, sizeof surv, "%s", cut + 2);
    } else {
        surv[0] = '\0';
    }
    wt_outcome_t o = {
        .headline = tr(STR_G_ERASED_NEXT),
        .para     = para,
        .f1c      = surv[0] ? tr(STR_C_SURVIVES) : NULL,
        .f1v      = surv[0] ? surv : NULL,
        .ok       = true,
    };
    wt_outcome(ovl, &o);

    // Two ways off this screen, because there are two reasons to have been
    // here. The erase used to be half of a pair whose other half made new
    // seed words; that pair is gone, so the offer moves to where it is
    // actually true -- after the erase, on a device that now holds nothing.
    // OK still locks, exactly as it did, for the owner handing the box on.
    wt_arrow_action(ovl, tr(STR_W_CHOOSE_NEW), false, true, WT_ACT_X,
                    WT_ACTION_Y, 0, false, wiped_new_cb, NULL);
    wt_arrow_action(ovl, tr(STR_C_OK), true, false, 592, WT_ACTION_Y, 160,
                    true, wiped_ok_cb, NULL);
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
    s_scr = wt_chrome(s_parent, tr(STR_G_WIPEC_T));

    // The trail says how the owner got here, and its mark is the one
    // breadcrumb on the device in full WT_STOP: the red reaches the header
    // before the sentence does.
    char trail[128];
    snprintf(trail, sizeof trail, "%s / %s", tr(STR_G_T),
             tr(STR_I_SEC_NO_UNDO));
    wt_trail(s_scr, WT_ICON_ERASE, trail, true);

    // The shape answers the only question an owner has at a gate. The keys
    // being ended are NAMED, by fingerprint, in the loss line: every route
    // into Settings has an open session behind it, so kiss_ui_last_fp is
    // THIS signer's fingerprint, and the owner can hold it against the card
    // in their hand before touching the hold. G_WIPEC_B was already the two
    // halves -- what leaves, and what brings it back -- so the paragraph is
    // its first clause and WHAT SURVIVES is its second; no new sentence had
    // to be written, only stood where the shape wants it.
    uint8_t fp[4];
    kiss_ui_last_fp(fp);
    char goes[96], id[16];
    snprintf(id, sizeof id, "%02X%02X%02X%02X", fp[0], fp[1], fp[2], fp[3]);
    snprintf(goes, sizeof goes, tr(STR_G_WIPE_GOES_FMT), id);

    char para[192], surv[192];
    snprintf(para, sizeof para, "%s", tr(STR_G_WIPEC_B));
    char *cut = strstr(para, "\n\n");
    if (cut) {
        *cut = '\0';
        snprintf(surv, sizeof surv, "%s", cut + 2);
    } else {
        snprintf(surv, sizeof surv, "%s", tr(STR_G_WIPEC_B));
    }

    wt_gate_t g = {
        .sentence = tr(STR_G_WIPE_CANT),
        .para     = para,
        .warn     = kiss_ui_backup_checked() ? NULL
                                             : tr(STR_I_WORDS_UNVERIFIED),
        .surv = surv,
        .goes = goes,
        .stop     = true,
    };
    wt_gate(s_scr, &g);

    // DOUBLE TRAVEL, and it is what a timed hold used to be for. The erase is
    // the one action on this device with no undo, so it wanted a gesture no
    // pocket and no accident produces -- which used to be "hold still for
    // 2000ms", a length that punished a steady hand and stopped nothing a
    // resting thumb could not do. Two opposite strokes cannot happen by
    // accident and cost a deliberate owner about a second.
    //
    // ONCE MORE is the word between the legs, and it already ships in 21
    // locales as the unlock stroke's own "draw it again" title -- the same
    // sentence to the same person.
    const lv_color_t wipe_ink = WT_STOP_INK, wipe_fill = WT_STOP;
    wt_slide_t wipe = {
        .txt   = tr(STR_G_HOLD_WIPE),
        .held  = tr(STR_G_FW_KEEP_HOLDING),
        .again = tr(STR_GD_DRAW_AGAIN_T),
        .x = WT_ACT_X, .y = WT_ACTION_Y_SLIDE, .w = 330,
        .ink = &wipe_ink, .fill = &wipe_fill,
        .done = do_wipe,
        .twice = true,
    };
    wt_slide(s_scr, &wipe);
    wt_arrow_action(s_scr, tr(STR_C_CANCEL), true, false, 592, WT_ACTION_Y,
                    160, true, erase_back_cb, NULL);
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
        const bool on = id == i18n_get_lang();
        // Flag, name, and a trailing tick on the current one. The tick is a
        // resolve, so it follows the word; a tap closes the overlay, so the
        // shift a tick would cause on a re-pick is never seen.
        lv_obj_t *p = wt_word_action(ovl, on ? LV_SYMBOL_OK : NULL,
                                     i18n_lang_info(id)->native, false,
                                     on ? wt_accent() : WT_INK, on,
                                     lang_pick_cb, (void *)(intptr_t)id);
        lv_obj_set_pos(p, 16 + (i % 3) * 260, 76 + (i / 3) * 52);
        // Every row is in its own script. Select its regional font explicitly;
        // the current UI language must not control another locale's glyph form.
        lv_obj_t *name = lv_obj_get_child(p, 0);
        lv_obj_set_style_text_font(name, wt_font14_for_lang(id), 0);
        lv_obj_set_style_text_letter_space(name, 0, 0);
        if (img_lang_flags[id]) {             // en deliberately has no flag
            lv_obj_t *fl = lv_image_create(p);
            lv_image_set_src(fl, img_lang_flags[id]);
            lv_obj_remove_flag(fl, LV_OBJ_FLAG_CLICKABLE);  // the row takes the tap
            lv_obj_move_to_index(fl, 0);      // the flag leads the name
        }
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

// ---- the ten terms, as a reference (Part 19) ------------------------------
// Every word this device teaches, in one place, for the owner who wants to
// read them rather than meet them. Five to a page, because a definition row
// at n=5 still opens to 148px and that is the point of the idiom -- ten rows
// squeezed into one lane would be the eight cell glossary again with more
// cells.
//
// A PANE FLIP, not a scroll. wt_screen is deliberately not scrollable: LVGL
// hands a press to the nearest scrollable ancestor once the finger moves, so
// a scrolling list eats every stroke a few pixels in, which is the bug that
// comment exists to prevent.
static int s_terms_page;
static lv_obj_t *s_terms_body;      // the list and the pager, in one holder

static void terms_back_cb(lv_event_t *e)
{
    (void)e;
    kiss_terms_leaving();
    s_terms_page = 0;
    s_terms_body = NULL;
    settings_reopen();
}

// Only the LIST and the pager are rebuilt on a page turn, never the screen.
// Rebuilding the screen under a finger that is still down is the hazard the
// sign slide's own comment spells out -- the indev re-targets a live press,
// so the stroke's tail lands on whatever the new screen put there -- and here
// it also left the settings page's pane pointer naming a deleted object.
static void terms_build_page(void)
{
    if (s_terms_body) lv_obj_delete(s_terms_body);
    s_terms_body = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_terms_body);
    lv_obj_set_pos(s_terms_body, 0, 0);
    lv_obj_set_size(s_terms_body, LV_HOR_RES, WT_CONTENT_BOTTOM);
    lv_obj_remove_flag(s_terms_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_terms_body, LV_OBJ_FLAG_CLICKABLE);

    // FOUR to a page, not five. At n=5 the open row is 148px and a two line
    // definition with its TECHNICAL line under it needs 153 -- the term line
    // was being drawn five pixels past the row's own floor on every card, and
    // nothing overlapped until the row was open, which is the state no sweep
    // had ever measured. The TERM gate says so now; n=4 opens to 182.
    const int first = s_terms_page * KISS_TERMS_PER_PAGE;
    int n = KISS_TERM_N - first;
    if (n > KISS_TERMS_PER_PAGE) n = KISS_TERMS_PER_PAGE;
    kiss_terms_list(s_terms_body, &KISS_TERMS_ALL[first], n);
    kiss_terms_hint(s_scr, KISS_TERMS_ALL, KISS_TERM_N);

    // DOTS IN THE TRAIL, not a pager line at the lane's foot. A definition
    // list at n=5 fills the whole 284px lane by construction, so a pager
    // under it is printed across the fifth row -- which is what the first
    // frame of this screen showed. The count is on the SETTINGS row that
    // opens this page and does not need saying twice.
    wt_sheet_dots(s_scr, KISS_TERMS_PAGES, s_terms_page);
}

static void terms_gesture_cb(lv_event_t *e)
{
    const int step = wt_swipe_step(e);
    if (step == 0) return;
    if (step < 0 && s_terms_page == 0) { terms_back_cb(NULL); return; }
    const int next = s_terms_page + (step > 0 ? 1 : -1);
    if (next < 0 || next >= KISS_TERMS_PAGES) return;
    // The page turn ends the reading of whatever was open on the page being
    // left, exactly as walking out of the screen does -- but the SCREEN is
    // still here, and so is its band.
    kiss_terms_turned();
    s_terms_page = next;
    terms_build_page();
}

static void terms_screen(void)
{
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_terms_body = NULL;
    s_scr = wt_screen(s_parent, tr(STR_I_TERMS_T), NULL);
    wt_chrome_head(s_scr);
    {
        char trail[96];
        snprintf(trail, sizeof trail, "%s / %s", tr(STR_G_T),
                 tr(STR_I_TAB_DEVICE));
        wt_trail(s_scr, WT_ICON_WHAT, trail, false);
    }
    terms_build_page();
    wt_swipe_watch(s_scr, terms_gesture_cb);
    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, WT_BACK_X,
                    WT_ACTION_Y, 140, true, terms_back_cb, NULL);
}

static void terms_open_cb(lv_event_t *e) { (void)e; s_terms_page = 0;
                                           terms_screen(); }

static void device_open_cb(lv_event_t *e) { (void)e; device_screen(); }

static void device_screen(void)
{
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_scr = wt_screen(s_parent, tr(STR_I_DEVICE_T), NULL);
    wt_chrome_head(s_scr);
    {
        char trail[96];
        snprintf(trail, sizeof trail, "%s / %s", tr(STR_G_T),
                 tr(STR_I_TAB_DEVICE));
        wt_trail(s_scr, LV_SYMBOL_SETTINGS, trail, false);
    }

    // FOUR ROWS ON THE LANE, not a 96px card of font14 with 180px of empty
    // glass under it. kiss_build_id_make is a CORNER DIAGNOSTIC -- font14 is
    // right for it in the band of the settings page, where it lives -- and
    // this page put the same block at the top of a 704px lane and left the
    // rest blank. The bench: "the text is quite fucking tiny when there is a
    // lot of space to be filled and used on the screen don't you think??"
    //
    // Same four facts, as the page's own def rows, at the size the rest of
    // SETTINGS reads at. The lamps carry the two that have a bad state; the
    // fifth row is the card, which is the only one that opens anything.
    bool enc = false, radio = true, noise = true;
    kiss_build_id_facts(NULL, &enc, &radio, &noise);

    wt_def_t defs[5] = {
        { .cap = tr(STR_I_DEV_BUILD), .val = KISS_VERSION_STR,
          .sub = kiss_build_commit() },
        { .cap = tr(STR_I_DEV_ENC),
          .val = tr(enc ? STR_G_HIST_ON_BTN : STR_G_HIST_OFF_BTN),
          .val_col = enc ? (lv_color_t){0} : WT_WARN,
          .lamp = true, .lamp_col = enc ? WT_OK : WT_WARN },
        // HELD is the SAFE state: the C6 is kept in reset, so the radio is
        // not merely off, it cannot come up. NOT HELD is the one worth a lamp.
        { .cap = tr(STR_I_DEV_RADIO),
          .val = tr(radio ? STR_I_DEV_RADIO_OFF : STR_I_DEV_RADIO_ON),
          .val_col = radio ? (lv_color_t){0} : WT_WARN,
          .lamp = true, .lamp_col = radio ? WT_OK : WT_WARN },
        { .cap = tr(STR_I_DEV_RANDOM),
          .val = tr(noise ? STR_W_RNG_ON : STR_W_RNG_OFF),
          .val_col = noise ? (lv_color_t){0} : WT_WARN,
          .lamp = true, .lamp_col = noise ? WT_OK : WT_WARN },
        { .cap = tr(STR_W_SD_BTN), .val = "",
          .sub = tr(STR_I_CARD_SUB), .go = sdinfo_open_cb },
    };
    wt_def_list(s_scr, defs, 5);

    lv_obj_set_ext_click_area(
        wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160, true, device_back_cb, NULL), 10);
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

// DECIDED: the amber dots on the tab strip are the attention chip's ROUTING and
// cannot be deleted as a duplicate of the count.
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
        if (!s_entering && s_pane) wt_pane_point(&s_pane_ctx);
        return;
    }
    go_tab(want);
}

static void settings_what_cb(lv_event_t *e);

static void go_tab(int tab)
{
    // A real tab is also the way back from [ ? ]: tapping the one already
    // selected re-lands on its rows, which the same-tab refusal below would
    // otherwise swallow.
    if (s_what_open && tab == s_tab) { settings_what_cb(NULL); return; }
    s_what_open = false;
    if (tab == s_tab) return;
    // A tab change no longer rebuilds the SCREEN, only the group. Nothing on
    // the strip or the action bar depends on which tab is open -- the dots and
    // the chip read the same three conditions -- so the things that would have
    // been rebuilt identically are simply left alone, and the highlight has
    // something continuous to slide along.
    help_close();                // an overlay does not outlive the group under it
    wt_pane_go(&s_pane_ctx, tab, tab == TAB_NOUNDO, build_tab);
}

static void tab_cb(lv_event_t *e)
{
    go_tab((int)(intptr_t)lv_event_get_user_data(e));
}

// The stroke, on the SETTINGS page: five tabs, one horizontal deck. No pages
// inside any of them, so a swipe is a tab step and nothing else. [ ? ] stays
// a toggle rather than a position on the deck, but the stroke reaches it:
// past NO UNDO opens it, and from it a right swipe is the way back -- the
// bench asked for exactly this, in the words "i cant swipe to the question
// mark".
static void settings_gesture_cb(lv_event_t *e)
{
    if (s_help) return;
    const int step = wt_swipe_step(e);
    if (!step) return;
    if (s_what_open) {
        if (step < 0) settings_what_cb(NULL);
        return;
    }
    const int to = s_tab + step;
    if (to >= TAB_N) { settings_what_cb(NULL); return; }
    if (to < 0) return;                  // the deck still ends on the left
    go_tab(to);
}

// Every tab is the KEYS page's own list now: the whole content lane, big
// line rows, rules between. The one difference SETTINGS adds is the mark --
// a loop on a row whose tap resolves IN PLACE, the chevron on one that
// leaves -- the same promise the old wide rows made.
static lv_obj_t *def_list(const wt_def_t *defs, int n)
{
    return s_still ? wt_def_list_still(s_pane, defs, n)
                   : wt_def_list(s_pane, defs, n);
}

static void tab_signer(void)
{
    int sc = kiss_script(), tn = kiss_testnet();

    // NO BIP NUMBER on the row. It was composed here as "Native SegWit·BIP84",
    // and the card behind the "?" beside it already names all three by number
    // -- so the row was carrying the card's content in a lane that had to
    // ellipsise to hold it. The bench worked this out in the middle of the
    // sentence: "the BIP84 beside Native Segwit can be moved to the question
    // mark popup... wait it already is so just remove that text".

    wt_def_t defs[2] = {
        // Amber on both test networks, in the value AND the sub: the colour
        // says "these coins are not real" and the words say it again, so the
        // state never rests on colour alone. The lamp is KEYS' own.
        { .cap = tr(STR_I_ROW_NETWORK), .val = kiss_net_name(),
          .val_col = tn ? WT_WARN : (lv_color_t){0},
          .sub = tr(tn ? STR_G_TESTNET_NOTE : STR_G_MAINNET_NOTE),
          .sub_col = tn ? WT_WARN : (lv_color_t){0},
          .lamp = true, .lamp_col = tn ? WT_WARN : WT_OK, .lamp_pulse = tn,
          .mark = LV_SYMBOL_LOOP, .go = net_cb },
        // The address PREFIX is the value. That is the way round it has to
        // be, not a preference: bc1 is what an owner sees in their
        // coordinator, and "Native SegWit" is the name for it.
        { .cap = tr(STR_I_ROW_TYPE), .val = type_prefix(sc, tn),
          .sub = type_name(sc), .mark = LV_SYMBOL_LOOP, .go = type_cb },
    };
    // DENOMINATION is on DEVICE now. It is a PRESENTATION preference -- how a
    // number is drawn -- and it sat here ranked equal to which chain the coins
    // are on and which keys the addresses come from, which are the two facts on
    // this device that can lose money. It also had no sub, because the only one
    // it ever had restated the SATS beside it, so the tab read as two finished
    // rows and a stub with 400px of nothing to their right. That is what the
    // bench saw: "the SIGNER tab looks off compared to other which looks
    // clean".
    //
    // DEVICE is where the other two presentation preferences already went --
    // theme and language, both on the action band because neither "earned a
    // 142px row". Same argument, same tab. And the SIGN screen keeps the
    // control that matters: tapping an amount switches units where the amount
    // is actually being read, which is the bench's own point.
    lv_obj_t *list = def_list(defs, 2);
    wt_def_row_help(list, 1, help_open_cb, NULL);
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
    // Persist depends on storage. AMNESIC keeps nothing by contract, so the
    // cycle has nothing to cycle -- and instead of a dead control the row
    // becomes the page's one in-place definition, saying WHY in the same
    // spot the switch would be. A control that vanishes sends the owner
    // hunting for it; one that explains itself does not.
    bool amnesic = kiss_seed_mode() == WSEED_MODE_AMNESIC;
    bool on = kiss_persist_enabled();

    wt_def_t defs[3] = {
        // Amber NOT SET beside "opens real keys" is the whole lesson; the
        // lamp breathes until the mark exists, the same beat as the tab dot.
        { .cap = tr(STR_I_ROW_WAYSIN),
          .val = tr(set ? STR_GD_ON : STR_GD_OFF),
          .val_col = set ? (lv_color_t){0} : WT_WARN,
          .sub = tr(STR_I_WAYSIN_SHORT),
          .lamp = true, .lamp_col = set ? WT_OK : WT_WARN, .lamp_pulse = !set,
          .go = duress_cb },
        amnesic
            // "nothing saved" is the OFF state's own sub, reused: it is as
            // true of AMNESIC as of OFF, and "storage is AMNESIC" was wider
            // than the lane UNAVAILABLE leaves. The definition says the rest.
            ? (wt_def_t){ .cap = tr(STR_I_ROW_HISTORY),
                  .val = tr(STR_I_PERSIST_DEAD_VAL),
                  .sub = tr(STR_I_POP_NOTHING),
                  .plain = tr(STR_I_PERSIST_DEAD_PLAIN) }
            // The sub follows the STATE rather than naming the feature: ON
            // says what is kept, OFF says that nothing is. It is the only
            // warning the flip gets, and it is on screen before the tap.
            : (wt_def_t){ .cap = tr(STR_I_ROW_HISTORY),
                  .val = tr(on ? STR_G_HIST_ON_BTN : STR_G_HIST_OFF_BTN),
                  .sub = tr(on ? STR_I_HIST_SHORT : STR_I_POP_NOTHING),
                  .mark = LV_SYMBOL_LOOP, .go = persist_cb },
        // DECIDED: ONE door onto AUDIT, and it is this one. The row was
        // duplicated onto BACKUP because "how were these made" is a question
        // about the seed and an outside reader guessed that tab; that argument
        // is still true and did not survive what the pair cost. From the
        // bench, "why the fuck now is there TWO audit button in SETTINGS
        // screens" -- a settings page that lists the same row twice reads as a
        // page that does not know what it holds, and a reader who cannot find
        // a screen is not helped by finding it twice.
        //
        // It was moved to DEVICE for one commit on the reasoning that HOW YOUR
        // KEYS WERE MADE and the RANDOMNESS AUDIT are facts about this box.
        // Wrong tab: what an owner does with them is decide whether to TRUST
        // the box, which is what SECURITY is for, and it is where the row has
        // always been. DEVICE is FIRMWARE, THIS DEVICE and TERMS, and it is
        // three rows again.
        //
        // No value: AUDIT has no state to report, so what it is FOR rides
        // the sub lane. A phrase in the value lane wraps into the chevron --
        // the value never yields, so it has to be short or absent.
        { .cap = tr(STR_I_ROW_AUDIT), .val = "",
          .sub = tr(STR_I_AUDIT_SUB), .go = audit_open_cb },
    };
    def_list(defs, 3);
}

// What a seed IS, on the row that names one. STR_W_WHATSEED_* is the setup
// flow's own answer -- "12 or 24 ordered words, called a BIP39 mnemonic" --
// translated everywhere and locked to a path a settled owner never walks
// again. This is the second door onto it.
static void seedwords_help_cb(lv_event_t *e)
{
    (void)e;
    wt_explain_t x = {
        .title  = tr(STR_I_ROW_WORDS),
        .icon   = WT_ICON_KEY,
        // .sub, not .cap: cap is half of a cap/val PAIR and renders nothing
        // on its own. This line is the whole answer in one breath -- "12 or
        // 24 ordered words, called a BIP39 mnemonic" -- and it belongs under
        // the title where the reader's eye already is.
        .sub    = tr(STR_W_WHATSEED_S),
        .body   = tr(STR_W_WHATSEED_B),
        // The real term, on the card that defines the plain one. T_SEED_TERM
        // already ships in 21 locales beside every other term card, so the
        // line this overlay was missing costs no key.
        .term   = tr(STR_T_SEED_TERM),
        .term_label = tr(STR_G_TECHNICAL),
        .ok_txt = tr(STR_C_OK),
        .mode   = WT_BODY_PROSE,
    };
    wt_explain_open(s_scr, &x);
}

static void tab_backup(void)
{
    bool ok = kiss_ui_backup_checked();
    int mode = kiss_seed_mode();
    int ssub = mode == WSEED_MODE_SD      ? STR_I_STORE_SD_SUB
             : mode == WSEED_MODE_AMNESIC ? STR_I_STORE_AMN_SUB
             : kiss_seed_flash_encrypted() ? STR_I_STORE_FLASH_ENC_SUB
                                           : STR_I_STORE_FLASH_SUB;
    // Amber across the row, not just an amber note. Words in a flash this
    // build does not encrypt is the one fact on the page a holder should
    // catch without reading anything.
    bool warn = words_unencrypted();

    wt_def_t defs[2] = {
        // A word AND a lamp: in the GREEN theme the accent is byte identical
        // to WT_OK, so colour alone stops carrying meaning. No fingerprint in
        // the sub any more -- the bench said it does not help here, and the
        // check screen behind the row names it at full size where the
        // against-the-paper comparison actually happens.
        { .cap = tr(STR_I_ROW_WORDS),
          .val = tr(ok ? STR_I_WORDS_OK_VAL : STR_I_WORDS_NO_VAL),
          .val_col = ok ? (lv_color_t){0} : WT_WARN,
          .lamp = true, .lamp_col = ok ? WT_OK : WT_WARN, .lamp_pulse = !ok,
          .go = words_cb },
        { .cap = tr(STR_I_ROW_STORAGE), .val = storage_mode_name(mode),
          .val_col = warn ? WT_WARN : (lv_color_t){0},
          .sub = tr(ssub), .sub_col = warn ? WT_WARN : (lv_color_t){0},
          .lamp = warn, .lamp_col = WT_WARN, .lamp_pulse = warn,
          .go = store_open_cb },
    };
    lv_obj_t *list = def_list(defs, 2);
    // A "?" beside SEED WORDS, opening what a seed actually IS. The bench
    // asked for exactly this: "there should be a ? mark next to seed words
    // which when tapped explains bip39 mnemonic". The explainer is the one
    // the setup flow already teaches from and it already ships in 21
    // locales, so this costs no new key at all.
    wt_def_row_help(list, 0, seedwords_help_cb, NULL);
}

static void tab_device(void)
{
    // LANGUAGE and THEME live on the action band -- one is its own label, the
    // other is its own preview, and neither earned a 142px row.
    //
    // DENOMINATION is gone too, and it is the clearer case: every amount on
    // the signing screens IS the switch (wt_denom_bind), the tap persists
    // through the same kiss_settings_set_denom this row called, and the row's
    // own sub-line said so -- "or tap an amount". A preference with two
    // values, changed where the number it changes is on the glass, does not
    // also need a row three screens away.
    char terms_count[64];
    const int tunread = kiss_terms_unread(KISS_TERMS_ALL, KISS_TERM_N);
    if (tunread > 0)
        snprintf(terms_count, sizeof terms_count,
                 tr(STR_I_TERMS_UNREAD_FMT), tunread);
    else
        snprintf(terms_count, sizeof terms_count, "%s",
                 tr(STR_I_TERMS_ALL_READ));

    wt_def_t defs[3] = {
        // The version is a FACT, in the page's own ink. It was amber once,
        // with no predicate behind it -- amber on this page means a dot and
        // a count, both of which this row has never had.
        { .cap = tr(STR_I_ROW_FW), .val = KISS_VERSION_STR,
          .sub = tr(STR_I_FW_SUB), .go = fw_open_cb },
        // What the row OPENS rides the sub lane -- a phrase in the value
        // lane wraps into the chevron. The diagnostics it once carried are
        // already printed by kiss_build_id_make on the screen behind it.
        { .cap = tr(STR_I_ROW_DEVICE), .val = "",
          .sub = tr(STR_I_ROW_DEVICE_SUB), .go = device_open_cb },
        // TERMS. A CHEVRON, not a plus: it leaves the page, so it takes the
        // glyph that means leaves the page. The value is a count because a
        // reference nobody has read and one they have finished are different
        // things and the row is where that is worth saying.
        //
        // ABSENT IN A DECOY SESSION, for the same reason WAYS IN is: the list
        // contains THE DECOY, and a screen that explains the decoy to whoever
        // is holding the device is the one thing the decoy cannot survive.
        { .cap = tr(STR_I_ROW_TERMS), .val = terms_count,
          .sub = tr(STR_I_ROW_TERMS_SUB), .go = terms_open_cb },
    };
    def_list(defs, kiss_session_decoy() ? 2 : 3);
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
    wt_pane_scenery(wash);                              // scenery, not a row

    // ONE card, because a destructive action deserves its reason on the same
    // screen as its button. This group holds exactly one thing: storage and
    // duress are not destructive and are not in it.
    // 270 rather than 258. The claim PAIR needs more room than the paragraph it
    // replaces -- two heads and two bodies in two 340px columns -- and the card
    // has it: 126 + 270 lands on 396, two clear of the floor.
    lv_obj_t *card = wt_card(s_pane, WT_WIDE_X, WT_WIDE_Y(0), WT_WIDE_W, 270);
    wt_row_sev(card, WT_SEV_STOP);

    // WT_STOP_INK rather than WT_STOP: full stop red on a stop tinted card is
    // the one pairing on this page that vibrates.
    lv_obj_t *head = wt_lbl(card, tr(STR_I_ROW_ENDWORDS), 24, 22, wt_font28(),
                            WT_STOP_INK);

    // No fingerprint badge beside the heading any more. It was font14 in a
    // bubble -- the two shapes this look removes -- and it confused more than
    // it checked; the confirmation behind the button still names the
    // fingerprint at full size, which is where the against-the-paper check
    // actually happens.
    (void)head;

    // TWO CLAIMS, side by side, not one paragraph stacking three. It was a
    // 600x91 block reading "this signer forgets its keys. your paper words and
    // passphrase are the only way back. the next screen asks again and needs a
    // held press" -- and overlapcheck called it what it was: a wall of text
    // with the only chrome on the screen drawn around it.
    //
    // The rule colours carry the split the house style asks for: the accent on
    // what HAPPENS, WT_WARN on what it COSTS. The third clause is gone. The
    // next screen does ask again and does need a held press, and an owner
    // reaches it in one tap -- saying so in advance was the card explaining a
    // screen instead of its own decision.
    // Two ROWS inside the card, 24 in from its edge, which leaves exactly the
    // 704 the content lane gets -- so the captions and values line up with
    // every other pair of claims on the device. The marks carry the split:
    // the bin on what goes, the page on what stays, and STOP on the bin
    // because this card is the one place a group is irreversible.
    const int by = 22 + lv_font_get_line_height(wt_font28()) + 8;
    {
        wt_fact_t facts[2] = {
            { .cap = tr(STR_I_ERASE_H1), .val = tr(STR_I_ERASE_B1),
              .icon = WT_ICON_ERASE,
              .icon_col = WT_STOP_INK },
            { .cap = tr(STR_I_ERASE_H2), .val = tr(STR_I_ERASE_B2),
              .icon = LV_SYMBOL_FILE },
        };
        wt_facts_in(card, 24, by, 704, facts, 2);
    }

    // The action, and NO HOLD on it. The hold stays where it already is, on
    // the confirmation behind it: two gates in a row teaches an owner to grind
    // through both, and the screen that names the fingerprint is the one worth
    // holding on. A red ARROW ACTION, not a drawn button: the boxed red pill
    // was the page's last box, and the arrow says a tap here goes somewhere
    // (the confirmation) rather than doing the thing.
    // No bin on the label: the left why-head already wears it, and the icon's
    // width is what pushed the caption beside this row into its ellipsis.
    const int byy = 270 - 20 - WT_ACTION_H;
    lv_obj_t *btn = wt_arrow_action(card, tr(STR_I_ERASE_BTN), false, false,
                                    24, byy, 0, false, endwords_cb, NULL);
    for (uint32_t i = 0; i < lv_obj_get_child_count(btn); i++) {
        lv_obj_t *ch = lv_obj_get_child(btn, i);
        lv_obj_set_style_text_color(ch, WT_STOP_INK, 0);
        lv_obj_remove_flag(ch, WT_FLAG_ACCENT);
    }
    // No caption beside the button. It read "nothing else here erases", which
    // explains the page's OTHER FOUR TABS from inside the fifth -- and the
    // bench asked what it was even saying. The two claim blocks above say what
    // this tab does; nothing on it needs to speak for the rest of the page.
}

// The group the strip is pointing at, drawn into whatever pane is current.
static void build_tab(void)
{
    if (s_what_open) {
        wt_fact_t facts[3] = {
            // The loop, not the key: SAFE TO TRY is the page's own loop mark
            // making its promise in words -- every pick can be picked back.
            { .cap = tr(STR_G_HELP_F1C), .val = tr(STR_G_HELP_F1V),
              .icon = LV_SYMBOL_LOOP },
            { .cap = tr(STR_G_HELP_F2C), .val = tr(STR_G_HELP_F2V),
              .icon = LV_SYMBOL_BELL },
            { .cap = tr(STR_G_HELP_F3C), .val = tr(STR_G_HELP_F3V),
              .icon = LV_SYMBOL_TRASH },
        };
        wt_explain(s_pane, tr(STR_G_HELP_HEAD), tr(STR_G_HELP_BODY), facts,
                   3);
        return;
    }
    switch (s_tab) {
    case TAB_SECURITY: tab_security(); break;
    case TAB_BACKUP:   tab_backup();   break;
    case TAB_DEVICE:   tab_device();   break;
    case TAB_NOUNDO:   tab_noundo();   break;
    default:           tab_signer();   break;
    }
}

// The same same-tab pane swap KEYS does by hand, for the same reason:
// wt_pane_go refuses a same-tab call and [ ? ] is not a section, so the
// strip's marker never moves for it.
static void settings_what_cb(lv_event_t *e)
{
    (void)e;
    s_what_open = !s_what_open;
    // The strip releases its tab while the explainer is up, and takes it back
    // on the way out. [ ? ] is still not a section -- it never highlights --
    // but the tab BEHIND it must not keep claiming to be where the owner is.
    wt_tabs_flex_help(s_tabs, s_tab, s_what_open);
    const bool was_moving = s_pane_ctx.entering;
    wt_pane_stop(&s_pane_ctx);
    if (was_moving && s_pane) {
        lv_obj_delete(s_pane);
        s_pane = NULL;
    }
    s_pane_ctx.pane_out = s_pane;
    s_pane = wt_pane_new(&s_pane_ctx);
    build_tab();
    wt_accent_restyle(s_pane);
    const int dir = s_what_open ? 1 : -1;
    wt_pane_enter(&s_pane_ctx, dir, false);
    wt_pane_exit(&s_pane_ctx, dir);
}

void kiss_settings_open(lv_obj_t *parent)
{
    if (s_scr) return;
    s_parent = parent;
    // An overlay open when the screen died was deleted with it; the handles
    // must not survive to block the next open, or to be animated after it.
    s_help = NULL;
    s_pane = s_pane_ctx.pane_out = s_tabs = NULL;
    s_entering = false;
    s_what_open = false;
    if (s_tab < 0 || s_tab >= TAB_N) s_tab = TAB_SIGNER;
    s_scr = s_pane_ctx.scr = wt_chrome(parent, tr(STR_G_T));

    // The dots are what a collapsed group costs, paid back. Every condition
    // the attention chip counts lights the dot on the tab that holds it:
    // SECURITY while no duress mark is set, BACKUP for either of its two. The
    // chip counts conditions and a tab has one dot, so BACKUP holding both is
    // the one state where the number is larger than the marks.
    // DECIDED: these tabs keep their NOUNS and are not renamed after the jobs
    // they hold. The proposal was HOW IT SIGNS / WHAT IT KEEPS / HOW IT PROVES
    // / WHAT IT IS, and it does not fit: the five-up strip is 620px, the
    // shipped labels measure 612 of it, and those four plus NO UNDO measure
    // 920. Three hundred pixels over is not a layout to tune.
    //
    // The only form that fits is single verbs -- SIGNS / KEEPS / PROVES / IS,
    // 500px -- and "IS" is not a word to put on a tab an owner is looking for
    // something in. Verbs without subjects read worse than the nouns they
    // would replace, for a reader who bought their first signing device last
    // week.
    //
    // The miscategorisation the proposal was built on is also gone: it argued
    // that storage sat on SECURITY while recovery words sat on BACKUP, so
    // checking a backup crossed two tabs. STORAGE is on BACKUP beside SEED
    // WORDS, which is one job on one tab.
    const wt_tab_t tabs[TAB_N] = {
        { WT_ICON_KEY,        tr(STR_I_TAB_SIGNER),   false,          false },
        { WT_ICON_SHIELD,     tr(STR_I_TAB_SECURITY), duress_unset(), false },
        { LV_SYMBOL_SAVE,     tr(STR_I_TAB_BACKUP),
          backup_unchecked() || words_unencrypted(), false },
        { LV_SYMBOL_SETTINGS, tr(STR_I_TAB_DEVICE),   false,          false },
        { LV_SYMBOL_TRASH,    tr(STR_I_SEC_NO_UNDO),  false,          true  },
    };
    // The five-up flex strip (frame 7a): content-sized labels spread across
    // the 620 the [ ? ] divider leaves. Five of the 196px bracket boxes need
    // 980px, so the widest strip on the device is the one that lets the
    // words size themselves -- and drops the icons the drawing drops.
    s_pane_ctx.select = wt_tabs_flex_select;
    s_tabs = wt_tabs_flex(s_scr, tabs, TAB_N, s_tab, tab_cb);
    wt_pane_tabs_watch(&s_pane_ctx);
    wt_swipe_watch(s_scr, settings_gesture_cb);

    // No first-run hint on this band any more: its centre holds the language
    // and theme controls now, and the hint's one-line lane ran to 580 -- the
    // mark's own breathe (motion 19) is what teaches [ ? ] here, plus the
    // stroke past NO UNDO that now lands on it.
    wt_help_tab(s_scr, NULL, settings_what_cb, NULL);

    // The group lives in a pane of its own so that a tab change can hold TWO
    // of them for the 200ms the outgoing one takes to leave. Built here and
    // not animated: walking in from home is not a tab change, and neither is
    // the rebuild every value chip does when it is tapped.
    s_pane = wt_pane_new(&s_pane_ctx);
    build_tab();

    // BACK takes the bottom RIGHT corner as an ARROW rather than a pill, and
    // it is still what builds the action bar the attention chip stands on.
    lv_obj_t *back = wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592,
                                     WT_ACTION_Y, 160, true, close_cb, NULL);

    // Opposite it, and only when there is something to say. No "all good" chip:
    // a badge that is always there is a badge nobody reads.
    int n = attention_count();
    if (n > 0) {
        char lab[64];
        if (n == 1) snprintf(lab, sizeof lab, "%s", tr(STR_I_ATTN_1));
        else        snprintf(lab, sizeof lab, tr(STR_I_ATTN_N_FMT), n);
        wt_alert_chip(s_scr, lab, attn_cb, NULL);
    }

    // DECIDED: language and theme live on the BAND, moved there out of the DEVICE tab.
    // The band's centre: LANGUAGE and THEME, out of the DEVICE tab. The
    // language control needs no caption -- its label IS the active language's
    // own name, stripped of the regional qualifier ("ESPANOL (ESPANA)" ->
    // "ESPANOL") because the picker's flag carries the variant.
    //
    // A WORD ACTION with a GLOBE, not an arrow action. It was a forward arrow,
    // which is the mark the SCREEN'S OWN action wears -- so the one control on
    // the band that picks between 21 languages was signed exactly like a "go
    // on", and came back from the bench as "should have some icon better than
    // an arrow, no?". A globe says what the control is before its word is read,
    // in every one of those 21 languages at once.
    lv_obj_t *lang = NULL;
    {
        int li = i18n_get_lang();
        const char *nat = i18n_lang_info(li)->native;
        if (li == I18N_NB) nat = "BOKM\xC3\x85L";   // the flag already says Norway
        const char *par = strstr(nat, " (");
        char shortname[24];
        size_t sn = par ? (size_t)(par - nat) : strlen(nat);
        if (sn >= sizeof shortname) sn = sizeof shortname - 1;
        memcpy(shortname, nat, sn);
        shortname[sn] = 0;
        lang = wt_word_action(s_scr, WT_ICON_LANG, shortname, true, WT_INK,
                              true, lang_open_cb, NULL);
        lv_obj_update_layout(lang);
        // Pinned by its RIGHT edge at 512, exactly where the arrow action put
        // it: the word is a language name and its width moves with the pick,
        // and a left edge that moved would slide the whole band about.
        lv_obj_set_pos(lang, 512 - lv_obj_get_width(lang),
                       WT_ACTION_Y + (WT_ACTION_H - 40) / 2);
        lv_obj_set_ext_click_area(lang, 8);
    }

    // Beside it the theme: a solid colour SWATCH with the cycle mark trailing
    // it, wordless, because the page IS the preview -- tap it and every mark
    // on every tab is the new colour before the finger lifts. The hit box is
    // the band's full 52px.
    //
    // CENTRED between the two controls it sits between, and measured rather
    // than placed: its right edge was pinned at 586, which put it hard against
    // the language control with 70px of empty band before BACK, and the bench
    // filed exactly that -- "needs to be centred between the language picker
    // and BACK, too close to language picker currently". BACK is right-aligned
    // in its own lane, so where it actually STARTS depends on how long the word
    // is in this locale; asking the object is the only way to land between them
    // in all 21.
    {
        lv_point_t ms;
        lv_text_get_size(&ms, LV_SYMBOL_LOOP, wt_font23(), 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        const int sw = 28, cw = sw + 10 + ms.x;
        lv_obj_update_layout(back);
        const int gap_l = 512 + 16;                    // clear of the language
        // BACK's MEASURED left edge, not its declared 592 lane. The lane is
        // where a right-aligned control is allowed to start; the word inside
        // it is 70px further right in English, and clamping to the lane is
        // what pinned this control against the language picker in the first
        // place. 20 of air is the clearance.
        const int gap_r = lv_obj_get_x(back) - 20;
        int tx = (gap_l + gap_r - cw) / 2;
        if (tx < gap_l) tx = gap_l;
        if (tx + cw > gap_r) tx = gap_r - cw;

        lv_obj_t *td = lv_obj_create(s_scr);
        lv_obj_remove_style_all(td);
        lv_obj_set_pos(td, tx, WT_ACTION_Y);
        lv_obj_set_size(td, cw, WT_ACTION_H);
        lv_obj_add_flag(td, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(td, LV_OBJ_FLAG_SCROLLABLE);
        wt_tap_feedback(td);
        lv_obj_add_event_cb(td, theme_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *sq = lv_obj_create(td);
        lv_obj_remove_style_all(sq);
        lv_obj_set_size(sq, sw, 20);
        lv_obj_set_style_radius(sq, 6, 0);
        lv_obj_set_style_bg_color(sq, wt_accent(), 0);
        lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
        lv_obj_add_flag(sq, WT_FLAG_ACCENT_FILL);
        lv_obj_remove_flag(sq, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(sq, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *lp = wt_lbl(td, LV_SYMBOL_LOOP, 0, 0, wt_font23(),
                              wt_accent());
        lv_obj_add_flag(lp, WT_FLAG_ACCENT);
        lv_obj_align(lp, LV_ALIGN_LEFT_MID, sw + 10, 0);
    }

    restyle();
}
