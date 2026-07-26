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

#define HOLD_MS   1200
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
static bool s_ack;                      // CAUTION acknowledged? (gates hold-to-sign)
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

// ---- shared bits: thin wrappers over the wallet_theme kit (module keeps
// its s_scr; call sites keep their historical signatures) ----
static void mk_screen(lv_obj_t *parent, const char *title, const char *sub)
{
    s_scr = wt_screen(parent, title, sub);
}

static lv_obj_t *mk_pill(const char *txt, int x, int y, int w, lv_event_cb_t cb)
{
    return wt_pill(s_scr, txt, x, y, w, cb, NULL);
}

static lv_obj_t *mk_lbl(const char *txt, int x, int y, const lv_font_t *f, lv_color_t col)
{
    return wt_lbl(s_scr, txt, x, y, f, col);
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

static void mk_status_light(void)
{
    const char *word = s_sum.status == WPSBT_READY ? tr(STR_S_READY)
                     : s_sum.status == WPSBT_CAUTION ? tr_sym(LV_SYMBOL_WARNING, STR_S_CAUTION)
                     : tr_sym(LV_SYMBOL_CLOSE, STR_S_STOP);
    lv_color_t col = s_sum.status == WPSBT_READY ? MUT_COL
                   : s_sum.status == WPSBT_CAUTION ? WARN_COL : STOP_COL;
    lv_obj_t *p = lv_obj_create(s_scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, 160, 44);
    lv_obj_set_pos(p, 592, 30);
    lv_obj_set_style_radius(p, 22, 0);
    lv_obj_set_style_bg_color(p, KEY_COL, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_set_style_border_color(p, col, 0);
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, word);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_text_font(l, wt_font14(), 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_center(l);
}

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
    mk_screen(parent, tr(STR_S_SIGNED_T), tr(STR_S_DONE_SD_SUB));
    lv_obj_t *big = mk_lbl(LV_SYMBOL_OK, 0, 150, &lv_font_montserrat_48, OK_COL);
    lv_obj_align(big, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_t *fn = mk_lbl(outname, 0, 230, wt_font28(), INK_COL);
    lv_obj_align(fn, LV_ALIGN_TOP_MID, 0, 230);
    lv_obj_t *note = mk_lbl(tr(STR_S_SAVED_NOTE), 0, 280,
                            wt_font14(), MUT_COL);
    lv_obj_align(note, LV_ALIGN_TOP_MID, 0, 280);
    mk_pill(tr(STR_C_DONE), 330, 404, 140, close_cb);
    // nothing needs to stay on screen (the file is saved), so drift back to home
    s_done_tmr = lv_timer_create(auto_home_cb, 6000, NULL);
    lv_timer_set_repeat_count(s_done_tmr, 1);
}

static void fail_screen(const char *why)
{
    lv_obj_t *parent = lv_obj_get_parent(s_scr);
    lv_obj_delete(s_scr); s_scr = NULL; s_arc = NULL; s_sign_lbl = NULL;
    mk_screen(parent, tr(STR_S_FAIL_T), why);
    mk_pill(tr(STR_C_BACK), 330, 404, 140, close_cb);
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
        s_hold_t0 = lv_tick_get();
        if (!s_hold_tmr) s_hold_tmr = lv_timer_create(hold_tick, 30, NULL);
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        hold_stop();                                      // let go early = no signature
    }
}

static void details_cb(lv_event_t *e);
static void verify_screen(lv_obj_t *parent);

// ---- cautions: a short summary on the verify screen, the "why" one tap away ----
// Terse one-liner naming the categories that fired (user: "main warning short").
static void caution_summary(uint16_t f, char *out, size_t cap)
{
    size_t o = 0;
    out[0] = 0;
    const char *parts[4];
    int n = 0;
    if (f & WPSBT_C_HIGHFEE)      parts[n++] = tr(STR_S_C_HIGHFEE);
    if (f & WPSBT_C_DUST_INPUT)   parts[n++] = tr(STR_S_C_DUSTIN);
    if (f & WPSBT_C_DUST_CHANGE)  parts[n++] = tr(STR_S_C_DUSTCH);
    else if (f & WPSBT_C_SMALL_CHANGE) parts[n++] = tr(STR_S_C_SMALLCH);
    for (int i = 0; i < n && o + 1 < cap; i++) {
        // snprintf returns the WOULD-BE length: clamp o inside the buffer or
        // `cap - o` underflows on long (e.g. Cyrillic) translations
        int w = snprintf(out + o, cap - o, "%s%s", i ? " + " : "", parts[i]);
        if (w < 0) break;
        o += (size_t)w;
        if (o >= cap) o = cap - 1;
    }
}

static void caution_ok_cb(lv_event_t *e)
{
    lv_obj_delete_async((lv_obj_t *)lv_event_get_user_data(e));
}

// The full "why", plain words + the concrete next step (freeze/label in the
// coordinator). Reuses the app's dim-overlay explainer style.
static void caution_help_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *ovl = lv_obj_create(s_scr);
    lv_obj_remove_style_all(ovl);
    lv_obj_set_size(ovl, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ovl, BG_COL, 0);
    lv_obj_set_style_bg_opa(ovl, 245, 0);
    lv_obj_add_flag(ovl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ovl, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(ovl);
    lv_label_set_text(t, tr(STR_S_WHY_T));
    lv_obj_set_style_text_color(t, WARN_COL, 0);
    lv_obj_set_style_text_font(t, wt_font28(), 0);
    lv_obj_set_style_text_letter_space(t, 2, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 40);

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
    // reasons stack one per line — three at once plus a blank line before the
    // footer is exactly the 8 rows this card holds at the big font
    if (f & WPSBT_C_HIGHFEE)
        BODY_ADD("%s\n", tr(STR_S_WHY_HIGHFEE));
    if (f & WPSBT_C_DUST_INPUT)
        BODY_ADD("%s\n", tr(STR_S_WHY_DUSTIN));
    if (f & (WPSBT_C_DUST_CHANGE | WPSBT_C_SMALL_CHANGE))
        BODY_ADD("%s\n", tr(STR_S_WHY_TINYCH));
    BODY_ADD(o ? "\n%s" : "%s", tr(STR_S_WHY_FOOT));
    #undef BODY_ADD

    // several cautions can stack here, so this body is the longest in the app:
    // auto-fit keeps it readable when it is short and inside the card when not
    lv_obj_t *b = lv_label_create(ovl);
    lv_label_set_text(b, body);
    lv_obj_set_style_text_color(b, MUT_COL, 0);
    lv_obj_set_style_text_font(b, wt_body_font(body, 720, 300), 0);
    lv_obj_set_width(b, 720);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 96);

    wt_pill(ovl, tr(STR_C_OK), 300, 412, 200, caution_ok_cb, ovl);
    wt_card_intro(ovl);
}

