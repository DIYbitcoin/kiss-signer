// Steps 5+6: Sign. Get a PSBT by QR scan (static / pMofN / animated BC-UR) or
// from the SD card, show the spec's verify screen (status light, every output
// with change re-derived ON THIS DEVICE, fee three ways, network/RBF/locktime),
// then hold-to-sign. Signed PSBT goes back the way it came: SD file in ->
// <name>-signed.psbt on the card; QR in -> animated QR out.
// Compiled in BOTH device and sim builds; the sim stubs wallet_psbt_* in sim_main.c.
#include "wallet_sign.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "i18n.h"
#include "platform_sd.h"
#include "qr_transport.h"
#include "wallet_crypto.h"
#include "wallet_psbt.h"
#include "wallet_scan.h"
#include "wallet_theme.h"
#include "wallet_ui.h"   // wallet_ui_last_fp: the SIGNING AS fingerprint
#include "wallet_usage.h"   // reuse guard: mark receive indexes used on sign

#define BG_COL   WT_BG
#define INK_COL  WT_INK
#define MUT_COL  WT_MUT
#define KEY_COL  WT_KEY
#define OK_COL   WT_OK    // status light: label + shape + color,
#define WARN_COL WT_WARN  // never color alone (spec)
#define STOP_COL WT_STOP

// Why this file logs at all: a refused PSBT used to say one translated
// sentence on the glass and nothing anywhere else. wallet_psbt.c is
// deliberately LVGL- and IDF-free (it feeds the desktop test runner and the
// fuzzer) so it cannot log, and this file had no logging either -- which meant
// the only way to find out WHY a transaction was rejected was to get the file
// off the coordinator and parse it on a laptop. That is a terrible loop when
// the whole point of the device is that it works over QR with no cable.
//
// So the one boundary that sees both the bytes and the verdict says so out
// loud. Sizes and reasons only: never the PSBT itself, and never anything
// derived from the seed.
#ifdef ESP_PLATFORM
#include "esp_log.h"
#define SIGN_LOG(...) ESP_LOGI("sign", __VA_ARGS__)
#else
#define SIGN_LOG(...) ((void)0)
#endif

static void log_summary(const char *src);   // defined beside the QR path below

// Field-debug escape hatch: dump the received PSBT as hex so it can be pulled
// off the serial line and parsed on a host with tools/psbt_fields.py. Exists
// because "what did the coordinator ACTUALLY send" is otherwise unanswerable
// without asking the user to export a file, which defeats the point of a
// device whose whole workflow is QR.
//
// OFF by default and must stay that way: an unsigned PSBT is not a secret, but
// it is the user's financial history -- amounts, addresses, counterparties --
// and none of that belongs on a debug line by accident. Turn it on for one
// build, read the log, turn it off.
//   idf.py -B build-disp build -DKISS_PSBT_DUMP=1
#if defined(ESP_PLATFORM) && defined(KISS_PSBT_DUMP)
static void log_psbt_hex(const uint8_t *b, size_t n)
{
    static const char H[] = "0123456789abcdef";
    char line[129];                                   // 64 bytes a line
    SIGN_LOG("PSBT DUMP BEGIN %u bytes", (unsigned)n);
    for (size_t off = 0; off < n; off += 64) {
        size_t k = n - off < 64 ? n - off : 64;
        for (size_t i = 0; i < k; i++) {
            line[i * 2]     = H[b[off + i] >> 4];
            line[i * 2 + 1] = H[b[off + i] & 0x0F];
        }
        line[k * 2] = 0;
        SIGN_LOG("PSBT %04u %s", (unsigned)off, line);
    }
    SIGN_LOG("PSBT DUMP END");
}
#else
#define log_psbt_hex(b, n) ((void)0)
#endif

#define HOLD_MS   1200
// HOLD TO SIGN ignores presses for this long after I UNDERSTAND was tapped.
// The redraw moved I UNDERSTAND into the caution row, so the two no longer
// overlap in x and this is no longer the only thing standing between a double
// tap and a signature nobody read. It stays because the repaint still swaps
// what is under the finger, and 500ms is the cost of nothing: the finger has to
// travel from the row to the action bar and the screen has to repaint first.
#define SIGN_ARM_MS 500
#define MAX_FILES 8
#define SHOW_OUTS 3

enum { SRC_SD = 0, SRC_QR = 1 };

static lv_obj_t *s_scr;
static lv_obj_t *s_arc, *s_sign_lbl;
static lv_timer_t *s_hold_tmr;
static uint32_t s_hold_t0;
static char s_files[MAX_FILES][SD_NAME_LEN];
static char s_cur[SD_NAME_LEN];
static wpsbt_summary_t s_sum;
static bool s_ack;                      // every caution acknowledged? (gates hold-to-sign)
static uint16_t s_ack_flags;            // WHICH ones, so each row answers for itself
static uint32_t s_ack_t0;               // when, for SIGN_ARM_MS below
static uint8_t s_in[4096], s_out[4680];
static lv_obj_t *s_parent;             // where this flow's screens are built
static int s_src;                      // SRC_SD / SRC_QR: where the PSBT came from
static int s_qr_fmt;                   // QRT_FMT_* the scan arrived in
static qrt_encoder_t *s_qenc;          // QR-out encoder (animated signed PSBT)
static lv_timer_t *s_qr_tmr;
static lv_timer_t *s_done_tmr;   // SD sign: auto-return to home after the success screen
static lv_obj_t *s_qr_img, *s_part_lbl;
static int s_part_i;
static bool s_qr_ez;                   // easy-scan mode: sparser QRs, slower loop
static size_t s_out_len;               // signed PSBT length (easy-scan re-encodes)
static lv_obj_t *s_ez_pill;
static char s_sig_fp[9];               // fingerprint of the just-signed PSBT (8 hex)
static char s_done_name[SD_NAME_LEN + 8]; // saved outname, so the ? panel can rebuild

static void qr_out_screen(size_t sw);

bool wallet_sign_active(void) { return s_scr != NULL; }

static void hold_stop(void)
{
    if (s_hold_tmr) { lv_timer_delete(s_hold_tmr); s_hold_tmr = NULL; }
    if (s_arc) lv_arc_set_value(s_arc, 0);
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    hold_stop();
    s_arc = NULL; s_sign_lbl = NULL;
    if (s_done_tmr) { lv_timer_delete(s_done_tmr); s_done_tmr = NULL; }
    if (s_qr_tmr) { lv_timer_delete(s_qr_tmr); s_qr_tmr = NULL; }
    if (s_qenc) { qrt_encoder_free(s_qenc); s_qenc = NULL; }
    s_qr_img = NULL; s_part_lbl = NULL; s_ez_pill = NULL;
    wallet_psbt_free();
    platform_sd_unmount();
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
}

void wallet_sign_close(void) { close_cb(NULL); }   // idle auto-lock path

// ---- BACK means one step back, not "abandon SIGN" ----
//
// Every BACK in this flow used to be close_cb, which tears the whole thing
// down: it unmounts the card and drops the user on the home screen. Opening
// the wrong PSBT therefore cost them the entire trip back through
// SIGN > FROM SD CARD > pick the card > find the list. step_back() drops only
// the current screen and whatever transaction it had loaded; the caller then
// rebuilds the screen behind it.
static void sd_open(lv_obj_t *parent);

static void step_back(void)
{
    hold_stop();
    s_arc = NULL; s_sign_lbl = NULL;
    if (s_qr_tmr) { lv_timer_delete(s_qr_tmr); s_qr_tmr = NULL; }
    if (s_qenc) { qrt_encoder_free(s_qenc); s_qenc = NULL; }
    s_qr_img = NULL; s_part_lbl = NULL; s_ez_pill = NULL;
    wallet_psbt_free();               // the next pick loads its own
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
}

static void files_back_cb(lv_event_t *e)      // -> the PSBT file list
{
    (void)e;
    lv_obj_t *parent = s_parent;
    step_back();
    sd_open(parent);                          // the card stays mounted
}

static void choose_back_cb(lv_event_t *e)     // -> SCAN QR / FROM SD CARD
{
    (void)e;
    lv_obj_t *parent = s_parent;
    step_back();
    platform_sd_unmount();                    // leaving the SD path for good
    wallet_sign_open(parent);
}

// ---- shared bits: thin wrappers over the wallet_theme kit (module keeps
// its s_scr; call sites keep their historical signatures) ----
// This module owns exactly ONE screen at a time. Overwriting s_scr without
// deleting what it pointed at does not close that screen, it orphans it: the
// old one stays parented to s_parent, underneath the new one, and the only way
// to find out is to press BACK enough times to peel the top one off and see a
// transaction you already left.
//
// Every call site clears s_scr itself rather than leaning on the recovery
// below, because clearing it is never just the delete: the hold timer ticks
// against s_arc, and s_arc and s_sign_lbl both point into the outgoing screen.
// mk_screen() cannot see any of that, so a call site that let it do the tidying
// would leave a timer running against a freed arc. The branch below is a net,
// not a mechanism.
#ifdef SIMULATOR
int g_sign_orphaned_screens;          // sim_main.c fails the walk on this
#endif

static void mk_screen(lv_obj_t *parent, const char *title, const char *sub)
{
    if (s_scr) {
        // Every real call site deletes and nulls s_scr before it gets here, so
        // reaching this branch is always a bug, never housekeeping. On device
        // it recovers silently; in the sim it fails the walk, which is the
        // regression test -- the frames cannot catch this on their own, because
        // an orphan is perfectly hidden under the screen that replaced it right
        // up until the moment a BACK uncovers it.
#ifdef SIMULATOR
        g_sign_orphaned_screens++;
        fprintf(stderr, "ORPHANED SIGN SCREEN: mk_screen(\"%s\") ran with a live "
                        "s_scr; the outgoing screen stays parented underneath\n",
                title ? title : "");
#endif
        lv_obj_delete_async(s_scr);
    }
    s_scr = wt_screen(parent, title, sub);
}

static lv_obj_t *mk_pill(const char *txt, int x, int y, int w, lv_event_cb_t cb)
{
    return wt_pill(s_scr, txt, x, y, w, cb, NULL);
}

// wt_note in a colour other than MUT: the STOP-red refusals and the amber
// prompts on the SD screens are notes like any other, they just are not grey.
static lv_obj_t *wt_note_col(lv_obj_t *par, const char *txt, int x, int y,
                             int w, int h, lv_color_t col)
{
    lv_obj_t *l = wt_note(par, txt, x, y, w, h);
    lv_obj_set_style_text_color(l, col, 0);
    return l;
}

static lv_obj_t *mk_lbl(const char *txt, int x, int y, const lv_font_t *f, lv_color_t col)
{
    return wt_lbl(s_scr, txt, x, y, f, col);
}

static const char *sp_onchain_note(void)
{
    // The two prefixes are arguments, so this reads the same in every locale
    // instead of falling back to the generic badge note outside English.
    static char buf[200];
    snprintf(buf, sizeof buf, tr(STR_S_SP_ONCHAIN_FMT),
             s_sum.testnet ? "tsp1" : "sp1",
             s_sum.testnet ? "tb1p" : "bc1p");
    return buf;
}

