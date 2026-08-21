// Steps 5+6: Sign. Get a PSBT by QR scan (static / pMofN / animated BC-UR) or
// from the SD card, show the spec's verify screen (status light, every output
// with change re-derived ON THIS DEVICE, fee three ways, network/RBF/locktime),
// then hold-to-sign. Signed PSBT goes back the way it came: SD file in ->
// <name>-signed.psbt on the card; QR in -> animated QR out.
// Compiled in BOTH device and sim builds; the sim stubs kiss_psbt_* in sim_main.c.
#include "kiss_sign.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "i18n.h"
#include "platform_sd.h"
#include "qr_transport.h"
#include <ctype.h>

#include "kiss_crypto.h"
#include "kiss_psbt.h"
#include "kiss_scan.h"
#include "kiss_theme.h"
#include "kiss_settings.h"   // the unit preference, written where it is changed
#include "kiss_ui.h"   // kiss_ui_last_fp: the SIGNING AS fingerprint
#include "kiss_usage.h"   // reuse guard: mark receive indexes used on sign
#include "kiss_payee.h"   // ...and the destinations this wallet has paid

#define BG_COL   WT_BG
#define INK_COL  WT_INK
#define MUT_COL  WT_MUT
#define KEY_COL  WT_KEY
#define OK_COL   WT_OK    // status light: label + shape + color,
#define WARN_COL WT_WARN  // never color alone (spec)
#define STOP_COL WT_STOP

// Why this file logs at all: a refused PSBT used to say one translated
// sentence on the glass and nothing anywhere else. kiss_psbt.c is
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
// 24, up from 8: signing writes a -signed sibling next to every file, so a card
// with a handful of transactions crossed 8 in one session. The list scrolls, so
// the cap is only the buffer bound (24 x 64 = 1.5 KB static), and past it the
// screen now says so instead of showing a shorter card than the one in the slot.
#define MAX_FILES 24
// Set from the card each time the list is built: s_sig[i] means s_files[i]
// already has a signature beside it, s_nsig is how many signed outputs the whole
// card holds (which is what REMOVE SIGNED is gated on and counts down).
static uint8_t s_sig[MAX_FILES];
static int     s_nsig;
static bool    s_cur_signed;      // ... for the one the owner then opened
#define SHOW_OUTS 3

enum { SRC_SD = 0, SRC_QR = 1 };

static lv_obj_t *s_scr;
static lv_obj_t *s_sign_lbl;
// The graph, its left caption, and the eyebrow that appears over the output
// side while the key is working. Held so the signing state can reach them
// without rebuilding the screen: a repaint here would tear down the arc
// mid-sweep and restart the hold the owner is in the middle of.
static lv_obj_t *s_graph, *s_graph_cap, *s_locked;
static int s_coins_chip_x = 24;   // measured off the caption; see verify_screen
// What the caption says at rest. It is a formatted count, so a hold that is let
// go has to put back a string rather than a key -- and the buffer it was built
// in is a local that went out of scope the moment the screen was drawn.
//
// Sized to that local, not to what English needs. At 64 the device compiler
// refused it outright: S_BUNDLE_IN_FMT runs to 160 bytes formatted, Cyrillic
// and CJK are 2 and 3 bytes a character, and the copy below is a byte copy --
// so a caption too long would have come back from an abandoned hold cut mid
// codepoint. The desktop build never said a word.
static char s_graph_cap_rest[160];
// The sweep under HOLD TO SIGN's label, filled left to right on the same
// fraction as the ring.
static lv_obj_t *s_sweep;
// DETAILS and BACK, NULL terminated, so the signing state can stand them down
// without knowing what else is on the row.
static lv_obj_t *s_inert[3];
static lv_timer_t *s_hold_tmr;
static uint32_t s_hold_t0;
static char s_files[MAX_FILES][SD_NAME_LEN];
static char s_cur[SD_NAME_LEN];
static wpsbt_summary_t s_sum;
static bool s_ack;                      // every caution acknowledged? (gates hold-to-sign)
static uint16_t s_ack_flags;            // WHICH ones, so each row answers for itself
static uint32_t s_ack_t0;               // when, for SIGN_ARM_MS below
// Has the recipient list been read to its end? Only ever consulted when the
// list actually has something below the fold; set true immediately when it
// does not, so the common single-recipient transaction gates on nothing.
static bool s_recip_seen;
// Is the single recipient's address shown whole? Folded by default; the
// toggle under it opens the rest. Per PSBT, not per session -- it resets
// wherever s_recip_seen resets, so a new file is always met folded.
#ifndef ESP_PLATFORM
// Walk only. The difference between an armed HOLD TO SIGN and an inert one is
// a colour, and the walk cannot read a colour -- so without this the gate
// below could be deleted and every frame would still compare equal.
static bool s_armed;
bool kiss_sign_test_armed(void) { return s_armed; }
// Also walk only. An abandoned hold has to put the screen back, and most of
// what it puts back is colour: strands to WT_MUT, output rows up, the dot to
// its resting radius. A frame comparison cannot see any of it. The padlock is
// the one piece that is an object rather than a shade, so it stands in for the
// rest -- if the retract ever stops running, this is what says so.
bool kiss_sign_test_locked(void) { return s_locked != NULL; }
#endif
static uint8_t s_in[4096], s_out[4680];
static lv_obj_t *s_parent;             // where this flow's screens are built
static int s_src;                      // SRC_SD / SRC_QR: where the PSBT came from
static int s_qr_fmt;                   // QRT_FMT_* the scan arrived in
static qrt_encoder_t *s_qenc;          // QR-out encoder (animated signed PSBT)
static lv_timer_t *s_qr_tmr;
static lv_obj_t *s_qr_img, *s_part_lbl;
static int s_part_i;
static bool s_qr_ez;                   // easy-scan mode: sparser QRs, slower loop
static size_t s_out_len;               // signed PSBT length (easy-scan re-encodes)
static lv_obj_t *s_ez_pill;
static char s_sig_fp[9];               // fingerprint of the just-signed PSBT (8 hex)
static char s_done_name[SD_NAME_LEN + 8]; // saved outname, so the ? panel can rebuild

static void qr_out_screen(size_t sw);
static size_t s_qr_sw;            // signed length, kept so help can rebuild the QR screen
static bool s_help_from_qr;       // which signed screen the SIGNATURE panel returns to

bool kiss_sign_active(void) { return s_scr != NULL; }

// The figure the verify screen prints largest, so DETAILS can show the other
// unit of the SAME number. One function and two call sites, because the two
// used to be one expression each and the day the hero changed only one of them
// changed with it: the screen said 60 000 and the page one tap behind it
// offered 0.00061000 BTC as "the same total", which is a coordinator check
// that fails for a reason the reader cannot see.
//
// One recipient and it is what that recipient gets. None or several and it is
// the sum of what leaves, because there is then no single send amount to be
// the headline. See the hero in verify_screen for why that is the split.
static uint64_t hero_sats(void)
{
    int recip = 0;
    for (int i = 0; i < (int)s_sum.n_out && i < WPSBT_MAX_OUTS; i++)
        if (!s_sum.outs[i].is_change) recip++;
    return recip == 1 ? s_sum.send_sats : s_sum.send_sats + s_sum.fee_sats;
}

// Is this name one of our own signed outputs?
static bool is_signed_name(const char *nm)
{
    size_t n = strlen(nm);
    return n >= 12 && strcasecmp(nm + n - 12, "-signed.psbt") == 0;
}

// The name a signature for `src` gets written under, into s_done_name -- which
// is also the buffer the "?" panel rebuilds the SIGNED screen from, so filling
// it here is what stops that screen showing a blank filename.
//
// Two things this gets right that the open-coded version did not:
//
//  - It CLAMPS. The old code appended unconditionally, so a 57 character source
//    produced a 64 character name, name_ok refused it, and platform_sd_write
//    returned -3 -- discarding a signature that already existed in RAM, behind a
//    generic write error. A truncated name is recoverable; a lost signature is
//    a second hold-to-sign at best.
//  - It does not re-append. Signing an already-signed file used to make
//    "x-signed-signed.psbt", growing seven characters a round toward that same
//    cliff. Re-signing is deterministic, so writing the same name back is not a
//    loss: it is the same bytes.
static const char *signed_name(const char *src)
{
    if (is_signed_name(src)) {
        snprintf(s_done_name, sizeof s_done_name, "%s", src);
        return s_done_name;
    }
    size_t bl = strlen(src);
    if (bl > 5) bl -= 5;                                  // strip ".psbt"
    // "-signed.psbt" is 12 bytes, and name_ok wants the whole thing under
    // SD_NAME_LEN including its NUL.
    size_t room = SD_NAME_LEN - 1 - 12;
    if (bl > room) bl = room;
    snprintf(s_done_name, sizeof s_done_name, "%.*s-signed.psbt", (int)bl, src);
    return s_done_name;
}

static void hold_stop(void)
{
    if (s_hold_tmr) { lv_timer_delete(s_hold_tmr); s_hold_tmr = NULL; }
}

// Every pointer into the screen about to go, and the timers that would call
// back into it. One function and not two copies, because the two copies had
// drifted: both nulled the QR group and neither nulled the graph group, which
// three other drop sites in this file do null.
//
// Every read of the graph group is guarded by `if (x)`, so a stale non-NULL is
// the one state that turns a guard into a dereference. Nothing reaches them
// after a drop today -- their readers are callbacks on the screen being
// deleted. That was true of Settings' pane too, right up to the day a function
// reachable from another screen read it. kiss_ui's login_teardown says the
// same thing from the other side, in a comment about the teardown that
// dereferenced a freed entry label because an earlier one nulled only the root.
static void widgets_drop(void)
{
    hold_stop();
    s_sign_lbl = NULL;
    s_graph = NULL; s_graph_cap = NULL; s_locked = NULL;
    s_inert[0] = NULL; s_sweep = NULL;
    if (s_qr_tmr) { lv_timer_delete(s_qr_tmr); s_qr_tmr = NULL; }
    if (s_qenc) { qrt_encoder_free(s_qenc); s_qenc = NULL; }
    s_qr_img = NULL; s_part_lbl = NULL; s_ez_pill = NULL;
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    widgets_drop();
    kiss_psbt_free();
    platform_sd_unmount();
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
}

void kiss_sign_close(void) { close_cb(NULL); }   // idle auto-lock path

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
    widgets_drop();
    kiss_psbt_free();               // the next pick loads its own
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
    kiss_sign_open(parent);
}

// ---- shared bits: thin wrappers over the kiss_theme kit (module keeps
// its s_scr; call sites keep their historical signatures) ----
// This module owns exactly ONE screen at a time. Overwriting s_scr without
// deleting what it pointed at does not close that screen, it orphans it: the
// old one stays parented to s_parent, underneath the new one, and the only way
// to find out is to press BACK enough times to peel the top one off and see a
// transaction you already left.
//
// Every call site clears s_scr itself rather than leaning on the recovery
// below, because clearing it is never just the delete: the hold timer ticks
// against s_sweep and the graph, and those and s_sign_lbl all point into the
// outgoing screen. mk_screen() cannot see any of that, so a call site that let
// it do the tidying would leave a timer widening a freed sweep. The branch
// below is a net, not a mechanism.
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

// The nth TERM out of S_GLOSSARY_B, which every locale already writes one
// `TERM: definition` per line -- the same string the explainers page reads and
// the same split, wt_split_colon, that reads it there.
//
// It exists so the graph can label its change strand with the word the glossary
// teaches, in 21 locales, without a twenty-second key. S_CHANGE_TAG was the
// obvious candidate and is the wrong length: "change back to you, verified
// here" is a panel's caption, and the graph row has ~200px beside a mono23
// amount. The claim that clause makes belongs to the panel's WT_OK border,
// which the graph replaces with the accent on the strand itself.
//
// Order is GLOSS_ICONS' order: 0 inputs, 1 outputs, 2 change, 3 txid,
// 4 fee rate, 5 locktime, 6 derivation path, 7 descriptor.
//
// The table itself is up here now, beside the words it marks, because the
// graph rows use it too: a mark on every row is what tells the fee from the
// send without asking the reader to compare two shades of white, and the mark
// it uses has to be the one the glossary teaches for that word rather than a
// second one invented for this screen. It used to sit beside the DETAILS
// glossary page, 2200 lines down, which is the only reader it had.
//
// Every glyph is already baked into the Latin faces (tools/fonts/gen_fonts.sh),
// so this costs no font work and no flash. Chosen to mean the thing without the
// word: coins arriving, coins leaving, the part that comes back, a marker for
// finding it later, scissors for what you pay for speed, a lock for the
// earliest it may confirm, a folder for a path, and an open eye for the map
// that can watch but not spend.
static const char *const GLOSS_ICONS[] = {
    LV_SYMBOL_DOWNLOAD,     // INPUTS
    LV_SYMBOL_UPLOAD,       // OUTPUTS
    LV_SYMBOL_LOOP,         // CHANGE
    LV_SYMBOL_GPS,          // TXID
    LV_SYMBOL_CUT,          // FEE RATE
    WT_ICON_LOCK,           // LOCKTIME
    LV_SYMBOL_DIRECTORY,    // DERIVATION PATH
    LV_SYMBOL_EYE_OPEN,     // DESCRIPTOR
};

static const char *gloss_line(int idx, char *head, size_t head_len)
{
    const char *p = tr(STR_S_GLOSSARY_B);
    for (int i = 0; i < idx && p; i++) {
        p = strchr(p, '\n');
        if (p) p++;
    }
    if (!p || !*p) { if (head_len) head[0] = 0; return ""; }
    // One line at a time into a buffer this call owns, because wt_split_colon
    // writes the term into `head` and hands back a pointer INTO the line it was
    // given -- a pointer into the translation table would be to a string with
    // the next seven terms still attached.
    static char line[160];
    const char *nl = strchr(p, '\n');
    size_t n = nl ? (size_t)(nl - p) : strlen(p);
    if (n >= sizeof line) n = sizeof line - 1;
    memcpy(line, p, n);
    line[n] = 0;
    const char *def = wt_split_colon(line, head, head_len);
    return def ? def : "";
}

