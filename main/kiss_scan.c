// Step 6: scan the coordinator's QR (static, pMofN, or animated BC-UR) into a
// PSBT. While the camera is live the direct video path covers all LVGL, so
// progress is drawn INTO the video (camera_spike scan bar) and any tap cancels.
#include "kiss_scan.h"

#include <stdio.h>
#include <string.h>

#include "qr_transport.h"
#ifndef SIMULATOR
#include "camera_spike.h"
#endif

#include "i18n.h"
#include "kiss_theme.h"
#include "kiss_wipe.h"

// A QR that decodes cleanly but is not a transport format we know is dropped
// on the floor below (rc != 0, "some other QR in view"). That rule is right --
// a wifi QR or a URL in frame must not derail a scan -- but it makes an
// unsupported coordinator format indistinguishable from never seeing the code
// at all: the overlay just keeps saying it is looking. Logging the payload's
// first bytes is what tells those two apart.
//
// The prefix is a format tag, not content: "UR:CRYPTO-PSBT/", "cHNidP8B",
// "p1of4/". Bounded hard, and nothing here is derived from the seed.
#ifdef ESP_PLATFORM
#include "esp_log.h"
#define SCAN_LOG(...) ESP_LOGI("scan", __VA_ARGS__)
#define SCAN_PREFIX 28
#else
#define SCAN_LOG(...) ((void)0)
#endif

#define BG_COL  WT_BG
#define INK_COL WT_INK
#define MUT_COL WT_MUT
#define KEY_COL WT_KEY

static lv_obj_t *s_scr;
static lv_obj_t *s_prog, *s_hint;
// Said once per transfer, not ten times a second. An unrecognised code sits in
// frame at the decoder's full rate, and the state line is the one thing on this
// screen that moves -- rewriting it on every frame is a flicker where the point
// is a fact. Cleared when a part of a real transfer lands, so a code shown
// after a refusal still reports.
static bool s_said_wrong;

static lv_timer_t *s_tmr;
static qrt_parser_t *s_parser;
static void (*s_on_psbt)(const uint8_t *, size_t, int);
static void (*s_on_text)(const char *, size_t);   // raw mode (verify-address)
static void (*s_on_cancel)(void);
static void *s_bus;
static uint8_t s_psbt[QRT_MAX_PSBT];

// camera-task -> LVGL handoff: one pending payload slot. If a decode lands
// while the slot is full it is dropped — animated formats repeat parts anyway.
// SPSC protocol: producer fills s_pend then release-stores the length; the
// consumer acquire-loads it, copies out, then release-stores 0. The barriers
// matter — the P4 is dual-core with a weak memory model, and a plain/volatile
// store can publish the length before the payload (torn read on the consumer).
static char s_pend[2600];
static size_t s_pend_len;            // 0 = slot empty; atomic acquire/release only

// Writes the status card's two lines. Defined with the layout it places, down
// beside the geometry; declared here because the decode path calls it.
static void scan_status(const char *state, const char *hint);

bool kiss_scan_active(void) { return s_scr != NULL; }
void kiss_scan_set_bus(void *bus) { s_bus = bus; }

static void scan_teardown(void)
{
    if (s_tmr) { lv_timer_delete(s_tmr); s_tmr = NULL; }
#ifndef SIMULATOR
    camera_scan_stop();
    // The video wrote its rect straight into the framebuffer, past LVGL, so
    // LVGL believes that area is already painted. Without this, whatever
    // frame the camera left behind sits there until something else happens
    // to dirty it -- the black box the bench reported on the way out.
    lv_obj_invalidate(lv_screen_active());
#endif
    if (s_parser) { qrt_parser_free(s_parser); s_parser = NULL; }
    // camera_scan_stop() has ended the producer. Wipe before publishing the
    // slot as empty so no new decode can race with these stores.
    kiss_wipe(s_pend, sizeof s_pend);
    __atomic_store_n(&s_pend_len, 0, __ATOMIC_RELEASE);
    s_on_text = NULL;                  // raw mode never survives a teardown
    s_prog = NULL; s_hint = NULL;
}

void kiss_scan_close(void)   // idle auto-lock: no on_cancel (caller locks next)
{
    scan_teardown();
    kiss_wipe(s_psbt, sizeof s_psbt);
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
}