// wallet_psbt.c stays LVGL/i18n-free (it feeds the desktop test runner and the
// fuzzer), so its English STOP reasons are mapped to translations here, at the
// UI boundary. Unknown reasons fall back to the raw English string.
static const char *tr_reason(const char *r)
{
    static const struct { const char *en; int id; } MAP[] = {
        {"nonstandard output script", STR_P_NONSTD_OUT},
        {"malformed: tx/psbt count mismatch", STR_P_MALFORMED},
        {"too many outputs", STR_P_TOO_MANY_OUTS},
        {"sighash is not ALL", STR_P_SIGHASH},
        {"input is not this wallet's", STR_P_NOT_MINE},
        {"wrong network: mainnet transaction", STR_P_WRONG_NET_MAIN},
        {"wrong network: testnet transaction", STR_P_WRONG_NET_TEST},
        {"unsupported input derivation path", STR_P_BAD_PATH},
        {"input's previous transaction does not match", STR_P_PREV_MISMATCH},
        {"legacy input needs its full previous transaction", STR_P_LEGACY_PREV},
        {"input amount unverifiable", STR_P_AMT_UNVERIFIED},
        {"input amount over 21M BTC (corrupt)", STR_P_AMT_HUGE_IN},
        {"input script does not re-derive", STR_P_IN_NO_DERIVE},
        {"too many outputs to verify safely", STR_P_TOO_MANY_VERIFY},
        {"output amount over 21M BTC (corrupt)", STR_P_AMT_HUGE_OUT},
        {"change address does not re-derive", STR_P_CHANGE_NO_DERIVE},
        {"outputs exceed inputs", STR_P_OUT_GT_IN},
        {"unknown data in this transaction", STR_P_UNKNOWN_DATA},
        {"SP output needs PSBTv2", STR_P_SP_NEED_V2},
        {"silent payments need native segwit inputs", STR_P_SP_INPUTS},
        // BIP376 receive-spend failures. "not this wallet's" reuses the exact
        // send-side wording; the malformed-field cases map to the generic
        // malformed-tx string (rare, hostile-input paths) to avoid new i18n keys.
        {"silent-payment input is not this wallet's", STR_P_NOT_MINE},
        {"silent-payment input must be taproot", STR_P_MALFORMED_TX},
        {"malformed SP spend derivation", STR_P_MALFORMED_TX},
        {"malformed SP tweak", STR_P_MALFORMED_TX},
        {"silent payment key derivation failed", STR_P_SP_DERIVE},
        {"silent payment derivation failed", STR_P_SP_DERIVE},
        {"silent payment self-check failed", STR_P_SP_DERIVE},
        {"too many inputs for silent payments", STR_P_SP_DERIVE},
        {"malformed SP output info", STR_P_SP_INFO},
        {"malformed SP output label", STR_P_SP_INFO},
        {"malformed transaction (v2 fields)", STR_P_MALFORMED_TX},
        {"malformed transaction (v2 extract)", STR_P_MALFORMED_TX},
        {"malformed transaction", STR_P_MALFORMED_TX},
    };
    for (size_t i = 0; i < sizeof MAP / sizeof MAP[0]; i++)
        if (strcmp(r, MAP[i].en) == 0) return tr(MAP[i].id);
    return r;
}

// same grouped-by-4 convention as the Receive screen: visual compare against
// the coordinator is the whole point of this screen
#define group4   wt_group4
#define fmt_sats wt_fmt_sats

// ---- signing ----
static void auto_home_cb(lv_timer_t *t)   // SD success screen returns to home on its own
{
    s_done_tmr = NULL;
    lv_timer_delete(t);
    close_cb(NULL);
}

static void done_screen(const char *outname)
{
    lv_obj_t *parent = lv_obj_get_parent(s_scr);
    lv_obj_delete(s_scr); s_scr = NULL; s_arc = NULL; s_sign_lbl = NULL;
    // Two lines, drawn here rather than by wt_screen: "return this card to
    // Sparrow, load the -signed.psbt file, then broadcast" is the whole point
    // of the screen and does not fit one line at a readable size. Nothing is
    // above the checkmark at y=150, so the second line costs nothing.
    mk_screen(parent, tr(STR_S_SIGNED_T), NULL);
    wt_note(s_scr, tr(STR_S_DONE_SD_SUB), 48, 66, 704, 58);
    lv_obj_t *big = mk_lbl(LV_SYMBOL_OK, 0, 150, &lv_font_montserrat_48, OK_COL);
    lv_obj_align(big, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_t *fn = mk_lbl(outname, 0, 230, wt_font28(), INK_COL);
    lv_obj_align(fn, LV_ALIGN_TOP_MID, 0, 230);
    // "take the card back to your coordinator" is the next thing to do, and
    // this screen auto-returns home after 6s. It has 110px of empty width-704
    // page under it; it does not need to be the small type.
    lv_obj_t *note = wt_note(s_scr, tr(STR_S_SAVED_NOTE), 48, 284, 704, 90);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    mk_pill(tr(STR_C_DONE), 330, WT_ACTION_Y, 140, close_cb);
    // nothing needs to stay on screen (the file is saved), so drift back to home
    s_done_tmr = lv_timer_create(auto_home_cb, 6000, NULL);
    lv_timer_set_repeat_count(s_done_tmr, 1);
}

static void fail_screen(const char *why)
{
    lv_obj_t *parent = lv_obj_get_parent(s_scr);
    lv_obj_delete(s_scr); s_scr = NULL; s_arc = NULL; s_sign_lbl = NULL;
    mk_screen(parent, tr(STR_S_FAIL_T), why);
    mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, close_cb);
}

// Spending from receive index N proves N was used: record it so the Receive
// screen hands out a fresh address next time (reuse guard). Best effort: only
// the inputs KISS could show; the coordinator remains the source of truth.
static void mark_used_receives(void)
{
    wpsbt_details_t det;
    if (wallet_psbt_details(&det) != 0)
        return;
    uint8_t fp[4];
    wallet_ui_last_fp(fp);
    for (uint32_t i = 0; i < det.n_in; i++)
        if (det.ins[i].change == 0) {                // 0 = receive branch (1 = change)
            // key by THIS input's own type, not the current Settings type: we
            // sign native/nested/legacy regardless of the setting, and Receive
            // buckets the guard per type, so a mismatch would mark the wrong one
            int sc = det.ins[i].purpose == 44 ? WSCRIPT_LEGACY
                   : det.ins[i].purpose == 49 ? WSCRIPT_NESTED : WSCRIPT_NATIVE;
            wallet_usage_mark(fp, s_sum.testnet ? 1 : 0, sc, det.ins[i].index);
        }
}

static void do_sign_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    size_t sw = 0;
    if (wallet_psbt_sign(s_out, sizeof s_out, &sw) != 0) {
        fail_screen(tr(STR_S_FAIL_SIGN));
        return;
    }
    // Fingerprint the signature now, while the bytes are in hand, for both exit
    // screens. On the (unexpected) failure path it is left empty and the screens
    // just omit the aid.
    if (wallet_psbt_sig_fingerprint(s_out, sw, s_sig_fp) != 0)
        s_sig_fp[0] = 0;
    mark_used_receives();
    if (s_src == SRC_QR) {                       // came by QR: goes back by QR
        qr_out_screen(sw);
        return;
    }
    char outname[SD_NAME_LEN + 8];
    size_t bl = strlen(s_cur);
    if (bl > 5) bl -= 5;                                  // strip ".psbt"
    snprintf(outname, sizeof outname, "%.*s-signed.psbt", (int)bl, s_cur);
    int rc = platform_sd_write(outname, s_out, sw);
    if (rc != 0) {
        fail_screen(tr(STR_S_FAIL_SD_WRITE));
        return;
    }
    done_screen(outname);
}

static void hold_tick(lv_timer_t *t)
{
    (void)t;
    uint32_t el = lv_tick_elaps(s_hold_t0);
    if (s_arc) lv_arc_set_value(s_arc, (int32_t)(el * 100 / HOLD_MS));
    if (el >= HOLD_MS) {
        hold_stop();
        if (s_arc) lv_arc_set_value(s_arc, 100);
        if (s_sign_lbl) lv_label_set_text(s_sign_lbl, tr(STR_S_SIGNING));
        lv_timer_create(do_sign_cb, 30, NULL);            // let the label paint first
    }
}

static void sign_press_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        // Not yet armed: this press is the tail of the one that acknowledged
        // the caution, landing on the button that replaced it. Swallow it.
        if (s_ack_t0 && lv_tick_elaps(s_ack_t0) < SIGN_ARM_MS) return;
        s_hold_t0 = lv_tick_get();
        if (!s_hold_tmr) s_hold_tmr = lv_timer_create(hold_tick, 30, NULL);
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        hold_stop();                                      // let go early = no signature
    }
}

static void details_cb(lv_event_t *e);
static void verify_screen(lv_obj_t *parent);

// The full "why", plain words + the concrete next step (freeze/label in the
// coordinator). One wt_explain_open card like every other "?" on the device: the
// reasons stack in the first block and the footer, which is the thing to DO
// about them, takes the second. The blank line between them was already in the
// composed string, so the split costs nothing.
static void caution_help_cb(lv_event_t *e)
{
    (void)e;
    // sized for the longest translations (Cyrillic/CJK run 2-3 bytes per char);
    // every append clamps o because snprintf returns the WOULD-BE length
    char body[1536];
    size_t o = 0;
    uint16_t f = s_sum.caution_flags;
    #define BODY_ADD(...) do { \
        if (o + 1 < sizeof body) { \
            int w_ = snprintf(body + o, sizeof body - o, __VA_ARGS__); \
            if (w_ > 0) { o += (size_t)w_; if (o >= sizeof body) o = sizeof body - 1; } \
        } } while (0)
    if (f & WPSBT_C_HIGHFEE)
        BODY_ADD("%s\n", tr(STR_S_WHY_HIGHFEE));
    if (f & WPSBT_C_DUST_INPUT)
        BODY_ADD("%s\n", tr(STR_S_WHY_DUSTIN));
    if (f & (WPSBT_C_DUST_CHANGE | WPSBT_C_SMALL_CHANGE))
        BODY_ADD("%s\n", tr(STR_S_WHY_TINYCH));
    BODY_ADD(o ? "\n%s" : "%s", tr(STR_S_WHY_FOOT));
    #undef BODY_ADD

    wt_explain_t x = {
        .title  = tr(STR_S_WHY_T),
        .icon   = LV_SYMBOL_WARNING,
        .body   = body,
        .ok_txt = tr(STR_C_OK),
        .sev    = WT_SEV_WARN,
    };
    wt_explain_open(s_scr, &x);
}

// ---- "?" on the RBF line: plain-words Replace-By-Fee ----
static void rbf_help_cb(lv_event_t *e)
{
    (void)e;
    // The card was a wall of text about a yes/no property of the transaction,
    // and the two answers looked identical until you read to the end. The icon
    // badge says which one this is before a word is read: a replace arrow when
    // the fee can still be raised, a padlock when it cannot.
    //
    // The replaceable state is PLAIN, not OK, and that is deliberate. One
    // element rendering an accent in one state and a status colour in the other
    // is the thing ADDENDUM-02 leads with, and GREEN theme is where it bites:
    // the accent there is 0x35D07F, which IS WT_OK, so "the fee can still be
    // raised" would arrive in the exact green this device uses to say verified.
    // That is a neutral property of the transaction wearing the colour of a
    // safety check. The glyphs differ either way, so meaning never rested on
    // colour. MONO is unchanged to the byte: its accent IS WT_INK.
    const char *body = s_sum.rbf ? tr(STR_S_RBF_B_ON) : tr(STR_S_RBF_B_OFF);
    wt_explain_t x = {
        .title  = s_sum.rbf ? tr(STR_S_RBF_T_ON) : tr(STR_S_RBF_T_OFF),
        .icon   = s_sum.rbf ? WT_ICON_REPLACE : WT_ICON_LOCK,
        .body   = body,
        .ok_txt = tr(STR_C_OK),
        .sev    = s_sum.rbf ? WT_SEV_PLAIN : WT_SEV_WARN,
    };
    wt_explain_open(s_scr, &x);
}

