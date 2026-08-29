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
#include "kiss_wipe.h"
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
// The trail behind SLIDE TO SIGN's thumb, filled left to right on the same
// fraction as the ring.
static lv_obj_t *s_sweep;
// The thumb itself: the accent knob the finger drags along the track.
static lv_obj_t *s_thumb;
// DETAILS and BACK, NULL terminated, so the signing state can stand them down
// without knowing what else is on the row.
static lv_obj_t *s_inert[3];
// The slide's live state: a drag in progress, and where it started. The
// finger is the clock now -- there is no timer.
static bool s_slide_on;
static int  s_slide_x0;
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
static uint8_t s_in[QRT_MAX_PSBT], s_out[QRT_MAX_SIGNED_PSBT];
static lv_obj_t *s_parent;             // where this flow's screens are built
static int s_src;                      // SRC_SD / SRC_QR: where the PSBT came from
static int s_qr_fmt;                   // QRT_FMT_* the scan arrived in
static qrt_encoder_t *s_qenc;          // QR-out encoder (animated signed PSBT)
static lv_timer_t *s_qr_tmr;
static lv_obj_t *s_qr_img, *s_part_lbl;
static int s_part_i;
static bool s_qr_ez;                   // easy-scan mode: sparser QRs, slower loop
static size_t s_out_len;               // signed PSBT length (easy-scan re-encodes)
static lv_obj_t *s_ez_act;
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
    s_slide_on = false;
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
    s_inert[0] = NULL; s_sweep = NULL; s_thumb = NULL;
    if (s_qr_tmr) { lv_timer_delete(s_qr_tmr); s_qr_tmr = NULL; }
    if (s_qenc) { qrt_encoder_free(s_qenc); s_qenc = NULL; }
    s_qr_img = NULL; s_part_lbl = NULL; s_ez_act = NULL;
    // The unsigned and the signed transaction, 13 KB of BSS, live here until
    // the next PSBT happens to overwrite them. kiss_scan wipes the identical
    // bytes on every exit path it has; this file had no wipe of any kind.
    //
    // Both callers are already done with them: close_cb leaves SIGN, and
    // step_back drops the loaded transaction so the next pick loads its own.
    // The QR-out screen and its easy-scan re-encode read s_out, and neither
    // comes through here -- qr_out_screen clears the widget pointers itself.
    kiss_wipe(s_in, sizeof s_in);
    kiss_wipe(s_out, sizeof s_out);
    s_out_len = 0;
    kiss_wipe(s_sig_fp, sizeof s_sig_fp);
    kiss_wipe(s_done_name, sizeof s_done_name);
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
// The SIGN page's own context: two tabs and the [ ? ], the RECEIVE shape.
// Declared this early because the one-step-back callbacks put the owner on
// the tab they came from.
static wt_pane_t s_cctx;
static bool s_choose_help;         // the lane is showing [ ? ], not a tab

static void step_back(void)
{
    widgets_drop();
    kiss_psbt_free();               // the next pick loads its own
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
}

static bool s_keep_page;   // one step back returns to the page you left

static void files_back_cb(lv_event_t *e)      // -> the SIGN page, SD CARD tab
{
    (void)e;
    lv_obj_t *parent = s_parent;
    step_back();
    s_cctx.tab = 1;                           // the card stays mounted
    s_keep_page = true;                       // ...and so does the list page
    kiss_sign_open(parent);
}

static void choose_back_cb(lv_event_t *e)     // -> the SIGN page, SCAN QR tab
{
    (void)e;
    lv_obj_t *parent = s_parent;
    step_back();
    platform_sd_unmount();                    // leaving the SD path for good
    s_cctx.tab = 0;
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
    // The chrome contract's HEADER only. The rest of the contract stops at
    // this chain's door on purpose: the hero, the facts strip and the graph
    // fill 64..390, they are device-tested, and the middle of the sign flow
    // does not move for a hairline. The title identity still lands, so SIGN
    // reads as the same device as every page around it.
    wt_chrome_head(s_scr);
}

// Same net, for the screens on the full contract: the chooser, the file
// list, the REMOVE list and the empty states.
static void mk_chrome(lv_obj_t *parent, const char *title)
{
    if (s_scr) {
#ifdef SIMULATOR
        g_sign_orphaned_screens++;
        fprintf(stderr, "ORPHANED SIGN SCREEN: mk_chrome(\"%s\") ran with a "
                        "live s_scr; the outgoing screen stays parented "
                        "underneath\n", title ? title : "");
#endif
        lv_obj_delete_async(s_scr);
    }
    s_scr = wt_chrome(parent, title);
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
#define SG_HOLD_W    310   // the track; the knob's travel is measured in it
// The knob, and the travel that arms the slide: a little square starting at
// the track's left end, so full travel is the track minus the knob.
#define SG_THUMB      20
#define SG_TRAVEL    (SG_HOLD_W - SG_THUMB)


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
    lv_obj_set_style_text_font(l, wt_font_mono21(), 0);
    lv_obj_set_style_text_color(l, INK_COL, 0);
    return c;
}

static void sig_fp_help_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *parent = lv_obj_get_parent(s_scr);
    lv_obj_delete(s_scr); s_scr = NULL; s_sign_lbl = NULL;
    s_graph = NULL; s_graph_cap = NULL; s_locked = NULL;
    s_inert[0] = NULL; s_sweep = NULL; s_thumb = NULL;
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
    // A VERDICT, not an operator, so it keeps its status colour and drops the
    // accent flag the kit now puts on every op -- otherwise the next theme
    // change repaints this tick, and on GREEN the accent is WT_OK to the byte.
    lv_obj_t *ok = wt_diagram_op(r1, LV_SYMBOL_OK);
    lv_obj_remove_flag(ok, WT_FLAG_ACCENT);
    lv_obj_set_style_text_color(ok, OK_COL, 0);
    lv_obj_set_style_text_font(ok, wt_font23(), 0);   // the verdict is the payload
    lv_obj_align(r1, LV_ALIGN_TOP_MID, 0, 116);

    lv_obj_t *r2 = wt_diagram_row(s_scr);
    sig_code_chip(r2, "3F00 C01D");
    wt_diagram_op(r2, LV_SYMBOL_CLOSE);
    sig_code_chip(r2, "8A41 77E2");
    lv_obj_t *warn = wt_diagram_op(r2, LV_SYMBOL_WARNING);
    lv_obj_remove_flag(warn, WT_FLAG_ACCENT);          // a verdict, as above
    lv_obj_set_style_text_color(warn, wt_ink_for(WARN_COL), 0);
    lv_obj_set_style_text_font(warn, wt_font23(), 0);
    lv_obj_align(r2, LV_ALIGN_TOP_MID, 0, 162);

    // Ruled claims under the picture: wt_why_body splits the answer the copy
    // was already written in without asking for a new string in twenty one
    // locales. Started below the rows, so the body takes whatever rung fits
    // the room the diagram left it.
    wt_why_body(s_scr, tr(STR_S_SIG_FP_HELP_B), 216, wt_accent(), true);
    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160,
                    true, sig_help_back_cb, NULL);
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
    s_inert[0] = NULL; s_sweep = NULL; s_thumb = NULL;
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
    wt_arrow_action(s_scr, tr(STR_C_DONE), false, true, 592, WT_ACTION_Y, 160,
                    true, close_cb, NULL);
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
    s_inert[0] = NULL; s_sweep = NULL; s_thumb = NULL;
    mk_chrome(parent, tr(STR_S_FAIL_T));
    char trail[96];
    snprintf(trail, sizeof trail, "%s / %s", tr(STR_S_T),
             tr(s_src == SRC_SD ? STR_S_FROM_SD : STR_S_SCAN_QR));
    wt_trail(s_scr, WT_ICON_SIGN, trail, false);
    fail_body(why);
    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160,
                    true, close_cb, NULL);
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
// 82, up from 74. The card centres a mono28 fold (32) over its caption with 6
// between, and the top clamp at 8 left the caption 28px -- one pixel under a
// font23 line, so "compare the lit characters" was set at font14 on the screen
// a signature is authorised from.
//
// The card sits at ay+16 = 316, so 316 + 82 is EXACTLY WT_CONTENT_BOTTOM. That
// is legal and it is also the end of the road: nothing in this band may grow
// after this without moving ay.
#define ADDR_CARD_H 82
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
        // SIGNED -- which the caption on the left and the control below already
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

static void thumb_x_exec(void *v, int32_t x) { lv_obj_set_x(v, x); }

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
    // The thumb RUNS back rather than teleporting: the same 200ms ease-out
    // every slide bar's fill retracts with, because a knob that jumps home
    // says the control broke rather than that the slide was abandoned.
    if (s_thumb) {
        lv_anim_delete(s_thumb, thumb_x_exec);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_thumb);
        lv_anim_set_values(&a, lv_obj_get_x(s_thumb), 0);
        lv_anim_set_duration(&a, 200);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, thumb_x_exec);
        lv_anim_start(&a);
    }
    if (s_graph) {
        wt_bundle_hold(s_graph, 0);
        wt_bundle_state(s_graph, WT_BUNDLE_LIVE);
    }
    if (s_locked) { lv_obj_delete(s_locked); s_locked = NULL; }
    if (s_graph_cap && s_graph_cap_rest[0])
        lv_label_set_text(s_graph_cap, s_graph_cap_rest);
}

// The fill crossing from the hold's red into the track's accent, then leaving.
// It is width 0 at the end either way, so an abandoned hold and a completed one
// both finish with the track in its plain fill -- hold_abandon just gets there
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

// How far the current drag has come, for the release test: full travel
// ARMS the slide, and the LIFT is what signs -- completing under a still
// down finger would rebuild the screen beneath it and let the drag's tail
// press whatever lands there.
static int s_slide_at;

static void slide_drive(int px)
{
    if (px < 0) px = 0;
    if (px > SG_TRAVEL) px = SG_TRAVEL;
    s_slide_at = px;
    // The graph, the knob and the trail run on the same fraction, because
    // there is only one thing being measured: how far this finger has
    // travelled. Three readings of one number, not three numbers.
    if (s_graph) wt_bundle_hold(s_graph, (uint8_t)(px * 255 / SG_TRAVEL));
    if (s_thumb) lv_obj_set_x(s_thumb, px);
    // The trail ends under the knob's middle, not at its leading edge: a bar
    // ending short of the knob would read as the knob outrunning its fill.
    if (s_sweep) lv_obj_set_width(s_sweep, px + SG_THUMB / 2);
}