static void cancel_now(void)
{
    if (!s_scr) return;                // idempotent: two paths can reach here
    scan_teardown();
    kiss_wipe(s_psbt, sizeof s_psbt);
    lv_obj_delete_async(s_scr); s_scr = NULL;
    if (s_on_cancel) s_on_cancel();
}

// Second way out, for main.c. The close below is LVGL-routed, and on hardware
// the live camera bypasses LVGL entirely -- reported from a board where the
// top-left CLOSE did nothing and the only escape was pulling the power. The
// game's unlock gesture does NOT go through LVGL: game_tick reads the panel
// itself. This lets that known-working path reach the same cancel, without
// changing anything about the LVGL controls, which still work in the simulator.
void kiss_scan_cancel(void) { cancel_now(); }

// The visible control. Its own callback, so it does not repeat the corner
// hit-test below.
static void cancel_btn_cb(lv_event_t *e) { (void)e; cancel_now(); }

static void cancel_cb(lv_event_t *e)
{
    // Cancel ONLY from the top-left corner (the camera-close convention).
    // Tap-anywhere cancelled the scan every time the user's grip grazed the
    // glass — device testing found that out the hard way.
    //
    // The visible CANCEL lives on the bottom action row now; this corner
    // region stays live anyway because it costs nothing and it keeps working
    // for anyone who learned the camera close convention while the pill still
    // sat up here.
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        if (p.x >= 200 || p.y >= 110) return;
    }
    cancel_now();
}

#ifndef SIMULATOR
// Vertical drag anywhere on the scan screen = preview zoom (same feel as the
// camera dev view). Zoom only changes the PREVIEW crop — the decoder always
// sees the full sensor.
static void zoom_drag_cb(lv_event_t *e)
{
    static int anchor = -1;
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) { anchor = -1; return; }
    if (c != LV_EVENT_PRESSING) return;
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (anchor < 0) { anchor = p.y; return; }
    while (anchor - p.y >= 60) { camera_spike_zoom(+1); anchor -= 60; }
    while (p.y - anchor >= 60) { camera_spike_zoom(-1); anchor += 60; }
}
#endif

#ifndef SIMULATOR
// camera stream-task context: copy out and publish, nothing else
static void decode_cb(const char *data, size_t len)
{
    if (__atomic_load_n(&s_pend_len, __ATOMIC_ACQUIRE) || len == 0 ||
        len + 1 >= sizeof s_pend) return;
    memcpy(s_pend, data, len);
    s_pend[len] = 0;                 // defensive: parser is length-bounded anyway
    __atomic_store_n(&s_pend_len, len, __ATOMIC_RELEASE);   // publish LAST
}
#endif