// Just the term, for a caller that only needs the word.
static const char *gloss_term(int idx)
{
    static char head[64];
    gloss_line(idx, head, sizeof head);
    return head;
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

// kiss_psbt.c stays LVGL/i18n-free (it feeds the desktop test runner and the
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
        // Short, like every other verdict here. The sentence that says what to
        // do about it is a body, not a headline: see stop_body().
        {"input amounts not proven", STR_S_C_UNPROVEN},
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

// What the owner can DO about a refusal, when there is anything. Almost never:
// a malformed PSBT, a coin that is not this wallet's, a sighash this signer
// does not sign -- none of those is fixable from the device, and inventing an
// instruction for them would be worse than the silence.
//
// Exactly one refusal is different. A coordinator that stripped the previous
// transactions can be told to put them back, it costs it nothing, and it is the
// only STOP an honest one can trip. So this is a lookup rather than a second
// field on all thirty reasons -- the asymmetry is real and worth showing.
static const char *stop_body(const char *r)
{
    if (strcmp(r, "input amounts not proven") == 0) return tr(STR_S_WHY_UNPROVEN);
    return NULL;
}

// same grouped-by-4 convention as the Receive screen: visual compare against
// the coordinator is the whole point of this screen
#define group4   wt_group4
#define fmt_sats wt_fmt_sats

// ---- signing ----
// This screen's lane is 24..776, not the page's 48..752, because its panels are
// drawn at sg_panel(24, y, 752). So its corner is 776 and its exits are pinned
// against that, not against WT_BACK_X.
#define SG_BACK_X    672   // 672..776, the 104px exit on the verify row
#define SG_BACK_X140 636   // 636..776, the standard 140px exit
#define SG_DETAILS_X 366   // 366..516
#define SG_HOLD_X     48   // 48..358, off the corner: it signs the transaction
#define SG_HOLD_W    310   // named because the sweep across it is measured in it


// The ? explainer: what the SIGNATURE code is for. Same pattern as the entropy
// screen's WHY THREE SOURCES. BACK rebuilds the SD signed screen from the saved
// outname (the file is written; nothing is re-signed).
static void done_screen(const char *outname);
static void sig_help_back_cb(lv_event_t *e)
{
    (void)e;
    if (s_help_from_qr) qr_out_screen(s_qr_sw);
    else                done_screen(s_done_name);
}

// A code chip for the comparison rows below: wt_chip with its label in mono.
// Mono because the whole point of the code is digit for digit comparison, and
// INK because the code is the subject, not the furniture around it.
static lv_obj_t *sig_code_chip(lv_obj_t *row, const char *code)
{
    lv_obj_t *c = wt_chip(row, code, false);
    lv_obj_t *l = lv_obj_get_child(c, 0);
    lv_obj_set_style_text_font(l, wt_font_mono14(), 0);
    lv_obj_set_style_text_color(l, INK_COL, 0);
    return c;
}

static void sig_fp_help_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *parent = lv_obj_get_parent(s_scr);
    lv_obj_delete(s_scr); s_scr = NULL; s_sign_lbl = NULL;
    s_graph = NULL; s_graph_cap = NULL; s_locked = NULL;
    s_inert[0] = NULL; s_sweep = NULL;
    mk_screen(parent, tr(STR_S_SIG_FP_HELP_T), NULL);

    // This device's own code first, real and big: the signed screens no
    // longer carry it on their main surface (it is a check for the cautious,
    // not a step for everyone), so this panel is where it lives now.
    if (s_sig_fp[0]) {
        char code[12];
        snprintf(code, sizeof code, "%c%c%c%c %c%c%c%c",
                 toupper((unsigned char)s_sig_fp[0]), toupper((unsigned char)s_sig_fp[1]),
                 toupper((unsigned char)s_sig_fp[2]), toupper((unsigned char)s_sig_fp[3]),
                 toupper((unsigned char)s_sig_fp[4]), toupper((unsigned char)s_sig_fp[5]),
                 toupper((unsigned char)s_sig_fp[6]), toupper((unsigned char)s_sig_fp[7]));
        lv_obj_t *own = mk_lbl(code, 0, 74, wt_font_mono23(), INK_COL);
        lv_obj_update_layout(own);
        lv_obj_set_x(own, (800 - lv_obj_get_width(own)) / 2);
    }

    // The answer, drawn before it is said: the same transaction signed on two
    // signers either shows one code twice, or it does not. Two rows, two
    // verdicts, no vocabulary -- a reader who cannot parse the claims below
    // yet still leaves knowing what to compare and what a mismatch looks like.
    // The codes are invented, and deliberately not this device's own format
    // example from the signed screen: these chips are two DIFFERENT devices.
    lv_obj_t *r1 = wt_diagram_row(s_scr);
    sig_code_chip(r1, "3F00 C01D");
    wt_diagram_op(r1, "=");
    sig_code_chip(r1, "3F00 C01D");
    lv_obj_t *ok = wt_diagram_op(r1, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(ok, OK_COL, 0);
    lv_obj_set_style_text_font(ok, wt_font23(), 0);   // the verdict is the payload
    lv_obj_align(r1, LV_ALIGN_TOP_MID, 0, 116);

    lv_obj_t *r2 = wt_diagram_row(s_scr);
    sig_code_chip(r2, "3F00 C01D");
    wt_diagram_op(r2, LV_SYMBOL_CLOSE);
    sig_code_chip(r2, "8A41 77E2");
    lv_obj_t *warn = wt_diagram_op(r2, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(warn, WARN_COL, 0);
    lv_obj_set_style_text_font(warn, wt_font23(), 0);
    lv_obj_align(r2, LV_ALIGN_TOP_MID, 0, 162);

    // Ruled claims under the picture: wt_why_body splits the answer the copy
    // was already written in without asking for a new string in twenty one
    // locales. Started below the rows, so the body takes whatever rung fits
    // the room the diagram left it.
    wt_why_body(s_scr, tr(STR_S_SIG_FP_HELP_B), 216, wt_accent(), true);
    mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, sig_help_back_cb);
}


// The compact form the signed screens carry now: caption + "?" chip only.
// The code itself moved into the panel the chip opens -- it is a check for
// the cautious, not a step in everyone's flow, and at full size it was the
// loudest thing on a screen whose real message is "take the card back".
static void sig_fp_open_cb(lv_event_t *e)
{
    s_help_from_qr = lv_event_get_user_data(e) != NULL;
    sig_fp_help_cb(NULL);
}

static void draw_sig_chip(int x, int y, bool from_qr)
{
    if (!s_sig_fp[0]) return;
    // Chip FIRST, at a fixed x, caption trailing: the caption is translated
    // and grows rightward harmlessly, while the one tappable thing on the
    // pair sits at the same spot in every locale -- which is also what lets
    // the walk tap it without measuring text.
    wt_help_chip(s_scr, x, y, MUT_COL, sig_fp_open_cb,
                 from_qr ? (void *)1 : NULL);
    mk_lbl(tr(STR_S_SIG_FP_CAP), x + 40, y + 5, wt_font14(), MUT_COL);
}

// The receipt: what left, what it cost, and where it went, in one card.
//
// Three rows, each a mark and a fact, and every one of them a string that was
// already on the verify screen wearing the same glyph -- RECIPIENT GETS with
// the download arrow, NETWORK FEE with the scissors, the destination with the
// GPS pin the DETAILS output rows use. Nothing new is taught here; the point is
// that the page you signed and the page that says you signed it agree.
//
// Muted, not accent. The tick below is the accent on this screen and it is the
// claim being made; a card of facts competing with it in the same colour would
// make the answer harder to find, which is ADDENDUM-02 rule 3.
static void done_summary(int y)
{
    const int H = 104;
    lv_obj_t *c = wt_card(s_scr, 48, y, 704, H);

    // Recipient and fee side by side, because they are the two halves of the
    // number the hero on the verify screen showed as one.
    struct { const char *icon; int str; uint64_t sats; int x; } cells[2] = {
        { LV_SYMBOL_DOWNLOAD, STR_S_SENDING_CAP, s_sum.send_sats,  14 },
        { LV_SYMBOL_CUT,      STR_S_FEE,         s_sum.fee_sats,  366 },
    };
    for (int i = 0; i < 2; i++) {
        char a[40], b[64];
        wt_lbl(c, cells[i].icon, cells[i].x, 16, wt_font14(), MUT_COL);
        wt_lbl(c, tr(cells[i].str), cells[i].x + 26, 14, wt_font14(), MUT_COL);
        wt_fmt_amount(cells[i].sats, a, sizeof a);
        snprintf(b, sizeof b, "%s %s", a, wt_denom_unit());
        lv_obj_t *v = wt_lbl(c, b, cells[i].x, 38, wt_font28(), INK_COL);
        wt_denom_bind(v);
    }

    // The destination, folded exactly as the screen before this one folded it.
    // Change outputs are skipped for the same reason they are skipped there:
    // an address the device proved is its own is not where the money went.
    //
    // One recipient, or none -- never the first of several. RECIPIENT GETS
    // above carries the total across every destination, and this line used to
    // print the first address it found under it and stop. Two recipients and
    // the receipt stated, in the shape of a fact, that the whole amount went to
    // an address that got part of it; the owner keeps this page, or photographs
    // it, and nothing later contradicts it.
    //
    // The verify screen decided this already and the reason is written there:
    // above one recipient every strand carries its own address, because "a
    // single line below it could name the first and no other". There is no
    // graph on a 104px card, so the count takes the slot instead -- in the
    // string the DETAILS page already uses for exactly this fact, so the honest
    // version costs nothing in 21 locales.
    uint32_t n_recip = 0, n_ours = 0;
    int only = -1;
    for (int i = 0; i < (int)s_sum.n_out && i < WPSBT_MAX_OUTS; i++) {
        if (s_sum.outs[i].is_change) { n_ours++; continue; }
        if (!n_recip++) only = i;
    }
    if (n_recip == 1) {
        wt_lbl(c, LV_SYMBOL_GPS, 14, 76, wt_font14(), MUT_COL);
        lv_obj_t *ad = wt_addr_short(c, s_sum.outs[only].addr, wt_font_mono14());
        lv_obj_set_pos(ad, 40, 74);
    } else if (n_recip > 1) {
        char b[80];
        snprintf(b, sizeof b, tr(STR_S_D_OUTPUTS_FMT),
                 (unsigned)s_sum.n_out, (unsigned)n_ours);
        wt_lbl(c, LV_SYMBOL_LIST, 14, 76, wt_font14(), MUT_COL);
        lv_obj_t *l = wt_lbl(c, b, 40, 76, wt_font14(), MUT_COL);
        lv_obj_set_width(l, 640);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }
}

static void done_screen(const char *outname)
{
    lv_obj_t *parent = lv_obj_get_parent(s_scr);
    lv_obj_delete(s_scr); s_scr = NULL; s_sign_lbl = NULL;
    s_graph = NULL; s_graph_cap = NULL; s_locked = NULL;
    s_inert[0] = NULL; s_sweep = NULL;
    // Two lines, drawn here rather than by wt_screen: "return this card to
    // Sparrow, load the -signed.psbt file, then broadcast" is the whole point
    // of the screen and does not fit one line at a readable size. Nothing is
    // above the checkmark at y=150, so the second line costs nothing.
    mk_screen(parent, tr(STR_S_SIGNED_T), NULL);
    wt_note(s_scr, tr(STR_S_DONE_SD_SUB), 48, 66, 704, 48);

    // ---- what was signed ------------------------------------------------
    //
    // This screen was a tick, a filename and 110px of nothing, on the one page
    // an owner might photograph or read back to somebody. It says what left the
    // keys and where it went, in the same three marks the verify screen used
    // for the same three facts -- so it reads as the receipt for the page
    // before it rather than as a new screen with new words.
    //
    // Every string here already shipped. The amounts are wt_denom_bind, so a
    // tap still flips the whole device between sats and BTC on this screen too.
    // The address is the same fold RECEIVE and the verify screen draw.
    done_summary(120);

    lv_obj_t *big = mk_lbl(LV_SYMBOL_OK, 0, 236, &lv_font_montserrat_48, OK_COL);
    lv_obj_align(big, LV_ALIGN_TOP_MID, 0, 236);
    // The padlock comes with it. It went up over the output column the moment
    // the finger went down, meaning "the destinations are settled", and then
    // left with the screen at the one moment that was most true. Two marks,
    // two claims: the lock says where this can go is fixed, the tick says a
    // signature now exists over it. Beside rather than under, at the smaller
    // rung, because the tick is the answer and this is the condition it was
    // reached under.
    lv_obj_t *lk = mk_lbl(WT_ICON_LOCK, 0, 248, wt_font28(), MUT_COL);
    lv_obj_align_to(lk, big, LV_ALIGN_OUT_LEFT_MID, -18, 0);
    // Bounded, because signed_name clamps to 63 bytes and nothing here did.
    // At font28 that is roughly 900px of text laid out on an 800px panel with
    // no width and no long mode set, so a long name ran off BOTH edges of the
    // one screen an owner reads a filename back to a coordinator from -- and
    // took its own first and last characters with it, which are the two an
    // eye actually uses to match a name against a card.
    //
    // DOT, not a smaller font. wt_note_fit and its siblings would have shrunk
    // this to font14 and reported nothing, which is the failure mode the house
    // rules name: a fit helper landing on font14 means the string is too long
    // for the space, and a filename is not copy that can be cut.
    lv_obj_t *fn = mk_lbl(outname, 48, 300, wt_font28(), INK_COL);
    // Width AND height. DOT on a content-sized label wraps first and dots only
    // once it runs out of lines, so bounding the width alone turned the name
    // into two centred lines that ran straight through the SIGNATURE chip 36px
    // below -- a different way of being unreadable, and one that also took the
    // chip with it. One line is the whole budget here.
    lv_obj_set_size(fn, 704, lv_font_get_line_height(wt_font28()));
    lv_label_set_long_mode(fn, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(fn, LV_TEXT_ALIGN_CENTER, 0);
    // The signature fingerprint, centred under the filename, with its ? panel.
    draw_sig_chip(296, 336, false);
    // S_SAVED_NOTE went with the space it was filling. "saved to the card" sat
    // under a filename ending in .psbt, on a screen whose subtitle already says
    // to take the card back -- the copy rule about restating a value sitting
    // next to it, three times over.
    // No drift-home timer. This screen used to leave on its own after 6s,
    // which read as a crash mid-test and stole a filename the owner was
    // meant to read back to the coordinator. DONE is the only exit; the idle
    // auto-lock still covers a walk-away (the registered close unmounts).
    mk_pill(tr(STR_C_DONE), SG_BACK_X140, WT_ACTION_Y, 140, close_cb);
}

// Every refusal on this screen answers the same second question, and it is the
// one the owner is actually asking: did anything get out. Nothing did on any of
// the three paths -- the signing call returned nothing, or the atomic write
// kept the file that was already there, or the encoder never produced a frame
// -- so the claim is true wherever it is shown, and the screen becomes the
// split pair the rest of the flow uses instead of one line under a title.
static void fail_body(const char *why)
{
    // 174 is the cap a translated string is generated under, twice over plus
    // the blank line. The caption buffer that shipped at 64 was caught by the
    // compiler, not by a screen: a short buffer here would cut a translation
    // mid codepoint at exactly the moment the owner needs to read it.
    char body[384];
    snprintf(body, sizeof body, "%s\n\n%s", why, tr(STR_S_FAIL_SAFE_B));
    wt_why_body(s_scr, body, 136, STOP_COL, true);
}

static void fail_screen(const char *why)
{
    lv_obj_t *parent = lv_obj_get_parent(s_scr);
    lv_obj_delete(s_scr); s_scr = NULL; s_sign_lbl = NULL;
    s_graph = NULL; s_graph_cap = NULL; s_locked = NULL;
    s_inert[0] = NULL; s_sweep = NULL;
    mk_screen(parent, tr(STR_S_FAIL_T), NULL);
    fail_body(why);
    mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, close_cb);
}

// Spending from receive index N proves N was used: record it so the Receive
// screen hands out a fresh address next time (reuse guard). Best effort: only
// the inputs KISS could show; the coordinator remains the source of truth.
static void mark_used_receives(void)
{
    wpsbt_details_t det;
    if (kiss_psbt_details(&det) != 0)
        return;
    uint8_t fp[4];
    kiss_ui_last_fp(fp);
    kiss_usage_batch_begin();            // one NVS commit per spend, not per input
    for (uint32_t i = 0; i < det.n_in; i++)
        if (det.ins[i].change == 0) {                // 0 = receive branch (1 = change)
            // key by THIS input's own type, not the current Settings type: we
            // sign native/nested/legacy regardless of the setting, and Receive
            // buckets the guard per type, so a mismatch would mark the wrong one
            int sc = det.ins[i].purpose == 44 ? WSCRIPT_LEGACY
                   : det.ins[i].purpose == 49 ? WSCRIPT_NESTED : WSCRIPT_NATIVE;
            kiss_usage_mark(fp, s_sum.testnet ? 1 : 0, sc, det.ins[i].index);
        }
    kiss_usage_batch_end();
}

// Every destination this signature actually pays, recorded so the next spend to
// the same payee can say so. AFTER the signature exists, never at load: a
// transaction the owner read and walked away from is not a payment, and a
// refused one is not either.
//
// Change is skipped. It is ours by re-derivation, the screen already labels it,
// and a change address the owner sees twice is a reuse problem rather than a
// payee they know.
static void mark_paid_recipients(void)
{
    kiss_payee_batch_begin();            // one NVS commit per spend, not per payee
    for (int i = 0; i < (int)s_sum.n_out && i < WPSBT_MAX_OUTS; i++)
        if (!s_sum.outs[i].is_change)
            kiss_payee_mark(s_sum.outs[i].addr);
    kiss_payee_batch_end();
}

// How long the reveal is on the glass before the exit screen replaces it.
//
// The graph spends the whole flow claiming that a strand in the accent means a
// signature exists, and this is the one moment that claim is discharged. It is
// worth a beat. Not a fake progress bar and not a pause pretending work is
// still happening -- the work is done, and this is the answer being shown for
// long enough to read before the screen changes underneath it.
//
// 700 was not long enough to be that. libwally returns in a few milliseconds,
// so SIGNING was never legible and the reward for holding a button for 1200ms
// was the bar disappearing, followed by the screen. 1600 with the crossing
// below inside it is about a second of settled answer -- reported from the
// bench as "it happens too fast after the hold bar is full", which is the only
// instrument that can measure this.
#define REVEAL_MS 1600
// The signature crossing the input strands. Inside REVEAL_MS, not added to it.
#define REVEAL_TRAVEL_MS 520
// The address card under the graph. One mono23 line plus the compare caption,
// centred as a block: 29 + 6 + 18 is 53, and 66 gives it the same breathing
// room RECEIVE's 114 gives two lines of the same type.
// 74, not 66: the address inside it is mono28 now (see verify_screen), and a
// 34px line, 6 of air and a 17px caption is 57 -- centred in 66 that left 4px
// top and bottom, which reads as a line jammed into a box. The card starts at
// 316 and WT_CONTENT_BOTTOM is 398, so the extra 8 is room the screen had.
#define ADDR_CARD_H 74
// The bar holding full while its fill crosses from the stop red to the accent.
// The sweep measured a finger and there is no longer a finger to measure, but
// snapping it to zero at the instant it fills takes the answer away in the
// frame it was earned.
#define SWEEP_SETTLE_MS 240

static size_t s_signed_len;

static void finish_sign_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    const size_t sw = s_signed_len;
    if (s_src == SRC_QR) {                       // came by QR: goes back by QR
        s_qr_sw = sw;
        qr_out_screen(sw);
        return;
    }
    const char *outname = signed_name(s_cur);
    // Atomic, like the seed and the proof. This overwrites when the file is
    // already there, and a plain fopen("wb") truncates on open -- so a card
    // pulled mid-write replaced a good signature with a short one. The atomic
    // form keeps the old file until the new one is written and verified.
    int rc = platform_sd_write_atomic(outname, s_out, sw);
    if (rc < 0) {
        fail_screen(tr(STR_S_FAIL_SD_WRITE));
        return;
    }
    done_screen(outname);
}

static void do_sign_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    size_t sw = 0;
    if (kiss_psbt_sign(s_out, sizeof s_out, &sw) != 0) {
        // Nothing on the graph moved: no strand ever wore the accent, because
        // no signature was ever made.
        fail_screen(tr(STR_S_FAIL_SIGN));
        return;
    }
    // Fingerprint the signature now, while the bytes are in hand, for both exit
    // screens. On the (unexpected) failure path it is left empty and the screens
    // just omit the aid.
    if (kiss_psbt_sig_fingerprint(s_out, sw, s_sig_fp) != 0)
        s_sig_fp[0] = 0;
    mark_used_receives();
    mark_paid_recipients();
    // Every input at once, which is what actually happened: one libwally call
    // signed all of them and there was never a per coin moment to show.
    if (s_graph) wt_bundle_signed_reveal(s_graph, REVEAL_TRAVEL_MS);
    if (s_graph_cap) {
        // "ALL 1 COINS SIGNED" is what the count format says about a one coin
        // spend, and it is wrong in English before it is wrong anywhere else.
        // A single coin gets the word on its own. The plural form still cannot
        // be right in Russian, Polish or Czech, which need three -- this file
        // has no plural machinery and the rest of it dodges the problem the
        // same way, by parenthesising the count or not printing one.
        static char done_buf[64];
        if (s_sum.n_in == 1) {
            snprintf(done_buf, sizeof done_buf, "%s", tr(STR_S_SIGNED_T));
        } else {
            snprintf(done_buf, sizeof done_buf, tr(STR_S_ALL_SIGNED_FMT),
                     (unsigned)s_sum.n_in);
        }
        lv_label_set_text(s_graph_cap, done_buf);
        lv_obj_set_style_text_color(s_graph_cap, wt_accent(), 0);
        lv_obj_add_flag(s_graph_cap, WT_FLAG_ACCENT);
    }
    if (s_sign_lbl) lv_label_set_text(s_sign_lbl, tr(STR_S_SIGNED_T));
    s_signed_len = sw;
    lv_timer_create(finish_sign_cb, REVEAL_MS, NULL);
}

// The output side settles the moment the owner commits, which is the press and
// not the completion: where the money goes was decided on the screen behind
// this one, and holding the button is not a decision about it. So the strands
// stand down and the padlock goes up under the finger, and the coins on the
// left become the subject for as long as the hold lasts.
//
// The caption does NOT move here, and that is the whole of the difference
// between this and the state below. "SIGNING" while the finger is still
// deciding is a screen claiming a signature that does not exist, which is the
// one sentence a signer may never print. The padlock is the honest press time
// mark: destinations settled, nothing signed.
static void lock_opa_exec(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void sign_lock_outputs(void)
{
    if (s_graph) wt_bundle_state(s_graph, WT_BUNDLE_HOLDING);
    if (!s_locked && s_scr) {
        // The mark alone. "LOCKED" has no key, and the word it would borrow is
        // SIGNED -- which the caption on the left and the pill below already
        // say, so spelling it here would put the same word on one screen three
        // times. A padlock over the output column says the destinations are
        // settled, in the same glyph the RBF cell uses for a transaction that
        // can no longer be replaced, and it needs no translating.
        // On the caption's own line and at its rung, not above it: the output
        // column's first row begins at y=170, and a larger glyph centred on
        // this band reaches into it.
        s_locked = mk_lbl(WT_ICON_LOCK, 748, 150, wt_font14(), wt_accent());
        lv_obj_add_flag(s_locked, WT_FLAG_ACCENT);
        // It LANDS rather than appearing. A mark that says "the destinations are
        // settled" arriving between two frames is indistinguishable from a mark
        // that was always there, and this one is the only feedback the output
        // half gives for a press that has not finished yet.
        lv_obj_set_style_opa(s_locked, LV_OPA_TRANSP, 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_locked);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&a, 180);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, lock_opa_exec);
        lv_anim_start(&a);
    }
}