// The verify screen has no partial redraw: every state change rebuilds it. The
// rebuild has to drop the screen-scoped state first, because the hold timer
// ticks against s_arc and both s_arc and s_sign_lbl are about to point at
// objects on the outgoing screen. Both call sites need this and each one used to
// spell it out; the one that forgot a line is what shipped the orphan.
static void repaint_verify(void)
{
    hold_stop();
    lv_obj_delete_async(s_scr); s_scr = NULL; s_arc = NULL; s_sign_lbl = NULL;
    verify_screen(s_parent);
}

// ---- verify screen (the heart of the safety model) ----
// Panel geometry, from design/sign-screens-buildable.html option 1b. Every one
// of these was drawn at true 800x480 and measured there rather than guessed.
#define SG_PANEL_Y   150
#define SG_PANEL_H   120
#define SG_RECIP_X    24
#define SG_RECIP_W   468
#define SG_CHANGE_X  506
#define SG_CHANGE_W  270
#define SG_PAD        15
#define SG_FOOT_RULE 288
#define SG_FOOT_Y    300
#define SG_ROW_H      56   // a caution row is NEVER taller than this: a taller
                           // row pushes the footer into the action bar
#define SG_ROW_PILL_W 170

// The action row spans the same lane as everything above it: 24..776.
//
// It did not, and that is what the redraw looked wrong from across the room for.
// Every panel on this screen ends at 776 (SG_CHANGE_X + SG_CHANGE_W), the caution
// rows are 24 + 752, and the footer cells sit inside the same lane, but BACK was
// drawn at the app wide WT_BACK_X and stopped at 750. A 26px step, in the one
// place the eye is already tracking a hard vertical edge down the page.
//
// WT_BACK_X is NOT changed to fix this, and the distinction matters. The app has
// no single content lane: Settings' right column ends at 770 and Receive's at
// about 751, both measured off the frames, so moving the shared constant to 776
// would leave BACK hanging past the content on those screens instead. 610 is very
// nearly right for a 48px page margin, which is what wallet_theme.c:373 declares
// and what every screen except this one is drawn to. This screen is the exception,
// so this screen carries the exception.
//
// HANDOFF-01 lists WT_BACK_X 610 under "fixed action geometry" while its own
// coordinate table puts the panels at 24..776. Both cannot hold. The lane won,
// because the lane is the thing the owner can see. That needs amending in
// HANDOFF-01 constraint 3 and in design/README.md rule 3.
// Re-measured off redraw 01, which reverses the order this file used to build:
// BACK at 48, DETAILS at 160, and HOLD TO SIGN in the FAR RIGHT of the lane.
//
// The way out sits leftmost where a thumb rests and can be found without
// looking, and the right corner is reserved for the action that does the
// screen's work. Redraws 01, 02 and 03 all agree on that, so Receive follows the
// same rule. The old arrangement put DETAILS leftmost and BACK in the corner,
// which gave the escape hatch the most valuable target on a screen whose one
// irreversible action was in the middle.
//
// The safety property that mattered is unchanged and is the reason HOLD keeps
// hard coordinates: it never moves, never changes width and never changes label
// between the normal and caution screens, so a tap learned on one lands on the
// same pill on the other.
#define SG_BACK_X     48   // 48..152, leftmost
#define SG_DETAILS_X 160   // 160..310
#define SG_HOLD_X    466   // 466..776, flush with the panels' right edge
#define SG_ARC_DX      8   // the hold arc's inset from HOLD TO SIGN's left edge

// A caution row carries its own acknowledgement now, so the answer to "I read
// it" lives next to the thing being read instead of in the action row. That is
// what frees HOLD TO SIGN to keep its coordinates in both states, which is the
// entire safety argument of the redraw: the old layout put I UNDERSTAND at
// 238..490 and the sign pill at 310..582, so two taps in the same place could
// become a signature nobody read. SIGN_ARM_MS still guards the seam.
static uint16_t caution_rows(uint16_t f, const char **parts, uint16_t *bits, int cap)
{
    int n = 0;
    if (n < cap && (f & WPSBT_C_HIGHFEE))
        { bits[n] = WPSBT_C_HIGHFEE;     parts[n++] = tr(STR_S_C_HIGHFEE); }
    if (n < cap && (f & WPSBT_C_DUST_INPUT))
        { bits[n] = WPSBT_C_DUST_INPUT;  parts[n++] = tr(STR_S_C_DUSTIN); }
    if (n < cap && (f & WPSBT_C_DUST_CHANGE))
        { bits[n] = WPSBT_C_DUST_CHANGE; parts[n++] = tr(STR_S_C_DUSTCH); }
    else if (n < cap && (f & WPSBT_C_SMALL_CHANGE))
        { bits[n] = WPSBT_C_SMALL_CHANGE; parts[n++] = tr(STR_S_C_SMALLCH); }
    return (uint16_t)n;
}

static uint16_t caution_all_bits(uint16_t f)
{
    const char *p[4]; uint16_t b[4];
    uint16_t n = caution_rows(f, p, b, 4), all = 0;
    for (int i = 0; i < n; i++) all |= b[i];
    return all;
}

static void row_ack_cb(lv_event_t *e)
{
    uint16_t bit = (uint16_t)(uintptr_t)lv_event_get_user_data(e);
    s_ack_flags |= bit;
    s_ack_t0 = lv_tick_get();               // arm the seam, same as the old gate
    if ((s_ack_flags & caution_all_bits(s_sum.caution_flags))
        == caution_all_bits(s_sum.caution_flags))
        s_ack = true;
    repaint_verify();                       // this row goes green, and
}                                           // HOLD TO SIGN lights when all are in

