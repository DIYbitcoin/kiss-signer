// Steps 5+6: Sign. Get a PSBT by QR scan (static / pMofN / animated BC-UR) or
// from the SD card, show the spec's verify screen (status light, every output
// with change re-derived ON THIS DEVICE, fee three ways, network/RBF/locktime),
// then hold-to-sign. Signed PSBT goes back the way it came: SD file in ->
// <name>-signed.psbt on the card; QR in -> animated QR out.
// Compiled in BOTH device and sim builds; the sim stubs wallet_psbt_* in sim_main.c.
#include "wallet_sign.h"

#include <stdio.h>
#include <string.h>

#include "platform_sd.h"
#include "qr_transport.h"
#include "wallet_crypto.h"
#include "wallet_psbt.h"
#include "wallet_scan.h"
#include "wallet_theme.h"
#include "wallet_ui.h"   // wallet_ui_last_fp: the SIGNING AS fingerprint

#define BG_COL   WT_BG
#define INK_COL  WT_INK
#define MUT_COL  WT_MUT
#define KEY_COL  WT_KEY
#define OK_COL   WT_OK    // status light: label + shape + color,
#define WARN_COL WT_WARN  // never color alone (spec)
#define STOP_COL WT_STOP

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
static uint8_t s_in[4096], s_out[4680];
static lv_obj_t *s_parent;             // where this flow's screens are built
static int s_src;                      // SRC_SD / SRC_QR: where the PSBT came from
static int s_qr_fmt;                   // QRT_FMT_* the scan arrived in
static qrt_encoder_t *s_qenc;          // QR-out encoder (animated signed PSBT)
static lv_timer_t *s_qr_tmr;
static lv_timer_t *s_done_tmr;   // SD sign: auto-return to home after the success screen
static lv_obj_t *s_qr_img, *s_part_lbl;
static int s_part_i;

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
    s_qr_img = NULL; s_part_lbl = NULL;
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

// same grouped-by-4 convention as the Receive screen: visual compare against
// the coordinator is the whole point of this screen
#define group4   wt_group4
#define fmt_sats wt_fmt_sats