// Let go early and all of it retracts. This is the only place the flow says out
// loud that a hold can be abandoned, so it puts back everything the press
// moved rather than merely stopping the motion.
//
// It is deliberately NOT part of hold_stop(). hold_tick calls hold_stop the
// instant the hold completes, one line before it builds the signing state, so a
// retract living in there would undo the screen it is about to draw. hold_stop
// is also called from close_cb, from step_back and from three screen builders,
// where s_graph belongs to a screen already being torn down.
static void hold_abandon(void)
{
    hold_stop();
    if (s_sweep) lv_obj_set_width(s_sweep, 0);
    if (s_graph) {
        wt_bundle_hold(s_graph, 0);
        wt_bundle_state(s_graph, WT_BUNDLE_LIVE);
    }
    if (s_locked) { lv_obj_delete(s_locked); s_locked = NULL; }
    if (s_graph_cap && s_graph_cap_rest[0])
        lv_label_set_text(s_graph_cap, s_graph_cap_rest);
}

// The fill crossing from the hold's red into the pill's accent, then leaving.
// It is width 0 at the end either way, so an abandoned hold and a completed one
// both finish with the pill in its plain fill -- hold_abandon just gets there
// without the crossing, because nothing was accepted.
static void sweep_settle_exec(void *var, int32_t v)
{
    lv_obj_set_style_bg_color((lv_obj_t *)var,
                              lv_color_mix(wt_accent(), WT_STOP, (uint8_t)v), 0);
}

static void sweep_settle_done(lv_anim_t *a)
{
    lv_obj_set_width((lv_obj_t *)a->var, 0);
    lv_obj_set_style_bg_color((lv_obj_t *)a->var, WT_STOP, 0);   // ready to sweep again
}

static void hold_tick(lv_timer_t *t)
{
    (void)t;
    uint32_t el = lv_tick_elaps(s_hold_t0);
    if (el > HOLD_MS) el = HOLD_MS;
    // The graph and the sweep run on the same fraction as the ring, because
    // there is only one thing being measured: how long this finger has been
    // down. Three readings of one number, not three numbers.
    if (s_graph) wt_bundle_hold(s_graph, (uint8_t)(el * 255 / HOLD_MS));
    if (s_sweep) lv_obj_set_width(s_sweep, (int32_t)(el * SG_HOLD_W / HOLD_MS));
    if (el >= HOLD_MS) {
        hold_stop();
        // The sweep SETTLES rather than snapping to zero. It measured a finger
        // and there is no longer a finger to measure, and a bar sitting full
        // while libwally works would be read as a progress bar for the signing,
        // which is a thing nothing here can time -- so it does not sit. It holds
        // its full width for SWEEP_SETTLE_MS while its fill crosses from the
        // stop red to the accent the pill is already wearing, and then it is
        // gone into that fill rather than deleted out from under the finger.
        //
        // Taking it away in the frame it filled was the complaint from the
        // bench: the reward for holding the button for 1200ms was the bar
        // disappearing. The fill still means "a finger was down this long"; the
        // crossing is what says the measurement is finished and accepted.
        if (s_sweep) {
            lv_obj_set_style_bg_color(s_sweep, WT_STOP, 0);
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, s_sweep);
            lv_anim_set_values(&a, 0, 255);
            lv_anim_set_duration(&a, SWEEP_SETTLE_MS);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_set_exec_cb(&a, sweep_settle_exec);
            lv_anim_set_completed_cb(&a, sweep_settle_done);
            lv_anim_start(&a);
        }
        if (s_sign_lbl) lv_label_set_text(s_sign_lbl, tr(STR_S_SIGNING));
        // Now the caption may say it. The strands are landed, the button is
        // spent, and the next thing that happens on this thread is the call.
        // The accent they are wearing is the commitment; the amounts beside
        // them stay muted until there is a signature over them.
        //
        // The inputs come up to full strength underneath that accent, which is
        // the difference between HOLDING and SIGNING and is invisible while the
        // overlay covers them. It is what the strand falls back to if the
        // signature fails: WT_INK, a coin that was committed, not an accent
        // claiming one that was signed.
        sign_lock_outputs();
        if (s_graph) wt_bundle_state(s_graph, WT_BUNDLE_SIGNING);
        if (s_graph_cap)
            lv_label_set_text(s_graph_cap, tr(STR_S_SIGNING));
        // DETAILS and BACK go inert HERE and not on the press -- kiss_psbt_sign
        // blocks the LVGL loop, so a tap landing on either is a tap answered
        // after the signature exists, and a control that looks live while it
        // cannot respond is a control that lies. During the hold nothing
        // blocks, both are answered normally, and standing them down for 1.2s
        // to bring them back would be two controls flickering about nothing.
        for (int i = 0; s_inert[i]; i++) {
            lv_obj_set_style_border_color(s_inert[i], WT_EDGE, 0);
            lv_obj_set_style_text_color(lv_obj_get_child(s_inert[i], 0),
                                        WT_DIM, 0);
            lv_obj_remove_flag(s_inert[i], LV_OBJ_FLAG_CLICKABLE);
        }
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
        sign_lock_outputs();
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        // Only a hold still running can be abandoned. The finger also comes up
        // AFTER a completed hold -- kiss_psbt_sign holds the loop, so that
        // release is delivered on the far side of the signature -- and
        // retracting the graph there would erase a signed transaction's reveal.
        // The timer is what tells the two apart: completion deleted it.
        if (s_hold_tmr) hold_abandon();
        else            hold_stop();
    }
}

static void details_cb(lv_event_t *e);
static void verify_screen(lv_obj_t *parent);

// The full "why". Four reasons can fire at once, and stacked as prose they
// arrived as one grey block that had to be read start to finish to find the one
// that applied. Every reason is already written `TERM: definition` in all 21
// locales, which is exactly what WT_GRID_ICONS eats, so each one becomes a
// badge and a heading and the reader finds theirs by its mark.
//
// The icons are built in the same order as the lines, because the grid pairs
// them by index: a reason that does not fire must take neither slot.
//
// The footer is the thing to DO about all of this, so it belongs to the card
// rather than to any one reason. It rides in the subtitle, where it reads once
// under the title instead of pretending to be a fifth entry with no mark.
static void caution_help_cb(lv_event_t *e)
{
    (void)e;
    // sized for the longest translations (Cyrillic/CJK run 2-3 bytes per char);
    // every append clamps o because snprintf returns the WOULD-BE length
    char body[1792];
    size_t o = 0;
    const char *icons[4];
    int ni = 0;
    uint16_t f = s_sum.caution_flags;
    #define BODY_ADD(icon_, ...) do { \
        if (o + 1 < sizeof body) { \
            int w_ = snprintf(body + o, sizeof body - o, __VA_ARGS__); \
            if (w_ > 0) { o += (size_t)w_; if (o >= sizeof body) o = sizeof body - 1; } \
        } \
        icons[ni++] = (icon_); \
    } while (0)
    // Unproven input amounts used to lead this card. They are a STOP now: every
    // reason left here argues about a number the screen behind can show, and
    // that one argued about whether the number was knowable at all, which is not
    // something to hand an owner as a checkbox. See kiss_psbt.c.
    if (f & WPSBT_C_HIGHFEE)
        BODY_ADD(LV_SYMBOL_CUT, "%s%s", o ? "\n" : "", tr(STR_S_WHY_HIGHFEE));
    if (f & WPSBT_C_DUST_INPUT)
        BODY_ADD(WT_ICON_DUST, "%s%s", o ? "\n" : "", tr(STR_S_WHY_DUSTIN));
    if (f & WPSBT_C_MERGE_INS) {
        // The number is ADDRESSES, not coins. Coins on one address are already
        // one owner to anyone watching, so counting them named a loss that had
        // already happened and inflated every figure it printed. This is what
        // the transaction actually gives away.
        char m[256];
        snprintf(m, sizeof m, tr(STR_S_WHY_MERGE_FMT),
                 (unsigned)s_sum.n_in_addr);
        BODY_ADD(LV_SYMBOL_LIST, "%s%s", o ? "\n" : "", m);
    }
    if (f & (WPSBT_C_DUST_CHANGE | WPSBT_C_SMALL_CHANGE))
        BODY_ADD(LV_SYMBOL_MINUS, "%s%s", o ? "\n" : "", tr(STR_S_WHY_TINYCH));
    #undef BODY_ADD
    (void)ni;

    // No flag set means no grid to draw, and a card with a title and nothing
    // under it is worse than the prose it replaced. The "?" only exists on a
    // flagged row, so this is defensive, not a state a reader reaches.
    if (!o) return;

    wt_explain_t x = {
        .title  = tr(STR_S_WHY_T),
        .sub    = tr(STR_S_WHY_FOOT),
        .icon   = LV_SYMBOL_WARNING,
        .body   = body,
        .ok_txt = tr(STR_C_OK),
        .sev    = WT_SEV_WARN,
        .mode   = WT_GRID_ICONS,
        .icons  = icons,
    };
    wt_explain_open(s_scr, &x);
}

// Tapping the total switches the unit every amount is drawn in. A full
// repaint, like every other state change on this screen: the strand labels,
// the lists and the pair of totals all read the preference, and a partial
// redraw here would be the one path that has to stay in step with
// verify_screen by hand.
static void repaint_verify(void);
static void denom_flip(void)
{
    kiss_settings_set_denom(wt_denom() == WT_DENOM_BTC ? WT_DENOM_SATS
                                                       : WT_DENOM_BTC);
}

// The verify screen redraws whole; the details page rebuilds itself. Both
// register what a tap on any of their figures costs them, so the binding on
// the label stays a plain "this is an amount" and nothing more.
static void denom_tap_verify(void) { denom_flip(); repaint_verify(); }
static void denom_tap_details(void)
{
    denom_flip();
    details_cb(NULL);          // this page rebuilds itself from s_parent
}

// ---- "?" beside the coins caption: what a coin is ----
// The word on the glass stays "coins", which is what a coordinator's own coin
// control calls them; the card names the term the rest of Bitcoin writes down
// and corrects what the word implies.
//
// It said "a coin is spent whole, never a piece of one", which is true of the
// output and wrong about everything around it: a signer does not hold coins.
// The output is a record on the bitcoin network; what this box holds is the
// key that can unlock it, and that key is the whole of what it holds. Getting
// that backwards on the one screen where somebody is about to sign teaches
// them the device is a wallet full of money -- which is also exactly the
// belief that makes a lost passphrase feel survivable.
//
// The consumed-entirely part stays, because it is real and it is why the
// graph has a change strand at all: unlocking an output uses all of it, and
// the remainder comes back as a new one.
static void coins_help_cb(lv_event_t *e)
{
    (void)e;
    static const char *const ICONS[] = {
        LV_SYMBOL_GPS,           // where it actually is: out there, not in here
        WT_ICON_KEY,             // what this box holds, and the whole of it
        LV_SYMBOL_LOOP,          // used up, and the remainder coming back
    };
    // 64 for the term plus the parenthesis, because head is 64: the device
    // compiler refuses a snprintf that could cut a translated word in half,
    // and it is right -- this is the second buffer on this screen to be caught
    // that way and neither could have been seen from a frame.
    static char title[80];
    char head[64];
    gloss_line(0, head, sizeof head);          // INPUTS, already in 21 locales
    // UTXO is not translated by wallets anywhere: it goes on as it stands, so
    // the owner can carry the word to any other tool and be understood.
    snprintf(title, sizeof title, "%s  (UTXO)", head);
    wt_explain_t x = {
        .title  = title,
        .icon   = LV_SYMBOL_DOWNLOAD,
        .body   = tr(STR_S_COINS_HELP_B),
        .ok_txt = tr(STR_C_OK),
        .sev    = WT_SEV_PLAIN,
        .mode   = WT_GRID_ICONS,
        .icons  = ICONS,
    };
    wt_explain_open(s_scr, &x);
}

// ---- "?" beside the address: the address, big, and what to do with it ----
//
// The card carries the destination ITSELF as its aside, blocked in fours with
// the compared tail lit, over the two-entry lesson. That is the whole point of
// the chip: reading "compare it with your coordinator" on a card that does not
// show you the thing to compare is an instruction with no object, and it is
// what the owner hit when the "?" here answered RBF instead.
//
// It is also where a long destination gets the room it needs. The verify screen
// gives the address one line of a band it shares with the meta row; a silent
// payment address is 117 characters and a taproot one is 62, and this card has
// the whole 704 lane and as many lines as it wants.
//
// A signer can prove the change output is its own and can prove nothing at all
// about someone else's address, so this is the only screen where a destination
// swapped anywhere upstream still gets caught.
static char s_addr_help[128];          // the destination this card is about
// ...and whether these keys have paid it before, read once when the screen is
// built rather than per draw: kiss_payee_seen hashes under the session key, and
// the answer cannot change while one screen is up.
static bool s_addr_known;

static int aside_addr(lv_obj_t *par, int x, int y, int w)
{
    if (!s_addr_help[0]) return 0;
    char grouped[200];                 // 117 + 29 spaces, the worst case
    wt_group4(s_addr_help, grouped, sizeof grouped);
    // mono23 with the tail in the accent, blocked: 42 characters measure 915px
    // grouped, so a bech32 takes two lines of this 704 lane and the silent
    // payment form takes four. Lines are what this card has spare, which is
    // exactly why the address is here and not down there.
    lv_obj_t *ad = wt_addr_spans(par, grouped, w, wt_font_mono23());
    lv_obj_set_pos(ad, x, y);
    lv_obj_update_layout(ad);
    // The caption goes UNDER it, naming the run that is lit rather than the
    // block as a whole -- S_CMP_8 already ships in 21 locales and already says
    // it on the receive screen, so the two screens teach one habit.
    lv_obj_t *cap = wt_lbl(par, tr(STR_S_CMP_8), x, y + lv_obj_get_height(ad) + 6,
                           wt_font14(), MUT_COL);
    (void)cap;
    return lv_obj_get_height(ad) + 6 + lv_font_get_line_height(wt_font14());
}

static void addr_help_cb(lv_event_t *e)
{
    (void)e;
    // Two entries always, three when the mark is on the screen. The third is
    // the ONLY place the repeat mark is explained, which is why it is built
    // here rather than given a "?" of its own: the mark answers a question
    // about this destination, and this is the destination's card.
    static const char *const ICONS[]  = { LV_SYMBOL_EYE_OPEN, LV_SYMBOL_WARNING };
    static const char *const ICONS3[] = { LV_SYMBOL_EYE_OPEN, LV_SYMBOL_WARNING,
                                          LV_SYMBOL_REFRESH };
    static char body[512];
    snprintf(body, sizeof body, "%s%s%s", tr(STR_S_ADDR_HELP_B),
             s_addr_known ? "\n" : "",
             s_addr_known ? tr(STR_S_PAYEE_HELP) : "");
    wt_explain_t x = {
        .title  = tr(STR_R_VT),          // VERIFY ADDRESS, the receive screen's word
        .icon   = LV_SYMBOL_EYE_OPEN,
        .body   = body,
        .ok_txt = tr(STR_C_OK),
        .sev    = WT_SEV_PLAIN,
        .mode   = WT_GRID_ICONS,
        .icons  = s_addr_known ? ICONS3 : ICONS,
        .aside  = s_addr_help[0] ? aside_addr : NULL,
    };
    wt_explain_open(s_scr, &x);
}

// Opening the same card from the address block itself, so the destination is
// its own control and not a label parked under a chip.
static void addr_tap_cb(lv_event_t *e)
{
    const char *a = lv_event_get_user_data(e);
    if (a) {
        snprintf(s_addr_help, sizeof s_addr_help, "%s", a);
        s_addr_known = kiss_payee_seen(a);   // a row's own answer, not the card's
    }
    addr_help_cb(NULL);
}

// ---- "?" beside the network/RBF pair ----
//
// The question from the bench was "where is the option to RBF", and the honest
// answer is that there is not one and cannot be. RBF is the nSequence value on
// every input; it is inside the sighash preimage, so it is covered by the
// signature, and a signer that changed it would be signing a transaction its
// coordinator does not have. Sparrow, the coordinator this device names on its
// own screens, has no pre-send toggle either: it is a right click on an
// unconfirmed transaction afterwards.
//
// One body for both states, because two of its three lines are the same either
// way and the third is now true either way as well: Bitcoin Core v28 turned
// full RBF on by default and v29 removed the setting, so the flag no longer
// decides whether a replacement is accepted. That is also why the OFF label
// stopped saying FINAL -- see S_RBF_T_OFF.
//
// The TITLE carries the state, so the card names this transaction rather than
// explaining a general fact and leaving the owner to work out which half is
// theirs.
static void rbf_help_cb(lv_event_t *e)
{
    (void)e;
    static const char *const ICONS[] = {
        WT_ICON_REPLACE,         // who sets it, and it is not this box
        LV_SYMBOL_CUT,           // the fee, if it sticks
        LV_SYMBOL_OK,            // what cannot change either way
    };
    wt_explain_t x = {
        .title  = tr(s_sum.rbf ? STR_S_RBF_T_ON : STR_S_RBF_T_OFF),
        .icon   = s_sum.rbf ? WT_ICON_REPLACE : WT_ICON_LOCK,
        .body   = tr(STR_S_RBF_HELP_B),
        .ok_txt = tr(STR_C_OK),
        .sev    = WT_SEV_PLAIN,
        .mode   = WT_GRID_ICONS,
        .icons  = ICONS,
    };
    wt_explain_open(s_scr, &x);
}

// The verify screen has no partial redraw: every state change rebuilds it. The
// rebuild has to drop the screen-scoped state first, because the hold timer
// ticks against s_sweep and the graph, and those and s_sign_lbl are about to
// point at objects on the outgoing screen. Both call sites need this and used to
// spell it out; the one that forgot a line is what shipped the orphan.
static void repaint_verify(void)
{
    hold_stop();
    lv_obj_delete_async(s_scr); s_scr = NULL; s_sign_lbl = NULL;
    s_graph = NULL; s_graph_cap = NULL; s_locked = NULL;
    s_inert[0] = NULL; s_sweep = NULL;
    verify_screen(s_parent);
}