// A raised block: WT_PANEL on a 1px border, radius 12. Used for both output
// panels and every caution row, so they read as the same kind of object.
static lv_obj_t *sg_panel(int x, int y, int w, int h, lv_color_t border)
{
    lv_obj_t *p = lv_obj_create(s_scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, WT_PANEL, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p, border, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static lv_obj_t *sg_lbl(lv_obj_t *par, const char *txt, int x, int y,
                        const lv_font_t *f, lv_color_t col)
{
    lv_obj_t *l = lv_label_create(par);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, col, 0);
    return l;
}

// A 1px rule. WT_DIV, because it divides content inside one surface rather than
// marking where a surface meets the page (that is WT_HAIR, and it belongs to
// the action bar alone).
static void sg_rule(int x, int y, int w, int h)
{
    lv_obj_t *r = lv_obj_create(s_scr);
    lv_obj_remove_style_all(r);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, w, h);
    lv_obj_set_style_bg_color(r, WT_DIV, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
}

// One footer cell: caption, value, qualifier. The value is the only thing here
// that changes a decision, so it takes the middle rung; the other two are 14.
static void sg_cell(int x, int w, const char *cap, const char *val,
                    const lv_font_t *vf, lv_color_t vc)
{
    if (cap) {
        lv_obj_t *c = sg_lbl(s_scr, cap, x, SG_FOOT_Y, wt_font14(), MUT_COL);
        lv_obj_set_style_text_letter_space(c, 2, 0);
    }
    lv_obj_t *v = lv_label_create(s_scr);
    lv_obj_set_pos(v, x, SG_FOOT_Y + 20);
    lv_obj_set_style_text_color(v, vc, 0);
    if (vf) {                       // a mono figure: fixed width, known to fit
        lv_label_set_text(v, val);
        lv_obj_set_style_text_font(v, vf, 0);
    } else {
        // A TRANSLATED value. Sized to the column rather than assumed to fit:
        // "MAINNET, real bitcoin" is about 250px at font23 in a 210px cell, and
        // an unwidthed LVGL label runs into its neighbour silently.
        wt_note_fit(v, val, w, 29);
        // wt_note_fit picks the rung that fits; it does not bound the box.
        // Russian "TESTNET, тренировочные монеты" still ran 2px into the RBF
        // cell at the smallest rung, so clamp and let it ellipsise. A network
        // name the owner can half read is recoverable; one drawn over its
        // neighbour is not.
        lv_obj_set_width(v, w);
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    }
}

static void verify_screen(lv_obj_t *parent)
{
    char buf[160], a[32], b[32];
    s_parent = parent;                    // details page rebuilds us from here
    mk_screen(parent, tr(STR_S_T), NULL);

    const char *parts[4]; uint16_t bits[4];
    uint16_t np = (s_sum.status == WPSBT_CAUTION)
                    ? caution_rows(s_sum.caution_flags, parts, bits, 4) : 0;

    // ---- header ---------------------------------------------------------
    // The filename is not translated and never will be, so it is mono: it is
    // the one string on this row whose exact characters the owner may need to
    // read back against what their coordinator sent.
    // wt_screen puts the title at x=48, font34, letter_space 3. The drawing's
    // x=132 assumed a tighter title than this device actually draws, and in a
    // locale whose word for SIGN is longer than English the collision gets
    // worse. Measured, not assumed.
    {
        lv_point_t ts;
        lv_text_get_size(&ts, tr(STR_S_T), wt_font34(), 3, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        sg_lbl(s_scr, s_cur, 48 + ts.x + 18, 34, wt_font_mono14(), MUT_COL);
    }

    // The chip at the top right is ONE slot in two states. The caution count
    // replaces the fingerprint at the same x, y, w and h, so nothing new can
    // ever appear here and collide with the title or the filename. That is
    // defect 01 from the review, closed by deletion rather than by relocation.
    {
        lv_obj_t *chip = sg_panel(540, 14, 236, 36, np ? WARN_COL : WT_EDGE);
        lv_obj_set_style_radius(chip, 10, 0);   // was 18: half of 36, a lozenge
        lv_obj_set_style_bg_opa(chip, LV_OPA_TRANSP, 0);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(chip, 10, 0);
        if (np) {
            // Two labels, not one. The symbol is a FontAwesome codepoint and
            // the mono faces carry no icon plane, so setting both in mono drew
            // the count next to a placeholder box. That box is the missing
            // fallback doing its job: it makes a wrong font obvious instead of
            // quietly resolving one size smaller.
            lv_obj_t *g = lv_label_create(chip);
            lv_label_set_text(g, LV_SYMBOL_WARNING);
            lv_obj_set_style_text_font(g, wt_font23(), 0);
            lv_obj_set_style_text_color(g, WARN_COL, 0);
            snprintf(buf, sizeof buf, "%u", (unsigned)np);
            lv_obj_t *t = lv_label_create(chip);
            lv_label_set_text(t, buf);
            lv_obj_set_style_text_font(t, wt_font_mono23(), 0);
            lv_obj_set_style_text_color(t, WARN_COL, 0);
        } else {
            uint8_t fp[4];
            wallet_ui_last_fp(fp);
            lv_obj_t *c = lv_label_create(chip);
            lv_label_set_text(c, tr(STR_S_SIGNING_AS));
            lv_obj_set_style_text_font(c, wt_font14(), 0);
            lv_obj_set_style_text_color(c, MUT_COL, 0);
            snprintf(buf, sizeof buf, "%02X%02X%02X%02X", fp[0], fp[1], fp[2], fp[3]);
            lv_obj_t *f = lv_label_create(chip);
            lv_label_set_text(f, buf);
            lv_obj_set_style_text_font(f, wt_font_mono14(), 0);
            lv_obj_set_style_text_color(f, INK_COL, 0);
        }
    }
    // 64, not 60: the title's line box at font34 ends at y=61.
    sg_rule(0, 64, 800, 1);
    // No mk_status_light(). The header chip carries the status word now, in the
    // same corner the badge used, and drawing both put "CAUTION" on top of the
    // count that says the same thing.

    // ---- the hero -------------------------------------------------------
    // One number, not two. The old screen showed RECIPIENT GETS and TOTAL
    // LEAVING at the same rung and left the owner to work out which one they
    // were agreeing to. What leaves the wallet is the number being signed for.
    uint64_t total = s_sum.send_sats + s_sum.fee_sats;
    {
        lv_obj_t *cap = sg_lbl(s_scr, tr(STR_S_TOTAL_LEAVING), 24, 74,
                               wt_font14(), MUT_COL);
        lv_obj_set_style_text_letter_space(cap, 2, 0);

        lv_obj_t *row = lv_obj_create(s_scr);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 24, 88);
        lv_obj_set_size(row, 760, 52);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_column(row, 14, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        fmt_sats(total, a, sizeof a);
        // font_kiss_num48: digits, A to F, space and full stop. It cannot spell
        // a word, so it can never be handed a translated string by accident.
        lv_obj_t *big = lv_label_create(row);
        lv_label_set_text(big, a);
        lv_obj_set_style_text_font(big, wt_font_num48(), 0);
        lv_obj_set_style_text_color(big, INK_COL, 0);

        lv_obj_t *unit = lv_label_create(row);
        lv_label_set_text(unit, "sats");
        lv_obj_set_style_text_font(unit, wt_font23(), 0);
        lv_obj_set_style_text_color(unit, INK_COL, 0);

        wt_fmt_btc(total, b, sizeof b);
        snprintf(buf, sizeof buf, "%s BTC", b);
        lv_obj_t *btc = lv_label_create(row);
        lv_label_set_text(btc, buf);
        lv_obj_set_style_text_font(btc, wt_font_mono14(), 0);
        lv_obj_set_style_text_color(btc, MUT_COL, 0);
    }

    // ---- STOP: the verdict is the screen, nothing else is ----------------
    if (s_sum.status == WPSBT_STOP) {
        lv_obj_t *p = sg_panel(24, SG_PANEL_Y, 752, SG_PANEL_H, STOP_COL);
        lv_obj_t *r = sg_lbl(p, tr_reason(s_sum.reason), SG_PAD, SG_PAD,
                             wt_font23(), STOP_COL);
        lv_obj_set_width(r, 752 - 2 * SG_PAD);
        lv_label_set_long_mode(r, LV_LABEL_LONG_WRAP);
        // Same 752 lane as the panel it just drew, so the same BACK as verify.
        wt_pillh(s_scr, tr(STR_C_BACK), SG_BACK_X, WT_ACTION_Y, 140, WT_ACTION_H,
                 s_src == SRC_SD ? files_back_cb : choose_back_cb, NULL);
        return;
    }

    if (np) {
        // ---- caution rows -------------------------------------------------
        // Each row is one line by construction and never taller than 56px, so
        // the stack's height is known before it is built. Two rows keep the
        // footer strip below them, exactly as drawn. Three or four drop it:
        // fee, network and RBF all already live on DETAILS, and a row that
        // wraps to gain height would push the footer into the action bar.
        int gap = (np >= 4) ? 4 : 8;
        bool keep_footer = (np <= 2);
        int y = SG_PANEL_Y;
        for (int i = 0; i < np; i++) {
            bool done = (s_ack_flags & bits[i]) != 0;
            lv_obj_t *row = sg_panel(24, y, 752, SG_ROW_H,
                                     done ? OK_COL : WARN_COL);
            sg_lbl(row, done ? LV_SYMBOL_OK : LV_SYMBOL_WARNING, SG_PAD, 16,
                   wt_font23(), done ? OK_COL : WARN_COL);
            // The text column runs from x=52 inside the row to the pill's left
            // edge at 543, so 491px at font14. wt_note_fit drops a rung rather
            // than taking a second line, because one row is one line, always.
            lv_obj_t *t = lv_label_create(row);
            lv_obj_set_pos(t, 52, 19);
            lv_obj_set_style_text_color(t, done ? MUT_COL : INK_COL, 0);
            wt_note_fit(t, parts[i], 491 - 16, 24);

            if (done) {
                lv_obj_t *p = sg_panel(543, 8, SG_ROW_PILL_W, 40, OK_COL);
                lv_obj_set_style_radius(p, 10, 0);   // was 20: half of 40, a lozenge
                lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
                lv_obj_set_flex_flow(p, LV_FLEX_FLOW_ROW);
                lv_obj_set_flex_align(p, LV_FLEX_ALIGN_CENTER,
                                      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
                lv_obj_set_parent(p, row);
                lv_obj_set_pos(p, 543, 8);
                lv_obj_t *l = lv_label_create(p);
                lv_label_set_text(l, LV_SYMBOL_OK);
                lv_obj_set_style_text_font(l, wt_font23(), 0);
                lv_obj_set_style_text_color(l, OK_COL, 0);
            } else {
                wt_pillh(row, tr(STR_C_I_UNDERSTAND), 543, 8, SG_ROW_PILL_W, 40,
                         row_ack_cb, (void *)(uintptr_t)bits[i]);
            }
            y += SG_ROW_H + gap;
        }
        if (keep_footer) goto footer;
        goto actions;
    }

    // ---- the two outputs -------------------------------------------------
    {
        int recipient_n = 0, change_n = 0;
        for (int i = 0; i < (int)s_sum.n_out && i < WPSBT_MAX_OUTS; i++)
            { if (s_sum.outs[i].is_change) change_n++; else recipient_n++; }

        int rw = change_n ? SG_RECIP_W : 752;
        lv_obj_t *rp = sg_panel(SG_RECIP_X, SG_PANEL_Y, rw, SG_PANEL_H, WT_HAIR);
        lv_obj_t *cap = sg_lbl(rp, tr(STR_S_SENDING_OUT), SG_PAD, 12,
                               wt_font14(), MUT_COL);
        lv_obj_set_style_text_letter_space(cap, 2, 0);

        // Every output is still shown. One recipient is the common case and
        // gets the panel to itself; more than one scrolls inside it, because
        // nothing the owner is asked to sign may be hidden.
        lv_obj_t *list = lv_obj_create(rp);
        lv_obj_remove_style_all(list);
        lv_obj_set_pos(list, SG_PAD, 34);
        lv_obj_set_size(list, rw - 2 * SG_PAD, SG_PANEL_H - 42);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(list, 6, 0);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        // MODE_ON, not AUTO: a list with more below the fold must not look
        // identical to one that ends there.
        lv_obj_set_scrollbar_mode(list, recipient_n > 1 ? LV_SCROLLBAR_MODE_ON
                                                        : LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_width(list, 5, LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_color(list, MUT_COL, LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_opa(list, LV_OPA_50, LV_PART_SCROLLBAR);

        for (int i = 0; i < (int)s_sum.n_out && i < WPSBT_MAX_OUTS; i++) {
            if (s_sum.outs[i].is_change) continue;
            char ga[200];
            if (recipient_n > 1) {
                fmt_sats(s_sum.outs[i].sats, a, sizeof a);
                snprintf(buf, sizeof buf, "%s sats", a);
                lv_obj_t *amt = lv_label_create(list);
                lv_label_set_text(amt, buf);
                lv_obj_set_style_text_font(amt, wt_font_mono23(), 0);
                lv_obj_set_style_text_color(amt, INK_COL, 0);
            }
            // UNGROUPED, deliberately. Grouped in fours a 42 character address
            // becomes 52 and no longer fits the panel, so it wraps and its
            // second line lands where the comparison belongs. Measured in
            // design/sign-screens-buildable.html: 353px ungrouped against a
            // 438px box, 437px grouped. Grouping would also split the final
            // four across a group boundary, because 38 does not divide by four,
            // and that final four is half of what "compare these" points at.
            (void)ga;
            // mono14, not mono23. The 353px the drawing measured is the body
            // at FOURTEEN; the 23 belongs to the two compared runs on their own
            // line beneath. At 23 the whole address is about 580px and wraps
            // again, which is the thing ungrouping was meant to prevent.
            wt_addr_spans(list, s_sum.outs[i].addr, rw - 2 * SG_PAD,
                          wt_font_mono14());
            // The compared runs again, large, on their own line. This is the
            // part of the screen doing security work: the body above is there
            // to be scanned, this is the pair the caption asks you to check
            // against your coordinator. Fixed pitch matters here specifically,
            // because both runs are four characters and so come out the same
            // width, which a proportional face cannot do.
            if (recipient_n == 1) {
                wt_addr_short(list, s_sum.outs[i].addr, wt_font_mono23());
                // The caption HANDOFF-01 asks for, next line down. Without it
                // the two lit runs are still there and still landing at the
                // same character width above the address body, but the reason
                // to keep looking at them is not spelled out. "compare these 8"
                // makes the four plus four total the label claims explicit, in
                // one glance: two runs of four is what the elided line above
                // shows, one four is what the label used to point at.
                lv_obj_t *ccap = lv_label_create(list);
                lv_label_set_text(ccap, tr(STR_S_CMP_8));
                lv_obj_set_style_text_font(ccap, wt_font14(), 0);
                lv_obj_set_style_text_color(ccap, MUT_COL, 0);
                lv_obj_set_style_text_letter_space(ccap, 1, 0);
            }
            if (s_sum.outs[i].is_sp) {
                lv_obj_t *n = lv_label_create(list);
                lv_label_set_text(n, sp_onchain_note());
                lv_obj_set_style_text_font(n, wt_font14(), 0);
                lv_obj_set_style_text_color(n, MUT_COL, 0);
                lv_obj_set_width(n, rw - 2 * SG_PAD);
                lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
            }
        }

        if (change_n) {
            // WT_OK is a status here, not decoration: it means re-derived and
            // verified on this device. Per ADDENDUM-02 the accent never lands
            // on this panel, and the glyph carries the meaning without colour.
            lv_obj_t *cp = sg_panel(SG_CHANGE_X, SG_PANEL_Y, SG_CHANGE_W,
                                    SG_PANEL_H, OK_COL);
            for (int i = 0; i < (int)s_sum.n_out && i < WPSBT_MAX_OUTS; i++) {
                if (!s_sum.outs[i].is_change) continue;
                // Sized to the panel, not assumed to fit: this caption is
                // translated and the panel is only 270 wide, so English alone
                // already ran off the edge and clipped silently.
                lv_obj_t *ct = lv_label_create(cp);
                lv_obj_set_pos(ct, SG_PAD, 12);
                lv_obj_set_style_text_color(ct, OK_COL, 0);
                lv_obj_set_width(ct, SG_CHANGE_W - 2 * SG_PAD);
                lv_label_set_long_mode(ct, LV_LABEL_LONG_WRAP);
                lv_label_set_text(ct, tr_sym(LV_SYMBOL_OK, STR_S_CHANGE_TAG));
                lv_obj_set_style_text_font(ct, wt_font14(), 0);
                fmt_sats(s_sum.outs[i].sats, a, sizeof a);
                snprintf(buf, sizeof buf, "%s sats", a);
                lv_obj_update_layout(ct);
                sg_lbl(cp, buf, SG_PAD, 12 + lv_obj_get_height(ct) + 10,
                       wt_font_mono23(), INK_COL);
                break;
            }
        }
    }

footer:
    // ---- the facts strip -------------------------------------------------
    sg_rule(0, SG_FOOT_RULE, 800, 1);
    fmt_sats(s_sum.fee_sats, a, sizeof a);
    snprintf(buf, sizeof buf, "%s sats", a);
    sg_cell(24, 230, tr(STR_S_FEE), buf, wt_font_mono23(),
            np ? WARN_COL : INK_COL);
    sg_rule(270, SG_FOOT_Y + 2, 1, 70);
    sg_cell(294, 210, tr(STR_I_SEC_NET),
            s_sum.testnet ? tr(STR_I_NET_TEST) : tr(STR_I_NET_MAIN),
            NULL, s_sum.testnet ? WARN_COL : INK_COL);
    sg_rule(520, SG_FOOT_Y + 2, 1, 70);
    // No caption: there is no uppercase "if it gets stuck" key, and inventing
    // one would mean 21 translations for a label the value already states.
    // S_RBF_T_* is already caption-cased, so it carries the cell on its own.
    sg_cell(544, 190, NULL,
            s_sum.rbf ? tr(STR_S_RBF_T_ON) : tr(STR_S_RBF_T_OFF),
            NULL, INK_COL);
    wt_help_chip(s_scr, 738, SG_FOOT_Y - 6, MUT_COL, rbf_help_cb, NULL);

actions:
    if (np) wt_help_chip(s_scr, 738, 108, WARN_COL, caution_help_cb, NULL);

    // ---- the action row --------------------------------------------------
    // HOLD TO SIGN never moves, never changes label, and never changes width.
    // Acknowledgement lives in the caution rows now, so there is no second
    // button competing for this position and no way for two taps in the same
    // place to become a signature nobody read.
    wt_pillh(s_scr, tr(STR_C_BACK), SG_BACK_X, WT_ACTION_Y, 104, WT_ACTION_H,
             s_src == SRC_SD ? files_back_cb : choose_back_cb, NULL);
    wt_pillh(s_scr, tr(STR_S_DETAILS), SG_DETAILS_X, WT_ACTION_Y, 150,
             WT_ACTION_H, details_cb, NULL);

    s_arc = lv_arc_create(s_scr);
    lv_obj_set_size(s_arc, 40, 40);
    lv_obj_set_pos(s_arc, SG_HOLD_X + SG_ARC_DX, WT_ACTION_Y + 6);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, KEY_COL, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, wt_accent(), LV_PART_INDICATOR);

    lv_obj_t *p = wt_pillh(s_scr, tr(STR_S_HOLD_TO_SIGN), SG_HOLD_X, WT_ACTION_Y,
                           310, WT_ACTION_H, NULL, NULL);
    wt_pill_label_max(p);          // the most consequential button in the app
    s_sign_lbl = lv_obj_get_child(p, 0);
    if (np && !s_ack) {
        // Present, in place, and visibly inert. Disabled ink rather than a
        // hidden or moved button, so the owner can see what acknowledging the
        // rows above is going to unlock. No accent here on purpose: the accent
        // is this app's "press this one" marker, so wearing it while inert
        // would be a lie.
        lv_obj_set_style_border_color(p, WT_EDGE, 0);
        lv_obj_set_style_text_color(s_sign_lbl, WT_DIM, 0);
        lv_obj_add_state(s_arc, LV_STATE_DISABLED);
    } else {
        lv_obj_add_event_cb(p, sign_press_cb, LV_EVENT_ALL, NULL);
        // The same primary marker every other screen's suggested action wears,
        // rather than a bare 1px accent border invented here: 2px, an accent
        // tinted fill, a pressed fill the hold can be felt against, and the top
        // label rung. ADDENDUM-02 asks for this, and the reason is that a
        // hand rolled variant of the app's loudest affordance is exactly the
        // kind of near miss the redraw is meant to remove.
        wt_pill_primary(p);
    }
}

// ---- DETAILS: the second page for people who want the raw facts. One page,
// one tap in, one tap back — the verify screen stays simple. ----
static void details_back_cb(lv_event_t *e)
{
    (void)e;
    repaint_verify();
}

// The eight glossary icons, in the order STR_S_GLOSSARY_B lists its terms:
// inputs, outputs, change, txid, fee rate, locktime, derivation path, descriptor.
// Every one is already baked into the Latin faces (tools/fonts/gen_fonts.sh),
// so this costs no font work and no flash.
//
// Chosen to mean the thing without the word: coins arriving, coins leaving, the
// part that comes back, a marker for finding it later, a bolt for what you pay
// for speed, a lock for the earliest it may confirm, a folder for a path, and an
// open eye for the map that can watch but not spend.
static const char *const GLOSS_ICONS[] = {
    LV_SYMBOL_DOWNLOAD,     // INPUTS
    LV_SYMBOL_UPLOAD,       // OUTPUTS
    LV_SYMBOL_LOOP,         // CHANGE
    LV_SYMBOL_GPS,          // TXID
    LV_SYMBOL_CHARGE,       // FEE RATE
    WT_ICON_LOCK,           // LOCKTIME
    LV_SYMBOL_DIRECTORY,    // DERIVATION PATH
    LV_SYMBOL_EYE_OPEN,     // DESCRIPTOR
};

static void glossary_cb(lv_event_t *e)
{
    (void)e;
    // A list, not prose: one `term: definition` per line, drawn as a grid of
    // icon badges. The colon split is why this needs no new string in any of the
    // 21 locales -- they all already write it that way.
    wt_explain_t x = {
        .title  = tr(STR_S_GLOSSARY_T),
        .icon   = LV_SYMBOL_LIST,
        .body   = tr(STR_S_GLOSSARY_B),
        .ok_txt = tr(STR_C_OK),
        .mode   = WT_GRID_ICONS,
        .icons  = GLOSS_ICONS,
    };
    wt_explain_open(s_scr, &x);
}

// Measured height of a label that was just built, so the next thing can go
// under it. LVGL sizes a wrapped label lazily; without the update the height is
// whatever it was before the text landed.
static int det_h(lv_obj_t *o)
{
    lv_obj_update_layout(o);
    return lv_obj_get_height(o);
}

// One flag row: an icon, the value beside it in ink, and the note under both.
// Advances *y past whatever it used.
//
// The notes are hard wrapped in the tables with a "\n", from when this column
// was a stack of sentences in a 330px lane. Those breaks are wrong for a lane
// that now starts past an icon, so they are flattened to spaces and the label
// wraps where the width actually is. Nothing translated changes; only where the
// line happens to end.
//
// `floor_y` is the last y this row may touch, and it is what makes the strip
// safe in twenty locales rather than in the one it was measured in. English
// fits three rows with room to spare; Polish and Russian run about 40% longer,
// and the two that would spill are held to the lines they have left and
// ellipsised. A clipped sentence is bad. A sentence drawn over the BACK pill,
// on the page whose job is telling you what you are about to sign, is worse.
static void det_flag_row(int x, int *y, const char *icon, const char *head,
                         const char *tail, int w, int floor_y)
{
    const int IW = 26;              // icon column, generous enough for the widest
    if (*y + 18 > floor_y) return;
    wt_lbl(s_scr, icon, x, *y + 2, wt_font14(), MUT_COL);

    lv_obj_t *h = wt_lbl(s_scr, head, x + IW, *y, wt_font14(), INK_COL);
    lv_obj_set_style_text_letter_space(h, 1, 0);
    lv_obj_set_width(h, w - IW);
    lv_label_set_long_mode(h, LV_LABEL_LONG_DOT);
    *y += det_h(h) + 2;

    if (!tail || !*tail) { *y += 6; return; }

    char flat[256];
    snprintf(flat, sizeof flat, "%s", tail);
    for (char *p = flat; *p; p++) if (*p == '\n') *p = ' ';

    // The note hangs back to the icon's own left edge rather than lining up
    // under the value. It buys the 26px the icon column costs, which is the
    // difference between one line and two for the sighash note, and a hanging
    // indent is how a list of marked items is normally set anyway.
    lv_obj_t *t = wt_lbl(s_scr, flat, x, *y, wt_font14(), MUT_COL);
    lv_obj_set_width(t, w);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    int th = det_h(t);
    if (*y + th > floor_y) {
        // Pin to the whole lines that fit and let LONG_DOT end it honestly.
        int lh = lv_font_get_line_height(wt_font14());
        int lines = (floor_y - *y) / lh;
        if (lines < 1) lines = 1;
        lv_obj_set_height(t, lines * lh);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        th = lines * lh;
    }
    *y += th + 6;
}

static void details_cb(lv_event_t *e)
{
    (void)e;
    wpsbt_details_t det;
    if (wallet_psbt_details(&det) != 0)
        return;
    hold_stop();
    lv_obj_delete_async(s_scr); s_scr = NULL; s_arc = NULL; s_sign_lbl = NULL;
    mk_screen(s_parent, tr(STR_S_DETAILS), s_cur);
    // The subtitle here is the file name, and the SIMPLE EXPLANATIONS pill
    // starts at x=560 with a label that takes two lines in the longer locales.
    // The lane stops at 548 so the two cannot meet.
    wt_sub_fit(s_scr, 500);

    lv_obj_t *learn = wt_pillh(s_scr, tr(STR_S_GLOSSARY_T),
                               560, 28, 192, 44, glossary_cb, NULL);
    lv_obj_set_style_border_color(learn, MUT_COL, 0);

    char buf[256], a[32];   // ja details header ~140 bytes; 3 bytes/char worst
    if (det.n_total > det.n_in)          // more inputs than the page can hold
        snprintf(buf, sizeof buf,
                 tr(STR_S_D_MANYIN_FMT),
                 (unsigned)det.n_total, (unsigned)det.n_in);
    else
        snprintf(buf, sizeof buf, tr(STR_S_D_INPUTS_FMT), (unsigned)det.n_in);
    // wt_section, not a muted font14 line. Every single label on this screen
    // used to be font14, which is not "dense", it is no hierarchy at all: the
    // count of inputs, the amount of each one, and the sighash flag all
    // shouted at the same volume, so nothing led and the eye had to read all
    // of it to find any of it. The eyebrow style is what WALLET and RECEIVE
    // put above a value, and this is the same relationship.
    // The two columns get a card each, and the cards go in BEHIND the content
    // rather than around it: every y on this page is hand measured against a 372
    // and a 330 wide lane, and a txid is exactly 64 hex characters, so shaving
    // padding off either lane turns two lines of it into three and walks the
    // whole column down into the next label. Created first, so they are behind
    // everything that follows in z-order, and nothing below moves by a pixel.
    //
    // 88..396 for both, which is the header row's floor to just above the action
    // bar. The right column ends higher than the left one and keeps the
    // difference as air, because two cards of different heights beside each other
    // read as a layout accident rather than as two columns.
    // The divide moved: 384/334 became 288/424. The left column is a LIST of
    // amounts and truncated txids, and the widest thing in it is "100 000 sats"
    // at font23, so it never needed 384. The right one carries every fact about
    // the transaction itself and could not hold them with any hierarchy at 334:
    // giving it 90 more is what turns the sighash note from two lines into one,
    // which is exactly the room the value-over-note rows cost. Both columns
    // still land on the page's 28 and 752 margins.
    wt_card(s_scr, 28, 100, 288, 296);
    wt_card(s_scr, 328, 100, 424, 296);

    lv_obj_t *ihdr = wt_section(s_scr, buf, 40, 108);
    // Bounded to the left column. STR_S_D_MANYIN_FMT is a sentence, not a
    // word, and in Spanish it ran straight across into the TXID caption in the
    // right column. It was font14 and unbounded before, which only hid the
    // fault behind a smaller face.
    lv_obj_set_width(ihdr, 264);
    lv_label_set_long_mode(ihdr, LV_LABEL_LONG_WRAP);

    lv_obj_t *il = lv_obj_create(s_scr);
    lv_obj_remove_style_all(il);
    // Start the list under whatever the header actually became. This used to be
    // a two-way guess (130, or 158 when the header wrapped), which was already
    // wrong for a locale that took three lines and is certainly wrong now the
    // lane is 288 rather than 372. Measure it instead.
    int ly = 108 + det_h(ihdr) + 8;
    lv_obj_set_pos(il, 40, ly);
    lv_obj_set_size(il, 264, 392 - ly);
    lv_obj_set_style_pad_all(il, 8, 0);
    lv_obj_set_style_pad_row(il, 4, 0);
    lv_obj_set_flex_flow(il, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(il, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(il, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(il, LV_OPA_TRANSP, 0);
    for (uint32_t i = 0; i < det.n_in; i++) {
        lv_obj_t *row = lv_obj_create(il);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_bottom(row, 10, 0);

        fmt_sats(det.ins[i].sats, a, sizeof a);
        snprintf(buf, sizeof buf, "%s sats", a);
        lv_obj_t *amt = lv_label_create(row);
        lv_label_set_text(amt, buf);
        lv_obj_set_style_text_color(amt, INK_COL, 0);
        // The amount leads the row at 23 and the txid trails it at 14. That is
        // the whole fix for this list: what is being spent is the fact, and the
        // coin it came from is the reference you check it against.
        lv_obj_set_style_text_font(amt, wt_font23(), 0);

        // coin being spent: first 8 + last 8 of its txid, and the output index
        snprintf(buf, sizeof buf, "%.8s...%s : %u",
                 det.ins[i].txid, det.ins[i].txid + 56, (unsigned)det.ins[i].vout);
        lv_obj_t *tid = lv_label_create(row);
        lv_label_set_text(tid, buf);
        lv_obj_set_style_text_color(tid, MUT_COL, 0);
        lv_obj_set_style_text_font(tid, wt_font14(), 0);

        // BIP376 received-SP input: its key is spend+tweak, not a BIP84 child,
        // so show the silent-payment badge instead of a misleading BIP32 path.
        if (det.ins[i].is_sp)
            snprintf(buf, sizeof buf, LV_SYMBOL_OK " m/352'/%d'/0'   %s",
                     s_sum.testnet ? 1 : 0, tr(STR_S_SP_BADGE));
        else
            snprintf(buf, sizeof buf, LV_SYMBOL_OK " m/%u'/%d'/0'/%u/%u",
                     (unsigned)det.ins[i].purpose, s_sum.testnet ? 1 : 0,
                     (unsigned)det.ins[i].change, (unsigned)det.ins[i].index);
        lv_obj_t *pl = lv_label_create(row);
        lv_label_set_text(pl, buf);
        lv_obj_set_style_text_color(pl, OK_COL, 0);
        lv_obj_set_style_text_font(pl, wt_font14(), 0);
    }

    // ---- the right column ----
    // Every element here used to sit on a hand measured y, and the seven of them
    // were all font14 and all MUT_COL: no hierarchy, so a reader had to read the
    // whole column to find any one fact in it. They are now placed by a CURSOR,
    // each one measured after it is built and the next one put under it. That is
    // what makes the hierarchy affordable — a heading line costs 17px, and seven
    // fixed y values had no 17px anywhere to give.
    const int RX = 340, RW = 400;    // inside the 328..752 card, 12 of padding
    const int RFLOOR = 388;          // the card's own floor, 8 above its edge
    int ry = 108;

    // the id to find it by, once broadcast — final only for segwit-only spends
    wt_section(s_scr, tr(STR_S_D_TXID), RX, ry);
    ry += 20;
    char gt[80];
    group4(det.txid, gt, sizeof gt);
    lv_obj_t *tx = mk_lbl(gt, RX, ry, wt_font14(), INK_COL);
    lv_obj_set_width(tx, RW);
    lv_label_set_long_mode(tx, LV_LABEL_LONG_WRAP);
    ry += det_h(tx) + 4;
    // The fee rate, arrived from the verify screen's right column, which had to
    // give up 66px so three simultaneous cautions could each have a row. A txid
    // is always 64 hex characters, so the block above is always two lines and
    // the gap under it was always 55px of nothing.
    //
    // S_FEERATE_PCT_FMT rather than S_FEERATE_FMT: it was already written and
    // already translated into all 21 locales and used nowhere, and it says the
    // useful thing. "57.0 sat/vB" alone is a number for people who already know
    // what a good one looks like; "57.0 sat/vB, 21.0% of what you send" is the
    // sentence that makes somebody stop.
    // Percent of the SEND amount, one decimal, integers only: there are no
    // floats on this device. A sweep with nothing left over would divide by
    // zero, so that case prints the rate on its own.
    uint64_t pct10 = s_sum.send_sats
                   ? (uint64_t)s_sum.fee_sats * 1000ull / s_sum.send_sats : 0;
    if (s_sum.send_sats)
        snprintf(buf, sizeof buf, tr(STR_S_FEERATE_PCT_FMT),
                 (unsigned)(s_sum.fee_rate_x10 / 10),
                 (unsigned)(s_sum.fee_rate_x10 % 10),
                 (unsigned long long)(pct10 / 10), (unsigned long long)(pct10 % 10));
    else
        snprintf(buf, sizeof buf, tr(STR_S_FEERATE_FMT),
                 (unsigned)(s_sum.fee_rate_x10 / 10),
                 (unsigned)(s_sum.fee_rate_x10 % 10));
    char fee_line[sizeof buf];
    snprintf(fee_line, sizeof fee_line, "%s", buf);   // kept for the strip below

    lv_obj_t *cn = mk_lbl(det.txid_final ? tr(STR_S_D_TXID_SAME)
                                         : tr(STR_S_D_TXID_CHANGES),
                          RX, ry, wt_font14(), MUT_COL);
    lv_obj_set_width(cn, RW);
    lv_label_set_long_mode(cn, LV_LABEL_LONG_WRAP);
    ry += det_h(cn) + 6;
    // The same total in BTC, directly under the line about comparing against
    // the coordinator, because comparing is the only reason to want it: a
    // coordinator that displays BTC needs this row to check the sats form.
    //
    // Just the BTC form now, not "N sats   =   N BTC". The sats form used to
    // repeat here and pushed the composite line to about x=791 at font23, past
    // the 750 lane every other line on this page respects, and past the 776
    // Sign lane too. Bounding it to 330 with wrap collided with the version
    // and locktime line beneath at y=258 in every locale the overlap gate ran,
    // because at font23 the composite is ~360px wide and every 330 wrap took
    // its second line into that row. Copy: cut the value that is beside it, at
    // arm's length the sats total is a scan away on the verify screen the tap
    // to DETAILS came from.
    uint64_t leaving = s_sum.send_sats + s_sum.fee_sats;   // as the verify screen counts it
    wt_fmt_btc(leaving, gt, sizeof gt);
    snprintf(buf, sizeof buf, "= %s BTC", gt);
    // 23, and INK. This is the number a holder reads off the glass and compares
    // against the coordinator, which is the entire reason the BTC form is here
    // at all. It was the same size and the same grey as the locktime note.
    lv_obj_t *bt = mk_lbl(buf, RX, ry, wt_font23(), INK_COL);
    ry += det_h(bt) + 10;

    // ---- the three flag rows ----
    // "is this transaction normal" has exactly three answers on this device, and
    // they were three more grey sentences in the same stack as everything else.
    // Each is written `head: tail` in all 21 locales, so the head becomes the
    // VALUE, in ink beside an icon, and the tail becomes the note under it. No
    // new string anywhere: wt_split_colon reads the shape the translators
    // already wrote, wide colon and French spacing included.
    //
    // The version and locktime numbers ride on the locktime row's head instead
    // of a line of their own, and the head the locale wrote for that row is
    // dropped: "locktime 0" beside "version 2, locktime 0" is the same value
    // printed twice.
    char lt_head[64], sh_head[64], rbf_head[64];
    snprintf(buf, sizeof buf, tr(STR_S_D_VER_LT_FMT),
             (unsigned)det.version, (unsigned)det.locktime);
    const char *lt_tail = wt_split_colon(det.locktime ? tr(STR_S_D_LT_NONZERO)
                                                      : tr(STR_S_D_LT_ZERO),
                                         lt_head, sizeof lt_head);
    const char *sh_tail = wt_split_colon(tr(STR_S_D_SIGHASH),
                                         sh_head, sizeof sh_head);
    const char *rbf_tail = wt_split_colon(s_sum.rbf ? tr(STR_S_D_RBF_ON)
                                                    : tr(STR_S_D_RBF_OFF),
                                          rbf_head, sizeof rbf_head);

    // The fee rate joins the strip rather than floating above it as a loose
    // muted line. It is a property of the transaction exactly like the three
    // below it, and STR_S_FEERATE_PCT_FMT is already a whole sentence, so it
    // takes the head slot with no note under it.
    det_flag_row(RX, &ry, LV_SYMBOL_CHARGE, fee_line, NULL, RW, RFLOOR);
    det_flag_row(RX, &ry, WT_ICON_LOCK, buf, lt_tail, RW, RFLOOR);
    det_flag_row(RX, &ry, LV_SYMBOL_OK, sh_head, sh_tail, RW, RFLOOR);
    // The same mark the RBF explainer wears, so the row and the card that
    // explains it are recognisably about one thing.
    det_flag_row(RX, &ry, s_sum.rbf ? WT_ICON_REPLACE : WT_ICON_LOCK,
                 rbf_head, rbf_tail, RW, RFLOOR);

    mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, details_back_cb);
}

// ---- QR out: the signed PSBT as an animated QR (UR, or pMofN if it came
// that way). Static-in still leaves as animated UR — a signed PSBT rarely
// fits one readable QR at 288px, and every coordinator that speaks QR reads UR.
static void qr_tick(lv_timer_t *t)
{
    (void)t;
    char part[600];
    if (!s_qenc || qrt_encoder_next(s_qenc, part, sizeof part) != 0) return;
    if (s_qr_img) wt_qr_update(s_qr_img, part, (uint32_t)strlen(part));
    int n = qrt_encoder_parts(s_qenc);
    if (s_part_lbl && n > 1) {
        s_part_i = s_part_i % n + 1;
        char b[64];
        snprintf(b, sizeof b, tr(STR_S_QR_PART_FMT), s_part_i, n);
        lv_label_set_text(s_part_lbl, b);
    }
}

// (re)create the out-encoder for the current mode. Easy-scan halves the data
// per frame (sparser QR = bigger modules at the same 288px) and the loop slows
// below — for phone cameras that never lock onto the default loop.
static int qr_enc_start(void)
{
    int fmt = (s_qr_fmt == QRT_FMT_PMOFN) ? QRT_FMT_PMOFN : QRT_FMT_UR;
    qrt_encoder_t *ne = qrt_encoder_new_frag(fmt, s_out, s_out_len,
        s_qr_ez ? (fmt == QRT_FMT_PMOFN ? 50 : 60) : 0);
    if (!ne) return -1;
    if (s_qenc) qrt_encoder_free(s_qenc);
    s_qenc = ne;
    return 0;
}

static void qr_ez_cb(lv_event_t *e)
{
    (void)e;
    s_qr_ez = !s_qr_ez;
    if (qr_enc_start() != 0) { s_qr_ez = !s_qr_ez; return; }  // old QR keeps playing
    wt_pill_select(s_ez_pill, s_qr_ez);
    if (s_qr_tmr) { lv_timer_delete(s_qr_tmr); s_qr_tmr = NULL; }
    int n = qrt_encoder_parts(s_qenc);
    if (n > 1)
        s_qr_tmr = lv_timer_create(qr_tick, s_qr_ez ? 600 : 250, NULL);
    else if (s_part_lbl)
        lv_label_set_text(s_part_lbl, tr(STR_S_QR_SINGLE));
    s_part_i = 0;
    qr_tick(NULL);
}

static void qr_out_screen(size_t sw)
{
    lv_obj_t *parent = lv_obj_get_parent(s_scr);
    lv_obj_delete(s_scr); s_scr = NULL; s_arc = NULL; s_sign_lbl = NULL;

    s_qr_ez = false;
    s_out_len = sw;
    if (qr_enc_start() != 0) {
        mk_screen(parent, tr(STR_S_FAIL_T), tr(STR_S_QR_FAIL_ENC));
        mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, close_cb);
        return;
    }

    mk_screen(parent, tr(STR_S_SIGNED_T), tr(STR_S_QR_SUB));
    // 302/274, down from 316/288 at y=100. The old card ran to y=415 and the
    // QR bitmap itself to 401, so its bottom 4px sat in the action band and is
    // now painted over by the bar: a signed transaction that will not scan.
    // The card shrinks rather than the quiet zone, so the 14px of white around
    // the code is exactly what it was and only the modules are 5% smaller.
    wt_qr_card(s_scr, &s_qr_img, 48, 96, 302, 274);

    int n = qrt_encoder_parts(s_qenc);
    mk_lbl(tr_sym(LV_SYMBOL_OK, STR_S_SIGNED_T), 430, 100, wt_font14(), OK_COL);
    s_part_lbl = mk_lbl(n > 1 ? tr(STR_S_QR_PART1) : tr(STR_S_QR_SINGLE), 430, 124,
                        wt_font28(), INK_COL);
    // What to DO with the QR on screen, previously all at 14 beside a 28px
    // part counter. The right column is 322 wide and nothing but the EASY SCAN
    // pill sits between here and DONE, so each of these gets its own line.
    if (n > 1) {
        wt_note(s_scr, tr(STR_S_QR_LOOP), 430, 168, 322, 29);
        s_qr_tmr = lv_timer_create(qr_tick, 250, NULL);
    }
    wt_note(s_scr, tr(STR_S_NO_NETWORK), 430, 201, 322, 29);
    s_ez_pill = wt_pill(s_scr, tr(STR_S_EASY_SCAN), 430, 244, 200, qr_ez_cb, NULL);
    wt_note(s_scr, tr(STR_S_EZ_NOTE), 430, 304, 322, 87);
    mk_pill(tr(STR_C_DONE), 610, WT_ACTION_Y, 140, close_cb);
    s_part_i = 0;
    qr_tick(NULL);                               // first part right away
}

// ---- file list ----
static void file_tap_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    snprintf(s_cur, sizeof s_cur, "%s", s_files[idx]);
    size_t len = 0;
    int rrc = platform_sd_read(s_cur, s_in, sizeof s_in, &len);
    lv_obj_t *parent = lv_obj_get_parent(s_scr);
    lv_obj_delete_async(s_scr); s_scr = NULL;
    if (rrc != 0) {
        mk_screen(parent, tr(STR_S_T), s_cur);
        // A refusal to sign, alone on an otherwise empty screen with 230px
        // of room under it. There is no reason for it to be the small type.
        wt_note_col(s_scr, tr(STR_S_READ_FAIL), 48, 140, 704, 232, STOP_COL);
        mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, files_back_cb);
        return;
    }
    SIGN_LOG("SD read: %s, %u bytes", s_cur, (unsigned)len);
    int lrc = wallet_psbt_load(s_in, len, &s_sum);
    s_ack = false;                         // fresh PSBT: re-acknowledge any caution
    s_ack_flags = 0;
    s_ack_t0 = 0;
    if (lrc != 0) {
        SIGN_LOG("REJECTED: not a parseable PSBT (rc %d)", lrc);
        mk_screen(parent, tr(STR_S_T), s_cur);
        wt_note_col(s_scr, tr(STR_S_NOT_PSBT), 48, 140, 704, 232, STOP_COL);
        mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, files_back_cb);
        return;
    }
    log_summary("SD");
    verify_screen(parent);
}

// Both dead ends on the SD path: no card in the slot, and a card with no .psbt
// on it. They were a bare 28px line and a grey paragraph floating on an empty
// page -- the only two screens in SIGN with no card frame and no mark, which is
// the wrong pair of screens to leave looking unfinished, because they are the
// two the owner reaches when something has already gone wrong. Same card, same
// amber SD glyph, same words.
static void sd_empty_screen(lv_obj_t *parent, const char *head, const char *body)
{
    mk_screen(parent, tr(STR_S_T), tr(STR_S_SD_SUB));
    lv_obj_t *card = wt_card(s_scr, 48, 140, 704, 200);
    lv_obj_t *ic = wt_lbl(card, WT_ICON_SD, 0, 0, wt_font28(), WARN_COL);
    lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 28, 26);
    lv_obj_t *h = wt_lbl(card, head, 76, 22, wt_font28(), INK_COL);
    lv_obj_set_width(h, 600);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_update_layout(h);
    wt_note_col(card, body, 28, 22 + lv_obj_get_height(h) + 14, 648,
                200 - 58 - lv_obj_get_height(h), MUT_COL);
    mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, choose_back_cb);
}

static void sd_open(lv_obj_t *parent)
{
    s_src = SRC_SD;
    if (platform_sd_mount() != 0) {
        sd_empty_screen(parent, tr(STR_S_NO_SD), tr(STR_S_INSERT_CARD));
        return;
    }
    int n = platform_sd_list_psbt(s_files, MAX_FILES);
    if (n <= 0) {
        sd_empty_screen(parent, tr(STR_S_NO_PSBT_FILES),
                        tr(STR_S_SPARROW_SAVE));
        return;
    }
    mk_screen(parent, tr(STR_S_T), tr(STR_S_CHOOSE_FILE));
    lv_obj_t *sd = mk_lbl(tr_sym(LV_SYMBOL_OK, STR_S_SD_READY), 560, 38, wt_font14(), OK_COL);
    lv_obj_set_style_text_letter_space(sd, 1, 0);
    // Stays at 14, and stays a hint: it describes the SORT ORDER of the list
    // below, which is not a decision anyone makes. y=98 because the subtitle is
    // a readable 23 now and bottoms at 95; at 88 the two were overlapping.
    mk_lbl(tr(STR_S_FILES_HINT), 48, 98, wt_font14(), MUT_COL);

    // All discovered files fit in one scrollable, deterministic list. Unsigned
    // work is sorted first; signed PSBTs remain available for multisig handoffs
    // but are visibly labelled so nobody accidentally treats one as fresh.
#define FILE_ROW_W 560
    lv_obj_t *list = lv_obj_create(s_scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 48, 126);
    lv_obj_set_size(list, FILE_ROW_W, 264);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    // One wt_row per file. These were hand built at radius 26 -- pill shaped
    // list items, which is the loudest form of the idiom this device has
    // stopped using: a row of buttons reads as six things to press, a row of
    // cards reads as six things to choose between, and choosing is what this
    // screen is for. wt_row_x also brings the chevron, which is the one thing
    // the old rows never said: that tapping a filename OPENS it.
    //
    // The file glyph does the work the border colour used to. UNSIGNED stays as
    // the value in the row's right slot, in WT_WARN when the file has already
    // been signed, so "this one is spent" is still said twice.
    for (int i = 0; i < n; i++) {
        size_t nl = strlen(s_files[i]);
        bool signed_file = nl >= 12
                        && strcasecmp(s_files[i] + nl - 12, "-signed.psbt") == 0;
        lv_obj_t *row = wt_row_x(list, LV_SYMBOL_FILE, s_files[i], NULL, NULL,
                                 signed_file ? tr(STR_S_SIGNED_T)
                                             : tr(STR_S_FILE_UNSIGNED),
                                 wt_font14(),
                                 signed_file ? WARN_COL : MUT_COL, false,
                                 0, 0, FILE_ROW_W, 0, file_tap_cb,
                                 (void *)(intptr_t)i);
        // The flex list places it, so the absolute x/y above are ignored, but
        // the WIDTH is not: wt_row_x measures the label lane against it before
        // flex ever runs. Passing the list's real width is what keeps a long
        // filename ellipsising instead of running under the tag.
        lv_obj_set_width(row, lv_pct(100));
    }
    mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, choose_back_cb);
}