static void slide_complete(void)
{
    hold_stop();
    // The sweep SETTLES rather than snapping to zero. It measured a finger
    // and there is no longer a finger to measure, and a bar sitting full
    // while libwally works would be read as a progress bar for the signing,
    // which is a thing nothing here can time -- so it does not sit. It holds
    // its full width for SWEEP_SETTLE_MS while its fill crosses from the
    // stop red to the accent the track is already wearing, and then it is
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
    if (s_sign_lbl) {
        lv_label_set_text(s_sign_lbl, tr(STR_S_SIGNING));
        // The arrow promised travel and the travel is spent: SIGNING is a
        // state, not a direction, so the word stands alone.
        lv_obj_t *par = lv_obj_get_parent(s_sign_lbl);
        lv_obj_t *arr = par ? lv_obj_get_child(par, 1) : NULL;
        if (arr) lv_obj_add_flag(arr, LV_OBJ_FLAG_HIDDEN);
    }
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
        // Arrow actions now: dim every label (the word AND its arrow) and
        // drop the arrow's accent flag so a theme repaint cannot relight a
        // control that stopped answering.
        const uint32_t nc = lv_obj_get_child_count(s_inert[i]);
        for (uint32_t j = 0; j < nc; j++) {
            lv_obj_t *c = lv_obj_get_child(s_inert[i], j);
            lv_obj_set_style_text_color(c, WT_DIM, 0);
            lv_obj_remove_flag(c, WT_FLAG_ACCENT);
        }
        lv_obj_remove_flag(s_inert[i], LV_OBJ_FLAG_CLICKABLE);
    }
    lv_timer_create(do_sign_cb, 30, NULL);            // let the label paint first
}

static void sign_press_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        // Not yet armed: this press is the tail of the one that acknowledged
        // the caution, landing on the button that replaced it. Swallow it.
        if (s_ack_t0 && lv_tick_elaps(s_ack_t0) < SIGN_ARM_MS) return;
        lv_indev_t *in = lv_indev_active();
        lv_point_t pt = { 0, 0 };
        if (in) lv_indev_get_point(in, &pt);
        s_slide_x0 = pt.x;
        s_slide_on = true;
        // A press mid-runback owns the thumb again; the retreat animation
        // must not keep writing x underneath the new drag.
        if (s_thumb) lv_anim_delete(s_thumb, thumb_x_exec);
        sign_lock_outputs();
    } else if (c == LV_EVENT_PRESSING) {
        if (!s_slide_on) return;
        lv_indev_t *in = lv_indev_active();
        if (!in) return;
        lv_point_t pt;
        lv_indev_get_point(in, &pt);
        slide_drive(pt.x - s_slide_x0);
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        // The LIFT at full travel is the signature; a lift short of it (or a
        // press the system took away) abandons. hold_stop alone covers the
        // release delivered on the far side of a signature -- kiss_psbt_sign
        // holds the loop, and retracting the graph there would erase a signed
        // transaction's reveal.
        if (c == LV_EVENT_RELEASED && s_slide_on &&
            s_slide_at >= SG_TRAVEL - 10) {
            slide_complete();
        } else if (s_slide_on) {
            hold_abandon();
        } else {
            hold_stop();
        }
        s_slide_at = 0;
    }
}

static void details_cb(lv_event_t *e);
static void details_open_cb(lv_event_t *e);   // fresh entry: lands on INPUTS
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
// under the title instead of pretending to be an entry with no mark.

// How many reasons can be on screen at once: fee + dust in + merge + gap + one
// of the two change rows, which are mutually exclusive. It is a count of
// REASONS, not a piece of layout, which is why it lives here beside them rather
// than with the SG_ geometry -- and why this card can size its icon array off
// it. The rows page is measured against the same number.
#define SG_ROW_MAX 5

static void caution_help_cb(lv_event_t *e)
{
    (void)e;
    // sized for the longest translations (Cyrillic/CJK run 2-3 bytes per char);
    // every append clamps o because snprintf returns the WOULD-BE length
    char body[1792];
    size_t o = 0;
    const char *icons[SG_ROW_MAX];   // one per reason this card can explain
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
    // The glossary's DERIVATION PATH mark, because that is what this row is
    // about: not the amount and not the address, the number at the end of the
    // path it was sent to. GLOSS_ICONS[6].
    if (f & WPSBT_C_GAP_CHANGE)
        BODY_ADD(LV_SYMBOL_DIRECTORY, "%s%s", o ? "\n" : "", tr(STR_S_WHY_GAPCH));
    if (f & (WPSBT_C_DUST_CHANGE | WPSBT_C_SMALL_CHANGE))
        BODY_ADD(LV_SYMBOL_MINUS, "%s%s", o ? "\n" : "", tr(STR_S_WHY_TINYCH));
    #undef BODY_ADD
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
        .icons_count = (size_t)ni,
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
    // font23, the rung this exact string already takes on the receive screen
    // and in the sign screen's own address box. It was font14 here alone --
    // an INSTRUCTION about how to check an address, set at the size this
    // device keeps for marks, on the screen where the check happens.
    lv_obj_t *cap = wt_lbl(par, tr(STR_S_CMP_8), x, y + lv_obj_get_height(ad) + 6,
                           wt_font23(), MUT_COL);
    (void)cap;
    return lv_obj_get_height(ad) + 6 + lv_font_get_line_height(wt_font23());
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
        .icons_count = s_addr_known
                     ? sizeof ICONS3 / sizeof ICONS3[0]
                     : sizeof ICONS / sizeof ICONS[0],
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
    s_inert[0] = NULL; s_sweep = NULL; s_thumb = NULL;
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
// SG_ROW_MAX is not geometry and is defined with the reasons it counts, above
// caution_help_cb, which needs it before this block is reached.

// The caution bar: one row, always, however many reasons there are. 44 is the
// ack control (40) plus 2px above and below, the least that still reads as a bar.
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
// same control on the other.

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
    // Five rows is the ceiling this can reach (fee + dust in + merge + gap + one
    // of the two change rows), which is exactly the cap the row stack draws for.
    if (n < cap && (f & WPSBT_C_MERGE_INS))
        { bits[n] = WPSBT_C_MERGE_INS;   parts[n++] = tr(STR_S_C_MERGE); }
    // The gap row carries its NUMBER, which none of the others do. "change out
    // of reach" is a claim about a specific address and the index is the whole
    // evidence -- an owner who has genuinely spent a thousand times needs to
    // see it is 1024 and not 99999 to know which of the two this is.
    //
    // Function-static, like the merge line in caution_help_cb: this returns
    // POINTERS and every caller reads them before it calls again -- the rows
    // page builds its whole stack in the loop below, the verify bar takes
    // parts[0] on the next line. One UI task, so there is no second writer.
    if (n < cap && (f & WPSBT_C_GAP_CHANGE)) {
        static char g[64];
        uint32_t idx = 0;
        for (int i = 0; i < (int)s_sum.n_out && i < WPSBT_MAX_OUTS; i++)
            if (s_sum.outs[i].is_change && s_sum.outs[i].index >= WPSBT_GAP_INDEX)
                { idx = s_sum.outs[i].index; break; }
        snprintf(g, sizeof g, tr(STR_S_C_GAPCH), (unsigned)idx);
        bits[n] = WPSBT_C_GAP_CHANGE;    parts[n++] = g;
    }
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
    s_inert[0] = NULL; s_sweep = NULL; s_thumb = NULL;
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
    s_inert[0] = NULL; s_sweep = NULL; s_thumb = NULL;
    s_on_cautions = true;
    cautions_screen();
}

static void cautions_back_cb(lv_event_t *e)
{
    (void)e;
    s_on_cautions = false;
    repaint_verify();
}

// sg_panel is gone: the bordered blocks it drew -- caution rows, the caution
// bar, the header badge, the STOP verdict -- all read as boxes on a page the
// bench asked cleared of them. Rows are ruled now, controls are words with
// marks, and the verdict wears the left rule every claim block wears.

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