static void mk_status_light(void)
{
    const char *word = s_sum.status == WPSBT_READY ? LV_SYMBOL_OK "  READY"
                     : s_sum.status == WPSBT_CAUTION ? LV_SYMBOL_WARNING "  CAUTION"
                     : LV_SYMBOL_CLOSE "  STOP";
    lv_color_t col = s_sum.status == WPSBT_READY ? OK_COL
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
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
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
    mk_screen(parent, "SIGNED", "signed successfully. take this card to your coordinator to broadcast");
    lv_obj_t *big = mk_lbl(LV_SYMBOL_OK, 0, 150, &lv_font_montserrat_48, OK_COL);
    lv_obj_align(big, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_t *fn = mk_lbl(outname, 0, 230, &lv_font_montserrat_28, INK_COL);
    lv_obj_align(fn, LV_ALIGN_TOP_MID, 0, 230);
    lv_obj_t *note = mk_lbl("saved to the card. this device never touched the network", 0, 280,
                            &lv_font_montserrat_14, MUT_COL);
    lv_obj_align(note, LV_ALIGN_TOP_MID, 0, 280);
    mk_pill("DONE", 330, 404, 140, close_cb);
    // nothing needs to stay on screen (the file is saved), so drift back to home
    s_done_tmr = lv_timer_create(auto_home_cb, 6000, NULL);
    lv_timer_set_repeat_count(s_done_tmr, 1);
}

static void fail_screen(const char *why)
{
    lv_obj_t *parent = lv_obj_get_parent(s_scr);
    lv_obj_delete(s_scr); s_scr = NULL; s_arc = NULL; s_sign_lbl = NULL;
    mk_screen(parent, "SIGN FAILED", why);
    mk_pill("BACK", 330, 404, 140, close_cb);
}

static void do_sign_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    size_t sw = 0;
    if (wallet_psbt_sign(s_out, sizeof s_out, &sw) != 0) {
        fail_screen("the transaction could not be signed");
        return;
    }
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
        fail_screen("could not write to the SD card");
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
        if (s_sign_lbl) lv_label_set_text(s_sign_lbl, "SIGNING...");
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

// ---- verify screen (the heart of the safety model) ----
static void verify_screen(lv_obj_t *parent)
{
    char buf[160], a[32], b[32];
    s_parent = parent;                    // details page rebuilds us from here
    mk_screen(parent, "SIGN", s_cur);
    mk_status_light();

    // the one number to check first: everything leaving this wallet
    // (amount sent + fee), in both units the coordinator might display
    uint64_t total = s_sum.in_sats - s_sum.change_sats;
    mk_lbl("YOU ARE SENDING", 40, 96, &lv_font_montserrat_14, MUT_COL);
    fmt_sats(total, a, sizeof a);
    snprintf(buf, sizeof buf, "%s sats", a);
    mk_lbl(buf, 40, 116, &lv_font_montserrat_28, INK_COL);
    wt_fmt_btc(total, b, sizeof b);
    snprintf(buf, sizeof buf, "%s BTC = amount + fee", b);
    mk_lbl(buf, 40, 152, &lv_font_montserrat_14, MUT_COL);

    // outputs — EVERY output is shown (scroll if it doesn't fit); nothing the
    // user is asked to sign is ever hidden. change rows say why they're safe.
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

        fmt_sats(s_sum.outs[i].sats, a, sizeof a);
        wt_fmt_btc(s_sum.outs[i].sats, b, sizeof b);
        snprintf(buf, sizeof buf, "%s sats   (%s BTC)", a, b);
        lv_obj_t *amt = lv_label_create(row);
        lv_label_set_text(amt, buf);
        lv_obj_set_style_text_color(amt, INK_COL, 0);
        lv_obj_set_style_text_font(amt, &lv_font_montserrat_14, 0);

        char ga[120];
        group4(s_sum.outs[i].addr, ga, sizeof ga);
        if (s_sum.outs[i].is_change) {       // verified ours: stays quiet
            lv_obj_t *ad = lv_label_create(row);
            lv_label_set_text(ad, ga);
            lv_obj_set_style_text_color(ad, MUT_COL, 0);
            lv_obj_set_style_text_font(ad, &lv_font_montserrat_14, 0);
            lv_obj_set_width(ad, 340);
            lv_label_set_long_mode(ad, LV_LABEL_LONG_WRAP);
        } else {                             // compare-me: bright ends
            wt_addr_spans(row, ga, 340, &lv_font_montserrat_14);
        }

        lv_obj_t *tag = lv_label_create(row);
        if (s_sum.outs[i].is_change) {
            lv_label_set_text(tag, LV_SYMBOL_OK " change back to you, verified here");
            lv_obj_set_style_text_color(tag, OK_COL, 0);
        } else {
            lv_label_set_text(tag, "sending out");
            lv_obj_set_style_text_color(tag, MUT_COL, 0);
        }
        lv_obj_set_style_text_font(tag, &lv_font_montserrat_14, 0);
    }

    // fee + facts, right column
    mk_lbl("FEE", 430, 100, &lv_font_montserrat_14, MUT_COL);
    fmt_sats(s_sum.fee_sats, a, sizeof a);
    snprintf(buf, sizeof buf, "%s sats", a);
    mk_lbl(buf, 430, 122, &lv_font_montserrat_28, INK_COL);
    if (s_sum.send_sats > 0)
        snprintf(buf, sizeof buf, "%u.%u sat/vB,  %llu.%llu%% of what you send",
                 (unsigned)(s_sum.fee_rate_x10 / 10), (unsigned)(s_sum.fee_rate_x10 % 10),
                 (unsigned long long)(s_sum.fee_sats * 1000 / s_sum.send_sats / 10),
                 (unsigned long long)(s_sum.fee_sats * 1000 / s_sum.send_sats % 10));
    else
        snprintf(buf, sizeof buf, "%u.%u sat/vB",
                 (unsigned)(s_sum.fee_rate_x10 / 10), (unsigned)(s_sum.fee_rate_x10 % 10));
    mk_lbl(buf, 430, 160, &lv_font_montserrat_14, MUT_COL);

    fmt_sats(s_sum.in_sats, a, sizeof a);
    fmt_sats(s_sum.change_sats, b, sizeof b);
    // spec: show the DETECTED script type (from the PSBT's own paths, not any
    // setting) — "mixed-type" when a transaction spends more than one kind
    const char *ity = s_sum.purpose == 44 ? "Legacy"
                    : s_sum.purpose == 49 ? "Nested SegWit"
                    : s_sum.purpose == 84 ? "Native SegWit" : "mixed-type";
    snprintf(buf, sizeof buf, "%u %s input%s",
             (unsigned)s_sum.n_in, ity, s_sum.n_in == 1 ? "" : "s");
    mk_lbl(buf, 430, 186, &lv_font_montserrat_14, MUT_COL);
    snprintf(buf, sizeof buf, "%s sats in,  %s back to you", a, b);
    mk_lbl(buf, 430, 208, &lv_font_montserrat_14, MUT_COL);

    // network: LOUD amber chip on testnet (spec: loud TESTNET banner); mainnet
    // stays a plain muted word. (No address-type setting shown: the signer is
    // type-agnostic — the PSBT's own paths declare the type, re-derive enforces.)
    lv_obj_t *net = mk_lbl(s_sum.testnet ? "TESTNET" : "MAINNET", 430, 232,
                           &lv_font_montserrat_14, s_sum.testnet ? WARN_COL : MUT_COL);
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
    snprintf(buf, sizeof buf, "%s,  locktime %u",
             s_sum.rbf ? "replaceable (RBF)" : "final", (unsigned)s_sum.locktime);
    mk_lbl(buf, 430, 266, &lv_font_montserrat_14, MUT_COL);

    // which passphrase-wallet is about to sign — fingerprint = the login check
    // (spec), so give it the same visual weight as the fee number
    {
        uint8_t fp[4];
        wallet_ui_last_fp(fp);
        mk_lbl("SIGNING AS", 430, 290, &lv_font_montserrat_14, MUT_COL);
        snprintf(buf, sizeof buf, "%02X%02X%02X%02X", fp[0], fp[1], fp[2], fp[3]);
        lv_obj_t *f = mk_lbl(buf, 430, 308, &lv_font_montserrat_28, INK_COL);
        lv_obj_set_style_text_letter_space(f, 2, 0);
    }

    if (s_sum.status != WPSBT_READY) {
        lv_obj_t *r = mk_lbl(s_sum.reason, 430, 350, &lv_font_montserrat_14,
                             s_sum.status == WPSBT_STOP ? STOP_COL : WARN_COL);
        lv_obj_set_width(r, 320);
        lv_label_set_long_mode(r, LV_LABEL_LONG_WRAP);
        if (s_sum.status == WPSBT_STOP)
            mk_lbl("this device will not sign it", 430, 388,
                   &lv_font_montserrat_14, MUT_COL);
    }

    mk_pill("BACK", 48, 404, 140, close_cb);
    if (s_sum.status != WPSBT_STOP) {
        // no DETAILS on STOP: the details page presents fields as verified,
        // and a refused transaction has nothing left to decide
        mk_pill("DETAILS", 208, 404, 170, details_cb);
        // hold-to-sign: ring fills while pressed; let go = nothing happens
        s_arc = lv_arc_create(s_scr);
        lv_obj_set_size(s_arc, 64, 64);
        lv_obj_set_pos(s_arc, 420, 398);
        lv_arc_set_rotation(s_arc, 270);
        lv_arc_set_bg_angles(s_arc, 0, 360);
        lv_arc_set_range(s_arc, 0, 100);
        lv_arc_set_value(s_arc, 0);
        lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(s_arc, 6, LV_PART_MAIN);
        lv_obj_set_style_arc_width(s_arc, 6, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_arc, KEY_COL, LV_PART_MAIN);
        lv_obj_set_style_arc_color(s_arc, OK_COL, LV_PART_INDICATOR);

        lv_obj_t *p = mk_pill("HOLD TO SIGN", 500, 404, 252, NULL);
        lv_obj_add_event_cb(p, sign_press_cb, LV_EVENT_ALL, NULL);
        lv_obj_set_style_border_color(p, OK_COL, 0);
        s_sign_lbl = lv_obj_get_child(p, 0);
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

static void details_cb(lv_event_t *e)
{
    (void)e;
    wpsbt_details_t det;
    if (wallet_psbt_details(&det) != 0)
        return;
    hold_stop();
    lv_obj_delete_async(s_scr); s_scr = NULL; s_arc = NULL; s_sign_lbl = NULL;
    mk_screen(s_parent, "DETAILS", s_cur);

    char buf[128], a[32];
    if (det.n_total > det.n_in)          // more inputs than the page can hold
        snprintf(buf, sizeof buf,
                 "THIS TRANSACTION HAS %u INPUTS\nshowing %u here - all are verified as yours",
                 (unsigned)det.n_total, (unsigned)det.n_in);
    else
        snprintf(buf, sizeof buf, "INPUTS (%u) - ALL VERIFIED YOURS", (unsigned)det.n_in);
    mk_lbl(buf, 40, 96, &lv_font_montserrat_14, MUT_COL);

    lv_obj_t *il = lv_obj_create(s_scr);
    lv_obj_remove_style_all(il);
    lv_obj_set_pos(il, 40, 118);
    lv_obj_set_size(il, 372, 274);
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
        lv_obj_set_style_text_font(amt, &lv_font_montserrat_14, 0);

        // coin being spent: first 8 + last 8 of its txid, and the output index
        snprintf(buf, sizeof buf, "%.8s...%s : %u",
                 det.ins[i].txid, det.ins[i].txid + 56, (unsigned)det.ins[i].vout);
        lv_obj_t *tid = lv_label_create(row);
        lv_label_set_text(tid, buf);
        lv_obj_set_style_text_color(tid, MUT_COL, 0);
        lv_obj_set_style_text_font(tid, &lv_font_montserrat_14, 0);

        snprintf(buf, sizeof buf, LV_SYMBOL_OK " m/%u'/%d'/0'/%u/%u",
                 (unsigned)det.ins[i].purpose, s_sum.testnet ? 1 : 0,
                 (unsigned)det.ins[i].change, (unsigned)det.ins[i].index);
        lv_obj_t *pl = lv_label_create(row);
        lv_label_set_text(pl, buf);
        lv_obj_set_style_text_color(pl, OK_COL, 0);
        lv_obj_set_style_text_font(pl, &lv_font_montserrat_14, 0);
    }

    // the id to find it by, once broadcast — final only for segwit-only spends
    mk_lbl("TRANSACTION ID", 430, 96, &lv_font_montserrat_14, MUT_COL);
    char gt[80];
    group4(det.txid, gt, sizeof gt);
    lv_obj_t *tx = mk_lbl(gt, 430, 118, &lv_font_montserrat_14, INK_COL);
    lv_obj_set_width(tx, 330);
    lv_label_set_long_mode(tx, LV_LABEL_LONG_WRAP);
    mk_lbl(det.txid_final ? "your coordinator shows this same id"
                          : "will change when signed (legacy inputs)",
           430, 210, &lv_font_montserrat_14, MUT_COL);

    snprintf(buf, sizeof buf, "version %u,  locktime %u",
             (unsigned)det.version, (unsigned)det.locktime);
    mk_lbl(buf, 430, 258, &lv_font_montserrat_14, MUT_COL);
    mk_lbl("sighash ALL: signatures cover every\namount and destination above",
           430, 284, &lv_font_montserrat_14, MUT_COL);
    mk_lbl(s_sum.rbf ? "replaceable (RBF): the fee can be\nbumped after broadcast"
                     : "final: not replaceable after broadcast",
           430, 330, &lv_font_montserrat_14, MUT_COL);

    mk_pill("BACK", 48, 404, 140, details_back_cb);
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
        char b[32];
        snprintf(b, sizeof b, "part %d of %d", s_part_i, n);
        lv_label_set_text(s_part_lbl, b);
    }
}