// What the device concluded about a PSBT, in one serial line.
//
// reason[] is the whole point: it carries the STOP root cause in plain English
// straight from wallet_psbt.c ("input is not this wallet's", "wrong network:
// mainnet transaction", "sighash is not ALL"), which is otherwise only ever
// seen translated on a screen with no way to copy it off.
//
// n_sp_in is here because it disambiguates the one refusal that lies about
// itself. A received silent-payment coin has no BIP32 keypath by design, so a
// coordinator that omits BIP376's PSBT_IN_SP_TWEAK leaves nothing to prove
// ownership with, and the input is rejected as though it belonged to a
// stranger. n_sp_in = 0 alongside that reason means the tweak field never
// arrived, not that the coin is foreign.
static void log_summary(const char *src)
{
    (void)src;
    SIGN_LOG("%s PSBT: %u in (%u silent-payment), %u out (%u SP), "
             "%u unknown fields, purpose %u, %s, status %d",
             src, (unsigned)s_sum.n_in, (unsigned)s_sum.n_sp_in,
             (unsigned)s_sum.n_out, (unsigned)s_sum.n_sp,
             (unsigned)s_sum.n_unknown, (unsigned)s_sum.purpose,
             s_sum.testnet ? "testnet" : "mainnet", (int)s_sum.status);
    SIGN_LOG("%s ownership: device fp %02x%02x%02x%02x, input0 fp "
             "%02x%02x%02x%02x from %u keypath(s) -> %s",
             src,
             s_sum.our_fp[0], s_sum.our_fp[1], s_sum.our_fp[2], s_sum.our_fp[3],
             s_sum.in0_fp[0], s_sum.in0_fp[1], s_sum.in0_fp[2], s_sum.in0_fp[3],
             (unsigned)s_sum.in0_keypaths,
             s_sum.in0_keypaths == 0 ? "NO DERIVATION SENT"
             : memcmp(s_sum.our_fp, s_sum.in0_fp, 4) == 0 ? "match"
                                                          : "MISMATCH");
    if (s_sum.reason[0])
        SIGN_LOG("%s PSBT reason: %s", src, s_sum.reason);
}