// The unboxed word action lives in the kit now: wt_word_action, the same
// form the band's arrow actions wear, for controls that live inside a row.

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

    // 88 + 5*56 + 4*4 = 384, against WT_CONTENT_BOTTOM at 398. The 4px gap
    // rather than the verify screen's 8 is what buys that: it was put here for
    // a fifth row that then did not exist, and the gap-limit reason is it.
    // Four rows is 324 and unchanged, so the common stacks did not move down.
    // RULED rows now, not bordered panels: the same lane the def lists use --
    // mark, sentence, the control at the right, a hairline between rows and
    // nothing drawn around any of it. The bench asked the warning boxes gone
    // with the rest of the page's.
    int y = 88;
    for (int i = 0; i < np; i++) {
        bool done = (s_ack_flags & bits[i]) != 0;
        lv_obj_t *row = lv_obj_create(s_scr);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 24, y);
        lv_obj_set_size(row, 752, SG_ROW_H);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        sg_lbl(row, done ? LV_SYMBOL_OK : LV_SYMBOL_WARNING, SG_PAD, 16,
               wt_font23(), done ? OK_COL : wt_ink_for(WARN_COL));
        lv_obj_t *t = lv_label_create(row);
        lv_obj_set_pos(t, 52, 19);
        lv_obj_set_style_text_color(t, done ? MUT_COL : INK_COL, 0);
        // A caution row's 24px lane, which is the BOX deciding rather than the
        // copy -- the same case the FIT gate carves out by height. The row
        // carries a mark, a sentence and an I UNDERSTAND control on one line,
        // and WHY FLAGGED behind the "?" is where the same caution is written
        // out at font23.
        wt_note_fit(t, parts[i], 491 - 16, 24);
        wt_tiny_ok(t);

        lv_obj_t *ctl;
        if (done)
            ctl = wt_word_action(row, LV_SYMBOL_OK, NULL, true, OK_COL,
                                 false, NULL, NULL);
        else
            ctl = wt_word_action(row, LV_SYMBOL_OK, tr(STR_C_I_UNDERSTAND),
                                 true, wt_accent(), true, row_ack_cb,
                                 (void *)(uintptr_t)bits[i]);
        lv_obj_align(ctl, LV_ALIGN_RIGHT_MID, -SG_PAD, 0);
        if (i) sg_rule(24, y - 2, 752, 1);
        y += SG_ROW_H + 4;
    }

    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, SG_BACK_X140,
                    WT_ACTION_Y, 140, true, cautions_back_cb, NULL);
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
        // Measured off the LABEL, not a font guess: the chrome head decides
        // the title's face now, and a measurement against the old font34 put
        // the filename 20px adrift the day the face changed.
        lv_obj_t *tl = wt_screen_title(s_scr);
        lv_obj_update_layout(tl);
        int fx = 48 + (tl ? lv_obj_get_width(tl) : 120) + 30, fr = 530;
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
                                 0, 33, wt_font14(),
                                 is_out ? OK_COL : wt_ink_for(WARN_COL));
            lv_obj_update_layout(w);
            int ww = lv_obj_get_width(w);
            lv_obj_set_pos(w, fr - ww, 33);
            fr -= ww + 12;
        }
        // THE NETWORK, and only when it is not mainnet. It was half of an
        // ellipsised strip below the graph -- "TESTNET, practice coins  ·
        // REPLACEABL..." -- which is where a fact goes to be unread. Here it is
        // a badge on the title line, beside the amount's own screen furniture
        // rather than buried under the graph, and absent entirely on mainnet so
        // an ordinary spend reads with nothing extra in the header.
        //
        // IN THE fr CHAIN, not floating on top of it. The comment below this
        // block says nothing new may appear on this line and collide with the
        // filename -- and it is right: the filename's box deliberately spans
        // the whole middle band so it can ellipsise, so anything parked over it
        // overlaps by box whatever the frame looks like. Two attempts did
        // exactly that. Reserving the width out of fr first is the mechanism
        // this line already has, and the filename shrinks for it like it does
        // for the signed badge above.
        if (s_sum.testnet) {
            lv_obj_t *nb = wt_state_chip(s_scr,
                                         tr_sym(LV_SYMBOL_WARNING,
                                                s_sum.net == KISS_NET_SIGNET
                                                    ? STR_I_NET_SIGNET
                                                    : STR_I_NET_TEST),
                                         WT_WARN);
            lv_obj_update_layout(nb);
            int nw = lv_obj_get_width(nb);
            // Only if the line can hold it. On a file that already carries a
            // SIGNED ALREADY badge the chain runs out of lane, and a badge
            // placed anyway lands on the TITLE -- which the gate caught on
            // sim_sign_known. The network is on the DETAILS deck as well, so
            // dropping it here costs the reader a tap, not the fact.
            if (fr - nw - 12 - fx >= 60) {
                lv_obj_set_pos(nb, fr - nw, 26);
                fr -= nw + 12;
            } else {
                lv_obj_delete(nb);
            }
        }
        // Below about 60px a filename is ellipsis and one character, which tells
        // nobody anything. It is already on the row that was tapped and in the
        // DETAILS page title, so drop it rather than let it collide.
        if (fr - fx >= 60) {
            lv_obj_t *f = sg_lbl(s_scr, s_cur, fx, 34, wt_font_mono14(), MUT_COL);
            lv_obj_set_width(f, fr - fx);
            // HEIGHT TOO, and this was latent for as long as the line existed:
            // LONG_DOT only elides once the box stops growing, so a filename
            // with a width and no height WRAPS first. Nothing had ever narrowed
            // this lane enough to show it -- the network badge did, and a
            // 60 character coordinator export dropped a second line straight
            // through the hero.
            lv_obj_set_height(f, lv_font_get_line_height(wt_font_mono14()));
            lv_label_set_long_mode(f, LV_LABEL_LONG_DOT);
        }
    }

    // The chip at the top right is ONE slot in two states. The caution count
    // replaces the fingerprint at the same x, y, w and h, so nothing new can
    // ever appear here and collide with the title or the filename. That is
    // defect 01 from the review, closed by deletion rather than by relocation.
    {
        // Bare labels in the corner now, not a bordered badge: the box drew a
        // button where nothing is tappable, and the bench asked the page's
        // boxes gone. Same slot, same two states, right-aligned to the 776
        // lane the exits use.
        lv_obj_t *chip = lv_obj_create(s_scr);
        lv_obj_remove_style_all(chip);
        lv_obj_set_pos(chip, 540, 14);
        lv_obj_set_size(chip, 236, 36);
        lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_END,
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
            lv_obj_set_style_text_color(t, wt_ink_for(WARN_COL), 0);
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
        // The verdict as a RULED block, not a red box: the left-rule shape
        // every claim on this device wears, in the STOP colour. The red
        // panel was the last bordered box on this flow and it went with the
        // others.
        lv_obj_t *r = sg_lbl(s_scr, tr_reason(s_sum.reason), 48, 96,
                             wt_font23(), STOP_COL);
        lv_obj_set_width(r, 728);
        lv_label_set_long_mode(r, LV_LABEL_LONG_WRAP);
        lv_obj_update_layout(r);
        int rh = lv_obj_get_height(r);
        lv_obj_t *vr = lv_obj_create(s_scr);
        lv_obj_remove_style_all(vr);
        lv_obj_set_pos(vr, 24, 96);
        lv_obj_set_size(vr, 4, rh < 40 ? 40 : rh);
        lv_obj_set_style_bg_color(vr, STOP_COL, 0);
        lv_obj_set_style_bg_opa(vr, LV_OPA_COVER, 0);
        if (body) wt_why_body(s_scr, body, 96 + (rh < 40 ? 40 : rh) + 24,
                              STOP_COL, true);
        // Same 776 lane, so the same exit as verify.
        wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, SG_BACK_X140,
                        WT_ACTION_Y, 140, true,
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

        // The network badge is NOT on this row. It was, for one build, and the
        // gate caught what the single-recipient frame could not show: with a
        // 4 200 000 hero and twenty inputs this row is 720px of content in a
        // 760px lane, so the badge ran straight into the caution chip at 738.
        // It lives on the title line now, in the empty middle both layouts
        // leave between the filename and the corner.

        // NO FEE RATE HERE. "7.0 sat/vB, 1.6% of what you send" was promoted
        // onto this row to answer "is that a lot", and it answered it in
        // font14 against a font48 figure, on the busiest line of the busiest
        // screen. Two things make it a duplicate: dtab_tx builds the identical
        // string on DETAILS, one tap away on this same band; and the
        // percentage that MATTERS already raises the HIGH FEE caution row, so
        // the bar says "is that a lot" in a place the eye cannot miss and the
        // owner has to acknowledge.
        //
        // What is left on this row is the amount, its unit, and the same amount
        // in the other unit. Three facts about one number.
    }

    // ---- the caution bar -------------------------------------------------
    // One row tall, whatever the count, and it never takes the panels' place.
    // The rows themselves are a page away (cautions_screen); what stays here is
    // the reason nearest the top of caution_rows' priority order, plus how many
    // more there are, plus the way in. A flagged transaction and a clean one
    // now differ by 52px of bar -- not by whether the owner is shown where the
    // coins are going.
    //
    // The single-caution case still acks in place: one reason, one ack, no
    // navigation, which is the shape most flagged transactions actually have.
    if (np) {
        bool all_done = (s_ack_flags & caution_all_bits(s_sum.caution_flags))
                        == caution_all_bits(s_sum.caution_flags);
        // The graph owns 172..282, so the bar sits in the band the facts strip
        // used to hold. A RULED row now, not a bordered panel -- hairlines
        // above and below, the mark and the sentence between them, and the
        // control at the right unboxed like the band's own actions. Same
        // 475px text box, which is what keeps the string clear of the
        // control's lane.
        lv_obj_t *bar = lv_obj_create(s_scr);
        lv_obj_remove_style_all(bar);
        lv_obj_set_pos(bar, 24, SG_BAR_Y_G);
        lv_obj_set_size(bar, 752, SG_BAR_H);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        sg_rule(24, SG_BAR_Y_G - 1, 752, 1);
        sg_rule(24, SG_BAR_Y_G + SG_BAR_H, 752, 1);
        sg_lbl(bar, all_done ? LV_SYMBOL_OK : LV_SYMBOL_WARNING, SG_PAD, 10,
               wt_font23(), all_done ? OK_COL : wt_ink_for(WARN_COL));
        // "+N" carries the rest of the list without a string to translate: the
        // header chip already states the total, so this only has to say that
        // the one line shown is not all of it.
        if (np > 1) snprintf(buf, sizeof buf, "%s   +%u", parts[0], (unsigned)(np - 1));
        else        snprintf(buf, sizeof buf, "%s", parts[0]);
        lv_obj_t *t = lv_label_create(bar);
        lv_obj_set_pos(t, 52, 13);
        lv_obj_set_style_text_color(t, all_done ? MUT_COL : INK_COL, 0);
        wt_note_fit(t, buf, 491 - 16, 24);

        lv_obj_t *ctl;
        if (np == 1 && !all_done) {
            ctl = wt_word_action(bar, LV_SYMBOL_OK, tr(STR_C_I_UNDERSTAND),
                                 true, wt_accent(), true, row_ack_cb,
                                 (void *)(uintptr_t)bits[0]);
        } else if (np == 1) {
            // One caution, already acknowledged: the spent tick, in place.
            // REVIEW here opened a page whose only content was this same
            // sentence with this same tick.
            ctl = wt_word_action(bar, LV_SYMBOL_OK, NULL, true, OK_COL,
                                 false, NULL, NULL);
        } else {
            // A direction, so the arrow trails the word: the rows live on a
            // page of their own.
            ctl = wt_word_action(bar, LV_SYMBOL_RIGHT, tr(STR_S_C_REVIEW),
                                 false, wt_accent(), true,
                                 cautions_open_cb, NULL);
        }
        lv_obj_align(ctl, LV_ALIGN_RIGHT_MID, -SG_PAD, 0);
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
        // The change strand names its INDEX when there is one change output,
        // which is every ordinary transaction. The amount coming back was on
        // this screen and where it landed was not, so an owner could read the
        // whole graph and still not know their change had been parked at
        // #99999 where no coordinator scans -- the whole of TX-17. One buffer
        // serves every change strand, so with more than one the bare word
        // stays rather than have them all claim the first one's number.
        if (change_n == 1) {
            uint32_t ci = 0;
            for (int i = 0; i < (int)s_sum.n_out && i < WPSBT_MAX_OUTS; i++)
                if (s_sum.outs[i].is_change) { ci = s_sum.outs[i].index; break; }
            snprintf(cbuf, sizeof cbuf, "%s  %s  #%u", GLOSS_ICONS[2],
                     gloss_term(2), (unsigned)ci);
        } else {
            snprintf(cbuf, sizeof cbuf, "%s  %s", GLOSS_ICONS[2], gloss_term(2));
        }
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
        // NO CHIP HERE. It defined "input" -- a glossary term, and the
        // glossary is one tap away behind DETAILS with the other seven. Three
        // "?" scattered through the content of the screen that matters most is
        // the shape being removed; what a reader needs at this moment is the
        // graph, not a definition of the word above it.
        (void)s_coins_chip_x;

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
            // NO CAPTION WORD. "recipient address" labelled the only address
            // on the screen, in font14, directly above it -- the copy rule's
            // first cut. What stays on this row is what it is FOR: the
            // recognition chip when these keys have paid here before, and the
            // "?" that explains what to compare.
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
            // NOT the "?" -- that moved onto the card below, where the thing
            // it explains actually is. With the caption word gone this row is
            // the recognition chip or nothing, and a lone "?" floating over an
            // empty line was the first thing the frame showed.
            //
            // The row is LV_SIZE_CONTENT, so a chip in it makes it TALLER than
            // the caption alone -- and it sits directly above the address card.
            // Parked at a fixed ay - 6 it grew down THROUGH the card's top
            // edge: 294 plus a 31px chip row is 325 against a card starting at
            // 316. Measure it and hang it off the card instead, so the row's
            // BOTTOM is what stays put and the chip rises when it arrives
            // rather than the card being covered.
            lv_obj_update_layout(arow);
            lv_obj_set_y(arow, ay + 16 - lv_obj_get_height(arow) - 6);
            if (!s_addr_known) lv_obj_add_flag(arow, LV_OBJ_FLAG_HIDDEN);
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
            // The "?" ON THE CARD, at its top right corner: it explains what
            // to compare, and the thing to compare is inside this box. It sat
            // on the caption line above until the caption's word was cut, and
            // then it was a mark floating over nothing.
            if (!np) wt_help_chip(box, 752 - 30 - 12, 8, MUT_COL,
                                  addr_help_cb, NULL);
            lv_obj_t *cmp = wt_lbl(box, tr(STR_S_CMP_8), 14, 0,
                                   wt_font23(), MUT_COL);
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

        // NO RBF CHIP AND NO META ROW. The pair below the graph read
        // "TESTNET, practice coins  ·  <REPLACEABL...>" -- ellipsised in
        // ENGLISH, at HEAD, with nothing added -- plus a third "?" of its own.
        // Both halves are already stated where a reader who wants them looks:
        // RBF is a flag row on DETAILS > TRANSACTION with the same "?" card
        // behind it, and the network is a badge in this screen's header now,
        // the idiom KEYS and RECEIVE already use.
        //
        // The band it held goes to the address card, which is the one thing on
        // this screen an attacker has to change and the one an owner has to
        // read character by character.
    }

    if (np) wt_help_chip(s_scr, 738, 108, WARN_COL, caution_help_cb, NULL);

    // ---- the action row --------------------------------------------------
    // HOLD TO SIGN never moves, never changes label, and never changes width.
    // Acknowledgement lives in the caution rows now, so there is no second
    // button competing for this position and no way for two taps in the same
    // place to become a signature nobody read.
    // DETAILS and BACK wear the band's word-and-arrow form, the same one
    // every other page's exits wear now; the pill boxes went with the
    // slider's. Both still stand down through s_inert while libwally works.
    s_inert[0] = wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 636,
                                 WT_ACTION_Y, 140, true,
                                 s_src == SRC_SD ? files_back_cb
                                                 : choose_back_cb, NULL);
    s_inert[1] = wt_arrow_action(s_scr, tr(STR_S_DETAILS), false, false,
                                 SG_DETAILS_X, WT_ACTION_Y, 150, false,
                                 details_open_cb, NULL);
    s_inert[2] = NULL;

    // The slide itself: the kit's rule shape -- the word, its arrow, a thin
    // track under them -- with a square KNOB riding the track. The accent
    // filled pill and then a boxed groove both came off the bench as "still
    // a box": what says drag is a bar and a thing to grab, so that is all
    // there is. sign_press_cb still measures distance from wherever the
    // press lands, and the lift at full travel is still what signs.
    lv_obj_t *p = lv_obj_create(s_scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, SG_HOLD_W, WT_ACTION_H);
    lv_obj_set_pos(p, SG_HOLD_X, WT_ACTION_Y);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    // One expression, read twice. Writing the condition out again for the test
    // seam let the seam keep reporting "inert" after the gate itself had been
    // deleted -- the self test passed against a build with no gate in it.
    const bool armed = !((np && !s_ack) || !s_recip_seen);