static void feed(const char *data, size_t len)
{
    if (s_on_text) {                    // raw mode: first decode wins, verbatim
        if (len == 0) return;
        static char txt[sizeof s_pend];
        if (len >= sizeof txt) len = sizeof txt - 1;
        memcpy(txt, data, len);
        txt[len] = 0;
        void (*cb)(const char *, size_t) = s_on_text;
        s_on_text = NULL;
        scan_teardown();
        if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
        cb(txt, len);
        kiss_wipe(txt, sizeof txt);
        return;
    }
    if (!s_parser) return;
    int rc = qrt_parser_feed(s_parser, data, len);
    int seen = qrt_parser_seen(s_parser), total = qrt_parser_total(s_parser);
    // Too large to hold is a fact about THIS transfer, not a stray QR in
    // frame, and the difference is what the owner sees: an ignored code
    // leaves the counter sitting there forever with no explanation, which is
    // what a refused set used to look like. Say it, and drop the parts
    // already held so nothing is kept from a set that will never finish.
    // No hint: the way through is FROM SD CARD, and the action row under
    // this box already says so.
    if (rc == QRT_FEED_TOO_BIG) {
        SCAN_LOG("REFUSED: transfer larger than %u bytes", (unsigned)QRT_MAX_PSBT);
        qrt_parser_reset(s_parser);
        if (s_prog) scan_status(tr(STR_N_TOO_BIG), "");
        return;
    }
    // A final pMofN/UR fragment can be structurally valid while the completed
    // set fails base64 or its message checksum. Those decoders are terminal at
    // that point: retaining them and treating later frames as harmless extras
    // leaves the counter stuck forever. Drop the set and give the owner the
    // existing translated retry instruction.
    if (rc == QRT_FEED_CORRUPT) {
        SCAN_LOG("REFUSED: completed transfer failed checksum or encoding");
        qrt_parser_reset(s_parser);
        if (s_prog) scan_status(tr(STR_N_RETRY), "");
        return;
    }
    if (rc != 0) {                             // not a format this screen takes
        // THE ONE OUTCOME THE SCREEN NEVER REPORTED. A readable code that is
        // not a transaction was dropped in silence: an owner holding their
        // coordinator's RECEIVING address up to the sign scanner got "waiting
        // for QR" for as long as they cared to hold it, which is the same
        // screen as no code at all and the same screen as a dead camera. Three
        // states, one sentence.
        //
        // Only before a transfer has started. Mid-set the counter is the news,
        // and a stray code in frame may not displace it.
        if (s_prog && seen == 0 && !s_said_wrong) {
            scan_status(tr(STR_N_NOT_TX), "");
            s_said_wrong = true;
        }
#ifdef ESP_PLATFORM
        // Rate-limited: an unrecognised code sits in frame at ~10 decodes a
        // second and would otherwise bury every other line in the log.
        static uint32_t drops;
        if ((drops++ % 15) == 0) {
            char p[SCAN_PREFIX + 1];
            size_t n = len < SCAN_PREFIX ? len : SCAN_PREFIX;
            memcpy(p, data, n);
            p[n] = 0;
            for (size_t i = 0; i < n; i++)
                if (p[i] < 0x20 || p[i] > 0x7E) p[i] = '.';
            SCAN_LOG("IGNORED: decoded %u bytes, not a known PSBT QR format "
                     "(rc %d) starts \"%s\"", (unsigned)len, rc, p);
        }
#endif
        return;
    }
    SCAN_LOG("part accepted: %u bytes, %d of %d", (unsigned)len, seen, total);
    s_said_wrong = false;                  // a real transfer outranks the refusal
    if (s_prog) {
        char b[48];
        if (total > 1) snprintf(b, sizeof b, tr(STR_N_PARTS_FMT), seen, total);
        else snprintf(b, sizeof b, "%s", tr(STR_N_READING));
        scan_status(b, NULL);
    }
#ifndef SIMULATOR
    camera_scan_progress(seen, total);
#endif
    if (qrt_parser_complete(s_parser)) {
        size_t n = 0;
        int fmt = qrt_parser_format(s_parser);
        int rrc = qrt_parser_result(s_parser, s_psbt, sizeof s_psbt, &n);
        SCAN_LOG("complete: fmt %d, %u parts, %u bytes, rc %d",
                 fmt, (unsigned)total, (unsigned)n, rrc);
        if (rrc != 0) {
            // This used to back out of the scanner, which on screen is the
            // same thing as tapping cancel: the transfer reached 100% and the
            // scanner quit, saying nothing. Almost nothing gets this far now
            // -- every format refuses an oversized set at feed time -- but
            // what does gets the same sentence as the rest.
            SCAN_LOG("REJECTED after assembly: rc %d (over %u-byte cap?)",
                     rrc, (unsigned)sizeof s_psbt);
            kiss_wipe(s_psbt, sizeof s_psbt);
            qrt_parser_reset(s_parser);
            if (s_prog)
                scan_status(tr(rrc == QRT_FEED_TOO_BIG ? STR_N_TOO_BIG
                                                       : STR_N_RETRY), "");
            return;
        }
        scan_teardown();
        if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
        if (s_on_psbt) s_on_psbt(s_psbt, n, fmt);
        kiss_wipe(s_psbt, sizeof s_psbt);
    }
}

void kiss_scan_inject(const char *data, size_t len) { feed(data, len); }