// ---- QR source: wallet_scan drives the camera; we get the assembled PSBT ----
static void scan_done_cb(const uint8_t *psbt, size_t len, int fmt)
{
    s_src = SRC_QR;
    s_qr_fmt = fmt;
    snprintf(s_cur, sizeof s_cur, "%s", tr(STR_S_SCANNED_TX));
    if (len > sizeof s_in) len = sizeof s_in;             // QRT_MAX_PSBT == sizeof s_in
    memcpy(s_in, psbt, len);
    SIGN_LOG("QR assembled: %u bytes, fmt %d", (unsigned)len, fmt);
    log_psbt_hex(s_in, len);
    int lrc = wallet_psbt_load(s_in, len, &s_sum);
    s_ack = false;                         // fresh PSBT: re-acknowledge any caution
    s_ack_flags = 0;
    s_ack_t0 = 0;
    if (lrc != 0) {
        SIGN_LOG("REJECTED: not a parseable PSBT (rc %d)", lrc);
        mk_screen(s_parent, tr(STR_S_T), s_cur);
        wt_note_col(s_scr, tr(STR_S_SCAN_NOT_PSBT), 48, 140, 704, 232, STOP_COL);
        mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, choose_back_cb);
        return;
    }
    log_summary("QR");
    verify_screen(s_parent);
}