#ifndef ESP_PLATFORM
    s_armed = armed;
#endif
    // Inert is the same shape with the ink taken out, not a hidden control:
    // the owner can see what acknowledging the rows above is going to
    // unlock. No accent while inert -- the accent is this app's "press this
    // one" marker, and wearing it dead would be a lie.
    const lv_font_t *sf = wt_chrome23(tr(STR_S_HOLD_TO_SIGN));
    lv_obj_t *sl = wt_lbl(p, tr(STR_S_HOLD_TO_SIGN), 0, 0, sf,
                          armed ? wt_accent() : WT_DIM);
    lv_obj_set_style_text_letter_space(sl, 2, 0);
    if (armed) lv_obj_add_flag(sl, WT_FLAG_ACCENT);
    s_sign_lbl = sl;
    lv_obj_update_layout(sl);
    // NO ARROW beside the word. The knob below is the direction, it is on the
    // thing the finger moves, and the kit's own slide rule lost its arrow in
    // the same edit -- two marks for one action is the shape this pass
    // removed from the firmware rows and the SIGN explainer too.
    const int ty = lv_obj_get_height(sl) + 8;
    lv_obj_t *track = lv_obj_create(p);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, SG_HOLD_W, 2);
    lv_obj_set_pos(track, 0, ty);
    lv_obj_set_style_bg_color(track, WT_DIV, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);

    // The knob exists in both states -- inert it stands at the start of the
    // track in the disabled ink, saying a slide will happen here.
    lv_obj_t *knob = lv_obj_create(p);
    lv_obj_remove_style_all(knob);
    lv_obj_set_size(knob, SG_THUMB, SG_THUMB);
    lv_obj_set_pos(knob, 0, ty + 1 - SG_THUMB / 2);
    lv_obj_set_style_radius(knob, 4, 0);
    lv_obj_set_style_bg_color(knob, armed ? wt_accent() : WT_EDGE, 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_remove_flag(knob, LV_OBJ_FLAG_CLICKABLE);

    if (armed) {
        lv_obj_add_flag(knob, WT_FLAG_ACCENT_FILL);
        s_thumb = knob;

        // The trail, the slide's second reading -- the track filling in the
        // ACCENT up to the knob, and not in WT_STOP. On this device a red
        // fill under a confirm means a destructive one, wipe or reset, and
        // signing is neither.
        lv_obj_t *f = lv_obj_create(p);
        lv_obj_remove_style_all(f);
        lv_obj_set_size(f, 0, 2);
        lv_obj_set_pos(f, 0, ty);
        lv_obj_set_style_bg_color(f, wt_accent(), 0);
        lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
        lv_obj_add_flag(f, WT_FLAG_ACCENT_FILL);
        lv_obj_remove_flag(f, LV_OBJ_FLAG_CLICKABLE);
        s_sweep = f;
        lv_obj_move_foreground(knob);   // the knob rides ON the fill

        lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(p, sign_press_cb, LV_EVENT_ALL, NULL);
        // The slide must not bubble a gesture out to any screen watcher --
        // dragging the confirm is not a page turn.
        lv_obj_remove_flag(p, LV_OBJ_FLAG_GESTURE_BUBBLE);
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

static void gloss_gesture_cb(lv_event_t *e)
{
    if (wt_swipe_step(e) < 0) gloss_back_cb(NULL);
}

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
    s_inert[0] = NULL; s_sweep = NULL; s_thumb = NULL;
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

        // EQUAL columns. The right one was 328 to the left's 344, which is
        // most of a word per line at font23 -- and a two column list whose
        // halves wrap differently reads as two lists.
        const int col = i / 4;                  // four down the left, four right
        const int x   = col ? 408 : 40;
        const int w   = 344;
        const int y   = 104 + (i % 4) * 68;

        // The mark first, in the accent, and flagged so it survives a theme
        // change: these are the same eight glyphs the detail rows wear, which
        // is how a reader meets a concept's mark before its word.
        lv_obj_t *ic = mk_lbl(GLOSS_ICONS[i], x, y + 3, wt_font14(),
                              wt_accent());
        lv_obj_add_flag(ic, WT_FLAG_ACCENT);

        // TERM AND DEFINITION ON ONE WRAPPED RUN, at font23. This was a
        // font14 term over a font14 definition -- eight of them, a whole page
        // of reading at the size this device keeps for MARKS, and no gate
        // could see it because the size was written in rather than fitted.
        //
        // Stacking them is what forced the rung: a term line plus two
        // definition lines does not fit a 68px pitch at 23. Running them
        // together does, because the term costs a few words of the first line
        // instead of a whole line of its own. A spangroup, so the two colours
        // wrap as one paragraph -- the same shape the folded address uses.
        lv_obj_t *sg = lv_spangroup_create(s_scr);
        lv_obj_set_pos(sg, x + 26, y);
        lv_obj_set_width(sg, w - 26);
        lv_obj_set_height(sg, 62);
        lv_obj_remove_flag(sg, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(sg, LV_OBJ_FLAG_SCROLLABLE);
        lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);
        lv_obj_set_style_text_font(sg, wt_font23(), 0);
        char tbuf[72];
        snprintf(tbuf, sizeof tbuf, "%s ", head);
        lv_span_t *s1 = lv_spangroup_new_span(sg);
        lv_span_set_text(s1, tbuf);
        lv_style_set_text_color(lv_span_get_style(s1), INK_COL);
        lv_span_t *s2 = lv_spangroup_new_span(sg);
        lv_span_set_text(s2, def ? def : "");
        lv_style_set_text_color(lv_span_get_style(s2), MUT_COL);
        lv_spangroup_refresh(sg);

        p = nl ? nl + 1 : NULL;
    }
    // The stroke that opened this page closes it: a right swipe lands back
    // on the DETAILS tab it left, the same promise every deck's [ ? ] keeps.
    wt_swipe_watch(s_scr, gloss_gesture_cb);
    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160,
                    true, gloss_back_cb, NULL);
}

// Measured height of a label that was just built, so the next thing can go
// under it. LVGL sizes a wrapped label lazily; without the update the height is
// whatever it was before the text landed.
static int det_h(lv_obj_t *o)
{
    lv_obj_update_layout(o);
    return lv_obj_get_height(o);
}


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


// ---- DETAILS: a deck now, one subject per tab ------------------------------
// The one-page form put two crammed columns and seven font14 facts on one
// screen and the bench called it overwhelming. The device's own deck carries
// it instead -- INPUTS, OUTPUTS, TRANSACTION -- each tab the full lane at
// the sizes the rest of the device reads at, the round [ ? ] in the corner
// opening SIMPLE EXPLAINERS where the pill box used to, and the stroke
// reaching it past the last tab exactly as on SETTINGS, KEYS and RECEIVE.
static wt_pane_t s_dctx;
static wpsbt_details_t s_det;     // fetched on entry; the tabs read from it

static void details_tab_build(void);

static void details_tab_cb(lv_event_t *e)
{
    wt_pane_go(&s_dctx, (int)(intptr_t)lv_event_get_user_data(e), false,
               details_tab_build);
}

static void details_gesture_cb(lv_event_t *e)
{
    const int step = wt_swipe_step(e);
    if (!step) return;
    const int to = s_dctx.tab + step;
    if (to > 2) { glossary_cb(NULL); return; }  // past the end: the explainers
    if (to < 0) return;
    wt_pane_go(&s_dctx, to, false, details_tab_build);
}