static void poll_cb(lv_timer_t *t)
{
    (void)t;
#ifndef SIMULATOR
    size_t n = __atomic_load_n(&s_pend_len, __ATOMIC_ACQUIRE);
    if (n) {
        static char tmp[sizeof s_pend];        // feed() may tear the timer down
        memcpy(tmp, s_pend, n);
        // Keep the slot marked occupied until its contents are gone.
        kiss_wipe(s_pend, sizeof s_pend);
        __atomic_store_n(&s_pend_len, 0, __ATOMIC_RELEASE);  // free the slot
        feed(tmp, n);
        kiss_wipe(tmp, sizeof tmp);
        return;
    }
    if (camera_spike_check_died() && s_prog) {
        scan_status(tr(STR_N_CAM_STOP), tr(STR_N_RETRY));
        lv_obj_invalidate(lv_screen_active()); // video gone: repaint the LVGL screen
    }
#endif
}

static void scan_open_common(lv_obj_t *parent, kiss_scan_task_t task);


void kiss_scan_open(lv_obj_t *parent,
                      void (*on_psbt)(const uint8_t *, size_t, int),
                      void (*on_cancel)(void))
{
    if (s_scr) return;
    s_on_psbt = on_psbt;
    s_on_text = NULL;
    s_on_cancel = on_cancel;
    s_parser = qrt_parser_new();
    scan_open_common(parent, KISS_SCAN_TASK_PSBT);
}

void kiss_scan_open_raw(lv_obj_t *parent, kiss_scan_task_t task,
                          void (*on_text)(const char *, size_t),
                          void (*on_cancel)(void))
{
    if (s_scr) return;
    s_on_psbt = NULL;
    s_on_text = on_text;
    s_on_cancel = on_cancel;
    s_parser = NULL;                    // raw: no PSBT assembly
    scan_open_common(parent, task);
}

// ---- geometry ----
// The viewfinder keeps the rect the camera was device tested on; everything
// around it is on the chrome contract now. The can/cannot cards that used to
// crowd the right column moved to the SIGN page's SCAN QR tab, where they
// can be read before the camera is even open -- what is left here is the
// picture, the one line that changes while you wait, and the way out.
#define SCN_CAM_X 48
#define SCN_CAM_Y 112
#define SCN_CAM_W 300
#define SCN_CAM_H 188

// The rect, for anything outside this file that has to put something in the
// same place. Below the #defines on purpose -- there is nowhere earlier it
// could read them from.
void kiss_scan_view_rect(int *x, int *y, int *w, int *h)
{
    if (x) *x = SCN_CAM_X;
    if (y) *y = SCN_CAM_Y;
    if (w) *w = SCN_CAM_W;
    if (h) *h = SCN_CAM_H;
}
#define SCN_COL_X 396
#define SCN_COL_W 356

static void scan_status(const char *state, const char *hint)
{
    if (!s_prog || !s_hint) return;
    if (state) lv_label_set_text(s_prog, state);
    if (hint)  lv_label_set_text(s_hint, hint);
}

static void scan_open_common(lv_obj_t *parent, kiss_scan_task_t task)
{
    s_said_wrong = false;
    kiss_wipe(s_pend, sizeof s_pend);
    kiss_wipe(s_psbt, sizeof s_psbt);
    __atomic_store_n(&s_pend_len, 0, __ATOMIC_RELEASE);   // camera not started yet

    // The chrome contract: title and cursor, empty strip row, hairline, band.
    // The subtitle went with it -- "show the coordinator's QR to the camera"
    // restated what the viewfinder and the status line already say.
    s_scr = wt_chrome(parent, tr(STR_N_T));
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scr, cancel_cb, LV_EVENT_CLICKED, NULL);
#ifndef SIMULATOR
    lv_obj_add_event_cb(s_scr, zoom_drag_cb, LV_EVENT_ALL, NULL);