static void scan_cancel_cb(void)
{
    wallet_sign_open(s_parent);                           // back to the chooser
}

static void scan_pick_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_delete_async(s_scr); s_scr = NULL;
    wallet_scan_open(s_parent, scan_done_cb, scan_cancel_cb);
}

// ---- PSBT help: one plain-English card with the complete signing loop ----
// A numbered sequence is intentionally used instead of the old
// COORDINATOR <- QR -> KISS equation.  That equation showed transport, but not
// which side acted first, what came back, or who actually broadcasts.  Those
// are exactly the facts a first-time signer needs.
//
// Each step also carries an icon, for the same reason the scan key card does:
// three rows of all caps type at the same size read as a wall, and the reader
// has to parse every word to find out which row is the device. An icon is read
// before the sentence is. The eye is deliberately the SAME glyph the scan key
// card uses for watch only, because it means the same thing in both places.
// Only one icon is coloured, and it is the key on step 2, which is the one row
// where the private keys are involved and the only row this device performs.
static void coord_step(lv_obj_t *parent, int y, const char *number,
                       const char *icon, lv_color_t icon_color,
                       const char *text, bool signer)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    // 48/704 is this device's standard content width (the same one the body
    // above uses), not the 80/640 these rows started at. Widened because step 3
    // finally says "transaction" instead of "payment" in English, and the
    // honest word is 4 characters longer than the vague one -- the fix for that
    // is room, not a shorter word. Still centred on 400, so the connectors
    // between the steps are unchanged.
    lv_obj_set_pos(row, 48, y);
    lv_obj_set_size(row, 704, 44);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_bg_color(row, signer ? wt_accent_bg() : KEY_COL, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, signer ? 2 : 1, 0);
    lv_obj_set_style_border_color(row, signer ? wt_primary() : MUT_COL, 0);

    lv_obj_t *n = lv_label_create(row);
    lv_label_set_text(n, number);
    lv_obj_set_style_text_color(n, signer ? INK_COL : MUT_COL, 0);
    lv_obj_set_style_text_font(n, wt_font23(), 0);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 18, 0);

    lv_obj_t *ic = lv_label_create(row);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_color(ic, icon_color, 0);
    lv_obj_set_style_text_font(ic, wt_font23(), 0);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 52, 0);

    // Text starts at 92 rather than 50, which is the icon's 26px column plus
    // the gap. The label loses the same 44px off its width so the right edge
    // does not move: the longest translations were already using it.
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, text);
    lv_obj_set_width(l, 596);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(l, INK_COL, 0);
    lv_obj_set_style_text_font(l, wt_font23(), 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 92, 0);
}