// A marked metadata line: the mark in its own label because the mono faces
// carry no icon plane -- one label in mono18 would draw the glyph as the
// missing-fallback box.
static void dtab_meta(lv_obj_t *row, const char *icon, const char *txt,
                      lv_color_t col)
{
    lv_obj_t *ln = lv_obj_create(row);
    lv_obj_remove_style_all(ln);
    lv_obj_set_size(ln, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ln, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(ln, 10, 0);
    lv_obj_t *ic = lv_label_create(ln);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_font(ic, wt_font14(), 0);
    lv_obj_set_style_text_color(ic, col, 0);
    lv_obj_t *l = lv_label_create(ln);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, wt_font_mono18(), 0);
    lv_obj_set_style_text_color(l, col, 0);
}

// INPUTS: what this transaction spends. The amount leads at 28 with its
// proven/claimed mark, the coin it came from and the key that owns it under
// it at mono18 -- all up from the font14 the two-column page forced.
static void dtab_inputs(lv_obj_t *p)
{
    char buf[256], a[32];
    if (s_det.n_total > s_det.n_in)          // more than the struct can hold
        snprintf(buf, sizeof buf, tr(STR_S_D_MANYIN_FMT),
                 (unsigned)s_det.n_total, (unsigned)s_det.n_in);
    else
        snprintf(buf, sizeof buf, tr(STR_S_D_INPUTS_FMT),
                 (unsigned)s_det.n_in);
    lv_obj_t *h = wt_section(p, buf, 24, 118);
    lv_obj_set_width(h, 728);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    const int ly = 118 + det_h(h) + 10;

    lv_obj_t *il = lv_obj_create(p);
    lv_obj_remove_style_all(il);
    lv_obj_set_pos(il, 24, ly);
    lv_obj_set_size(il, 752, WT_CONTENT_BOTTOM - 6 - ly);
    lv_obj_set_style_pad_row(il, 4, 0);
    lv_obj_set_flex_flow(il, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(il, LV_DIR_VER);
    // MODE_ON once anything is below the fold: a list with more must not
    // look identical to one that ends here.
    lv_obj_set_scrollbar_mode(il, s_det.n_in > 3 ? LV_SCROLLBAR_MODE_ON
                                                 : LV_SCROLLBAR_MODE_AUTO);
    wt_list_scrollbar(il);
    lv_obj_set_style_bg_opa(il, LV_OPA_TRANSP, 0);
    for (uint32_t i = 0; i < s_det.n_in; i++) {
        lv_obj_t *row = lv_obj_create(il);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_bottom(row, 14, 0);

        wt_fmt_amount(s_det.ins[i].sats, a, sizeof a);
        // The verify screen can only say "one of these amounts is not
        // proven". This is the page that says WHICH: a tick when a previous
        // transaction hashing to this outpoint vouched for the amount, an
        // eye-slash in WARN when it is only what the coordinator claimed.
        bool ok = s_det.ins[i].proven;
        snprintf(buf, sizeof buf, "%s %s %s",
                 ok ? LV_SYMBOL_OK : WT_ICON_HIDDEN, a, wt_denom_unit());
        lv_obj_t *amt = lv_label_create(row);
        lv_label_set_text(amt, buf);
        lv_obj_set_style_text_color(amt, ok ? INK_COL : wt_ink_for(WARN_COL), 0);
        lv_obj_set_style_text_font(amt, wt_font28(), 0);
        wt_denom_bind(amt);

        // The coin being spent -- first 8 + last 8 of its txid and the
        // output index -- and the key that owns it, each behind the
        // glossary's own mark so the line says what it is without a label.
        snprintf(buf, sizeof buf, "%.8s...%s : %u",
                 s_det.ins[i].txid, s_det.ins[i].txid + 56,
                 (unsigned)s_det.ins[i].vout);
        dtab_meta(row, GLOSS_ICONS[3], buf, MUT_COL);

        // BIP376 received-SP input: its key is spend+tweak, not a BIP84
        // child, so the silent-payment badge stands in for a misleading
        // BIP32 path.
        if (s_det.ins[i].is_sp)
            snprintf(buf, sizeof buf, "m/352'/%d'/0'   %s",
                     s_sum.testnet ? 1 : 0, tr(STR_S_SP_BADGE));
        else
            snprintf(buf, sizeof buf, "m/%u'/%d'/0'/%u/%u",
                     (unsigned)s_det.ins[i].purpose, s_sum.testnet ? 1 : 0,
                     (unsigned)s_det.ins[i].change,
                     (unsigned)s_det.ins[i].index);
        dtab_meta(row, GLOSS_ICONS[6], buf, OK_COL);
    }
}

// OUTPUTS: where it goes. Every destination readable without a gate: the
// amount at 28 with the change mark and index, the fold at mono28 -- the
// same face every other address on the device compares in -- and the row
// still OPENS the whole address, so the fold stays a summary with the full
// form behind it rather than a truncation with nothing behind it.
static void dtab_outputs(lv_obj_t *p)
{
    char buf[256], a[32];
    uint32_t n_ours = 0;
    for (int i = 0; i < (int)s_sum.n_out && i < WPSBT_MAX_OUTS; i++)
        if (s_sum.outs[i].is_change) n_ours++;
    snprintf(buf, sizeof buf, tr(STR_S_D_OUTPUTS_FMT),
             (unsigned)s_sum.n_out, (unsigned)n_ours);
    lv_obj_t *h = wt_section(p, buf, 24, 118);
    lv_obj_set_width(h, 728);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    const int oy = 118 + det_h(h) + 10;

    lv_obj_t *ol = lv_obj_create(p);
    lv_obj_remove_style_all(ol);
    lv_obj_set_pos(ol, 24, oy);
    lv_obj_set_size(ol, 752, WT_CONTENT_BOTTOM - 6 - oy);
    lv_obj_set_style_pad_row(ol, 4, 0);
    lv_obj_set_flex_flow(ol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(ol, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ol, s_sum.n_out > 2 ? LV_SCROLLBAR_MODE_ON
                                                  : LV_SCROLLBAR_MODE_AUTO);
    wt_list_scrollbar(ol);
    lv_obj_set_style_bg_opa(ol, LV_OPA_TRANSP, 0);
    for (int i = 0; i < (int)s_sum.n_out && i < WPSBT_MAX_OUTS; i++) {
        lv_obj_t *row = lv_obj_create(ol);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_bottom(row, 14, 0);

        wt_fmt_amount(s_sum.outs[i].sats, a, sizeof a);
        // The change mark is the output's own claim -- re-derived and
        // verified on this device -- and green because that is a status. A
        // recipient gets no mark: the signer has nothing to vouch for about
        // someone else's address.
        const bool ours = s_sum.outs[i].is_change;
        if (ours) snprintf(buf, sizeof buf, "%s %s %s  #%u", GLOSS_ICONS[2],
                           a, wt_denom_unit(), (unsigned)s_sum.outs[i].index);
        else      snprintf(buf, sizeof buf, "%s %s", a, wt_denom_unit());
        lv_obj_t *amt = lv_label_create(row);
        lv_label_set_text(amt, buf);
        lv_obj_set_style_text_color(amt, ours ? OK_COL : INK_COL, 0);
        lv_obj_set_style_text_font(amt, wt_font28(), 0);
        wt_denom_bind(amt);

        lv_obj_t *ao = wt_addr_short(row, s_sum.outs[i].addr,
                                     wt_font_mono28());
        lv_obj_add_flag(ao, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(ao, 8);
        lv_obj_add_event_cb(ao, addr_tap_cb, LV_EVENT_CLICKED,
                            (void *)s_sum.outs[i].addr);
        // A silent payment output's on-chain address is not the one handed
        // over -- said beside the very address it is about.
        if (s_sum.outs[i].is_sp) {
            lv_obj_t *spn = lv_label_create(row);
            lv_label_set_text(spn, sp_onchain_note());
            lv_obj_set_style_text_color(spn, MUT_COL, 0);
            lv_obj_set_width(spn, 728);
            lv_obj_set_style_text_font(spn,
                wt_body_font(sp_onchain_note(), 728, 200), 0);
            lv_label_set_long_mode(spn, LV_LABEL_LONG_WRAP);
        }
    }
}

// One TRANSACTION flag row: the accent mark, the fact beside it in ink at
// 23, its own "?" at the lane's edge. Advances *y.
static void dtab_flag_row(lv_obj_t *p, int *y, const char *icon,
                          const char *head, int term)
{
    lv_obj_t *ic = wt_lbl(p, icon, 24, *y + 3, wt_font23(), wt_accent());
    lv_obj_add_flag(ic, WT_FLAG_ACCENT);
    lv_obj_t *h = wt_lbl(p, head, 64, *y, wt_font23(), INK_COL);
    lv_obj_set_width(h, 728 - 40 - 40);
    lv_obj_set_height(h, lv_font_get_line_height(wt_font23()));
    lv_label_set_long_mode(h, LV_LABEL_LONG_DOT);
    wt_help_chip(p, 24 + 728 - 26, *y - 2, MUT_COL, det_term_cb,
                 (void *)(uintptr_t)term);
    *y += lv_font_get_line_height(wt_font23()) + 8;
}

// TRANSACTION: the id to find it by, the total in the other unit, and the
// four normalcy facts, each with its own "?" -- a whole lane each, where
// the old right column gave all seven of them 252px of font14.
static void dtab_tx(lv_obj_t *p)
{
    char buf[256], gt[96];
    int ry = 118;
    wt_section(p, tr(STR_S_D_TXID), 24, ry);
    wt_help_chip(p, 24 + 728 - 26, ry - 2, MUT_COL, det_term_cb,
                 (void *)(uintptr_t)DT_TXID);
    ry += 24;
    group4(s_det.txid, gt, sizeof gt);
    lv_obj_t *tx = wt_lbl(p, gt, 24, ry, wt_font_mono18(), INK_COL);
    lv_obj_set_width(tx, 728 - 34);
    lv_label_set_long_mode(tx, LV_LABEL_LONG_WRAP);
    ry += det_h(tx) + 8;

    // The same total in the other unit, at 28 and in ink: it is the number a
    // holder reads off the glass and compares against the coordinator, which
    // is the entire reason it is here.
    wt_fmt_amount_alt(hero_sats(), gt, sizeof gt);
    snprintf(buf, sizeof buf, "= %s %s", gt, wt_denom_unit_alt());
    lv_obj_t *bt = wt_lbl(p, buf, 24, ry, wt_font28(), INK_COL);
    wt_denom_bind(bt);
    ry += det_h(bt) + 10;

    // The flag rows: each head is the whole fact (wt_split_colon reads the
    // head:tail shape the locales already write; the tail lives on the "?"
    // card), and the version and locktime numbers ride one head.
    char sh_head[64], rbf_head[64];
    snprintf(buf, sizeof buf, tr(STR_S_D_VER_LT_FMT),
             (unsigned)s_det.version, (unsigned)s_det.locktime);
    wt_split_colon(tr(STR_S_D_SIGHASH), sh_head, sizeof sh_head);
    wt_split_colon(s_sum.rbf ? tr(STR_S_D_RBF_ON) : tr(STR_S_D_RBF_OFF),
                   rbf_head, sizeof rbf_head);
    uint64_t pct10 = s_sum.send_sats
                   ? (uint64_t)s_sum.fee_sats * 1000ull / s_sum.send_sats : 0;
    char fee_line[256];
    if (s_sum.send_sats)
        snprintf(fee_line, sizeof fee_line, tr(STR_S_FEERATE_PCT_FMT),
                 (unsigned)(s_sum.fee_rate_x10 / 10),
                 (unsigned)(s_sum.fee_rate_x10 % 10),
                 (unsigned long long)(pct10 / 10),
                 (unsigned long long)(pct10 % 10));
    else
        snprintf(fee_line, sizeof fee_line, tr(STR_S_FEERATE_FMT),
                 (unsigned)(s_sum.fee_rate_x10 / 10),
                 (unsigned)(s_sum.fee_rate_x10 % 10));

    dtab_flag_row(p, &ry, LV_SYMBOL_CUT, fee_line, DT_FEE);
    dtab_flag_row(p, &ry, WT_ICON_LOCK, buf, DT_LOCKTIME);
    dtab_flag_row(p, &ry, LV_SYMBOL_OK, sh_head, DT_SIGHASH);
    // The same mark the RBF explainer wears, so the row and the card that
    // explains it are recognisably about one thing.
    dtab_flag_row(p, &ry, s_sum.rbf ? WT_ICON_REPLACE : WT_ICON_LOCK,
                  rbf_head, DT_RBF);
}

static void details_tab_build(void)
{
    lv_obj_t *p = s_dctx.pane;
    switch (s_dctx.tab) {
    case 0:  dtab_inputs(p);  break;
    case 1:  dtab_outputs(p); break;
    default: dtab_tx(p);      break;
    }
}

static void details_cb(lv_event_t *e)
{
    (void)e;
    wt_denom_on_tap(denom_tap_details);   // a figure tapped here rebuilds here
    if (kiss_psbt_details(&s_det) != 0)
        return;
    hold_stop();
    lv_obj_delete_async(s_scr); s_scr = NULL; s_sign_lbl = NULL;
    s_graph = NULL; s_graph_cap = NULL; s_locked = NULL;
    s_inert[0] = NULL; s_sweep = NULL; s_thumb = NULL;
    // No subtitle: the tab strip owns that band now, and the file name is on
    // the verify screen this deck was opened from and returns to.
    mk_screen(s_parent, tr(STR_S_DETAILS), NULL);

    wt_tab_t t[3] = {
        { .icon = LV_SYMBOL_DOWNLOAD, .label = tr(STR_S_D_TAB_INS)  },
        { .icon = LV_SYMBOL_UPLOAD,   .label = tr(STR_S_D_TAB_OUTS) },
        { .icon = LV_SYMBOL_LIST,     .label = tr(STR_S_D_TAB_TX)   },
    };
    s_dctx.scr    = s_scr;
    s_dctx.select = wt_tabs_flex_select;
    s_dctx.tabs   = wt_tabs_flex(s_scr, t, 3, s_dctx.tab, details_tab_cb);
    wt_pane_tabs_watch(&s_dctx);
    wt_swipe_watch(s_scr, details_gesture_cb);
    // The corner [ ? ] is the way into SIMPLE EXPLAINERS now -- the pill box
    // it replaces was the last box on the page. The stroke past TRANSACTION
    // lands there too, and the glossary's own right stroke comes back.
    wt_help_tab(s_scr, NULL, glossary_cb, NULL);

    s_dctx.pane = wt_pane_new(&s_dctx);
    details_tab_build();

    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160,
                    true, details_back_cb, NULL);
}

// Fresh entry from the verify band lands on INPUTS; the context keeps its
// tab across the rebuild a figure tap or a glossary BACK causes, which is
// what returns a reader to the tab they left.
static void details_open_cb(lv_event_t *e)
{
    s_dctx.tab = 0;
    details_cb(e);
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

// The toggle's state is the MARK: the tick sits in the layout at all times
// (opa 0 when off) so the word never shifts under the finger -- a control
// that moves between visits is the thing the duress screens already banned.
static void ez_sync(void)
{
    if (!s_ez_act) return;
    lv_obj_t *mark = lv_obj_get_child(s_ez_act, 0);
    lv_obj_t *word = lv_obj_get_child(s_ez_act, 1);
    lv_obj_set_style_opa(mark, s_qr_ez ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(word, s_qr_ez ? wt_accent() : INK_COL, 0);
    if (s_qr_ez) {
        lv_obj_add_flag(mark, WT_FLAG_ACCENT);
        lv_obj_add_flag(word, WT_FLAG_ACCENT);
    } else {
        lv_obj_remove_flag(mark, WT_FLAG_ACCENT);
        lv_obj_remove_flag(word, WT_FLAG_ACCENT);
    }
}

static void qr_ez_cb(lv_event_t *e)
{
    (void)e;
    s_qr_ez = !s_qr_ez;
    if (qr_enc_start() != 0) { s_qr_ez = !s_qr_ez; return; }  // old QR keeps playing
    ez_sync();
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
    s_inert[0] = NULL; s_sweep = NULL; s_thumb = NULL;

    s_qr_ez = false;
    s_out_len = sw;
    if (qr_enc_start() != 0) {
        mk_chrome(parent, tr(STR_S_FAIL_T));
        char trail[96];
        snprintf(trail, sizeof trail, "%s / %s", tr(STR_S_T),
                 tr(STR_S_SCAN_QR));
        wt_trail(s_scr, WT_ICON_SIGN, trail, false);
        fail_body(tr(STR_S_QR_FAIL_ENC));
        wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y,
                        160, true, close_cb, NULL);
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
    // control sits between here and DONE, so each of these gets its own line.
    if (n > 1) {
        wt_note(s_scr, tr(STR_S_QR_LOOP), 430, 168, 322, 29);
        s_qr_tmr = lv_timer_create(qr_tick, 250, NULL);
    }
    wt_note(s_scr, tr(STR_S_NO_NETWORK), 430, 201, 322, 29);
    s_ez_act = wt_word_action(s_scr, LV_SYMBOL_OK, tr(STR_S_EASY_SCAN), true,
                               INK_COL, false, qr_ez_cb, NULL);
    lv_obj_set_pos(s_ez_act, 430, 244);
    ez_sync();
    wt_note(s_scr, tr(STR_S_EZ_NOTE), 430, 304, 322, 87);
    wt_arrow_action(s_scr, tr(STR_C_DONE), false, true, 592, WT_ACTION_Y, 160,
                    true, close_cb, NULL);
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
        mk_chrome(parent, tr(STR_S_T));
        // The filename the refusal is about, where every page names its
        // path. The trail's mono face carries a filename better than the
        // subtitle it replaces did.
        char trail[96];
        snprintf(trail, sizeof trail, "%s / %s", tr(STR_S_T), s_cur);
        wt_trail(s_scr, WT_ICON_SIGN, trail, false);
        // A refusal to sign, alone on an otherwise empty screen with 230px
        // of room under it. There is no reason for it to be the small type.
        wt_note_col(s_scr, tr(STR_S_READ_FAIL), 48, 140, 704, 232, STOP_COL);
        wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y,
                        160, true, files_back_cb, NULL);
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
        mk_chrome(parent, tr(STR_S_T));
        char trail[96];
        snprintf(trail, sizeof trail, "%s / %s", tr(STR_S_T), s_cur);
        wt_trail(s_scr, WT_ICON_SIGN, trail, false);
        wt_note_col(s_scr, tr(STR_S_NOT_PSBT), 48, 140, 704, 232, STOP_COL);
        wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y,
                        160, true, files_back_cb, NULL);
        return;
    }
    log_summary("SD");
    verify_screen(parent);
}

// Both dead ends on the SD path: no card in the slot, and a card with no .psbt
// on it. They render in the SD CARD tab's lane -- same card, same amber SD
// glyph, same words as when they owned a page of their own.
static void sd_lane_empty(lv_obj_t *p, const char *head, const char *body)
{
    lv_obj_t *card = wt_card(p, WT_LANE_X, 140, WT_LANE_W, 200);
    lv_obj_t *ic = wt_lbl(card, WT_ICON_SD, 0, 0, wt_font28(), WARN_COL);
    lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 28, 26);
    lv_obj_t *h = wt_lbl(card, head, 76, 22, wt_font28(), INK_COL);
    lv_obj_set_width(h, 600);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_update_layout(h);
    wt_note_col(card, body, 28, 22 + lv_obj_get_height(h) + 14, 648,
                200 - 58 - lv_obj_get_height(h), MUT_COL);
}