// ---- "?" on the RBF line: plain-words Replace-By-Fee ----
static void rbf_ok_cb(lv_event_t *e)
{
    lv_obj_delete_async((lv_obj_t *)lv_event_get_user_data(e));
}

static void rbf_help_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *ovl = lv_obj_create(s_scr);
    lv_obj_remove_style_all(ovl);
    lv_obj_set_size(ovl, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ovl, BG_COL, 0);
    lv_obj_set_style_bg_opa(ovl, 245, 0);
    lv_obj_add_flag(ovl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ovl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ovl, rbf_ok_cb, LV_EVENT_CLICKED, ovl);   // tap anywhere = close

    lv_obj_t *t = lv_label_create(ovl);
    lv_label_set_text(t, s_sum.rbf ? tr(STR_S_RBF_T_ON) : tr(STR_S_RBF_T_OFF));
    lv_obj_set_style_text_color(t, INK_COL, 0);
    lv_obj_set_style_text_font(t, wt_font28(), 0);
    lv_obj_set_style_text_letter_space(t, 2, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 92);

    const char *rbf_body = s_sum.rbf ? tr(STR_S_RBF_B_ON) : tr(STR_S_RBF_B_OFF);
    lv_obj_t *b = lv_label_create(ovl);
    lv_label_set_text(b, rbf_body);
    lv_obj_set_style_text_color(b, MUT_COL, 0);
    lv_obj_set_style_text_font(b, wt_body_font(rbf_body, 720, 230), 0);
    lv_obj_set_width(b, 720);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 156);

    wt_pill(ovl, tr(STR_C_OK), 300, 400, 200, rbf_ok_cb, ovl);
    wt_card_intro(ovl);
}

// "I UNDERSTAND" on a CAUTION: a deliberate second confirm before the hold pill
// even appears (user: "warn, second OK").
static void ack_cb(lv_event_t *e)
{
    (void)e;
    s_ack = true;
    hold_stop();
    lv_obj_delete_async(s_scr); s_scr = NULL; s_arc = NULL; s_sign_lbl = NULL;
    verify_screen(s_parent);
}