static void coord_connector(lv_obj_t *parent, int y)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_pos(line, 399, y);
    lv_obj_set_size(line, 2, 14);
    lv_obj_set_style_bg_color(line, MUT_COL, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
}

// The three step flow, as a wt_explain_open aside: draws into the lane it is
// given and reports the height it used. coord_step places itself at the page's
// own 48/704 content width, so the x and w it is handed are the full lane and it
// ignores them; only the y matters.
static int aside_coord_flow(lv_obj_t *par, int x, int y, int w)
{
    (void)x; (void)w;
    const int ROW = 44, GAP = 14, H = 3 * ROW + 2 * GAP;
    // ONE container, so the sequence enters as a unit. Five loose objects on
    // the overlay would each take their own turn in the entrance stagger and
    // the flow would assemble itself a step at a time in front of the reader,
    // which is the opposite of what a diagram of a sequence should do.
    lv_obj_t *box = lv_obj_create(par);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, 0, y);
    lv_obj_set_size(box, 800, H);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    coord_step(box, 0, "1", LV_SYMBOL_EYE_OPEN, MUT_COL, tr(STR_S_FLOW_1), false);
    coord_connector(box, ROW);
    coord_step(box, ROW + GAP, "2", WT_ICON_KEY, wt_primary(),
               tr(STR_S_FLOW_2), true);
    coord_connector(box, 2 * ROW + GAP);
    coord_step(box, 2 * (ROW + GAP), "3", LV_SYMBOL_UPLOAD, MUT_COL,
               tr(STR_S_FLOW_3), false);
    return H;
}

static void coord_help_cb(lv_event_t *e)
{
    (void)e;
    // The numbered sequence leads and the sentence closes it. Every locale gets
    // all three steps: the compact two-box fallback that used to stand in for
    // them said far less than the sequence it replaced.
    wt_explain_t x = {
        .title  = tr(STR_S_COORD_T),
        .icon   = WT_ICON_KEY,
        .body   = tr(STR_S_COORD_B),
        .ok_txt = tr(STR_C_OK),
        .aside  = aside_coord_flow,
    };
    wt_explain_open(s_scr, &x);
}

static void sd_pick_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_delete_async(s_scr); s_scr = NULL;
    sd_open(s_parent);
}

void wallet_sign_open(lv_obj_t *parent)
{
    if (s_scr) return;
    s_parent = parent;
    mk_screen(parent, tr(STR_S_T), tr(STR_S_GET_TX));
    // The PSBT help chip below sits at x=616, inside the subtitle's own lane.
    // The subtitle's box is the full 704 whatever the translation does, so the
    // two overlapped in every locale, English included. 544 stops the lane at
    // x=592, 24px clear of the chip. Was 580 against a chip that started at
    // 652; the chip grew left when its label went from font14 to font23.
    wt_sub_fit(s_scr, 544);
    // Two ways in, as rows. They were pills centred in cards with their
    // explanations floating alongside: a button apiece for two things that are
    // both destinations, each trailing a paragraph that belonged to it but was
    // not attached to it. A row says all of that in one object -- the way in is
    // the label, what it does is the sub-line, the chevron says it opens
    // something -- and it says it in the same shape SETTINGS and WALLET use, so
    // there is one list idiom on the device instead of two.
    //
    // The icons carry the distinction faster than the words do: a QR code and an
    // SD card are recognised across a room. Both are in the baked SYMS set.
    //
    // The primary/secondary pair is gone with the pills, and nothing is lost.
    // "QR first, card second" was said with fill and border weight; it is said
    // now by being the first row, which is how every list on this device already
    // says what to reach for first.
    // This chooser has its own y, and the reason is the PSBT help chip. Only
    // this screen carries one, it owns 64..108, and WT_CHOICE_Y(0) starts at the
    // 96 content line -- so the shared grid puts the first row's top edge
    // through it. Dropping to WT_CHOICE_Y(1) and (2) cleared the chip and left
    // 90px of empty page above the pair instead, which is what came back off the
    // device.
    //
    // Centred between the chip's bottom edge and the content floor, which is the
    // only arrangement a two-row screen with a header actually wants:
    // (398 - 108 - 2*96 - 20) / 2 = 39, so 148 and 252. The three-row choosers
    // keep WT_CHOICE_Y; they have no chip and they fill the page.
#define SGC_ROW0 148
#define SGC_ROW1 252
    // Both subs are forced to font14 rather than sized apiece. wt_body_font
    // answers per string, so the two-line SCAN QR note came back at 14 and the
    // one-line SD note at 23 -- two rows offering the same kind of choice, one
    // of them visibly shouting. A group shares a size or it stops being a group.
    wt_row_x(s_scr, WT_ICON_QR, tr(STR_S_SCAN_QR), tr(STR_S_POINT_CAM),
             wt_font14(), NULL, NULL, WT_INK, false, WT_CHOICE_X,
             SGC_ROW0, WT_CHOICE_W, WT_CHOICE_H, scan_pick_cb, NULL);
    wt_row_x(s_scr, WT_ICON_SD, tr(STR_S_FROM_SD), tr(STR_S_OR_LOAD),
             wt_font14(), NULL, NULL, WT_INK, false, WT_CHOICE_X,
             SGC_ROW1, WT_CHOICE_W, WT_CHOICE_H, sd_pick_cb, NULL);
    // A labelled help target teaches the acronym at first sight. An anonymous
    // "?" made users guess whether it explained QR, SD, or the coordinator.
    lv_obj_t *hc = lv_obj_create(s_scr);
    lv_obj_remove_style_all(hc);
    // 136x44 at font23, up from 100x36 at font14. The old chip was legible on a
    // desk and not at arm's length, which is the only distance that counts on a
    // screen you hold up to a coordinator. Right edge stays on the 752 page
    // margin and the top stays on 64, so it grows left and down into empty
    // space rather than into the title above it.
    lv_obj_set_size(hc, 136, 44);
    lv_obj_set_pos(hc, 616, 64);
    // 10, like wt_pillh, wt_card and the two rows underneath it. It was 22 --
    // exactly half the height, so a full lozenge -- and it was the last one on
    // the device after the buttons became rectangles. A single rounded object on
    // a screen of square-shouldered ones does not read as a different KIND of
    // control, it reads as the one that was missed.
    lv_obj_set_style_radius(hc, 10, 0);
    lv_obj_set_style_bg_color(hc, KEY_COL, 0);
    lv_obj_set_style_bg_opa(hc, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hc, 1, 0);
    lv_obj_set_style_border_color(hc, MUT_COL, 0);
    lv_obj_add_flag(hc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(hc, 14);                // small chip, honest target
    wt_tap_feedback(hc);
    lv_obj_add_event_cb(hc, coord_help_cb, LV_EVENT_CLICKED, NULL);
    // Two labels, not one recoloured string. The word carries the accent
    // because the accent is what this UI uses for "this is live, touch it", and
    // the word is the thing being explained. The "?" stays muted: it is the
    // grammar of the chip, not its subject. One help target on this screen and
    // one only, so there is never a question of which "?" opens what.
    lv_obj_t *hl = lv_label_create(hc);
    lv_label_set_text(hl, "PSBT");
    lv_obj_set_style_text_color(hl, wt_accent(), 0);
    lv_obj_set_style_text_font(hl, wt_font23(), 0);
    lv_obj_align(hl, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_t *hq = lv_label_create(hc);
    lv_label_set_text(hq, "?");
    lv_obj_set_style_text_color(hq, MUT_COL, 0);
    lv_obj_set_style_text_font(hq, wt_font23(), 0);
    lv_obj_align(hq, LV_ALIGN_RIGHT_MID, -18, 0);
    mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, close_cb);
}