// The trail the SD branch's opened-from screens wear: how the owner got here.
static void sd_trail(void)
{
    char trail[96];
    snprintf(trail, sizeof trail, "%s / %s", tr(STR_S_T), tr(STR_S_FROM_SD));
    wt_trail(s_scr, WT_ICON_SD, trail, false);
}

// ---- the paged list ----------------------------------------------------
// The file list and the REMOVE list used to scroll behind an invisible
// scrollbar; they PAGE now, three whole rows at a time. A horizontal SWIPE
// flips the page -- the stroke's own direction is the flip's -- and the dots
// by the count line say where in the deck the owner is. The arrow chips this
// replaces were 36px targets wedged between BACK and the row chevrons, and
// the bench said so.
#define SF_ROW_H 76                       // caption over a mono23 value
#define SF_PAGE   3                       // 3 * 76 = 228 in the 284 lane,
                                          // count line and dots at its foot
static int s_file_page, s_rm_page;
static int s_nfiles, s_ftotal;            // what the SD tab read off the card
static wt_pane_t s_fctx;                  // REMOVE's rows, so a flip can slide

static int sf_pages(int n) { return n > 0 ? (n + SF_PAGE - 1) / SF_PAGE : 1; }

// The count line and the page dots at the lane's foot. `warn` swaps the
// count for the more-files-than-the-list warning, which matters more. The
// dots are display only: the page moves under a finger, not a 36px chip.
// The pager foot, the same-tab page flip and the stroke reader all live in
// the kit now (wt_pager_line / wt_page_flip / wt_swipe_step): the owner asked
// for the SD list's swipe everywhere, so the plumbing moved to kiss_theme.c
// and this file keeps only the SIGN deck's own rules.

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
static int  s_rm_total;                 // what the card holds, listed or not