// ---- verify screen (the heart of the safety model) ----
static void verify_screen(lv_obj_t *parent)
{
    char buf[160], a[32], b[32];
    s_parent = parent;                    // details page rebuilds us from here
    mk_screen(parent, tr(STR_S_T), NULL);
    lv_obj_t *src = mk_lbl(s_cur, 40, 64, wt_font14(), MUT_COL);
    lv_obj_set_width(src, 360);
    lv_label_set_long_mode(src, LV_LABEL_LONG_CLIP);

    // The wallet identity belongs in the persistent header, not halfway down
    // the money hierarchy. It remains visible while the user compares every
    // amount, but no longer competes with fee/total.
    {
        uint8_t fp[4];
        wallet_ui_last_fp(fp);
        lv_obj_t *cap = mk_lbl(tr(STR_S_SIGNING_AS), 430, 30, wt_font14(), MUT_COL);
        lv_obj_set_width(cap, 150);
        lv_label_set_long_mode(cap, LV_LABEL_LONG_CLIP);
        snprintf(buf, sizeof buf, "%02X%02X%02X%02X", fp[0], fp[1], fp[2], fp[3]);
        lv_obj_t *f = mk_lbl(buf, 430, 50, wt_font23(), INK_COL);
        lv_obj_set_style_text_letter_space(f, 2, 0);
    }
    mk_status_light();

    // First question: what do the recipients get? Change stays itemized below
    // and is never counted as money sent away.
    uint64_t total = s_sum.send_sats + s_sum.fee_sats;
    mk_lbl(tr(STR_S_SENDING_CAP), 40, 96, wt_font14(), MUT_COL);
    fmt_sats(s_sum.send_sats, a, sizeof a);
    snprintf(buf, sizeof buf, "%s sats", a);
    mk_lbl(buf, 40, 116, wt_font28(), INK_COL);
    wt_fmt_btc(s_sum.send_sats, b, sizeof b);
    snprintf(buf, sizeof buf, "%s BTC", b);
    mk_lbl(buf, 40, 152, wt_font14(), MUT_COL);

    // Outputs — EVERY output is shown (scroll if it doesn't fit); nothing the
    // user is asked to sign is ever hidden.  In the common one-recipient case,
    // the amount is already the large "RECIPIENT GETS" value immediately
    // above, so don't repeat it.  That leaves enough room to show the full
    // recipient address and verified change without either row running under
    // the action bar.
    int recipient_n = 0;
    for (int i = 0; i < (int)s_sum.n_out && i < WPSBT_MAX_OUTS; i++)
        if (!s_sum.outs[i].is_change) recipient_n++;
    lv_obj_t *ol = lv_obj_create(s_scr);
    lv_obj_remove_style_all(ol);
    lv_obj_set_pos(ol, 40, 178);
    lv_obj_set_size(ol, 372, 214);
    lv_obj_set_style_pad_all(ol, 8, 0);
    lv_obj_set_style_pad_row(ol, 4, 0);
    lv_obj_set_flex_flow(ol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(ol, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ol, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(ol, LV_OPA_TRANSP, 0);
    for (int i = 0; i < (int)s_sum.n_out && i < WPSBT_MAX_OUTS; i++) {
        lv_obj_t *row = lv_obj_create(ol);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_bottom(row, 10, 0);

        lv_obj_t *tag = lv_label_create(row);
        if (s_sum.outs[i].is_change) {
            lv_label_set_text(tag, tr_sym(LV_SYMBOL_OK, STR_S_CHANGE_TAG));
            lv_obj_set_style_text_color(tag, OK_COL, 0);
        } else if (s_sum.outs[i].is_sp) {    // BIP375: destination derived here
            lv_label_set_text(tag, tr(STR_S_SP_BADGE));
            lv_obj_set_style_text_color(tag, wt_accent(), 0);
        } else {
            lv_label_set_text(tag, tr(STR_S_SENDING_OUT));
            lv_obj_set_style_text_color(tag, MUT_COL, 0);
        }
        lv_obj_set_style_text_font(tag, wt_font14(), 0);

        // A multi-recipient transaction still itemizes every recipient amount.
        // Change always keeps its own amount so "back to you" is explicit.
        if (s_sum.outs[i].is_change || recipient_n > 1) {
            fmt_sats(s_sum.outs[i].sats, a, sizeof a);
            wt_fmt_btc(s_sum.outs[i].sats, b, sizeof b);
            snprintf(buf, sizeof buf, "%s sats", a);
            lv_obj_t *amt = lv_label_create(row);
            lv_label_set_text(amt, buf);
            lv_obj_set_style_text_color(amt, INK_COL, 0);
            lv_obj_set_style_text_font(amt, wt_font23(), 0);
            snprintf(buf, sizeof buf, "%s BTC", b);
            lv_obj_t *btc = lv_label_create(row);
            lv_label_set_text(btc, buf);
            lv_obj_set_style_text_color(btc, MUT_COL, 0);
            lv_obj_set_style_text_font(btc, wt_font14(), 0);
        }

        char ga[160];   // sp1/tsp1 is ~117 chars; +grouping spaces needs >120
        group4(s_sum.outs[i].addr, ga, sizeof ga);
        if (s_sum.outs[i].is_change) {       // verified ours: stays quiet
            lv_obj_t *ad = lv_label_create(row);
            lv_label_set_text(ad, ga);
            lv_obj_set_style_text_color(ad, MUT_COL, 0);
            lv_obj_set_style_text_font(ad, wt_font14(), 0);
            lv_obj_set_width(ad, 340);
            lv_label_set_long_mode(ad, LV_LABEL_LONG_WRAP);
        } else {                             // compare-me (incl. SP): bright ends
            wt_addr_spans(row, ga, 340, wt_font14());
        }

        if (s_sum.outs[i].is_sp) {           // teach why a bc1p never appears here
            lv_obj_t *note = lv_label_create(row);
            lv_label_set_text(note, tr(STR_S_SP_NOTE));
            lv_obj_set_style_text_color(note, MUT_COL, 0);
            lv_obj_set_style_text_font(note, wt_font14(), 0);
            lv_obj_set_width(note, 340);
            lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
        }
    }

    // Second and third questions: fee, then the total leaving this wallet.
    // The three amounts are named explicitly so the user never has to infer
    // whether a number includes the fee.
    mk_lbl(tr(STR_S_FEE), 430, 100, wt_font14(), MUT_COL);
    fmt_sats(s_sum.fee_sats, a, sizeof a);
    snprintf(buf, sizeof buf, "%s sats", a);
    mk_lbl(buf, 430, 122, wt_font28(),
           s_sum.status == WPSBT_CAUTION ? WARN_COL : INK_COL);
    if (s_sum.send_sats > 0)
        snprintf(buf, sizeof buf, tr(STR_S_FEERATE_PCT_FMT),
                 (unsigned)(s_sum.fee_rate_x10 / 10), (unsigned)(s_sum.fee_rate_x10 % 10),
                 (unsigned long long)(s_sum.fee_sats * 1000 / s_sum.send_sats / 10),
                 (unsigned long long)(s_sum.fee_sats * 1000 / s_sum.send_sats % 10));
    else
        snprintf(buf, sizeof buf, tr(STR_S_FEERATE_FMT),
                 (unsigned)(s_sum.fee_rate_x10 / 10), (unsigned)(s_sum.fee_rate_x10 % 10));
    mk_lbl(buf, 430, 156, wt_font14(), MUT_COL);

    mk_lbl(tr(STR_S_TOTAL_LEAVING), 430, 184, wt_font14(), MUT_COL);
    fmt_sats(total, a, sizeof a);
    snprintf(buf, sizeof buf, "%s sats", a);
    mk_lbl(buf, 430, 204, wt_font23(), INK_COL);
    wt_fmt_btc(total, b, sizeof b);
    snprintf(buf, sizeof buf, "%s BTC", b);
    mk_lbl(buf, 430, 232, wt_font14(), MUT_COL);

    fmt_sats(s_sum.in_sats, a, sizeof a);
    fmt_sats(s_sum.change_sats, b, sizeof b);
    // spec: show the DETECTED script type (from the PSBT's own paths, not any
    // setting) — "mixed-type" when a transaction spends more than one kind. A
    // received silent-payment input (BIP376) has no BIP84 path, so label it as
    // such rather than "mixed" (reuses the existing badge string, no new i18n).
    const char *ity = s_sum.n_sp_in > 0 ? tr(STR_S_SP_BADGE)
                    : s_sum.purpose == 44 ? tr(STR_S_TY_LEGACY)
                    : s_sum.purpose == 49 ? tr(STR_S_TY_NESTED)
                    : s_sum.purpose == 84 ? tr(STR_S_TY_NATIVE) : tr(STR_S_TY_MIXED);
    snprintf(buf, sizeof buf, tr(STR_S_INPUTS_FMT),
             (unsigned)s_sum.n_in, ity);
    mk_lbl(buf, 430, 258, wt_font14(), MUT_COL);
    snprintf(buf, sizeof buf, tr(STR_S_IN_BACK_FMT), a, b);
    mk_lbl(buf, 430, 278, wt_font14(), MUT_COL);

    // network: LOUD amber chip on testnet (spec: loud TESTNET banner); mainnet
    // stays a plain muted word. (No address-type setting shown: the signer is
    // type-agnostic — the PSBT's own paths declare the type, re-derive enforces.)
    lv_obj_t *net = mk_lbl(s_sum.testnet ? "TESTNET" : "MAINNET", 430, 304,
                           wt_font14(), s_sum.testnet ? WARN_COL : MUT_COL);
    if (s_sum.testnet) {
        lv_obj_set_style_bg_color(net, lv_color_hex(0x2A2113), 0);
        lv_obj_set_style_bg_opa(net, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(net, WARN_COL, 0);
        lv_obj_set_style_border_width(net, 1, 0);
        lv_obj_set_style_radius(net, 10, 0);
        lv_obj_set_style_pad_hor(net, 10, 0);
        lv_obj_set_style_pad_ver(net, 4, 0);
        lv_obj_set_style_text_letter_space(net, 2, 0);
    }
    // locktime stays off the main screen: wallets set it to the current height
    // for anti-fee-sniping, so it appears on nearly every tx and reads as a
    // scary lock when it isn't. Only the actionable RBF/final line here; the raw
    // locktime value + a plain-words note live in DETAILS.
    snprintf(buf, sizeof buf, "%s",
             s_sum.rbf ? tr(STR_S_RBF_LINE_ON) : tr(STR_S_RBF_LINE_OFF));
    mk_lbl(buf, 430, 336, wt_font14(), MUT_COL);
    {   // "?" -> plain-words RBF explainer (most people don't know the term)
        int cx = s_sum.rbf ? 592 : 620;
        lv_obj_t *hc = lv_obj_create(s_scr);
        lv_obj_remove_style_all(hc);
        lv_obj_set_size(hc, 26, 26);
        lv_obj_set_pos(hc, cx, 332);
        lv_obj_set_style_radius(hc, 13, 0);
        lv_obj_set_style_bg_color(hc, KEY_COL, 0);
        lv_obj_set_style_bg_opa(hc, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(hc, 1, 0);
        lv_obj_set_style_border_color(hc, MUT_COL, 0);
        lv_obj_add_flag(hc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(hc, 12);
        lv_obj_add_event_cb(hc, rbf_help_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *hl = lv_label_create(hc);
        lv_label_set_text(hl, "?");
        lv_obj_set_style_text_color(hl, MUT_COL, 0);
        lv_obj_set_style_text_font(hl, wt_font14(), 0);
        lv_obj_center(hl);
    }

    if (s_sum.status == WPSBT_STOP) {
        lv_obj_t *r = mk_lbl(tr_reason(s_sum.reason), 430, 366, wt_font14(), STOP_COL);
        lv_obj_set_width(r, 320);
        lv_label_set_long_mode(r, LV_LABEL_LONG_WRAP);
    } else if (s_sum.status == WPSBT_CAUTION) {
        // short summary + a "?" chip to the full "why" (keeps the screen simple)
        char sum[160];   // three parts in a 2-3 byte/char script must fit
        caution_summary(s_sum.caution_flags, sum, sizeof sum);
        char line[200];
        snprintf(line, sizeof line, tr(STR_S_CAUTION_FMT), sum);
        lv_obj_t *r = mk_lbl(line, 430, 366, wt_font14(), WARN_COL);
        lv_obj_set_width(r, 280);
        lv_label_set_long_mode(r, LV_LABEL_LONG_WRAP);
        lv_obj_t *hc = lv_obj_create(s_scr);   // "?" -> WHY FLAGGED card
        lv_obj_remove_style_all(hc);
        lv_obj_set_size(hc, 30, 30);
        lv_obj_set_pos(hc, 720, 362);
        lv_obj_set_style_radius(hc, 15, 0);
        lv_obj_set_style_bg_color(hc, KEY_COL, 0);
        lv_obj_set_style_bg_opa(hc, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(hc, 1, 0);
        lv_obj_set_style_border_color(hc, WARN_COL, 0);
        lv_obj_add_flag(hc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(hc, 12);
        lv_obj_add_event_cb(hc, caution_help_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *hl = lv_label_create(hc);
        lv_label_set_text(hl, "?");
        lv_obj_set_style_text_color(hl, WARN_COL, 0);
        lv_obj_set_style_text_font(hl, wt_font14(), 0);
        lv_obj_center(hl);
    }

    // ACTION_H: tall enough for a label to take a SECOND LINE at font23 rather
    // than drop to font14 (two 29px lines plus padding). "HOLD TO SIGN" has no
    // one-line size above 14 in French, Italian or Swedish, and the button that
    // moves money is the last one that should be the smallest type on screen.
    // The whole row shares the height so the three pills still line up.
#define ACTION_H 66
#define ACTION_Y 398
    wt_pillh(s_scr, tr(STR_C_BACK), 48, ACTION_Y, 140, ACTION_H, close_cb, NULL);
    if (s_sum.status != WPSBT_STOP) {
        // no DETAILS on STOP: the details page presents fields as verified,
        // and a refused transaction has nothing left to decide
        wt_pillh(s_scr, tr(STR_S_DETAILS), 208, ACTION_Y, 170, ACTION_H,
                 details_cb, NULL);
        if (s_sum.status == WPSBT_CAUTION && !s_ack) {
            // gate the hold pill behind a deliberate acknowledgement
            lv_obj_t *ok = wt_pillh(s_scr, tr(STR_C_I_UNDERSTAND), 500, ACTION_Y,
                                    252, ACTION_H, ack_cb, NULL);
            wt_pill_primary(ok);
            lv_obj_set_style_border_color(ok, WARN_COL, 0);
            lv_obj_set_style_text_color(lv_obj_get_child(ok, 0), WARN_COL, 0);
        } else {
            // hold-to-sign: ring fills while pressed; let go = nothing happens
            s_arc = lv_arc_create(s_scr);
            lv_obj_set_size(s_arc, 64, 64);
            lv_obj_set_pos(s_arc, 420, ACTION_Y + 1);
            lv_arc_set_rotation(s_arc, 270);
            lv_arc_set_bg_angles(s_arc, 0, 360);
            lv_arc_set_range(s_arc, 0, 100);
            lv_arc_set_value(s_arc, 0);
            lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
            lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_arc_width(s_arc, 6, LV_PART_MAIN);
            lv_obj_set_style_arc_width(s_arc, 6, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(s_arc, KEY_COL, LV_PART_MAIN);
            lv_obj_set_style_arc_color(s_arc, wt_accent(), LV_PART_INDICATOR);

            lv_obj_t *p = wt_pillh(s_scr, tr(STR_S_HOLD_TO_SIGN), 480, ACTION_Y,
                                   272, ACTION_H, NULL, NULL);
            lv_obj_add_event_cb(p, sign_press_cb, LV_EVENT_ALL, NULL);
            lv_obj_set_style_border_color(p, wt_primary(), 0);
            wt_pill_label_max(p);      // the most consequential button in the app
            s_sign_lbl = lv_obj_get_child(p, 0);
        }
    }
}

// ---- DETAILS: the second page for people who want the raw facts. One page,
// one tap in, one tap back — the verify screen stays simple. ----
static void details_back_cb(lv_event_t *e)
{
    (void)e;
    hold_stop();
    lv_obj_delete_async(s_scr); s_scr = NULL; s_arc = NULL; s_sign_lbl = NULL;
    verify_screen(s_parent);
}

static void glossary_ok_cb(lv_event_t *e)
{
    lv_obj_delete_async((lv_obj_t *)lv_event_get_user_data(e));
}

static void glossary_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *ovl = lv_obj_create(s_scr);
    lv_obj_remove_style_all(ovl);
    lv_obj_set_size(ovl, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ovl, BG_COL, 0);
    lv_obj_set_style_bg_opa(ovl, 245, 0);
    lv_obj_add_flag(ovl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ovl, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(ovl);
    lv_label_set_text(t, tr(STR_S_GLOSSARY_T));
    lv_obj_set_style_text_color(t, INK_COL, 0);
    lv_obj_set_style_text_font(t, wt_font28(), 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 44);

    const char *copy = tr(STR_S_GLOSSARY_B);
    lv_obj_t *b = lv_label_create(ovl);
    lv_label_set_text(b, copy);
    lv_obj_set_style_text_color(b, MUT_COL, 0);
    lv_obj_set_style_text_font(b, wt_body_font(copy, 704, 294), 0);
    lv_obj_set_width(b, 704);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(b, 48, 98);

    wt_pill(ovl, tr(STR_C_OK), 300, 404, 200, glossary_ok_cb, ovl);
    wt_card_intro(ovl);
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
    mk_lbl(buf, 40, 96, wt_font14(), MUT_COL);

    lv_obj_t *il = lv_obj_create(s_scr);
    lv_obj_remove_style_all(il);
    // the many-inputs header wraps to 2 lines: start the list below it
    // (latent in the original layout, exposed by the 17-input fixture)
    int ly = det.n_total > det.n_in ? 146 : 118;
    lv_obj_set_pos(il, 40, ly);
    lv_obj_set_size(il, 372, 392 - ly);
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
        lv_obj_set_style_text_font(amt, wt_font14(), 0);

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

    // the id to find it by, once broadcast — final only for segwit-only spends
    mk_lbl(tr(STR_S_D_TXID), 430, 96, wt_font14(), MUT_COL);
    char gt[80];
    group4(det.txid, gt, sizeof gt);
    lv_obj_t *tx = mk_lbl(gt, 430, 118, wt_font14(), INK_COL);
    lv_obj_set_width(tx, 330);
    lv_label_set_long_mode(tx, LV_LABEL_LONG_WRAP);
    mk_lbl(det.txid_final ? tr(STR_S_D_TXID_SAME)
                          : tr(STR_S_D_TXID_CHANGES),
           430, 210, wt_font14(), MUT_COL);

    snprintf(buf, sizeof buf, tr(STR_S_D_VER_LT_FMT),
             (unsigned)det.version, (unsigned)det.locktime);
    mk_lbl(buf, 430, 258, wt_font14(), MUT_COL);
    mk_lbl(det.locktime ? tr(STR_S_D_LT_NONZERO)
                        : tr(STR_S_D_LT_ZERO),
           430, 280, wt_font14(), MUT_COL);
    mk_lbl(tr(STR_S_D_SIGHASH),
           430, 306, wt_font14(), MUT_COL);
    mk_lbl(s_sum.rbf ? tr(STR_S_D_RBF_ON)
                     : tr(STR_S_D_RBF_OFF),
           430, 352, wt_font14(), MUT_COL);

    mk_pill(tr(STR_C_BACK), 48, 404, 140, details_back_cb);
}

// ---- QR out: the signed PSBT as an animated QR (UR, or pMofN if it came
// that way). Static-in still leaves as animated UR — a signed PSBT rarely
// fits one readable QR at 288px, and every coordinator that speaks QR reads UR.
static void qr_tick(lv_timer_t *t)
{
    (void)t;
    char part[600];
    if (!s_qenc || qrt_encoder_next(s_qenc, part, sizeof part) != 0) return;
    if (s_qr_img) lv_qrcode_update(s_qr_img, part, (uint32_t)strlen(part));
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
        mk_pill(tr(STR_C_BACK), 330, 404, 140, close_cb);
        return;
    }

    mk_screen(parent, tr(STR_S_SIGNED_T), tr(STR_S_QR_SUB));
    wt_qr_card(s_scr, &s_qr_img, 48, 100, 316, 288);

    int n = qrt_encoder_parts(s_qenc);
    mk_lbl(tr_sym(LV_SYMBOL_OK, STR_S_SIGNED_T), 430, 100, wt_font14(), OK_COL);
    s_part_lbl = mk_lbl(n > 1 ? tr(STR_S_QR_PART1) : tr(STR_S_QR_SINGLE), 430, 124,
                        wt_font28(), INK_COL);
    if (n > 1) {
        mk_lbl(tr(STR_S_QR_LOOP), 430, 170,
               wt_font14(), MUT_COL);
        s_qr_tmr = lv_timer_create(qr_tick, 250, NULL);
    }
    mk_lbl(tr(STR_S_NO_NETWORK), 430, 196,
           wt_font14(), MUT_COL);
    s_ez_pill = wt_pill(s_scr, tr(STR_S_EASY_SCAN), 430, 244, 200, qr_ez_cb, NULL);
    mk_lbl(tr(STR_S_EZ_NOTE),
           430, 310, wt_font14(), MUT_COL);
    mk_pill(tr(STR_C_DONE), 610, 404, 140, close_cb);
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
        mk_lbl(tr(STR_S_READ_FAIL),
               48, 140, wt_font14(), STOP_COL);
        mk_pill(tr(STR_C_BACK), 48, 404, 140, close_cb);
        return;
    }
    SIGN_LOG("SD read: %s, %u bytes", s_cur, (unsigned)len);
    int lrc = wallet_psbt_load(s_in, len, &s_sum);
    s_ack = false;                         // fresh PSBT: re-acknowledge any caution
    if (lrc != 0) {
        SIGN_LOG("REJECTED: not a parseable PSBT (rc %d)", lrc);
        mk_screen(parent, tr(STR_S_T), s_cur);
        mk_lbl(tr(STR_S_NOT_PSBT), 48, 140, wt_font14(), STOP_COL);
        mk_pill(tr(STR_C_BACK), 48, 404, 140, close_cb);
        return;
    }
    log_summary("SD");
    verify_screen(parent);
}

static void sd_open(lv_obj_t *parent)
{
    s_src = SRC_SD;
    if (platform_sd_mount() != 0) {
        mk_screen(parent, tr(STR_S_T), tr(STR_S_SD_SUB));
        mk_lbl(tr(STR_S_NO_SD), 48, 140, wt_font28(), INK_COL);
        mk_lbl(tr(STR_S_INSERT_CARD),
               48, 184, wt_font14(), MUT_COL);
        mk_pill(tr(STR_C_BACK), 48, 404, 140, close_cb);
        return;
    }
    int n = platform_sd_list_psbt(s_files, MAX_FILES);
    if (n <= 0) {
        mk_screen(parent, tr(STR_S_T), tr(STR_S_SD_SUB));
        mk_lbl(tr(STR_S_NO_PSBT_FILES), 48, 140, wt_font28(), INK_COL);
        mk_lbl(tr(STR_S_SPARROW_SAVE),
               48, 184, wt_font14(), MUT_COL);
        mk_pill(tr(STR_C_BACK), 48, 404, 140, close_cb);
        return;
    }
    mk_screen(parent, tr(STR_S_T), tr(STR_S_CHOOSE_FILE));
    lv_obj_t *sd = mk_lbl(tr_sym(LV_SYMBOL_OK, STR_S_SD_READY), 560, 38, wt_font14(), OK_COL);
    lv_obj_set_style_text_letter_space(sd, 1, 0);
    mk_lbl(tr(STR_S_FILES_HINT), 48, 88, wt_font14(), MUT_COL);

    // All discovered files fit in one scrollable, deterministic list. Unsigned
    // work is sorted first; signed PSBTs remain available for multisig handoffs
    // but are visibly labelled so nobody accidentally treats one as fresh.
    lv_obj_t *list = lv_obj_create(s_scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 48, 112);
    lv_obj_set_size(list, 560, 278);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    for (int i = 0; i < n; i++) {
        size_t nl = strlen(s_files[i]);
        bool signed_file = nl >= 12
                        && strcasecmp(s_files[i] + nl - 12, "-signed.psbt") == 0;
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 56);
        lv_obj_set_style_radius(row, 26, 0);
        lv_obj_set_style_bg_color(row, KEY_COL, 0);
        lv_obj_set_style_bg_color(row, wt_accent_pressed(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, signed_file ? WARN_COL : MUT_COL, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, file_tap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, s_files[i]);
        lv_obj_set_style_text_color(name, signed_file ? MUT_COL : INK_COL, 0);
        lv_obj_set_style_text_font(name, wt_font14(), 0);
        lv_obj_set_width(name, 410);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 20, 0);

        lv_obj_t *tag = lv_label_create(row);
        lv_label_set_text(tag, signed_file ? tr(STR_S_SIGNED_T)
                                           : tr(STR_S_FILE_UNSIGNED));
        lv_obj_set_style_text_color(tag, signed_file ? WARN_COL : MUT_COL, 0);
        lv_obj_set_style_text_font(tag, wt_font14(), 0);
        lv_obj_set_style_text_letter_space(tag, 1, 0);
        lv_obj_align(tag, LV_ALIGN_RIGHT_MID, -18, 0);
    }
    mk_pill(tr(STR_C_BACK), 610, 404, 140, close_cb);
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
    int lrc = wallet_psbt_load(s_in, len, &s_sum);
    s_ack = false;                         // fresh PSBT: re-acknowledge any caution
    if (lrc != 0) {
        SIGN_LOG("REJECTED: not a parseable PSBT (rc %d)", lrc);
        mk_screen(s_parent, tr(STR_S_T), s_cur);
        mk_lbl(tr(STR_S_SCAN_NOT_PSBT), 48, 140,
               wt_font14(), STOP_COL);
        mk_pill(tr(STR_C_BACK), 48, 404, 140, close_cb);
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

// ---- "?" chip: what a coordinator wallet is, one plain-English card ----
static void coord_ok_cb(lv_event_t *e)
{
    lv_obj_delete_async((lv_obj_t *)lv_event_get_user_data(e));
}

static void coord_help_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *ovl = lv_obj_create(s_scr);
    lv_obj_remove_style_all(ovl);
    lv_obj_set_size(ovl, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ovl, BG_COL, 0);
    lv_obj_set_style_bg_opa(ovl, 245, 0);
    lv_obj_add_flag(ovl, LV_OBJ_FLAG_CLICKABLE);      // swallow stray taps
    lv_obj_clear_flag(ovl, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(ovl);
    lv_label_set_text(t, tr(STR_S_COORD_T));
    lv_obj_set_style_text_color(t, INK_COL, 0);
    lv_obj_set_style_text_font(t, wt_font28(), 0);
    lv_obj_set_style_text_letter_space(t, 2, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 92);

    lv_obj_t *b = lv_label_create(ovl);
    lv_label_set_text(b, tr(STR_S_COORD_B));
    lv_obj_set_style_text_color(b, MUT_COL, 0);
    lv_obj_set_style_text_font(b, wt_body_font(tr(STR_S_COORD_B), 720, 144), 0);
    lv_obj_set_width(b, 720);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 156);

    wt_diagram_pair(ovl, 300);                        // ONLINE APP <- QR -> KISS OFFLINE

    lv_obj_t *ok = lv_obj_create(ovl);
    lv_obj_remove_style_all(ok);
    lv_obj_set_size(ok, 200, 52);
    lv_obj_align(ok, LV_ALIGN_TOP_MID, 0, 362);
    lv_obj_set_style_radius(ok, 26, 0);
    lv_obj_set_style_bg_color(ok, KEY_COL, 0);
    lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ok, 1, 0);
    lv_obj_set_style_border_color(ok, MUT_COL, 0);
    lv_obj_add_flag(ok, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ok, coord_ok_cb, LV_EVENT_CLICKED, ovl);
    lv_obj_t *ol = lv_label_create(ok);
    lv_label_set_text(ol, tr(STR_C_OK));
    lv_obj_set_style_text_color(ol, INK_COL, 0);
    lv_obj_set_style_text_font(ol, wt_font14(), 0);
    lv_obj_set_style_text_letter_space(ol, 2, 0);
    lv_obj_center(ol);
    wt_card_intro(ovl);
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
    lv_obj_t *q = mk_pill(tr(STR_S_SCAN_QR), 48, 150, 340, scan_pick_cb);
    wt_pill_primary(q);                                   // QR primary, SD fallback (spec)
    mk_pill(tr(STR_S_FROM_SD), 48, 230, 340, sd_pick_cb);
    mk_lbl(tr(STR_S_POINT_CAM), 430, 152,
           wt_font14(), MUT_COL);
    // small "?" chip after the caption -> the coordinator explainer card
    lv_obj_t *hc = lv_obj_create(s_scr);
    lv_obj_remove_style_all(hc);
    lv_obj_set_size(hc, 36, 36);
    lv_obj_set_pos(hc, 700, 152);   // clear of the caption, inside the BACK-pill x-extent
    lv_obj_set_style_radius(hc, 18, 0);
    lv_obj_set_style_bg_color(hc, KEY_COL, 0);
    lv_obj_set_style_bg_opa(hc, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hc, 1, 0);
    lv_obj_set_style_border_color(hc, MUT_COL, 0);
    lv_obj_add_flag(hc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(hc, 14);                // small chip, honest target
    lv_obj_add_event_cb(hc, coord_help_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *hl = lv_label_create(hc);
    lv_label_set_text(hl, "?");
    lv_obj_set_style_text_color(hl, INK_COL, 0);
    lv_obj_set_style_text_font(hl, wt_font14(), 0);
    lv_obj_center(hl);
    mk_lbl(tr(STR_S_OR_LOAD), 430, 244,
           wt_font14(), MUT_COL);
    mk_pill(tr(STR_C_BACK), 610, 404, 140, close_cb);
}