static void qr_out_screen(size_t sw)
{
    lv_obj_t *parent = lv_obj_get_parent(s_scr);
    lv_obj_delete(s_scr); s_scr = NULL; s_arc = NULL; s_sign_lbl = NULL;

    int fmt = (s_qr_fmt == QRT_FMT_PMOFN) ? QRT_FMT_PMOFN : QRT_FMT_UR;
    s_qenc = qrt_encoder_new(fmt, s_out, sw);
    if (!s_qenc) {
        mk_screen(parent, "SIGN FAILED", "could not encode the signed transaction");
        mk_pill("BACK", 330, 404, 140, close_cb);
        return;
    }

    mk_screen(parent, "SIGNED", "signed successfully. scan this with your coordinator to broadcast");
    wt_qr_card(s_scr, &s_qr_img, 48, 100, 316, 288);

    int n = qrt_encoder_parts(s_qenc);
    mk_lbl(LV_SYMBOL_OK "  SIGNED", 430, 100, &lv_font_montserrat_14, OK_COL);
    s_part_lbl = mk_lbl(n > 1 ? "part 1" : "single QR", 430, 124,
                        &lv_font_montserrat_28, INK_COL);
    if (n > 1) {
        mk_lbl("it keeps looping, so hold your phone steady", 430, 170,
               &lv_font_montserrat_14, MUT_COL);
        s_qr_tmr = lv_timer_create(qr_tick, 250, NULL);
    }
    mk_lbl("this device never touched the network", 430, 196,
           &lv_font_montserrat_14, MUT_COL);
    mk_pill("DONE", 610, 404, 140, close_cb);
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
        mk_screen(parent, "SIGN", s_cur);
        mk_lbl("could not read that file (too big or unreadable)",
               48, 140, &lv_font_montserrat_14, STOP_COL);
        mk_pill("BACK", 48, 404, 140, close_cb);
        return;
    }
    int lrc = wallet_psbt_load(s_in, len, &s_sum);
    if (lrc != 0) {
        mk_screen(parent, "SIGN", s_cur);
        mk_lbl("that file is not a valid PSBT", 48, 140, &lv_font_montserrat_14, STOP_COL);
        mk_pill("BACK", 48, 404, 140, close_cb);
        return;
    }
    verify_screen(parent);
}