static void rm_screen(void);
static void rm_build(void);

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
    s_inert[0] = NULL; s_sweep = NULL; s_thumb = NULL;
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

static void rm_gesture_cb(lv_event_t *e)
{
    const int step = wt_swipe_step(e);
    const int to = s_rm_page + step;
    if (!step || to < 0 || to >= sf_pages(s_rmn)) return;
    s_rm_page = to;
    wt_page_flip(&s_fctx, rm_build, step);
}

static void rm_build(void)
{
    lv_obj_t *p = s_fctx.pane;
    const int npages = sf_pages(s_rmn);
    if (s_rm_page >= npages) s_rm_page = npages - 1;
    if (s_rm_page < 0) s_rm_page = 0;
    const int base = s_rm_page * SF_PAGE;
    int shown = 0;
    for (int i = 0; i < SF_PAGE && base + i < s_rmn; i++, shown++) {
        const int idx = base + i;
        const int y = WT_LANE_Y + i * SF_ROW_H;
        lv_obj_t *row = lv_obj_create(p);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, WT_LANE_X, y);
        lv_obj_set_size(row, WT_LANE_W, SF_ROW_H);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        const bool sgn = is_signed_name(s_rmf[idx]);
        lv_obj_t *ic = wt_lbl(row, sgn ? LV_SYMBOL_OK : LV_SYMBOL_FILE, 0, 0,
                              wt_font23(), sgn ? OK_COL : MUT_COL);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, WT_LINE_PAD, 0);
        lv_obj_t *nm = wt_lbl(row, s_rmf[idx], 0, 0, wt_font_mono23(),
                              INK_COL);
        lv_obj_set_width(nm, WT_LANE_W - 52 - 170 - 32);
        lv_obj_set_height(nm, lv_font_get_line_height(wt_font_mono23()));
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 52, 0);
        // The mark alone, no label: this bar is one of up to sixteen and a
        // translated phrase on each would not fit. The WT_STOP fill runs
        // under the finger, which is the affordance doing the explaining;
        // 170 of travel is the smallest slide on the device and still a
        // deliberate gesture, not a brush.
        wt_slide_rule_c(row, LV_SYMBOL_TRASH, NULL,
                        WT_LANE_W - WT_LINE_PAD - 170,
                        (SF_ROW_H - WT_ACTION_H) / 2, 170,
                        WT_STOP_INK, WT_STOP, rm_one,
                        (void *)(intptr_t)idx);
        wt_line_rule_draw(wt_line_rule(p, WT_LANE_X, y + SF_ROW_H - 1,
                                       WT_LANE_W), 42 * i + 110, 320);
    }
    // The lane's foot: the warning that the card holds more than the list
    // can, else the page count, else the sentence that makes this screen
    // safe -- each file needs its own hold.
    if (s_rm_total > s_rmn) {
        char more[96];
        snprintf(more, sizeof more, tr(STR_S_FILES_MORE_FMT), s_rmn,
                 s_rm_total);
        wt_pager_line(p, more, true, s_rm_page, npages);
    } else if (npages > 1) {
        char count[96];
        snprintf(count, sizeof count, tr(STR_S_FILES_COUNT), base + 1,
                 base + shown, s_rmn);
        wt_pager_line(p, count, false, s_rm_page, npages);
    } else {
        wt_pager_line(p, tr(STR_S_RM_C_B), false, 0, 1);
    }
}

static void rm_screen(void)
{
    // Every .psbt on the card, not only our own -signed outputs. The old
    // signed-only list made this screen safe by construction and useless for
    // the other half of the job: a card full of stale unsigned drafts had no
    // way to be cleaned except a computer. Safety moved into the gesture --
    // each row is its own 1200ms hold -- and REMOVE ALL below stays scoped
    // to signed files, where a sweep cannot destroy unsigned work.
    s_rmn = platform_sd_list_psbt(s_rmf, RM_MAX, &s_rm_total);
    if (s_rmn <= 0) {                      // nothing left: the job is done
        files_back_cb(NULL);
        return;
    }

    mk_chrome(s_parent, tr(STR_S_RM_SIGNED));
    sd_trail();
    wt_swipe_watch(s_scr, rm_gesture_cb);
    memset(&s_fctx, 0, sizeof s_fctx);
    s_fctx.scr = s_scr;
    s_fctx.pane = wt_pane_new(&s_fctx);
    rm_build();

    // Sweeping the card is what this screen is for, so it takes the left lane
    // and keeps its hold: the band invariant is that a TAP is never
    // irreversible, and both controls here are holds or exits.
    wt_slide_rule_c(s_scr, tr(STR_S_RM_ALL), tr(STR_G_FW_KEEP_HOLDING),
                    WT_ACT_X, WT_ACTION_Y, 300, WT_STOP_INK, WT_STOP,
                    rm_all, NULL);
    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160,
                    true, rm_back_cb, NULL);
}

static void rm_open_cb(lv_event_t *e)
{
    (void)e;
    s_rm_page = 0;                        // a fresh entry lands on page one
    hold_stop();
    lv_obj_delete_async(s_scr); s_scr = NULL; s_sign_lbl = NULL;
    s_graph = NULL; s_graph_cap = NULL; s_locked = NULL;
    s_inert[0] = NULL; s_sweep = NULL; s_thumb = NULL;
    rm_screen();
}

// The caption wears the file's STATE colour, which wt_line_row does not
// offer. Found by its text among the row's labels -- never by child index,
// which shifts the day the kit grows a part (the bracket strip learned that
// the hard way).
static void sf_cap_col(lv_obj_t *row, const char *cap, lv_color_t col)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(row); i++) {
        lv_obj_t *ch = lv_obj_get_child(row, i);
        if (lv_obj_check_type(ch, &lv_label_class) &&
            lv_obj_get_user_data(ch) != WT_LINE_VAL_TAG &&
            strcmp(lv_label_get_text(ch), cap) == 0) {
            lv_obj_set_style_text_color(ch, col, 0);
            return;
        }
    }
}

static void files_build(void);
static void sign_tab_go(int tab);

// The swipe, on the SIGN page: the whole page is one horizontal deck --
// [SCAN QR] [SD CARD p1..pN] -- so a stroke moves through the SD list's
// pages first and crosses the tab boundary at either end of them. The
// stroke can start anywhere -- a row, the glass under the rows, the count
// line -- because the gesture bubbles to the page. Not while [ ? ] is open:
// the explainer is a toggle, not a position on the deck.
static void files_gesture_cb(lv_event_t *e)
{
    if (s_choose_help) return;
    const int step = wt_swipe_step(e);
    if (!step) return;
    if (s_cctx.tab == 1 && s_nfiles > 0) {
        const int to = s_file_page + step;
        if (to >= 0 && to < sf_pages(s_nfiles)) {
            s_file_page = to;
            wt_page_flip(&s_cctx, files_build, step);
            return;
        }
    }
    sign_tab_go(s_cctx.tab + step);
}

// One page of files, three line rows on the lane. The state is the caption
// and the filename is the value, which is what ends the fight the old rows
// held between a long filename and its tag: each has a line of its own now,
// and the filename gets the whole 704 at mono23.
//
// THREE states, because there are three kinds of file here and the first
// version of this said the same words about two of them. Signing one
// transaction turns its source amber AND drops its output in as a new row;
// when both read SIGNED ALREADY in the same colour it looks like the device
// contradicting itself. It never was: the source is a transaction that has
// been signed, the output IS the signature.
//
//   *-signed.psbt        the signature itself   OK, a finished thing
//   source with one      you already did this   WARN, do not redo it
//   anything else        still to do            MUT
static void files_build(void)
{
    lv_obj_t *p = s_cctx.pane;
    const int npages = sf_pages(s_nfiles);
    if (s_file_page >= npages) s_file_page = npages - 1;
    if (s_file_page < 0) s_file_page = 0;
    const int base = s_file_page * SF_PAGE;
    int shown = 0;
    for (int i = 0; i < SF_PAGE && base + i < s_nfiles; i++, shown++) {
        const int idx = base + i;
        const int y = WT_LANE_Y + i * SF_ROW_H;
        const bool is_out = is_signed_name(s_files[idx]);
        const char *tag = is_out ? tr(STR_S_ROW_SIGNATURE)
                        : s_sig[idx] ? tr(STR_S_SIGNED_ALREADY)
                                     : tr(STR_S_FILE_UNSIGNED);
        lv_obj_t *row = wt_line_row(p, WT_LANE_X, y, WT_LANE_W, SF_ROW_H,
                                    tag, s_files[idx], wt_font_mono23(),
                                    INK_COL, NULL, NULL, file_tap_cb,
                                    (void *)(intptr_t)idx);
        if (is_out || s_sig[idx])
            sf_cap_col(row, tag, is_out ? OK_COL : wt_ink_for(WARN_COL));
        wt_line_rule_draw(wt_line_rule(p, WT_LANE_X, y + SF_ROW_H - 1,
                                       WT_LANE_W), 42 * i + 110, 320);
    }
    // The lane's foot, one line: the warning when the card holds more than
    // the list window (the file that matters may be one of the missing), the
    // page count when the list pages, else the sort-order hint -- the only
    // time the hint earns the slot.
    if (s_ftotal > s_nfiles) {
        char more[96];
        snprintf(more, sizeof more, tr(STR_S_FILES_MORE_FMT), s_nfiles,
                 s_ftotal);
        wt_pager_line(p, more, true, s_file_page, npages);
    } else if (npages > 1) {
        char count[96];
        snprintf(count, sizeof count, tr(STR_S_FILES_COUNT), base + 1,
                 base + shown, s_nfiles);
        wt_pager_line(p, count, false, s_file_page, npages);
    } else {
        wt_pager_line(p, tr(STR_S_FILES_HINT), false, 0, 1);
    }
}