#endif

    // The preview column. A PANEL, not an empty outline: before the first camera
    // frame lands (and always, in the simulator) this is the largest object on
    // the screen, and an unfilled rectangle that size reads as a hole rather than
    // as a viewport waiting for a picture.
    wt_viewfinder(s_scr, SCN_CAM_X, SCN_CAM_Y, SCN_CAM_W, SCN_CAM_H);

    // The right column: the one line that changes while you wait, at the size
    // the wait deserves, then the driver detail under it, then the sentence
    // that answers "can this rob me" -- de-boxed, on the glass, the way every
    // converted page carries its claims.
    s_prog = lv_label_create(s_scr);
    lv_label_set_text(s_prog, tr(STR_N_STARTING));
    lv_obj_set_style_text_color(s_prog, INK_COL, 0);
    lv_obj_set_style_text_font(s_prog, wt_chrome28(tr(STR_N_WAIT_QR)), 0);
    lv_obj_set_pos(s_prog, SCN_COL_X, 124);
    lv_obj_set_width(s_prog, SCN_COL_W);
    lv_obj_set_height(s_prog, lv_font_get_line_height(wt_chrome28(tr(STR_N_WAIT_QR))));
    lv_label_set_long_mode(s_prog, LV_LABEL_LONG_DOT);

    // The camera-failure path writes the driver status here: technical
    // metadata, two lines, height pinned so the longest retry instruction
    // degrades inside its box instead of past the column's edge.
    s_hint = lv_label_create(s_scr);
    lv_label_set_text(s_hint, "");
    lv_obj_set_style_text_color(s_hint, MUT_COL, 0);
    lv_obj_set_style_text_font(s_hint, wt_chrome18(tr(STR_N_RETRY)), 0);
    lv_obj_set_pos(s_hint, SCN_COL_X, 168);
    lv_obj_set_width(s_hint, SCN_COL_W);
    lv_obj_set_height(s_hint, 2 * lv_font_get_line_height(wt_chrome18(tr(STR_N_RETRY))));
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_DOT);

    // The sentence that answers "can this rob me", in the words of the door it
    // was opened by. The backup one is the note its own load screen already
    // carries beside SCAN, so the two say the same thing rather than two things.
    const int NOTE[] = {
        [KISS_SCAN_TASK_PSBT]   = STR_N_NOTHING_SIGNED,
        [KISS_SCAN_TASK_ADDR]   = STR_N_FOR_ADDR,
        [KISS_SCAN_TASK_BACKUP] = STR_W_LOAD_SCAN_NOTE,
        [KISS_SCAN_TASK_PASS]   = STR_N_FOR_PASS,
    };
    const char *ntxt = tr(NOTE[task]);
    lv_obj_t *note = lv_label_create(s_scr);
    lv_label_set_text(note, ntxt);
    lv_obj_set_style_text_color(note, MUT_COL, 0);
    lv_obj_set_style_text_font(note, wt_chrome18(ntxt), 0);
    lv_obj_set_pos(note, SCN_COL_X, 232);
    lv_obj_set_width(note, SCN_COL_W);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);

    scan_status(NULL, NULL);

    // CANCEL as the band's exit, and the SD alternative NAMED beside it: the
    // person struggling to scan does not know they have another option,
    // because that choice was a tab ago. Muted text, not a control, because
    // it is not reachable from here without cancelling first.
    wt_arrow_action(s_scr, tr(STR_C_CANCEL), true, false, 552, WT_ACTION_Y,
                    200, true, cancel_btn_cb, NULL);
    // ...and only on the two doors a card can actually be used at. The address
    // checker has no card route and the passphrase has no file: naming one
    // there is an instruction that dead ends, printed under a live camera.
    if (task == KISS_SCAN_TASK_PSBT || task == KISS_SCAN_TASK_BACKUP) {
        const lv_font_t *of = wt_chrome18(tr(STR_N_OR_SD));
        lv_obj_t *or = lv_label_create(s_scr);
        lv_label_set_text(or, tr(STR_N_OR_SD));
        lv_obj_set_style_text_color(or, MUT_COL, 0);
        lv_obj_set_style_text_font(or, of, 0);
        lv_obj_set_pos(or, WT_ACT_X,
                       WT_ACTION_Y + (WT_ACTION_H - lv_font_get_line_height(of)) / 2);
        lv_obj_set_width(or, 480);
        lv_obj_set_height(or, lv_font_get_line_height(of));
        lv_label_set_long_mode(or, LV_LABEL_LONG_DOT);
    }

    s_tmr = lv_timer_create(poll_cb, 80, NULL);

#ifndef SIMULATOR
    camera_spike_set_preview_rect(SCN_CAM_X, SCN_CAM_Y, SCN_CAM_W, SCN_CAM_H);
    if (camera_scan_start(s_bus, decode_cb)) {
        scan_status(tr(STR_N_WAIT_QR), NULL);
    } else {
        scan_status(tr(STR_C_CAM_UNAVAIL), camera_spike_status());
    }
#else
    scan_status(tr(STR_N_WAIT_QR), NULL);
#endif
}