static void sd_open(lv_obj_t *parent)
{
    s_src = SRC_SD;
    if (platform_sd_mount() != 0) {
        mk_screen(parent, "SIGN", "move the transaction by SD card");
        mk_lbl("no SD card found", 48, 140, &lv_font_montserrat_28, INK_COL);
        mk_lbl("insert a card holding the PSBT file your coordinator saved",
               48, 184, &lv_font_montserrat_14, MUT_COL);
        mk_pill("BACK", 48, 404, 140, close_cb);
        return;
    }
    int n = platform_sd_list_psbt(s_files, MAX_FILES);
    if (n <= 0) {
        mk_screen(parent, "SIGN", "move the transaction by SD card");
        mk_lbl("no .psbt files on this card", 48, 140, &lv_font_montserrat_28, INK_COL);
        mk_lbl("in Sparrow Wallet: save the transaction as a PSBT file onto the card",
               48, 184, &lv_font_montserrat_14, MUT_COL);
        mk_pill("BACK", 48, 404, 140, close_cb);
        return;
    }
    mk_screen(parent, "SIGN", "choose the transaction file to verify");
    lv_obj_t *sd = mk_lbl(LV_SYMBOL_OK "  SD card ready", 560, 38, &lv_font_montserrat_14, OK_COL);
    lv_obj_set_style_text_letter_space(sd, 1, 0);
    for (int i = 0; i < n && i < 4; i++) {
        lv_obj_t *p = mk_pill(s_files[i], 48, 110 + i * 66, 560, file_tap_cb);
        lv_obj_remove_event_cb(p, file_tap_cb);           // re-add with index payload
        lv_obj_add_event_cb(p, file_tap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if (n > 4)
        mk_lbl("(only the first 4 files are shown)", 48, 110 + 4 * 66,
               &lv_font_montserrat_14, MUT_COL);
    mk_pill("BACK", 610, 404, 140, close_cb);
}

// ---- QR source: wallet_scan drives the camera; we get the assembled PSBT ----
static void scan_done_cb(const uint8_t *psbt, size_t len, int fmt)
{
    s_src = SRC_QR;
    s_qr_fmt = fmt;
    snprintf(s_cur, sizeof s_cur, "scanned transaction");
    if (len > sizeof s_in) len = sizeof s_in;             // QRT_MAX_PSBT == sizeof s_in
    memcpy(s_in, psbt, len);
    int lrc = wallet_psbt_load(s_in, len, &s_sum);
    if (lrc != 0) {
        mk_screen(s_parent, "SIGN", s_cur);
        mk_lbl("the scanned data is not a valid PSBT", 48, 140,
               &lv_font_montserrat_14, STOP_COL);
        mk_pill("BACK", 48, 404, 140, close_cb);
        return;
    }
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
    lv_label_set_text(t, "YOUR COORDINATOR WALLET");
    lv_obj_set_style_text_color(t, INK_COL, 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_letter_space(t, 2, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 92);

    lv_obj_t *b = lv_label_create(ovl);
    lv_label_set_text(b,
        "the wallet app on your computer or phone - Sparrow Wallet,\n"
        "for example. it watches your balance and prepares each\n"
        "transaction, but it cannot spend on its own.\n\n"
        "it shows the transaction as a QR code (often a moving one -\n"
        "that's fine, hold steady and every frame gets read). this\n"
        "device signs it, then shows a QR to scan back into the app.");
    lv_obj_set_style_text_color(b, MUT_COL, 0);
    lv_obj_set_style_text_font(b, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 156);

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
    lv_obj_add_event_cb(ok, coord_ok_cb, LV_EVENT_CLICKED, ovl);
    lv_obj_t *ol = lv_label_create(ok);
    lv_label_set_text(ol, "OK");
    lv_obj_set_style_text_color(ol, INK_COL, 0);
    lv_obj_set_style_text_font(ol, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(ol, 2, 0);
    lv_obj_center(ol);
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
    mk_screen(parent, "SIGN", "get the transaction from your coordinator");
    lv_obj_t *q = mk_pill("SCAN QR", 48, 150, 340, scan_pick_cb);
    wt_pill_primary(q);                                   // QR primary, SD fallback (spec)
    mk_pill("FROM SD CARD", 48, 230, 340, sd_pick_cb);
    mk_lbl("point the camera at the QR your\ncoordinator wallet shows", 430, 152,
           &lv_font_montserrat_14, MUT_COL);
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
    lv_obj_set_style_text_font(hl, &lv_font_montserrat_14, 0);
    lv_obj_center(hl);
    mk_lbl("or load a .psbt file saved on a card", 430, 244,
           &lv_font_montserrat_14, MUT_COL);
    mk_pill("BACK", 610, 404, 140, close_cb);
}