// The SD CARD tab's lane: mount, list, and either the rows or the reason
// there are none. The "SD card ready" tick and the choose-the-file subtitle
// are both gone -- a tab reading files off the card is the readiness.
static void sd_tab_build(lv_obj_t *p)
{
    s_src = SRC_SD;
    if (platform_sd_mount() != 0) {
        s_nfiles = 0;
        sd_lane_empty(p, tr(STR_S_NO_SD), tr(STR_S_INSERT_CARD));
        return;
    }
    s_nfiles = platform_sd_list_psbt(s_files, MAX_FILES, &s_ftotal);
    // Which of these have a signature already sitting on the card, and how many
    // signed outputs are there to sweep. ONE pass answers both, so the badges
    // and the REMOVE action's existence can never disagree with each other.
    s_nsig = platform_sd_signed_scan(s_files, s_sig, s_nfiles > 0 ? s_nfiles : 0,
                                     0);
    if (s_nsig < 0) s_nsig = 0;
    if (s_nfiles <= 0) {
        s_nfiles = 0;
        sd_lane_empty(p, tr(STR_S_NO_PSBT_FILES), tr(STR_S_SPARROW_SAVE));
        return;
    }
    files_build();
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
    // Truncating fed kiss_psbt_load the front half of a transaction and let it
    // report on whatever parsed. The scanner caps at sizeof s_in before this is
    // reached, so nothing arrives here oversized -- and it stays that way for a
    // reason rather than by luck.
    int lrc = -1;
    if (len > sizeof s_in) {
        SIGN_LOG("REJECTED: %u bytes past the %u-byte input workspace",
                 (unsigned)len, (unsigned)sizeof s_in);
        len = 0;
    } else {
        memcpy(s_in, psbt, len);
        SIGN_LOG("QR assembled: %u bytes, fmt %d", (unsigned)len, fmt);
        log_psbt_hex(s_in, len);
        lrc = kiss_psbt_load(s_in, len, &s_sum);
    }
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
        wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y,
                        160, true, choose_back_cb, NULL);
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
    platform_sd_unmount();     // the QR path never reads the card, and the
                               // SD tab may have left it mounted
    lv_obj_delete_async(s_scr); s_scr = NULL;
    kiss_scan_open(s_parent, scan_done_cb, scan_cancel_cb);
}

// ---- the SIGN page: two tabs and the [ ? ] --------------------------------
// The RECEIVE shape, exactly: a bracket strip with the two ways a transaction
// arrives, the [ ? ] pinned past the divider, and the lane swapping under
// them. The old chooser -- two floating boxes under an empty strip row --
// read as an unfinished page, and the bench said so.
static void choose_help_cb(lv_event_t *e);

// The SCAN QR tab's lane: the airgap, drawn, and one instruction. It held a
// WHAT A SCAN CAN DO list here and the bench called it a lecture -- every
// fact on it is already behind the [ ? ] ("builds the transaction, cannot
// sign", "nothing moves until this signer signs it"), so the first look is
// now the picture and the one line the camera actually needs.
static void scanteach_build(lv_obj_t *p)
{
    // The airgap drawn, not the chip row: the machines themselves, with the
    // dashed break between them, are the picture this page opens on. The
    // whole figure is a tap target for the camera too -- the picture shows
    // the act, so the picture may start it.
    lv_obj_t *row = wt_diagram_airgap(p);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_translate_x(row, 0, 0);
    lv_obj_set_style_translate_x(row, 4, LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, scan_pick_cb, LV_EVENT_CLICKED, NULL);
    // Centered in the band with the instruction hanging under it: the pair
    // sits a little above the middle so diagram + line read as one figure.
    lv_obj_update_layout(row);
    const int band = 118, bandh = WT_CONTENT_BOTTOM - band;
    const lv_font_t *nf = wt_chrome23(tr(STR_S_POINT_CAM));
    lv_point_t ns;
    lv_text_get_size(&ns, tr(STR_S_POINT_CAM), nf, 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    const int gap = 26;
    const int total = lv_obj_get_height(row) + gap + ns.y;
    const int top = band + (bandh - total) / 2;
    lv_obj_set_pos(row, WT_LANE_X + (WT_LANE_W - lv_obj_get_width(row)) / 2,
                   top);
    lv_obj_t *note = wt_lbl(p, tr(STR_S_POINT_CAM), 0, 0, nf, WT_MUT);
    const int nw = ns.x > WT_LANE_W ? WT_LANE_W : ns.x;   // a parked locale
    lv_obj_set_width(note, nw);                           // may run long
    lv_obj_set_height(note, lv_font_get_line_height(nf));
    lv_label_set_long_mode(note, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(note, WT_LANE_X + (WT_LANE_W - nw) / 2,
                   top + lv_obj_get_height(row) + gap);
}

// The lane, per tab -- or the [ ? ] explainer over either: the numbered flow
// survives as the fact rows (who acts first, what comes back, who
// broadcasts) and the PSBT definition as the paragraph above them.
static void sign_tab_build(void)
{
    lv_obj_t *p = s_cctx.pane;
    if (s_choose_help) {
        wt_fact_t facts[3] = {
            // BUILD, SIGN, SEND. It was a pencil for "builds it", the
            // signature glyph for "signs it" and an upload arrow for
            // "broadcasts" -- and a pencil is what you SIGN with, so the
            // first two marks were the wrong way round. The bench: "instead
            // of a pencil icon for builds it, use a build icon".
            //
            // A wrench and a satellite dish would be better still and neither
            // is in SYMS; one font rebuild in this branch is enough, and
            // these three are already there. The gear is a machine
            // assembling, the signature glyph is the signature, and WIFI is
            // the only mark in the set that means "out to the network".
            { tr(STR_S_HELP_F1C), tr(STR_S_HELP_F1V), LV_SYMBOL_SETTINGS },
            { tr(STR_S_HELP_F2C), tr(STR_S_HELP_F2V), WT_ICON_SIGN },
            { tr(STR_S_HELP_F3C), tr(STR_S_HELP_F3V), LV_SYMBOL_WIFI },
        };
        // "PSBT" carries the emphasis: it is the one word this page exists
        // to teach, and the bench asked for it to stand out of the sentence.
        wt_explain_hi(p, tr(STR_S_HELP_HEAD), tr(STR_S_HELP_BODY), "PSBT",
                      facts, 3);
        return;
    }
    if (s_cctx.tab == 0) scanteach_build(p);
    else                 sd_tab_build(p);
}

// The band's LEFT action belongs to the tab -- OPEN CAMERA on one, REMOVE
// FILES on the other -- so unlike RECEIVE's static band it rebuilds on every
// tab change. BACK is built once and stays.
static lv_obj_t *s_band_act;

static void sign_band_update(void)
{
    if (s_band_act) { lv_obj_delete(s_band_act); s_band_act = NULL; }
    if (s_choose_help) return;
    if (s_cctx.tab == 0) {
        // The camera's own mark on the label: the bench read the bare words
        // as easy to miss under a picture that says nothing about tapping.
        char cam[WT_ICON_TEXT_MAX];
        wt_icon_text(cam, sizeof cam, WT_ICON_CAMERA, tr(STR_S_OPEN_CAM));
        s_band_act = wt_arrow_action(s_scr, cam, false, true,
                                     WT_ACT_X, WT_ACTION_Y, 0, false,
                                     scan_pick_cb, NULL);
    } else if (s_nfiles > 0) {
        // Opens a confirm-by-hold screen, so a tap here is never
        // irreversible; WT_WARN says what kind of door it is.
        s_band_act = wt_arrow_action(s_scr, tr(STR_S_RM_SIGNED), false, false,
                                     WT_ACT_X, WT_ACTION_Y, 0, false,
                                     rm_open_cb, NULL);
        for (uint32_t i = 0; i < lv_obj_get_child_count(s_band_act); i++) {
            lv_obj_t *ch = lv_obj_get_child(s_band_act, i);
            lv_obj_set_style_text_color(ch, wt_ink_for(WARN_COL), 0);
            lv_obj_remove_flag(ch, WT_FLAG_ACCENT);
        }
    }
}

static void sign_go_build(void)
{
    sign_tab_build();
    sign_band_update();
}

static void sign_tab_go(int tab)
{
    if (tab < 0 || tab > 1) return;      // the deck ends where the strip does
    // A real tab is also the way back from [ ? ]: tapping the one already
    // selected re-lands on its lane, which wt_pane_go's same-tab refusal
    // would otherwise swallow.
    if (s_choose_help && tab == s_cctx.tab) { choose_help_cb(NULL); return; }
    s_choose_help = false;
    if (tab == 1 && tab != s_cctx.tab) s_file_page = 0;  // fresh look
    // Re-tapping SD CARD mid-deck goes back to the top of the list -- the
    // one place a tap still moves the pages, and the way home from page
    // three without three swipes.
    if (tab == 1 && tab == s_cctx.tab && s_file_page > 0) {
        s_file_page = 0;
        wt_page_flip(&s_cctx, files_build, -1);
        return;
    }
    wt_pane_go(&s_cctx, tab, false, sign_go_build);
}

static void sign_tab_cb(lv_event_t *e)
{
    sign_tab_go((int)(intptr_t)lv_event_get_user_data(e));
}

static void choose_help_cb(lv_event_t *e)
{
    (void)e;
    s_choose_help = !s_choose_help;
    // wt_pane_go refuses a same-tab call, so this is its swap by hand -- the
    // same hand swap KEYS and RECEIVE do. [ ? ] never highlights, and the
    // strip releases the tab behind it for as long as the explainer is up.
    wt_tabs_flex_help(s_cctx.tabs, s_cctx.tab, s_choose_help);
    const bool was_moving = s_cctx.entering;
    wt_pane_stop(&s_cctx);
    if (was_moving && s_cctx.pane) {
        lv_obj_delete(s_cctx.pane);
        s_cctx.pane = NULL;
    }
    s_cctx.pane_out = s_cctx.pane;
    s_cctx.pane = wt_pane_new(&s_cctx);
    sign_tab_build();
    wt_accent_restyle(s_cctx.pane);
    const int dir = s_choose_help ? 1 : -1;
    wt_pane_enter(&s_cctx, dir, false);
    wt_pane_exit(&s_cctx, dir);
    sign_band_update();
}

void kiss_sign_open(lv_obj_t *parent)
{
    if (s_scr) return;
    s_parent = parent;
    s_scr = wt_chrome(parent, tr(STR_S_T));
    s_choose_help = false;               // a view, not a remembered state
    s_band_act = NULL;                   // died with the last screen
    const int tab = s_cctx.tab == 1 ? 1 : 0;  // remembered across reopens
    // A fresh entry starts the list at the top; only the one-step-back path
    // returns to the page the owner was reading.
    if (!s_keep_page) s_file_page = 0;
    s_keep_page = false;
    memset(&s_cctx, 0, sizeof s_cctx);
    s_cctx.scr = s_scr;
    s_cctx.tab = tab;
    wt_tab_t t[2] = {
        { .icon = WT_ICON_QR, .label = tr(STR_S_SCAN_QR) },
        { .icon = WT_ICON_SD, .label = tr(STR_S_FROM_SD) },
    };
    s_cctx.select = wt_tabs_flex_select;
    s_cctx.tabs = wt_tabs_flex(s_scr, t, 2, s_cctx.tab, sign_tab_cb);
    wt_pane_tabs_watch(&s_cctx);
    // No band hint: the left lane belongs to the tab's action, so the mark's
    // breathing is the whole first-run invitation, as on RECEIVE.
    wt_help_tab(s_scr, NULL, choose_help_cb, NULL);
    wt_swipe_watch(s_scr, files_gesture_cb);
    s_cctx.pane = wt_pane_new(&s_cctx);
    sign_tab_build();
    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, 592, WT_ACTION_Y, 160,
                    true, close_cb, NULL);
    sign_band_update();
}