// The recipient list reached its end. Once only: scroll events arrive on every
// frame of the drag, and the repaint that lights HOLD TO SIGN rebuilds the very
// object this is attached to.
static void recip_scroll_cb(lv_event_t *e)
{
    if (s_recip_seen) return;
    if (lv_obj_get_scroll_bottom(lv_event_get_target(e)) > 0)
        return;                             // still more under the fold
    s_recip_seen = true;
    repaint_verify();
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
#define SG_ROW_H      56   // a caution row, on the page the rows now live on
#define SG_ROW_PILL_W 170
#define SG_ROW_MAX      4  // fee + dust in + merge + one change row

// The caution bar: one row, always, however many reasons there are. 44 is the
// ack pill (40) plus 2px above and below, the least that still reads as a bar.
//
// Everything below it moves down, and the 52px has to come from somewhere. It
// does NOT come from the recipient panel's existence -- that was the defect --
// it comes from 16px of the panel's height and 24px of footer air:
//
//   bar     150..194
//   panels  202..306   (SG_PAN_H_C, 16 shorter; the "compare these 8" hint is
//                       what gives way, and only the hint -- the address and
//                       the two compared runs are both still drawn)
//   rule    314
//   footer  322..379   worst measured case, a network name wrapping to two
//                      lines at font14. WT_CONTENT_BOTTOM is 398.
//
// Measured by overlapcheck across 21 locales, not budgeted on paper: the first
// attempt put the strip 12px past the bottom in pt-BR, ru and tr, and the run
// said so before any of this shipped.
#define SG_BAR_Y     150
#define SG_BAR_Y_G   344   // ... and where it sits under the bundle graph
#define SG_BAR_H      44
// The graph's box. 118 tall on a clean screen; 110 when the caution bar is
// under it, which is the 8px the bar's band needs back.
#define SG_GRAPH_Y   172
#define SG_GRAPH_H   118
#define SG_GRAPH_H_C 110
// Several recipients: no address card below, so the graph runs to 366 and the
// output column shows two more destinations before anything is under the fold.
#define SG_GRAPH_H_MANY 194
#define SG_PAN_Y_C   202   // panels, with a bar above them
#define SG_PAN_H_C   104
#define SG_RULE_Y_C  314
#define SG_FOOT_Y_C  322
#define SG_FOOT_H     70   // column rule height, clean screen
#define SG_FOOT_H_C   46   // ... and with a bar above, where the strip sits lower

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
// nearly right for a 48px page margin, which is what kiss_theme.c:373 declares
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

// A caution row carries its own acknowledgement now, so the answer to "I read
// it" lives next to the thing being read instead of in the action row. That is
// what frees HOLD TO SIGN to keep its coordinates in both states, which is the
// entire safety argument of the redraw: the old layout put I UNDERSTAND at
// 238..490 and the sign pill at 310..582, so two taps in the same place could
// become a signature nobody read. SIGN_ARM_MS still guards the seam.
static uint16_t caution_rows(uint16_t f, const char **parts, uint16_t *bits, int cap)
{
    int n = 0;
    // The fee leads. An "amounts not proven" row used to sit above it, because it
    // was the one reason that put the fee row's own number in doubt -- a row that
    // says "the figure below may be wrong" is a refusal wearing a checkbox, and
    // it is one now.
    if (n < cap && (f & WPSBT_C_HIGHFEE))
        { bits[n] = WPSBT_C_HIGHFEE;     parts[n++] = tr(STR_S_C_HIGHFEE); }
    if (n < cap && (f & WPSBT_C_DUST_INPUT))
        { bits[n] = WPSBT_C_DUST_INPUT;  parts[n++] = tr(STR_S_C_DUSTIN); }
    // input-side, so it sits with the dust row rather than with the change ones.
    // Four rows is the ceiling this can reach (fee + dust in + merge + one of the
    // two change rows), which is exactly the cap the row stack draws for.
    if (n < cap && (f & WPSBT_C_MERGE_INS))
        { bits[n] = WPSBT_C_MERGE_INS;   parts[n++] = tr(STR_S_C_MERGE); }
    if (n < cap && (f & WPSBT_C_DUST_CHANGE))
        { bits[n] = WPSBT_C_DUST_CHANGE; parts[n++] = tr(STR_S_C_DUSTCH); }
    else if (n < cap && (f & WPSBT_C_SMALL_CHANGE))
        { bits[n] = WPSBT_C_SMALL_CHANGE; parts[n++] = tr(STR_S_C_SMALLCH); }
    return (uint16_t)n;
}

static uint16_t caution_all_bits(uint16_t f)
{
    const char *p[SG_ROW_MAX]; uint16_t b[SG_ROW_MAX];
    uint16_t n = caution_rows(f, p, b, SG_ROW_MAX), all = 0;
    for (int i = 0; i < n; i++) all |= b[i];
    return all;
}

// The row stack lives on its own screen now (cautions_screen), so acking has two
// possible things to redraw. See the note above verify_screen's caution bar for
// why the rows left the verify screen.
static bool s_on_cautions;
static void cautions_screen(void);
static void repaint_cautions(void)
{
    hold_stop();
    lv_obj_delete_async(s_scr); s_scr = NULL; s_sign_lbl = NULL;
    s_graph = NULL; s_graph_cap = NULL; s_locked = NULL;
    s_inert[0] = NULL; s_sweep = NULL;
    cautions_screen();
}

static void row_ack_cb(lv_event_t *e)
{
    uint16_t bit = (uint16_t)(uintptr_t)lv_event_get_user_data(e);
    s_ack_flags |= bit;
    s_ack_t0 = lv_tick_get();               // arm the seam, same as the old gate
    if ((s_ack_flags & caution_all_bits(s_sum.caution_flags))
        == caution_all_bits(s_sum.caution_flags))
        s_ack = true;
    if (s_on_cautions) repaint_cautions();  // this row goes green, and
    else               repaint_verify();    // HOLD TO SIGN lights when all are in
}

static void cautions_open_cb(lv_event_t *e)
{
    (void)e;
    hold_stop();
    lv_obj_delete_async(s_scr); s_scr = NULL; s_sign_lbl = NULL;
    s_graph = NULL; s_graph_cap = NULL; s_locked = NULL;
    s_inert[0] = NULL; s_sweep = NULL;
    s_on_cautions = true;
    cautions_screen();
}

static void cautions_back_cb(lv_event_t *e)
{
    (void)e;
    s_on_cautions = false;
    repaint_verify();
}

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

// ---- the caution rows, on a page of their own ------------------------------
// They used to be drawn on the verify screen INSTEAD of the output panels, and
// that is the bug this page exists to close: the address the owner was being
// asked to sign for disappeared the moment the device found anything to warn
// about, which is precisely when it matters most. A coordinator could reach
// that state on purpose -- five inputs, or a dust coin planted last week -- so
// the screen showed least about the transactions it trusted least.
//
// Rows cannot share the verify screen with the panels: four of them at the
// metric that keeps a row readable is 236px, and the band between the hero and
// the footer is 138. One of the two had to move, and it is not going to be the
// address. Here they get the whole page and the comfortable 56px metric back.
static void cautions_screen(void)
{
    const char *parts[SG_ROW_MAX]; uint16_t bits[SG_ROW_MAX];
    uint16_t np = caution_rows(s_sum.caution_flags, parts, bits, SG_ROW_MAX);

    // No subtitle. The file name is on the screen this one was opened from and
    // on the screen it goes back to, and here it would cost row 0 its top edge.
    mk_screen(s_parent, tr(STR_S_WHY_T), NULL);
    wt_help_chip(s_scr, 738, 34, WARN_COL, caution_help_cb, NULL);

    // 88 + 4*56 + 3*4 = 324, well clear of WT_CONTENT_BOTTOM. The gap is 4
    // rather than the verify screen's 8, from when a fifth row had to keep
    // SG_ROW_H; the tight metric existed only because the rows were sharing a
    // screen, and they no longer are.
    int y = 88;
    for (int i = 0; i < np; i++) {
        bool done = (s_ack_flags & bits[i]) != 0;
        lv_obj_t *row = sg_panel(24, y, 752, SG_ROW_H, done ? OK_COL : WARN_COL);
        sg_lbl(row, done ? LV_SYMBOL_OK : LV_SYMBOL_WARNING, SG_PAD, 16,
               wt_font23(), done ? OK_COL : WARN_COL);
        lv_obj_t *t = lv_label_create(row);
        lv_obj_set_pos(t, 52, 19);
        lv_obj_set_style_text_color(t, done ? MUT_COL : INK_COL, 0);
        wt_note_fit(t, parts[i], 491 - 16, 24);

        if (done) {
            lv_obj_t *p = sg_panel(543, 8, SG_ROW_PILL_W, 40, OK_COL);
            lv_obj_set_style_radius(p, 10, 0);
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
        y += SG_ROW_H + 4;
    }

    wt_pillh(s_scr, tr(STR_C_BACK), SG_BACK_X140, WT_ACTION_Y, 140, WT_ACTION_H,
             cautions_back_cb, NULL);
}

static void verify_screen(lv_obj_t *parent)
{
    wt_denom_on_tap(denom_tap_verify);   // what a tap on any figure here costs
    char buf[160], a[32], b[32];
    s_parent = parent;                    // details page rebuilds us from here
    mk_screen(parent, tr(STR_S_T), NULL);

    const char *parts[SG_ROW_MAX]; uint16_t bits[SG_ROW_MAX];
    uint16_t np = (s_sum.status == WPSBT_CAUTION)
                    ? caution_rows(s_sum.caution_flags, parts, bits, SG_ROW_MAX) : 0;

    // ---- header ---------------------------------------------------------
    // The filename is not translated and never will be, so it is mono: it is
    // the one string on this row whose exact characters the owner may need to
    // read back against what their coordinator sent.
    // wt_screen puts the title at x=48, font34, letter_space 3. The drawing's
    // x=132 assumed a tighter title than this device actually draws, and in a
    // locale whose word for SIGN is longer than English the collision gets
    // worse. Measured, not assumed.
    //
    // A file that already has a signature beside it on the card says so HERE
    // too, not just on the row that was tapped -- this is the screen that asks
    // for the hold, so it is the screen that has to carry the fact. It does not
    // block anything: re-signing is deterministic and is the right move when a
    // card write failed. It just means the owner always knows which it is.
    //
    // The badge is placed FIRST and right anchored, then the filename is bounded
    // to whatever lane is left, which is the discipline wt_row_x already uses.
    // Sized rather than assumed: this band is shared with the header chip at
    // x=540, and a long filename used to be free to run under it.
    {
        lv_point_t ts;
        lv_text_get_size(&ts, tr(STR_S_T), wt_font34(), 3, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int fx = 48 + ts.x + 18, fr = 530;      // 10 clear of the chip at 540
        if (s_src == SRC_SD && s_cur_signed) {
            // The badge has to say the SAME thing the row said. The list draws
            // the two signed states apart on purpose -- a *-signed.psbt IS the
            // signature (OK), a source that already carries one is a do-not-
            // redo (WARN) -- and this screen ignored the distinction and warned
            // about both. So one transaction signed once showed SIGNATURE in
            // green on one row and SIGNED ALREADY in amber on the other, then
            // said "signed already" on both when either was opened: two names
            // and two colours for one file, and the device appearing to
            // contradict its own list.
            const bool is_out = is_signed_name(s_cur);
            // font14, NOT mono: the mono faces carry no icon plane, so a
            // symbol set in them draws a placeholder box.
            lv_obj_t *w = sg_lbl(s_scr,
                                 is_out ? tr_sym(LV_SYMBOL_OK, STR_S_ROW_SIGNATURE)
                                        : tr_sym(LV_SYMBOL_WARNING, STR_S_SIGNED_ALREADY),
                                 0, 33, wt_font14(), is_out ? OK_COL : WARN_COL);
            lv_obj_update_layout(w);
            int ww = lv_obj_get_width(w);
            lv_obj_set_pos(w, fr - ww, 33);
            fr -= ww + 12;
        }
        // Below about 60px a filename is ellipsis and one character, which tells
        // nobody anything. It is already on the row that was tapped and in the
        // DETAILS page title, so drop it rather than let it collide.
        if (fr - fx >= 60) {
            lv_obj_t *f = sg_lbl(s_scr, s_cur, fx, 34, wt_font_mono14(), MUT_COL);
            lv_obj_set_width(f, fr - fx);
            lv_label_set_long_mode(f, LV_LABEL_LONG_DOT);
        }
    }

    // The chip at the top right is ONE slot in two states. The caution count
    // replaces the fingerprint at the same x, y, w and h, so nothing new can
    // ever appear here and collide with the title or the filename. That is
    // defect 01 from the review, closed by deletion rather than by relocation.
    {
        lv_obj_t *chip = sg_panel(540, 14, 236, 36, np ? WARN_COL : wt_accent());
        // Only when it is the SIGNING AS badge. With cautions it is a count in
        // WT_WARN, and that is a status: the accent does not go near it.
        if (!np) lv_obj_add_flag(chip, WT_FLAG_ACCENT_BORDER);
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
            kiss_ui_last_fp(fp);
            lv_obj_t *c = lv_label_create(chip);
            // The key is step 2's mark on the HOW SIGNING WORKS diagram: the
            // same mark on the chip says "this is the step you are at" without
            // a word of overlap between the two screens.
            lv_label_set_text(c, tr_sym(WT_ICON_KEY, STR_S_SIGNING_AS));
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

    // ---- STOP: the verdict is the screen, nothing else is ----------------
    // Above the hero, which is what makes that sentence true. It used to sit
    // below, so a refused transaction still got TOTAL LEAVING in 48px type
    // over the refusal -- a number the verifier had just declined to stand
    // behind, printed larger than the reason it declined.
    //
    // Worse on the STOPs that fire early. The summary is zeroed before the
    // parse, and a refusal like "too many outputs" returns before any output
    // is summed, so the biggest thing on the screen read 0 sats: the signer
    // asserting a refused transaction moves nothing. And "input amount
    // unverifiable" is precisely the case where a confident total is the one
    // claim the screen has no business making.
    //
    // Nothing was at risk -- signing is gated on status and the branch returns
    // before HOLD TO SIGN exists -- but this screen's whole job is not saying
    // things it cannot support.
    if (s_sum.status == WPSBT_STOP) {
        // Up into the band the hero used to hold, rather than leaving 86px of
        // nothing under the title where a number used to be. The reason is the
        // only content this screen has, so it begins where the eye lands. Same
        // left edge and lane as the hero it replaces; the extra height goes to
        // the reason, which is translated and is the longest string here.
        // A refusal with a remedy gets the verdict in a band and the two claims
        // -- what went wrong, what to do -- as ruled blocks below it, which is
        // the same shape every other explainer on this device uses. Without a
        // remedy the panel keeps the whole upper band: there is nothing to put
        // under it, and a short verdict floating over 230px of glass reads as a
        // screen that lost something.
        const char *body = stop_body(s_sum.reason);
        int ph = body ? 64 : SG_PANEL_H + 52;
        lv_obj_t *p = sg_panel(24, 84, 752, ph, STOP_COL);
        lv_obj_t *r = sg_lbl(p, tr_reason(s_sum.reason), SG_PAD, SG_PAD,
                             wt_font23(), STOP_COL);
        lv_obj_set_width(r, 752 - 2 * SG_PAD);
        lv_label_set_long_mode(r, LV_LABEL_LONG_WRAP);
        if (body) wt_why_body(s_scr, body, 84 + ph + 20, STOP_COL, true);
        // Same 776 lane as the panel it just drew, so the same exit as verify.
        wt_pillh(s_scr, tr(STR_C_BACK), SG_BACK_X140, WT_ACTION_Y, 140, WT_ACTION_H,
                 s_src == SRC_SD ? files_back_cb : choose_back_cb, NULL);
        return;
    }

    // ---- the hero -------------------------------------------------------
    // One number, not two. The old screen showed RECIPIENT GETS and TOTAL
    // LEAVING at the same rung and left the owner to work out which one they
    // were agreeing to.
    //
    // With ONE recipient that number is what the recipient gets, and it took a
    // bench report to get it there: the hero was send + fee, so a transaction
    // paying 10 000 with a 281 fee printed 10 281 in 48px type -- a figure that
    // appears on no coordinator screen, is in no field of the PSBT, and is not
    // the amount anyone decided to send. The reader was left comparing the
    // biggest number on the signer against a different number on the machine
    // that built the transaction. The fee is not lost by this: it has its own
    // row in the graph, its rate and its share of the send sit on this line,
    // and the input total above the graph is what the two are checked against.
    //
    // With none or several it stays send + fee, because there is no single send
    // amount to enlarge and the sum of what leaves is then the honest headline.
    // A spend with no recipient at all -- a consolidation back to yourself --
    // is the case that makes this exact: the only thing leaving is the fee, and
    // that is what the number says.
    //
    // Counted before the hero because it decides which number the hero IS, and
    // because the change count decides whether the graph draws a change strand
    // or the row that says there is none.
    int recipient_n = 0, change_n = 0;
    for (int i = 0; i < (int)s_sum.n_out && i < WPSBT_MAX_OUTS; i++)
        { if (s_sum.outs[i].is_change) change_n++; else recipient_n++; }

    const bool one_recip = (recipient_n == 1);
    uint64_t total = hero_sats();

    {
        lv_obj_t *cap = sg_lbl(s_scr, one_recip ? tr(STR_S_SENDING_CAP)
                                                : tr(STR_S_TOTAL_LEAVING),
                               24, 78, wt_font14(), MUT_COL);
        lv_obj_set_style_text_letter_space(cap, 2, 0);

        lv_obj_t *row = lv_obj_create(s_scr);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 24, 92);
        lv_obj_set_size(row, 760, 52);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_column(row, 14, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        wt_fmt_amount(total, a, sizeof a);
        // font_kiss_num48: digits, A to F, space and full stop. It cannot spell
        // a word, so it can never be handed a translated string by accident.
        // The total is the switch. Tapping it moves every amount on the
        // device between sats and BTC and writes the choice, which is how a
        // wallet that offers both units has always done it: the number a
        // coordinator is being compared against is the one place the question
        // comes up, and the settings page this would otherwise need a row on
        // is full to its margins.
        lv_obj_t *big = lv_label_create(row);
        lv_label_set_text(big, a);
        lv_obj_set_style_text_font(big, wt_font_num48(), 0);
        lv_obj_set_style_text_color(big, INK_COL, 0);
        wt_denom_bind(big);

        lv_obj_t *unit = lv_label_create(row);
        lv_label_set_text(unit, wt_denom_unit());
        lv_obj_set_style_text_font(unit, wt_font23(), 0);
        lv_obj_set_style_text_color(unit, INK_COL, 0);
        wt_denom_bind(unit);

        // The other unit, small, under the big one: a coordinator that counts
        // the other way is checked against this line without a trip to
        // Settings, which is the whole reason both are here.
        wt_fmt_amount_alt(total, b, sizeof b);
        snprintf(buf, sizeof buf, "%s %s", b, wt_denom_unit_alt());
        lv_obj_t *btc = lv_label_create(row);
        lv_label_set_text(btc, buf);
        lv_obj_set_style_text_font(btc, wt_font_mono14(), 0);
        lv_obj_set_style_text_color(btc, MUT_COL, 0);
        wt_denom_bind(btc);

        // ---- the fee rate, on the row that is already about the money ----
        //
        // The graph draws the fee as a strand with a thickness, which answers
        // "how much" and cannot answer "is that a lot". S_FEERATE_PCT_FMT
        // answers the second one and was already written and translated:
        // "7.0 sat/vB, 1.6% of what you send". It lived on DETAILS alone, two
        // taps from the decision, and the percentage is the number Coldcard
        // warns on at 5% and refuses at 10% -- the one a newcomer can reason
        // about without knowing what a good rate looks like this week.
        //
        // Here rather than in the band under the address, because the address
        // card took that band back, and because these are facts about the same
        // amount. The scissors, not a bolt: a bolt reads as Lightning.
        if (s_sum.fee_rate_x10) {
            uint64_t p10 = s_sum.send_sats
                         ? (uint64_t)s_sum.fee_sats * 1000ull / s_sum.send_sats : 0;
            if (s_sum.send_sats)
                snprintf(buf, sizeof buf, tr(STR_S_FEERATE_PCT_FMT),
                         (unsigned)(s_sum.fee_rate_x10 / 10),
                         (unsigned)(s_sum.fee_rate_x10 % 10),
                         (unsigned long long)(p10 / 10),
                         (unsigned long long)(p10 % 10));
            else
                snprintf(buf, sizeof buf, tr(STR_S_FEERATE_FMT),
                         (unsigned)(s_sum.fee_rate_x10 / 10),
                         (unsigned)(s_sum.fee_rate_x10 % 10));
            lv_obj_t *fr = lv_label_create(row);
            lv_label_set_text_fmt(fr, "%s  %s", LV_SYMBOL_CUT, buf);
            lv_obj_set_style_text_font(fr, wt_font14(), 0);
            lv_obj_set_style_text_color(fr, MUT_COL, 0);
        }
    }

    // ---- the caution bar -------------------------------------------------
    // One row tall, whatever the count, and it never takes the panels' place.
    // The rows themselves are a page away (cautions_screen); what stays here is
    // the reason nearest the top of caution_rows' priority order, plus how many
    // more there are, plus the way in. A flagged transaction and a clean one
    // now differ by 52px of bar -- not by whether the owner is shown where the
    // coins are going.
    //
    // The single-caution case still acks in place: one reason, one pill, no
    // navigation, which is the shape most flagged transactions actually have.
    if (np) {
        bool all_done = (s_ack_flags & caution_all_bits(s_sum.caution_flags))
                        == caution_all_bits(s_sum.caution_flags);
        // The graph owns 172..282, so the bar sits in the band the facts strip
        // used to hold. Same bar, same height, same 475px text box -- which is
        // what keeps the string from running under the pill at 543.
        lv_obj_t *bar = sg_panel(24, SG_BAR_Y_G, 752, SG_BAR_H,
                                 all_done ? OK_COL : WARN_COL);
        sg_lbl(bar, all_done ? LV_SYMBOL_OK : LV_SYMBOL_WARNING, SG_PAD, 10,
               wt_font23(), all_done ? OK_COL : WARN_COL);
        // "+N" carries the rest of the list without a string to translate: the
        // header chip already states the total, so this only has to say that
        // the one line shown is not all of it.
        if (np > 1) snprintf(buf, sizeof buf, "%s   +%u", parts[0], (unsigned)(np - 1));
        else        snprintf(buf, sizeof buf, "%s", parts[0]);
        lv_obj_t *t = lv_label_create(bar);
        lv_obj_set_pos(t, 52, 13);
        lv_obj_set_style_text_color(t, all_done ? MUT_COL : INK_COL, 0);
        wt_note_fit(t, buf, 491 - 16, 24);

        if (np == 1 && !all_done) {
            wt_pillh(bar, tr(STR_C_I_UNDERSTAND), 543, 2, SG_ROW_PILL_W, 40,
                     row_ack_cb, (void *)(uintptr_t)bits[0]);
        } else if (np == 1) {
            // One caution, already acknowledged: the spent tick, not a
            // REVIEW pill. REVIEW here opened a page whose only content was
            // this same sentence with this same tick -- a whole screen to
            // re-read one thing the owner had just read. The tick is the
            // cautions page's own spent-state mark, drawn in place.
            lv_obj_t *p = lv_obj_create(bar);
            lv_obj_remove_style_all(p);
            lv_obj_set_size(p, SG_ROW_PILL_W, 40);
            lv_obj_set_style_border_width(p, 2, 0);
            lv_obj_set_style_border_color(p, OK_COL, 0);
            lv_obj_set_style_radius(p, 10, 0);
            lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
            lv_obj_set_flex_flow(p, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(p, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_pos(p, 543, 2);
            lv_obj_t *l = lv_label_create(p);
            lv_label_set_text(l, LV_SYMBOL_OK);
            lv_obj_set_style_text_font(l, wt_font23(), 0);
            lv_obj_set_style_text_color(l, OK_COL, 0);
        } else {
            wt_pillh(bar, tr(STR_S_C_REVIEW), 543, 2, SG_ROW_PILL_W, 40,
                     cautions_open_cb, NULL);
        }
    }

    // ---- the outputs ------------------------------------------------------
    // Drawn on EVERY verify screen. There is no status, no count of warnings
    // and no acknowledgement state that removes them; that was the defect.
    {
        // ---- the bundle graph --------------------------------------------
        // The input amounts live on the DETAILS struct, not the summary, so
        // this is the first verify-side call to kiss_psbt_details(). If it
        // refuses -- it cannot here, the STOP branch returned already -- the
        // graph still draws, as one strand carrying the whole input side. A
        // graph that understates how many coins are being spent would be worse
        // than no graph, so the fallback overstates nothing: one strand, one
        // total, and the caption counts from the summary either way.
        wpsbt_details_t det;
        const bool have_det = (kiss_psbt_details(&det) == 0);

        wt_strand_t in[WT_BUNDLE_MAX], out[WT_BUNDLE_MAX];
        size_t n_in = 0, n_out = 0;
        uint64_t max_sats = 0;

        // The coins linked caution, drawn rather than only written. The strands
        // converging on one dot ARE the linkage the bar below is warning about,
        // so they wear WT_WARN and the words stop being the only place it is
        // said. Every input takes it, including the elided group -- the reason
        // is the convergence, and no single coin is more responsible for it
        // than another.
        const uint8_t in_role = (s_sum.caution_flags & WPSBT_C_MERGE_INS)
                              ? WT_STRAND_LINKED : WT_STRAND_IN;

        char gbuf[64];
        if (have_det && det.n_in) {
            // Five rows at any coin count. Above five: the first two, the
            // elided middle, the last two.
            //
            // Both of the group's numbers come from figures the verifier
            // stands behind rather than from the rows on screen. The count is
            // the summary's n_in, which is the real input count; the total is
            // in_sats less the four drawn. That matters because ins[] holds at
            // most WPSBT_MAX_INS, so a twenty input spend has sixteen entries
            // here -- summing what is in the array would understate the middle
            // by four coins, and a graph that quietly loses value is worse than
            // one that does not draw.
            //
            // The consequence of the same cap: above sixteen inputs the "last
            // two" are the last two HELD, not the last two spent. They are real
            // amounts of real coins in this transaction and nothing on screen
            // claims an ordering, so this is a narrowing, not a fiction.
            //
            // Four held entries is the floor for eliding at all: below it there
            // is no "first two and last two" to draw, so a short ins[] under a
            // large n_in draws the rows it actually has rather than indexing
            // off the end of the array to satisfy a shape.
            const uint32_t total_n = s_sum.n_in ? s_sum.n_in : det.n_in;
            if (total_n <= 5 || det.n_in < 4) {
                for (uint32_t i = 0; i < det.n_in && n_in < WT_BUNDLE_MAX; i++)
                    in[n_in++] = (wt_strand_t){ .sats = det.ins[i].sats,
                                                .role = in_role };
            } else {
                const uint32_t last = det.n_in - 1;
                uint64_t shown = det.ins[0].sats + det.ins[1].sats
                               + det.ins[last - 1].sats + det.ins[last].sats;
                uint64_t hidden = s_sum.in_sats > shown ? s_sum.in_sats - shown : 0;
                snprintf(gbuf, sizeof gbuf, tr(STR_S_BUNDLE_MORE_FMT),
                         (unsigned)(total_n - 4));
                in[n_in++] = (wt_strand_t){ .sats = det.ins[0].sats,
                                            .role = in_role };
                in[n_in++] = (wt_strand_t){ .sats = det.ins[1].sats,
                                            .role = in_role };
                in[n_in++] = (wt_strand_t){ .sats = hidden, .label = gbuf,
                                            .role = in_role,
                                            .is_group = true,
                                            .group_n = (uint16_t)(total_n - 4) };
                in[n_in++] = (wt_strand_t){ .sats = det.ins[last - 1].sats,
                                            .role = in_role };
                in[n_in++] = (wt_strand_t){ .sats = det.ins[last].sats,
                                            .role = in_role };
            }
        }
        if (!n_in) {
            in[0] = (wt_strand_t){ .sats = s_sum.in_sats, .role = in_role };
            n_in = 1;
        }

        // Recipients, then the fee, then change: the order every frame draws
        // and the order the sentence "amount plus fee, and what comes back"
        // is read in.
        //
        // Every row wears the glossary's mark for what it is: OUTPUTS on a
        // recipient, FEE RATE's scissors on the fee, CHANGE's loop on change.
        // That is what the bench was asking for when it said the fee is white
        // and the change is coloured -- the colours DO mean something (ink
        // leaves your control, the accent comes back to you, mute is a coin
        // waiting for its signature), but the send and the fee both leave, so
        // both are ink and colour alone could never tell them apart. The mark
        // does, at a glance, and it is the same mark the page one tap away
        // teaches the word with.
        //
        // Own buffers, alive until wt_bundle() has read them: the strand array
        // holds POINTERS, and the shared `buf` is written again before the
        // graph is built -- see the change row below, which learned this the
        // hard way.
        //
        // The recipient row goes bare when there is only ONE recipient: the
        // hero four lines above is that row's own amount and already carries
        // the words, and the same phrase twice in a 300px column is the legend
        // arguing with the headline. What is left is the number, which is not
        // a repeat -- it is the term in the sum the reader checks the fee with.
        // With several recipients the words come back, because then the hero is
        // a total and no single row owns it.
        char sbuf[80], fbuf[80], cbuf[80];
        snprintf(sbuf, sizeof sbuf, "%s  %s", GLOSS_ICONS[1],
                 tr(STR_S_SENDING_CAP));
        snprintf(fbuf, sizeof fbuf, "%s  %s", GLOSS_ICONS[4], tr(STR_S_FEE));
        snprintf(cbuf, sizeof cbuf, "%s  %s", GLOSS_ICONS[2], gloss_term(2));
        for (int i = 0; i < (int)s_sum.n_out && i < WPSBT_MAX_OUTS
                        && n_out < WT_BUNDLE_MAX; i++) {
            if (s_sum.outs[i].is_change) continue;
            // With one recipient the address gets its own full width line under
            // the graph, at mono23, which is the frame and the more legible of
            // the two. With more than one, the graph is the ONLY place an
            // address can appear -- a single line below it could name the first
            // and no other -- so every row carries its own.
            out[n_out++] = (wt_strand_t){ .sats  = s_sum.outs[i].sats,
                                          .label = one_recip ? NULL : sbuf,
                                          .role  = WT_STRAND_SEND,
                                          .known = kiss_payee_seen(s_sum.outs[i].addr),
                                          .addr  = recipient_n > 1
                                                   ? s_sum.outs[i].addr : NULL };
            // A silent payment used to claim its on-chain address here, in a
            // paragraph under its own row. That row cost the column its last
            // line: amount + paragraph + fee + change is 4 rows in a band that
            // holds 3, so every SP send scrolled and the read-to-the-end gate
            // held HOLD TO SIGN until the column had been dragged -- the SP
            // case, on a screen whose first job is saying a signature is
            // armed. The note lives on the DETAILS page now, beside the SP
            // output's address, where a reader who wants the raw facts has
            // already come.
        }
        if (n_out < WT_BUNDLE_MAX)
            out[n_out++] = (wt_strand_t){ .sats  = s_sum.fee_sats,
                                          .label = fbuf,
                                          .role  = WT_STRAND_FEE };
        for (int i = 0; i < (int)s_sum.n_out && i < WPSBT_MAX_OUTS
                        && n_out < WT_BUNDLE_MAX; i++) {
            if (!s_sum.outs[i].is_change) continue;
            out[n_out++] = (wt_strand_t){ .sats  = s_sum.outs[i].sats,
                                          .label = cbuf,   // CHANGE
                                          .role  = WT_STRAND_CHANGE };
        }
        // A transaction that leaves nothing behind says so on the row where
        // change would have been, rather than by having one row fewer. The
        // missing row is the fact.
        // Its own buffer, not the shared one: this string is handed to the graph
        // as a POINTER and read when the widget draws, which is after the next
        // snprintf into buf. Sharing it printed the input caption on the output
        // row -- "0  SPENDING 20 OF YOUR COINS" where the change would be.
        char nbuf[64];
        if (!change_n && n_out < WT_BUNDLE_MAX) {
            snprintf(nbuf, sizeof nbuf, tr(STR_S_BUNDLE_NOCHANGE_FMT),
                     (unsigned)s_sum.n_in);
            out[n_out++] = (wt_strand_t){ .label = nbuf, .role = WT_STRAND_FEE,
                                          .note_only = true };
        }

        // One scale for the whole graph, taken across both sides, so a strand's
        // thickness means the same thing wherever it is.
        for (size_t i = 0; i < n_in; i++)
            if (in[i].sats > max_sats) max_sats = in[i].sats;
        for (size_t i = 0; i < n_out; i++)
            if (out[i].sats > max_sats) max_sats = out[i].sats;

        snprintf(buf, sizeof buf, tr(STR_S_BUNDLE_IN_FMT), (unsigned)s_sum.n_in);
        snprintf(s_graph_cap_rest, sizeof s_graph_cap_rest, "%s", buf);
        lv_obj_t *lc = sg_lbl(s_scr, buf, 24, 150, wt_font14(), MUT_COL);
        lv_obj_set_style_text_letter_space(lc, 2, 0);
        s_graph_cap = lc;      // becomes SIGNING, then ALL %u COINS SIGNED
        // "SPENDING 3 OF YOUR COINS" is the first line on this screen that uses
        // a word a newcomer has to be taught, and it had nothing to tap. The
        // chip goes after the caption's MEASURED width rather than a guessed x,
        // because the caption is a formatted translation -- 160 bytes of it in
        // Cyrillic -- and clamped short of the outputs caption at 464 so the
        // widest locale cannot push it into the other half of the row.
        //
        // Measured off the WIDEST text this one label will ever hold, not the
        // text it holds now. It is s_graph_cap: the hold turns it into SIGNING
        // and the signature turns it into SIGNED, and "ONDERTEKEND" is wider
        // than "INPUTS (1)". Measuring only the resting caption put the chip
        // 8px inside the signed word in nl, ru and hr -- on a frame that exists
        // for 1.6 seconds, which is why the 21-locale gate found it and no
        // amount of looking at the English screen would have.
        lv_obj_update_layout(lc);
        int capw = lv_obj_get_width(lc);
        {
            char alt[64];
            const char *cands[3];
            snprintf(alt, sizeof alt, tr(STR_S_ALL_SIGNED_FMT), (unsigned)s_sum.n_in);
            cands[0] = tr(STR_S_SIGNING);
            cands[1] = tr(STR_S_SIGNED_T);
            cands[2] = alt;
            for (int c = 0; c < 3; c++) {
                lv_point_t p;
                lv_text_get_size(&p, cands[c], wt_font14(), 2, 0,
                                 LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                if (p.x > capw) capw = p.x;
            }
        }
        // ---- the input total, at the rung the outputs are read at ----------
        //
        // What came in is the only figure that says whether the fee is the fee:
        // in, less what goes out, IS the fee, and a reader who cannot see the
        // first number cannot check the last one. It was mono14 and muted, the
        // smallest text on the screen, and above one input it was not on the
        // screen at all -- five coins drew five amounts and never their sum, so
        // the check was a page away behind DETAILS on the one screen whose job
        // is catching a transaction that lies about itself.
        //
        // Here only when the graph draws a BREAKDOWN. With a single input strand
        // that row is already the total, and wt_bundle sets it at mono23 for
        // exactly that reason -- printing it again 40px above would be the same
        // number twice.
        //
        // Next to the caption it belongs to, past its "?", and never past 448:
        // WHERE IT GOES starts at 464, and a figure right aligned against that
        // reads as the first line of the other column instead of the last of
        // this one. Placed on the caption's BASELINE rather than its top,
        // because these are two faces of different heights and a shared top
        // edge is not a shared line.
        //
        // Measured BEFORE the chip is placed, and the chip's clamp comes off
        // it. The clamp was a flat 424, chosen when nothing but the caption
        // shared this half of the row; leaving it there let a wide locale push
        // the "?" onto the number. Which of the two gives way is not a
        // question: the chip explains a word, and this is half the arithmetic
        // the screen exists for.
        char intot[32] = "";
        int tot_w = 0, tot_x = 0, tot_y = 0;
        if (n_in > 1) {
            lv_point_t ts;
            const lv_font_t *f14 = wt_font14(), *f23 = wt_font_mono23();
            wt_fmt_amount(s_sum.in_sats, intot, sizeof intot);
            lv_text_get_size(&ts, intot, f23, 0, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
            tot_w = ts.x + 12;
            tot_y = 150 + (lv_font_get_line_height(f14) - f14->base_line)
                        - (lv_font_get_line_height(f23) - f23->base_line);
        }
        s_coins_chip_x = 24 + capw + 8;
        const int chip_max = 448 - tot_w - 30;
        if (s_coins_chip_x > chip_max) s_coins_chip_x = chip_max;
        tot_x = s_coins_chip_x + 30 + 12;
        lv_obj_t *rc = sg_lbl(s_scr, tr(STR_S_BUNDLE_OUT), 464, 150,
                              wt_font14(), MUT_COL);
        lv_obj_set_style_text_letter_space(rc, 2, 0);

        // With one recipient the address card takes the band under the graph.
        // With several there is no card -- every destination is a row up here
        // instead -- so the graph takes that band back and shows more of them,
        // which is the difference between a column an owner scrolls once and
        // one they scroll four times to clear the read-to-the-end gate.
        const int gh = np ? SG_GRAPH_H_C
                     : (recipient_n > 1 ? SG_GRAPH_H_MANY : SG_GRAPH_H);
        lv_obj_t *bg = wt_bundle(s_scr, 24, SG_GRAPH_Y, 752, gh,
                                 in, n_in, out, n_out, max_sats);
        s_graph = bg;

        // AFTER the graph, deliberately. The chip's box runs 144..174 and the
        // graph starts at 172, so a chip built before it is two pixels under a
        // later sibling that covers the whole width -- it drew correctly, and
        // every press went to the graph. The frame cannot show this: the chip
        // is right there, in the right place, and simply does nothing.
        wt_help_chip(s_scr, s_coins_chip_x, 144, MUT_COL, coins_help_cb, NULL);

        // The input total, here for the same reason and measured above for it:
        // the graph's box begins at SG_GRAPH_Y - BPAD = 156 and this figure's
        // line box reaches 174, so built before the graph it is a figure under
        // a sibling that covers the whole width. It is a CONTROL -- every
        // amount on this device flips the unit -- so that is not cosmetic:
        // built first it rendered perfectly and swallowed every press. Fourth
        // time in this file. The walk taps it now.
        if (tot_w) {
            lv_obj_t *tot = sg_lbl(s_scr, intot, tot_x, tot_y,
                                   wt_font_mono23(), INK_COL);
            wt_denom_bind(tot);
        }

        // The read-to-the-end gate, unchanged in every respect that matters:
        // the same question, measured the same way, with the same answer. Only
        // the object it is asked of moved, from a panel of addresses to the
        // graph's output column.
        //
        // It is why outputs are never elided. A destination folded into a group
        // strand would be a recipient hidden where no scroll can reveal it and
        // no page lists it, which is the exact failure this gate was built to
        // stop -- one honest destination on top and a second one under it,
        // signed on a glance.
        lv_obj_t *ocol = wt_bundle_outputs(bg);
        if (!ocol || lv_obj_get_scroll_bottom(ocol) <= 0)
            s_recip_seen = true;            // nothing hidden: nothing to demand
        else if (!s_recip_seen)
            lv_obj_add_event_cb(ocol, recip_scroll_cb, LV_EVENT_SCROLL, NULL);

        // The address, under the graph rather than inside a panel: the strand
        // above it is where the money goes, and this is the name of the place.
        // With several recipients the graph runs to 366, so the pair and its
        // chip sit under it rather than through it. One recipient and the graph
        // stops at 290, leaving the caption line and the card below it.
        const int ay = np ? 292 : (recipient_n > 1 ? 372 : 300);
        // Caption and its "?" on the left, the network and RBF pair right
        // aligned on the same line, and the address on the whole lane beneath.
        //
        // Each "?" sits against the thing it answers, which is the rule this
        // screen has broken twice: the chip parked at 738 answered RBF while
        // touching a FULL ADDRESS control, then answered the address while
        // touching the meta row. The address chip is 8px from the word
        // "recipient address" now, and the RBF chip is past the end of the pair.
        // With more than one recipient every address is in its own row above,
        // so the caption names nothing and goes -- and the address chip goes
        // with it, because each row is its own control.
        // The one destination, settled BEFORE the caption row is built. The
        // chip below reads it and the card behind the "?" reads it again, and
        // the loop that draws the address itself is further down -- setting it
        // there left the chip a screen behind, showing the previous
        // transaction's answer about this one's address.
        s_addr_known = false;
        s_addr_help[0] = 0;
        for (int i = 0; recipient_n == 1 && i < (int)s_sum.n_out
                        && i < WPSBT_MAX_OUTS; i++)
            if (!s_sum.outs[i].is_change) {
                snprintf(s_addr_help, sizeof s_addr_help, "%s",
                         s_sum.outs[i].addr);
                s_addr_known = kiss_payee_seen(s_sum.outs[i].addr);
                break;
            }

        if (recipient_n == 1) {
            lv_obj_t *arow = lv_obj_create(s_scr);
            lv_obj_remove_style_all(arow);
            lv_obj_set_pos(arow, 24, ay - 6);
            lv_obj_set_size(arow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_remove_flag(arow, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_flex_flow(arow, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(arow, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(arow, 8, 0);
            lv_obj_t *acap = lv_label_create(arow);
            lv_label_set_text(acap, tr(STR_S_SENDING_OUT));
            lv_obj_set_style_text_font(acap, wt_font14(), 0);
            lv_obj_set_style_text_color(acap, MUT_COL, 0);
            // The recognition chip, between the caption and its "?", so the
            // mark and the thing that explains it are one reach apart. Only
            // when the destination is known: see kiss_payee.h on why a first
            // payment is silent.
            if (s_addr_known) {
                char kbuf[64];
                snprintf(kbuf, sizeof kbuf, LV_SYMBOL_REFRESH "  %s",
                         tr(STR_S_PAYEE_SEEN));
                wt_chip(arow, kbuf, false);
            }
            wt_help_chip(arow, 0, 0, MUT_COL, addr_help_cb, NULL);
            // The row is LV_SIZE_CONTENT, so a chip in it makes it TALLER than
            // the caption alone -- and it sits directly above the address card.
            // Parked at a fixed ay - 6 it grew down THROUGH the card's top
            // edge: 294 plus a 31px chip row is 325 against a card starting at
            // 316. Measure it and hang it off the card instead, so the row's
            // BOTTOM is what stays put and the caption rises when a chip
            // arrives rather than the card being covered.
            lv_obj_update_layout(arow);
            lv_obj_set_y(arow, ay + 16 - lv_obj_get_height(arow) - 6);
        }
        // RBF's own chip is built AFTER the address, at the end of this block.
        // The address is a control now and its box is the whole 752 lane; a chip
        // built before it is an earlier sibling under a later one that covers
        // the band, so it draws in the right place and every press goes to the
        // address. That is the third time this file has made exactly this
        // mistake -- the coins chip under the graph, the caution chip under the
        // bar, and now this -- and no frame can show any of them, because the
        // chip is right there and simply does nothing.

        for (int i = 0; recipient_n == 1 && i < (int)s_sum.n_out
                        && i < WPSBT_MAX_OUTS; i++) {
            if (s_sum.outs[i].is_change) continue;
            // THE WHOLE ADDRESS, never a fold, and ONE form for every length.
            // It used to arrive folded to eight characters with the rest behind
            // a control labelled FULL ADDRESS -- which reads as a heading and
            // not as a button, and left the screen whose one job is catching a
            // swapped destination showing a fifth of the destination.
            //
            // Blocked in fours at mono14 with the tail lifted to mono23. The
            // two measurements that decide this are already recorded on
            // wt_addr_spans_lift and are the reason it can be one branch
            // instead of two: grouped at mono14 a 42 character bech32 is 438px
            // and a 117 character silent payment is 722px, so BOTH are one line
            // in this 752 lane. The mono23 form they replace was 739px for the
            // bech32 alone and had no answer at all for the long one except a
            // second rendering, which meant an owner compared against whichever
            // of two shapes the transaction happened to produce.
            //
            // It also gives the band back. This line, the caption above it and
            // the meta row below shared the 84px between the graph and the
            // action bar, and the mono23 line was taking most of it -- which is
            // what "the address takes the whole bottom" means from the bench.
            //
            // Groups of four is what Coldcard and Sparrow both moved to, and
            // the lift is what keeps the compared run readable at this size.
            // The whole lane in BOTH layouts: only the CAPTION row has to stop
            // short of the meta text beside it.
            // A CARD, and the card is the control -- the same object RECEIVE
            // draws its address in, at the same font, with the same fold and
            // the same caption under it. Owner's call, and the point of it is
            // that the two address screens are one habit rather than two.
            //
            // Stated once and left here, because it is the reason this was not
            // the default: a RECEIVE address is derived on this device and
            // nobody else picks it, while a destination is chosen by whoever
            // built the transaction. A fixed prefix and suffix is the pattern
            // an attacker grinds a lookalike against -- EthClipper, DSN 2022,
            // arXiv:2108.14004, which measured roughly even odds from matching
            // about a quarter of the characters. Every character is one tap
            // away on the card this opens, and the "?" beside the caption says
            // to compare the lit run.
            //
            // Cautioned, the card frame goes and the fold stays. The band
            // between the graph and the caution bar is 62px and the card is 66:
            // what a flagged transaction spends its frame budget on is the
            // flag. Same fold, same font, same target, no box around it.
            snprintf(s_addr_help, sizeof s_addr_help, "%s", s_sum.outs[i].addr);
            lv_obj_t *box = s_scr;
            if (!np) {
                box = wt_card(s_scr, 24, ay + 16, 752, ADDR_CARD_H);
                lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
                wt_tap_feedback(box);
                lv_obj_add_event_cb(box, addr_tap_cb, LV_EVENT_CLICKED,
                                    (void *)s_sum.outs[i].addr);
            }
            // mono28 on the card, mono23 without one. The destination is the
            // second thing this screen is about -- the first is how much, and
            // nothing else here competes -- so it takes the rung under the
            // hero rather than sharing the graph's. Cautioned there is no card:
            // that band is 62px and belongs to the flag, and the fold stays at
            // mono23 in it. The fold is the same in both, so the run being
            // compared is the same run at either size.
            lv_obj_t *ad = wt_addr_short(box, s_sum.outs[i].addr,
                                         np ? wt_font_mono23()
                                            : wt_font_mono28());
            if (np) {
                lv_obj_set_pos(ad, 24, ay + 18);
                lv_obj_add_flag(ad, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_ext_click_area(ad, 8);
                lv_obj_add_event_cb(ad, addr_tap_cb, LV_EVENT_CLICKED,
                                    (void *)s_sum.outs[i].addr);
                break;
            }
            lv_obj_t *cmp = wt_lbl(box, tr(STR_S_CMP_8), 14, 0,
                                   wt_font14(), MUT_COL);
            // Block centred in a fixed height card, the same arithmetic
            // recv_refresh uses: top aligning would leave one line floating in
            // a box sized for the taller state.
            lv_obj_update_layout(ad);
            lv_obj_update_layout(cmp);
            int ah = lv_obj_get_height(ad), ch = lv_obj_get_height(cmp);
            int top = (ADDR_CARD_H - (ah + 6 + ch)) / 2;
            if (top < 8) top = 8;
            lv_obj_set_pos(ad,  14, top);
            lv_obj_set_pos(cmp, 14, top + ah + 6);
            break;
        }

        // RBF's own chip, past the right end of the pair it explains, and built
        // LAST so it sits above the address lane. This is the answer to "where
        // is the RBF option": there is no option, and the card behind this chip
        // is where that gets said.
        wt_help_chip(s_scr, 738, ay - 6, MUT_COL, rbf_help_cb, NULL);
    }

    {
        // ---- the meta row ------------------------------------------------
        // Which network these coins are real on, and what the coordinator asked
        // for about replacing the transaction. The fee is a strand now, with its
        // own thickness, so the cell that used to print it is the one thing the
        // row does not carry.
        //
        // ON THE ADDRESS CAPTION'S LINE, right aligned, in BOTH layouts. It used
        // to have a band of its own at y=366, which the cautioned screen has no
        // room for -- so on a cautioned transaction the whole RBF half was
        // simply dropped, and the fact went missing at exactly the moment the
        // transaction became interesting enough to ask about. That is the bug
        // behind "where is RBF". Sharing the caption's line costs nothing: the
        // caption is one short phrase on the left and the address has the whole
        // lane below it either way.
        // With several recipients the graph runs to 366, so the pair and its
        // chip sit under it rather than through it. One recipient and the graph
        // stops at 290, leaving the caption line and the card below it.
        const int ay = np ? 292 : (recipient_n > 1 ? 372 : 300);
        const char *net = !s_sum.testnet             ? tr(STR_I_NET_MAIN)
                        : s_sum.net == KISS_NET_SIGNET ? tr(STR_I_NET_SIGNET)
                                                       : tr(STR_I_NET_TEST);
        const char *rbf = s_sum.rbf ? tr_sym(WT_ICON_REPLACE, STR_S_RBF_T_ON)
                                    : tr_sym(WT_ICON_LOCK, STR_S_RBF_T_OFF);
        snprintf(buf, sizeof buf, "%s  ·  %s", net, rbf);
        // Amber on testnet: the network is a status, not chrome, and it is the
        // one fact on this row that changes what a signature is worth.
        //
        // 364..724 right aligned, against a caption that starts at 24 and is one
        // phrase long. 360px is more than the pair measures in any locale and
        // the ellipsis is the backstop rather than the plan.
        // With several recipients nothing shares this line, so it gets the lane
        // from 24 rather than the 360 it leaves the caption. And the HEIGHT is
        // pinned to one line in every case: LONG_DOT on a sized label wraps
        // FIRST and ellipsises second, so eleven locales put a second line at
        // y=409 -- 12px past WT_CONTENT_BOTTOM, on the layout where this row is
        // lowest. Pinned, the ellipsis is what happens instead.
        const bool wide = !np && recipient_n > 1;
        lv_obj_t *m = sg_lbl(s_scr, buf, wide ? 24 : 364, ay, wt_font14(),
                             s_sum.testnet ? WARN_COL : MUT_COL);
        lv_obj_set_width(m, wide ? 700 : 360);
        lv_obj_set_height(m, lv_font_get_line_height(wt_font14()));
        lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(m, LV_LABEL_LONG_DOT);
    }

    if (np) wt_help_chip(s_scr, 738, 108, WARN_COL, caution_help_cb, NULL);

    // ---- the action row --------------------------------------------------
    // HOLD TO SIGN never moves, never changes label, and never changes width.
    // Acknowledgement lives in the caution rows now, so there is no second
    // button competing for this position and no way for two taps in the same
    // place to become a signature nobody read.
    s_inert[0] = wt_pillh(s_scr, tr(STR_C_BACK), SG_BACK_X, WT_ACTION_Y, 104,
                          WT_ACTION_H,
                          s_src == SRC_SD ? files_back_cb : choose_back_cb, NULL);
    s_inert[1] = wt_pillh(s_scr, tr(STR_S_DETAILS), SG_DETAILS_X, WT_ACTION_Y,
                          150, WT_ACTION_H, details_cb, NULL);
    s_inert[2] = NULL;

    // There used to be a 40x40 ring here, built a few lines before the pill and
    // placed at (56, 410) -- inside a pill at (48, 404) 310x52 whose background
    // is LV_OPA_COVER. Same parent, created first, so the pill painted over the
    // whole of it. It never drew a pixel, in any theme, from the day it was
    // added, and a pixel scan of its own rect returns nothing but the sweep and
    // one letter of the label.
    //
    // Not restored to the foreground: it would land on the label's first
    // character, and it would be a second reading of the number the sweep
    // already draws across the whole button. One control, one reading.
    lv_obj_t *p = wt_pillh(s_scr, tr(STR_S_HOLD_TO_SIGN), SG_HOLD_X, WT_ACTION_Y,
                           SG_HOLD_W, WT_ACTION_H, NULL, NULL);
    wt_pill_label_max(p);          // the most consequential button in the app
    s_sign_lbl = lv_obj_get_child(p, 0);
    // One expression, read twice. Writing the condition out again for the test
    // seam let the seam keep reporting "inert" after the gate itself had been
    // deleted -- the self test passed against a build with no gate in it.
    const bool armed = !((np && !s_ack) || !s_recip_seen);
#ifndef ESP_PLATFORM
    s_armed = armed;
#endif
    if (!armed) {
        // Present, in place, and visibly inert. Disabled ink rather than a
        // hidden or moved button, so the owner can see what acknowledging the
        // rows above is going to unlock. No accent here on purpose: the accent
        // is this app's "press this one" marker, so wearing it while inert
        // would be a lie.
        lv_obj_set_style_border_color(p, WT_EDGE, 0);
        lv_obj_set_style_text_color(s_sign_lbl, WT_DIM, 0);
    } else {
        lv_obj_add_event_cb(p, sign_press_cb, LV_EVENT_ALL, NULL);
        // The same primary marker every other screen's suggested action wears,
        // rather than a bare 1px accent border invented here: 2px, an accent
        // tinted fill, a pressed fill the hold can be felt against, and the top
        // label rung. ADDENDUM-02 asks for this, and the reason is that a
        // hand rolled variant of the app's loudest affordance is exactly the
        // kind of near miss the redraw is meant to remove.
        wt_pill_primary(p);

        // The sweep, the third reading of the hold. Built the way wt_hold_pill
        // builds its own -- a background child grown from zero, under the label
        // LVGL has already made -- but in the ACCENT and not WT_STOP. On this
        // device a red sweep under a pill means a destructive hold, wipe or
        // reset, and signing is neither. Red here would code the safest hold in
        // the app as the most dangerous one. At 90 of 255 over the pill's own
        // accent tinted fill it reads as the press deepening across the button.
        lv_obj_t *f = lv_obj_create(p);
        lv_obj_remove_style_all(f);
        lv_obj_set_size(f, 0, WT_ACTION_H);
        lv_obj_set_pos(f, 0, 0);
        lv_obj_set_style_radius(f, 10, 0);          // matches the pill it crosses
        lv_obj_set_style_bg_color(f, wt_accent(), 0);
        lv_obj_set_style_bg_opa(f, 90, 0);
        lv_obj_add_flag(f, WT_FLAG_ACCENT_FILL);
        lv_obj_remove_flag(f, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_background(f);
        s_sweep = f;
    }
}

// ---- DETAILS: the second page for people who want the raw facts. One page,
// one tap in, one tap back — the verify screen stays simple. ----
static void details_back_cb(lv_event_t *e)
{
    (void)e;
    repaint_verify();
}

static void gloss_back_cb(lv_event_t *e) { details_cb(e); }

static void glossary_cb(lv_event_t *e)
{
    (void)e;
    // A PAGE now, not an overlay card. Eight terms is a reference, and a
    // reference read through a dimmed backdrop over the page you were on is a
    // thing you dismiss rather than a thing you read: the card had to squeeze
    // all eight into the room left under a floating title, which is what put
    // the definitions on the smallest rung the device has.
    //
    // Still no new string. One `term: definition` per line is how every locale
    // already writes S_GLOSSARY_B, wt_split_colon reads it, and GLOSS_ICONS is
    // still one glyph per line in the same order. Only where the cells land
    // changed.
    lv_obj_t *parent = lv_obj_get_parent(s_scr);
    lv_obj_delete(s_scr); s_scr = NULL; s_sign_lbl = NULL;
    s_graph = NULL; s_graph_cap = NULL; s_locked = NULL;
    s_inert[0] = NULL; s_sweep = NULL;
    mk_screen(parent, tr(STR_S_GLOSSARY_T), NULL);

    wt_card(s_scr, 24, 88, 752, 290);
    sg_rule(400, 104, 1, 258);

    const char *p = tr(STR_S_GLOSSARY_B);
    for (int i = 0; i < 8 && p && *p; i++) {
        char head[64], line[200];
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = 0;
        const char *def = wt_split_colon(line, head, sizeof head);

        const int col = i / 4;                  // four down the left, four right
        const int x   = col ? 424 : 40;
        const int w   = col ? 328 : 344;
        const int y   = 104 + (i % 4) * 68;

        // The mark first, in the accent, and flagged so it survives a theme
        // change: these are the same eight glyphs the detail rows wear, which
        // is how a reader meets a concept's mark before its word.
        lv_obj_t *ic = mk_lbl(GLOSS_ICONS[i], x, y, wt_font14(), wt_accent());
        lv_obj_add_flag(ic, WT_FLAG_ACCENT);
        lv_obj_t *tm = mk_lbl(head, x + 26, y, wt_font14(), INK_COL);
        lv_obj_set_style_text_letter_space(tm, 2, 0);
        // 60 of the 68 pitch, so a long definition ellipsises inside its own
        // cell instead of growing into the term under it.
        lv_obj_t *dl = mk_lbl(def ? def : "", x, y + 20, wt_font14(), MUT_COL);
        lv_obj_set_width(dl, w);
        lv_label_set_long_mode(dl, LV_LABEL_LONG_DOT);
        lv_obj_set_height(dl, 44);

        p = nl ? nl + 1 : NULL;
    }
    mk_pill(tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, gloss_back_cb);
}

// Measured height of a label that was just built, so the next thing can go
// under it. LVGL sizes a wrapped label lazily; without the update the height is
// whatever it was before the text landed.
static int det_h(lv_obj_t *o)
{
    lv_obj_update_layout(o);
    return lv_obj_get_height(o);
}

// One flag row: an icon, the value beside it in ink. Advances *y past whatever
// it used. `tail` used to be the explainer sentence under the value; every row
// carries a "?" whose card is that sentence, so no row draws it twice anymore
// and the parameter survives for the day one locale genuinely needs the text
// on the page.
//
// `floor_y` is the last y this row may touch, and it is what keeps the strip
// safe in twenty locales rather than in the one it was measured in. A clipped
// sentence is bad. A sentence drawn over the BACK pill, on the page whose job
// is telling you what you are about to sign, is worse.
//
// The fee row used to carry a single chip whose card answered fee rate,
// version, locktime and sighash together -- four questions behind one mark, so
// a reader who did not know what sighash meant had to open a card about the fee
// and find it in there. Each row answers for itself now, and a reader taps the
// word they do not know.
//
// The bodies are the SAME strings the rows are built from, split at the colon
// every locale already writes: the row shows the term and its short form, the
// card shows the term and the whole of it. Nothing new to translate. The fee
// rate has no `TERM: definition` string of its own, so it borrows the
// glossary's, which is where a reader would have gone looking anyway.
enum { DT_FEE = 0, DT_LOCKTIME, DT_SIGHASH, DT_RBF, DT_TXID };

static void det_term_cb(lv_event_t *e)
{
    const int which = (int)(uintptr_t)lv_event_get_user_data(e);
    static char head[64];
    const char *body = NULL, *icon = NULL;
    wpsbt_details_t det;
    const bool have = (kiss_psbt_details(&det) == 0);

    switch (which) {
    case DT_LOCKTIME:
        body = wt_split_colon(have && det.locktime ? tr(STR_S_D_LT_NONZERO)
                                                   : tr(STR_S_D_LT_ZERO),
                              head, sizeof head);
        icon = WT_ICON_LOCK;
        break;
    case DT_TXID:
        // The note that used to sit under the id, in the column. It is an
        // explanation, and this page keeps its explanations behind a "?" --
        // the id is the fact, whether it survives signing is the lesson.
        body = det.txid_final ? tr(STR_S_D_TXID_SAME) : tr(STR_S_D_TXID_CHANGES);
        snprintf(head, sizeof head, "%s", tr(STR_S_D_TXID));
        icon = GLOSS_ICONS[3];
        break;
    case DT_SIGHASH:
        body = wt_split_colon(tr(STR_S_D_SIGHASH), head, sizeof head);
        icon = LV_SYMBOL_OK;
        break;
    case DT_RBF:
        body = wt_split_colon(s_sum.rbf ? tr(STR_S_D_RBF_ON)
                                        : tr(STR_S_D_RBF_OFF),
                              head, sizeof head);
        icon = s_sum.rbf ? WT_ICON_REPLACE : WT_ICON_LOCK;
        break;
    default:
        body = gloss_line(4, head, sizeof head);      // FEE RATE
        icon = LV_SYMBOL_CUT;
        break;
    }
    wt_explain_t x = {
        .title  = head,
        .icon   = icon,
        .body   = body ? body : "",
        .ok_txt = tr(STR_C_OK),
    };
    wt_explain_open(s_scr, &x);
}

static void det_flag_row(int x, int *y, const char *icon, const char *head,
                         const char *tail, int w, int floor_y)
{
    const int IW = 26;              // icon column, generous enough for the widest
    if (*y + 18 > floor_y) return;
    // Accent, not MUT: these four icons are the only marks on the page that
    // are pure decoration (the input-row OK/eye-slash are STATUS and keep
    // their verdict colours per ADDENDUM-02), and grey-on-grey hid them.
    wt_lbl(s_scr, icon, x, *y + 2, wt_font14(), wt_accent());

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
    wt_denom_on_tap(denom_tap_details);   // a figure tapped here rebuilds here
    wpsbt_details_t det;
    if (kiss_psbt_details(&det) != 0)
        return;
    hold_stop();
    lv_obj_delete_async(s_scr); s_scr = NULL; s_sign_lbl = NULL;
    s_graph = NULL; s_graph_cap = NULL; s_locked = NULL;
    s_inert[0] = NULL; s_sweep = NULL;
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
    // Two thirds to the left. What this transaction spends and where it goes
    // is the reason anyone opens this page; the right column is reference --
    // an id you compare, and four flags that each fit on a line. The split ran
    // the other way (288/424) and the important column was the narrow one, so
    // every address in it was folded to eight characters for want of room the
    // reference column was not using.
    wt_card(s_scr, 28, 100, 428, 296);
    wt_card(s_scr, 468, 100, 284, 296);

    lv_obj_t *ihdr = wt_section(s_scr, buf, 40, 108);
    // Bounded to the left column. STR_S_D_MANYIN_FMT is a sentence, not a
    // word, and in Spanish it ran straight across into the TXID caption in the
    // right column. It was font14 and unbounded before, which only hid the
    // fault behind a smaller face.
    lv_obj_set_width(ihdr, 360);       // 40px kept clear for the chip beside it
    lv_label_set_long_mode(ihdr, LV_LABEL_LONG_WRAP);
    // The right column gives every one of its four facts a "?". The left one
    // gave its two lists none, and the lists are the harder half: an amount,
    // then two lines of hex and a path under it, with nothing on screen saying
    // what either is. Both chips open the page that names them, and every mark
    // used in the rows below is defined on it.
    wt_help_chip(s_scr, 416, 106, MUT_COL, glossary_cb, NULL);

    lv_obj_t *il = lv_obj_create(s_scr);
    lv_obj_remove_style_all(il);
    // Start the list under whatever the header actually became. This used to be
    // a two-way guess (130, or 158 when the header wrapped), which was already
    // wrong for a locale that took three lines and is certainly wrong now the
    // lane is 288 rather than 372. Measure it instead.
    int ly = 108 + det_h(ihdr) + 8;
    lv_obj_set_pos(il, 40, ly);
    // Half the card, not all of it: the outputs list takes the other half. It
    // is here rather than in the right card because the right card ends at 388
    // already, and because a list of amounts belongs beside the other list of
    // amounts -- the page then reads in one direction, what this spends and
    // where it goes, exactly as the graph does.
    // The divide is measured from where the header actually ended, not fixed:
    // this header is one line for most transactions and three for one with more
    // inputs than the page can list, and a constant split cut an input row in
    // half through the middle of its txid. One whole row is the floor, and the
    // outputs list keeps what is left. Both scroll and both say so, so a short
    // list is a short list rather than a hidden one.
    const int split = ly + 84 < 232 ? 232 : (ly + 84 > 272 ? 272 : ly + 84);
    lv_obj_set_size(il, 404, split - 6 - ly);
    lv_obj_set_style_pad_all(il, 8, 0);
    lv_obj_set_style_pad_row(il, 4, 0);
    lv_obj_set_flex_flow(il, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(il, LV_DIR_VER);
    // MODE_ON past two inputs, same rule and same 5px bar as the verify
    // screen's output list: a list with more below the fold must not look
    // identical to one that ends there. AUTO hid the bar until the owner had
    // already scrolled, so a five input transaction read as a two input one --
    // on the page whose whole job is saying what the transaction spends.
    lv_obj_set_scrollbar_mode(il, det.n_in > 2 ? LV_SCROLLBAR_MODE_ON
                                               : LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(il, 5, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(il, MUT_COL, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(il, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(il, LV_OPA_TRANSP, 0);
    for (uint32_t i = 0; i < det.n_in; i++) {
        lv_obj_t *row = lv_obj_create(il);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_bottom(row, 10, 0);

        wt_fmt_amount(det.ins[i].sats, a, sizeof a);
        // The verify screen can only say "one of these amounts is not proven".
        // This is the page that says WHICH, so the mark leads the number and the
        // number wears the doubt: a tick when a previous transaction hashing to
        // this outpoint vouched for it, an eye-slash in WARN when the amount is
        // only what the coordinator claimed. Both glyphs are already in SYMS.
        bool ok = det.ins[i].proven;
        snprintf(buf, sizeof buf, "%s %s %s",
                 ok ? LV_SYMBOL_OK : WT_ICON_HIDDEN, a, wt_denom_unit());
        lv_obj_t *amt = lv_label_create(row);
        lv_label_set_text(amt, buf);
        lv_obj_set_style_text_color(amt, ok ? INK_COL : WARN_COL, 0);
        // The amount leads the row at 23 and the txid trails it at 14. That is
        // the whole fix for this list: what is being spent is the fact, and the
        // coin it came from is the reference you check it against.
        lv_obj_set_style_text_font(amt, wt_font23(), 0);
        wt_denom_bind(amt);

        // coin being spent: first 8 + last 8 of its txid, and the output index.
        // The mark is the glossary's own TXID icon, so the line says what it is
        // without a word of label, and the card one tap above names the mark.
        snprintf(buf, sizeof buf, "%s %.8s...%s : %u", GLOSS_ICONS[3],
                 det.ins[i].txid, det.ins[i].txid + 56, (unsigned)det.ins[i].vout);
        lv_obj_t *tid = lv_label_create(row);
        lv_label_set_text(tid, buf);
        lv_obj_set_style_text_color(tid, MUT_COL, 0);
        lv_obj_set_style_text_font(tid, wt_font14(), 0);

        // BIP376 received-SP input: its key is spend+tweak, not a BIP84 child,
        // so show the silent-payment badge instead of a misleading BIP32 path.
        // The folder is the glossary's DERIVATION PATH mark. It used to be a
        // second tick, one line under the tick on the amount, which said
        // "verified" twice and what the line was not at all. The colour still
        // carries ours.
        if (det.ins[i].is_sp)
            snprintf(buf, sizeof buf, "%s m/352'/%d'/0'   %s", GLOSS_ICONS[6],
                     s_sum.testnet ? 1 : 0, tr(STR_S_SP_BADGE));
        else
            snprintf(buf, sizeof buf, "%s m/%u'/%d'/0'/%u/%u", GLOSS_ICONS[6],
                     (unsigned)det.ins[i].purpose, s_sum.testnet ? 1 : 0,
                     (unsigned)det.ins[i].change, (unsigned)det.ins[i].index);
        lv_obj_t *pl = lv_label_create(row);
        lv_label_set_text(pl, buf);
        lv_obj_set_style_text_color(pl, OK_COL, 0);
        lv_obj_set_style_text_font(pl, wt_font14(), 0);
    }

    // ---- the outputs, under the inputs ----
    // Every destination readable somewhere that does not scroll under a gate.
    // The verify screen's column can hold a recipient below its fold, and
    // HOLD TO SIGN stays inert until it has been read -- but that is a gate on
    // signing, not a place to look things up. This is the place.
    sg_rule(40, split, 404, 1);
    uint32_t n_ours = 0;
    for (int i = 0; i < (int)s_sum.n_out && i < WPSBT_MAX_OUTS; i++)
        if (s_sum.outs[i].is_change) n_ours++;
    snprintf(buf, sizeof buf, tr(STR_S_D_OUTPUTS_FMT),
             (unsigned)s_sum.n_out, (unsigned)n_ours);
    lv_obj_t *ohdr = wt_section(s_scr, buf, 40, split + 10);
    lv_obj_set_width(ohdr, 360);
    lv_label_set_long_mode(ohdr, LV_LABEL_LONG_WRAP);
    wt_help_chip(s_scr, 416, split + 8, MUT_COL, glossary_cb, NULL);

    lv_obj_t *ol = lv_obj_create(s_scr);
    lv_obj_remove_style_all(ol);
    int oy = split + 10 + det_h(ohdr) + 6;
    lv_obj_set_pos(ol, 40, oy);
    lv_obj_set_size(ol, 404, 392 - oy);
    lv_obj_set_style_pad_all(ol, 8, 0);
    lv_obj_set_style_pad_row(ol, 4, 0);
    lv_obj_set_flex_flow(ol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(ol, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ol, s_sum.n_out > 1 ? LV_SCROLLBAR_MODE_ON
                                                  : LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(ol, 5, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(ol, MUT_COL, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(ol, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(ol, LV_OPA_TRANSP, 0);
    for (int i = 0; i < (int)s_sum.n_out && i < WPSBT_MAX_OUTS; i++) {
        lv_obj_t *row = lv_obj_create(ol);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_bottom(row, 10, 0);

        wt_fmt_amount(s_sum.outs[i].sats, a, sizeof a);
        // The tick is the change output's own claim -- re-derived and verified
        // on this device -- and it is WT_OK because that is a status, not the
        // accent. A recipient gets no tick: the signer has nothing to vouch for
        // about someone else's address, and a mark there would say it did.
        const bool ours = s_sum.outs[i].is_change;
        // The glossary's CHANGE mark, not a bare tick: on a list of outputs the
        // question is WHICH of them comes back, and a tick answered "this one
        // is fine". Green still says verified ours.
        if (ours) snprintf(buf, sizeof buf, "%s %s %s", GLOSS_ICONS[2], a,
                           wt_denom_unit());
        else      snprintf(buf, sizeof buf, "%s %s", a, wt_denom_unit());
        lv_obj_t *amt = lv_label_create(row);
        lv_label_set_text(amt, buf);
        lv_obj_set_style_text_color(amt, ours ? OK_COL : INK_COL, 0);
        lv_obj_set_style_text_font(amt, wt_font23(), 0);
        wt_denom_bind(amt);
        // The fold, not the whole address: this list's job is "one line of
        // facts per output", and the full form is a 248px wall of mono14 that
        // pushed the eighth output off the fold. The last eight still light,
        // the same rule every other screen here teaches.
        //
        // And the row OPENS the whole thing, so the fold is a summary with the
        // full form behind it rather than a truncation with nothing behind it.
        // That distinction is the one the EthClipper work is about: a fixed
        // prefix and suffix is what a lookalike gets ground against, so the
        // characters it drops have to stay reachable from the place they were
        // dropped. Two taps from the graph to every character of any output,
        // change included -- which the verify screen cannot show at all.
        lv_obj_t *ao = wt_addr_short(row, s_sum.outs[i].addr, wt_font14());
        lv_obj_add_flag(ao, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(ao, 8);
        lv_obj_add_event_cb(ao, addr_tap_cb, LV_EVENT_CLICKED,
                            (void *)s_sum.outs[i].addr);
        // A silent payment output's on-chain address is not the one handed
        // over: the graph used to carry this claim and lost it because a
        // paragraph costs the column a row. Here it is beside the very address
        // it is about, on the page a reader comes to for the raw facts.
        if (s_sum.outs[i].is_sp) {
            lv_obj_t *spn = lv_label_create(row);
            lv_label_set_text(spn, sp_onchain_note());
            lv_obj_set_style_text_color(spn, MUT_COL, 0);
            lv_obj_set_style_text_font(spn, wt_font14(), 0);
            lv_obj_set_width(spn, 248);
            lv_label_set_long_mode(spn, LV_LABEL_LONG_WRAP);
        }
    }

    // ---- the right column ----
    // Every element here used to sit on a hand measured y, and the seven of them
    // were all font14 and all MUT_COL: no hierarchy, so a reader had to read the
    // whole column to find any one fact in it. They are now placed by a CURSOR,
    // each one measured after it is built and the next one put under it. That is
    // what makes the hierarchy affordable — a heading line costs 17px, and seven
    // fixed y values had no 17px anywhere to give.
    const int RX = 480, RW = 252;    // inside the 468..752 card, 12 of padding
    const int RFLOOR = 388;          // the card's own floor, 8 above its edge
    int ry = 108;

    // the id to find it by, once broadcast — final only for segwit-only spends
    wt_section(s_scr, tr(STR_S_D_TXID), RX, ry);
    wt_help_chip(s_scr, RX + RW - 26, ry - 2, MUT_COL, det_term_cb,
                 (void *)(uintptr_t)DT_TXID);
    ry += 20;
    char gt[80];
    group4(det.txid, gt, sizeof gt);
    lv_obj_t *tx = mk_lbl(gt, RX, ry, wt_font14(), INK_COL);
    // 34 clear on the right, the same as every flag row below: the heading's
    // chip hangs into this block's first line otherwise, which the overlap
    // gate caught at 8x3 px in all 21 locales.
    lv_obj_set_width(tx, RW - 34);
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

    // The note about whether this id survives signing is behind the chip on
    // the heading now. It was two wrapped lines of grey under a block of hex
    // that is already three, and it pushed the fourth flag row off the card's
    // floor when this column narrowed to give the lists the room they needed.
    ry += 6;
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
    wt_fmt_amount_alt(hero_sats(), gt, sizeof gt);   // the hero, in the other unit
    snprintf(buf, sizeof buf, "= %s %s", gt, wt_denom_unit_alt());
    // 23, and INK. This is the number a holder reads off the glass and compares
    // against the coordinator, which is the entire reason the BTC form is here
    // at all. It was the same size and the same grey as the locktime note.
    lv_obj_t *bt = mk_lbl(buf, RX, ry, wt_font23(), INK_COL);
    wt_denom_bind(bt);
    ry += det_h(bt) + 10;

    // ---- the flag rows ----
    // "is this transaction normal" has exactly three answers on this device, and
    // they were three more grey sentences in the same stack as everything else.
    // Each is written `head: tail` in all 21 locales, so the head becomes the
    // VALUE, in ink beside an icon, and the tail becomes the note under it. No
    // new string anywhere: wt_split_colon reads the shape the translators
    // already wrote, wide colon and French spacing included.
    //
    // The tail stayed ON the row for a long time, and it is gone now. Every row
    // keeps a "?" at its right edge whose card answers for itself -- the same
    // head:tail string, in full -- and a note that duplicates its own card is a
    // note that costs the column a line in twenty locales for text the reader
    // must already have opened to learn anything from. The row shows the fact;
    // the "?" shows why it matters.
    //
    // The version and locktime numbers ride on the locktime row's head instead
    // of a line of their own, and the head the locale wrote for that row is
    // dropped: "locktime 0" beside "version 2, locktime 0" is the same value
    // printed twice.
    char sh_head[64], rbf_head[64];
    snprintf(buf, sizeof buf, tr(STR_S_D_VER_LT_FMT),
             (unsigned)det.version, (unsigned)det.locktime);
    wt_split_colon(tr(STR_S_D_SIGHASH), sh_head, sizeof sh_head);
    wt_split_colon(s_sum.rbf ? tr(STR_S_D_RBF_ON) : tr(STR_S_D_RBF_OFF),
                   rbf_head, sizeof rbf_head);

    // The fee rate joins the strip rather than floating above it as a loose
    // muted line. It is a property of the transaction exactly like the three
    // below it, and STR_S_FEERATE_PCT_FMT is already a whole sentence, so it
    // takes the head slot with no note under it.
    // The strip's own "?": version, locktime and sighash never made it into
    // the glossary card, and their inline notes are font14 -- the smallest
    // type on the page for the three terms a reader is least likely to know.
    // One card, composed at runtime from the same head:tail strings the rows
    // draw, so it costs no new key in 21 locales. The chip shares the FEE
    // row's line and that row's label lane is narrowed to match -- floated
    // over the strip it collided with the version row's head in all 21
    // locales, which the overlap gate caught before any bench did.
    // Every row keeps 34px clear on its right for its own chip, and each chip is
    // captured against the y the row STARTED at, since det_flag_row advances
    // past whatever the translation needed.
    int chip_y = ry;
    det_flag_row(RX, &ry, LV_SYMBOL_CUT, fee_line, NULL, RW - 34, RFLOOR);
    wt_help_chip(s_scr, RX + RW - 26, chip_y - 2, MUT_COL, det_term_cb,
                 (void *)(uintptr_t)DT_FEE);
    chip_y = ry;
    det_flag_row(RX, &ry, WT_ICON_LOCK, buf, NULL, RW - 34, RFLOOR);
    wt_help_chip(s_scr, RX + RW - 26, chip_y - 2, MUT_COL, det_term_cb,
                 (void *)(uintptr_t)DT_LOCKTIME);
    chip_y = ry;
    det_flag_row(RX, &ry, LV_SYMBOL_OK, sh_head, NULL, RW - 34, RFLOOR);
    wt_help_chip(s_scr, RX + RW - 26, chip_y - 2, MUT_COL, det_term_cb,
                 (void *)(uintptr_t)DT_SIGHASH);
    chip_y = ry;
    // The same mark the RBF explainer wears, so the row and the card that
    // explains it are recognisably about one thing.
    det_flag_row(RX, &ry, s_sum.rbf ? WT_ICON_REPLACE : WT_ICON_LOCK,
                 rbf_head, NULL, RW - 34, RFLOOR);
    wt_help_chip(s_scr, RX + RW - 26, chip_y - 2, MUT_COL, det_term_cb,
                 (void *)(uintptr_t)DT_RBF);

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
    lv_obj_delete(s_scr); s_scr = NULL; s_sign_lbl = NULL;
    s_graph = NULL; s_graph_cap = NULL; s_locked = NULL;
    s_inert[0] = NULL; s_sweep = NULL;

    s_qr_ez = false;
    s_out_len = sw;
    if (qr_enc_start() != 0) {
        mk_screen(parent, tr(STR_S_FAIL_T), NULL);
        fail_body(tr(STR_S_QR_FAIL_ENC));
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
    // The signature fingerprint takes the top slot of the right column. The
    // redundant "OK SIGNED" label that sat here is dropped: the page title and
    // the sub line both already say the transaction is signed, and this is the
    // one place the QR path can show the code without crowding (the card below
    // is 302 square, running to the action band). Same code the SD screen shows.
    // 96 tops the column flush with the QR card beside it; the code at mono23
    // runs to 125, so the part counter drops to 130 and still clears the first
    // note at 168.
    draw_sig_chip(430, 96, true);
    s_part_lbl = mk_lbl(n > 1 ? "" : tr(STR_S_QR_SINGLE), 430, 130,
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
    mk_pill(tr(STR_C_DONE), WT_BACK_X, WT_ACTION_Y, 140, close_cb);
    s_part_i = 0;
    qr_tick(NULL);                               // first part right away
}

// ---- file list ----
static void file_tap_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    snprintf(s_cur, sizeof s_cur, "%s", s_files[idx]);
    // Carried from the row to the verify screen, so the badge the owner just
    // read on the list is still there on the screen that asks them to sign. No
    // second card read: the list already asked.
    bool opened_signed = (idx >= 0 && idx < MAX_FILES && s_sig[idx])
                      || is_signed_name(s_cur);
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
    int lrc = kiss_psbt_load(s_in, len, &s_sum);
    s_ack = false;                         // fresh PSBT: re-acknowledge any caution
    s_ack_flags = 0;
    s_recip_seen = false;                  // ...and read its destinations again
    s_on_cautions = false;
    s_ack_t0 = 0;
    s_cur_signed = opened_signed;
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

// ---- REMOVE SIGNED --------------------------------------------------------
// The card accumulates one -signed.psbt per hold and the list window is 24, so
// the device can clean up after itself rather than telling the owner to go and
// find a computer.
//
// This is a LIST, not a count. The first shape put the number of files in a
// framed card, which was wrong twice over: a card with one signed file shows
// TWO amber rows (the source and its output) beside a confirm that said "1", so
// the two readings looked like a contradiction when both were right, and the
// owner has every reason to want to drop one signature and keep another. Naming
// the files answers both -- there is nothing left to reconcile, and each one has
// its own hold.
//
// Only files named *-signed.psbt are ever listed here, which is the whole safety
// property: an unsigned PSBT cannot appear, so it cannot be picked. The honest
// caveat is that the test is the NAME, so a file someone else called
// quarterly-signed.psbt would be offered too -- which is exactly why they are
// listed by name instead of counted.
#define RM_MAX 16
static char s_rmf[RM_MAX][SD_NAME_LEN];
static int  s_rmn;

static void rm_screen(void);

static void rm_back_cb(lv_event_t *e)
{
    (void)e;
    files_back_cb(NULL);
}

static void rm_repaint(void)
{
    hold_stop();
    lv_obj_delete_async(s_scr); s_scr = NULL; s_sign_lbl = NULL;
    s_graph = NULL; s_graph_cap = NULL; s_locked = NULL;
    s_inert[0] = NULL; s_sweep = NULL;
    rm_screen();
}

static void rm_one(void *ud)
{
    int i = (int)(intptr_t)ud;
    if (i >= 0 && i < s_rmn)
        platform_sd_delete(s_rmf[i]);
    // Re-read the card rather than shuffling the array: the file either went or
    // it did not, and the next screen should say which.
    rm_repaint();
}

static void rm_all(void *ud)
{
    (void)ud;
    platform_sd_signed_scan(NULL, NULL, 0, 1);
    rm_repaint();
}

static void rm_screen(void)
{
    int total = 0;
    // Every .psbt on the card, not only our own -signed outputs. The old
    // signed-only list made this screen safe by construction and useless for
    // the other half of the job: a card full of stale unsigned drafts had no
    // way to be cleaned except a computer. Safety moved into the gesture --
    // each row is its own 1200ms hold -- and REMOVE ALL below stays scoped
    // to signed files, where a sweep cannot destroy unsigned work.
    s_rmn = platform_sd_list_psbt(s_rmf, RM_MAX, &total);
    if (s_rmn <= 0) {                      // nothing left: the job is done
        files_back_cb(NULL);
        return;
    }

    mk_screen(s_parent, tr(STR_S_RM_SIGNED), NULL);
    // Both claims, above the list they describe. The second one is the reason
    // this screen is safe and it is the sentence that has to be here.
    lv_obj_t *h = mk_lbl(tr(STR_S_RM_C_B), 48, 74, wt_font14(), MUT_COL);
    lv_obj_set_width(h, 704);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);

    lv_obj_t *list = lv_obj_create(s_scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 24, 132);
    // A whole number of rows, not 258px of them: the list can hold every
    // .psbt on the card now, and a fourth row sliced mid-pill by the clip
    // edge read as a rendering fault (the overlap gate flagged the sliver in
    // all 21 locales). Row pitch is WT_ROW_H plus the 8px flex gap.
    lv_obj_set_size(list, 752, 3 * (WT_ROW_H + 8) - 8);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, s_rmn > 3 ? LV_SCROLLBAR_MODE_ON
                                              : LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_width(list, 5, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(list, MUT_COL, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_50, LV_PART_SCROLLBAR);

    for (int i = 0; i < s_rmn; i++) {
        lv_obj_t *row = sg_panel(0, 0, 752, WT_ROW_H, WT_EDGE);
        lv_obj_set_parent(row, list);
        lv_obj_set_width(row, lv_pct(100));
        bool sgn = is_signed_name(s_rmf[i]);
        sg_lbl(row, sgn ? LV_SYMBOL_OK : LV_SYMBOL_FILE, SG_PAD, 20,
               wt_font23(), MUT_COL);
        lv_obj_t *nm = lv_label_create(row);
        lv_obj_set_pos(nm, 52, 22);
        lv_obj_set_style_text_font(nm, wt_font_mono14(), 0);
        lv_obj_set_style_text_color(nm, INK_COL, 0);
        lv_obj_set_width(nm, 470);            // up to the pill's left edge
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_label_set_text(nm, s_rmf[i]);
        // The mark alone, no label: this pill is one of up to sixteen and a
        // translated phrase on each would not fit. wt_hold_pill sweeps WT_STOP
        // across it while held, which is the affordance doing the explaining.
        wt_hold_pill(row, LV_SYMBOL_TRASH, 543, 12, 170, 40, 1200,
                     rm_one, (void *)(intptr_t)i);
    }

    if (total > s_rmn) {                    // more than the screen can hold
        char more[96];
        snprintf(more, sizeof more, tr(STR_S_FILES_MORE_FMT), s_rmn, total);
        mk_lbl(more, 48, 108, wt_font14(), WARN_COL);
    }

    // Sweeping the card is what this screen is for, so it takes the corner and
    // BACK moves to WT_EXIT_X. Safe there only because REMOVE ALL is a 1500ms
    // hold: the corner invariant is that a TAP there is never irreversible.
    mk_pill(tr(STR_C_BACK), WT_EXIT_X, WT_ACTION_Y, 140, rm_back_cb);
    wt_hold_pill(s_scr, tr(STR_S_RM_ALL), WT_ACT_X, WT_ACTION_Y, 300, WT_ACTION_H,
                 1500, rm_all, NULL);
}

static void rm_open_cb(lv_event_t *e)
{
    (void)e;
    hold_stop();
    lv_obj_delete_async(s_scr); s_scr = NULL; s_sign_lbl = NULL;
    s_graph = NULL; s_graph_cap = NULL; s_locked = NULL;
    s_inert[0] = NULL; s_sweep = NULL;
    rm_screen();
}

static void sd_open(lv_obj_t *parent)
{
    s_src = SRC_SD;
    if (platform_sd_mount() != 0) {
        sd_empty_screen(parent, tr(STR_S_NO_SD), tr(STR_S_INSERT_CARD));
        return;
    }
    int total = 0;
    int n = platform_sd_list_psbt(s_files, MAX_FILES, &total);
    // Which of these have a signature already sitting on the card, and how many
    // signed outputs are there to sweep. ONE pass answers both, so the badges
    // and the REMOVE pill's existence can never disagree with each other.
    s_nsig = platform_sd_signed_scan(s_files, s_sig, n > 0 ? n : 0, 0);
    if (s_nsig < 0) s_nsig = 0;
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
    //
    // Unless the card holds more than the list: then this line's one slot goes
    // to the fact that changes what the owner is looking at. A list that shows
    // 24 of 31 files and describes only its sort order is quietly lying about
    // the card, and the file that matters may be one of the seven.
    if (total > n) {
        char more[96];
        snprintf(more, sizeof more, tr(STR_S_FILES_MORE_FMT), n, total);
        mk_lbl(more, 48, 98, wt_font14(), WARN_COL);
    } else {
        mk_lbl(tr(STR_S_FILES_HINT), 48, 98, wt_font14(), MUT_COL);
    }

    // All discovered files fit in one scrollable, deterministic list. Unsigned
    // work is sorted first; signed PSBTs stay listed so a signature can be
    // re-verified, and every row that already has one says so.
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
        // THREE states, because there are three kinds of file here and the
        // first version of this said the same words about two of them.
        //
        // Signing one transaction turns its source amber AND drops its output
        // in as a new row. When both read SIGNED ALREADY in the same colour it
        // looks like two things were signed, and then REMOVE offers only one,
        // which reads as the device contradicting itself. It never was: the
        // source is a transaction that has been signed, the output IS the
        // signature. Different facts, so different words and different colour.
        //
        //   *-signed.psbt        the signature itself   OK, a finished thing
        //   source with one      you already did this   WARN, do not redo it
        //   anything else        still to do            MUT
        bool is_out = is_signed_name(s_files[i]);
        const char *tag = is_out ? tr(STR_S_ROW_SIGNATURE)
                        : s_sig[i] ? tr(STR_S_SIGNED_ALREADY)
                                   : tr(STR_S_FILE_UNSIGNED);
        lv_color_t tcol = is_out ? OK_COL : s_sig[i] ? WARN_COL : MUT_COL;
        lv_obj_t *row = wt_row_x(list, LV_SYMBOL_FILE, s_files[i], NULL, NULL,
                                 tag, wt_font14(), tcol, false,
                                 0, 0, FILE_ROW_W, 0, file_tap_cb,
                                 (void *)(intptr_t)i);
        // The flex list places it, so the absolute x/y above are ignored, but
        // the WIDTH is not: wt_row_x measures the label lane against it before
        // flex ever runs. Passing the list's real width is what keeps a long
        // filename ellipsising instead of running under the tag.
        lv_obj_set_width(row, lv_pct(100));
    }
    // This used to read "BACK keeps the corner where the thumb rests; the
    // destructive control does not go there" -- while replace-or-erase put
    // ERASE THE WORDS in that same corner. Two destructive controls, opposite
    // rules, both written down. The corner now does the screen's job
    // everywhere; see kiss_theme.h.
    //
    // A tap in WT_WARN, not a hold in WT_STOP, because this only opens a
    // confirm -- the project's rule is that the hold belongs to the act itself,
    // and it is also what makes this safe in the corner. 412..752 against the
    // way out at 48..188 leaves 224px of clear air.
    mk_pill(tr(STR_C_BACK), WT_EXIT_X, WT_ACTION_Y, 140, choose_back_cb);
    if (n > 0) {   // any file is deletable now, not only our signed outputs
        lv_obj_t *rm = wt_pill_icon(s_scr, LV_SYMBOL_TRASH, tr(STR_S_RM_SIGNED),
                                    WT_ACT_X, WT_ACTION_Y, 340, WT_ACTION_H,
                                    rm_open_cb, NULL);
        lv_obj_set_style_border_color(rm, WARN_COL, 0);
        lv_obj_t *rl = lv_obj_get_child(rm, 0);
        if (rl) lv_obj_set_style_text_color(rl, WARN_COL, 0);
    }
}

// What the device concluded about a PSBT, in one serial line.
//
// reason[] is the whole point: it carries the STOP root cause in plain English
// straight from kiss_psbt.c ("input is not this wallet's", "wrong network:
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

// ---- QR source: kiss_scan drives the camera; we get the assembled PSBT ----
static void scan_done_cb(const uint8_t *psbt, size_t len, int fmt)
{
    s_src = SRC_QR;
    s_qr_fmt = fmt;
    snprintf(s_cur, sizeof s_cur, "%s", tr(STR_S_SCANNED_TX));
    if (len > sizeof s_in) len = sizeof s_in;             // QRT_MAX_PSBT == sizeof s_in
    memcpy(s_in, psbt, len);
    SIGN_LOG("QR assembled: %u bytes, fmt %d", (unsigned)len, fmt);
    log_psbt_hex(s_in, len);
    int lrc = kiss_psbt_load(s_in, len, &s_sum);
    s_ack = false;                         // fresh PSBT: re-acknowledge any caution
    s_ack_flags = 0;
    s_recip_seen = false;                  // ...and read its destinations again
    s_on_cautions = false;
    s_ack_t0 = 0;
    s_cur_signed = false;
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
    kiss_sign_open(s_parent);                           // back to the chooser
}

static void scan_pick_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_delete_async(s_scr); s_scr = NULL;
    kiss_scan_open(s_parent, scan_done_cb, scan_cancel_cb);
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

void kiss_sign_open(lv_obj_t *parent)
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
    // Both subs share ONE size rather than being sized apiece: wt_body_font
    // answers per string, so the two rows of one choice came back at different
    // sizes and one of them visibly shouted. A group shares a size or it stops
    // being a group.
    //
    // font23, not font14. The shared size was pinned to the SMALLEST rung the
    // longer of the two strings could reach, so shortening one string bought
    // nothing and the pair stayed at the size reserved for chip labels -- on
    // the screen that opens every signing session. The strings are now both
    // about thirty characters and the rung they share is one an owner can read
    // at arm's length. A long translation still falls back inside wt_row_x.
    wt_row_x(s_scr, WT_ICON_QR, tr(STR_S_SCAN_QR), tr(STR_S_POINT_CAM),
             wt_font23(), NULL, NULL, WT_INK, false, WT_CHOICE_X,
             SGC_ROW0, WT_CHOICE_W, WT_CHOICE_H, scan_pick_cb, NULL);
    wt_row_x(s_scr, WT_ICON_SD, tr(STR_S_FROM_SD), tr(STR_S_OR_LOAD),
             wt_font23(), NULL, NULL, WT_INK, false, WT_CHOICE_X,
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
