// Step 6: scan the coordinator's QR (static, pMofN, or animated BC-UR) into a
// PSBT. While the camera is live the direct video path covers all LVGL, so
// progress is drawn INTO the video (camera_spike scan bar) and any tap cancels.
#include "wallet_scan.h"

#include <stdio.h>
#include <string.h>

#include "qr_transport.h"
#ifndef SIMULATOR
#include "camera_spike.h"
#endif

#include "i18n.h"
#include "wallet_info.h"   // the one "?" card implementation lives there
#include "wallet_theme.h"

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

// memset can be optimized away once the compiler sees a buffer is dead.  This
// volatile store loop is intentionally boring: scanner payloads and assembled
// PSBTs must not remain in static RAM after the scan has finished.
static void scan_bzero(void *ptr, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len--) *p++ = 0;
}

bool wallet_scan_active(void) { return s_scr != NULL; }
void wallet_scan_set_bus(void *bus) { s_bus = bus; }

static void scan_teardown(void)
{
    if (s_tmr) { lv_timer_delete(s_tmr); s_tmr = NULL; }
#ifndef SIMULATOR
    camera_scan_stop();
#endif
    if (s_parser) { qrt_parser_free(s_parser); s_parser = NULL; }
    // camera_scan_stop() has ended the producer. Wipe before publishing the
    // slot as empty so no new decode can race with these stores.
    scan_bzero(s_pend, sizeof s_pend);
    __atomic_store_n(&s_pend_len, 0, __ATOMIC_RELEASE);
    s_on_text = NULL;                  // raw mode never survives a teardown
    s_prog = NULL; s_hint = NULL;
}

void wallet_scan_close(void)   // idle auto-lock: no on_cancel (caller locks next)
{
    scan_teardown();
    scan_bzero(s_psbt, sizeof s_psbt);
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
}

static void cancel_now(void)
{
    if (!s_scr) return;                // idempotent: two paths can reach here
    scan_teardown();
    scan_bzero(s_psbt, sizeof s_psbt);
    lv_obj_delete_async(s_scr); s_scr = NULL;
    if (s_on_cancel) s_on_cancel();
}

// Second way out, for main.c. The close below is LVGL-routed, and on hardware
// the live camera bypasses LVGL entirely -- reported from a board where the
// top-left CLOSE did nothing and the only escape was pulling the power. The
// game's unlock gesture does NOT go through LVGL: game_tick reads the panel
// itself. This lets that known-working path reach the same cancel, without
// changing anything about the LVGL controls, which still work in the simulator.
void wallet_scan_cancel(void) { cancel_now(); }

// The visible control. Its own callback, so it does not repeat the corner
// hit-test below: the pill IS the corner.
static void cancel_btn_cb(lv_event_t *e) { (void)e; cancel_now(); }

static void cancel_cb(lv_event_t *e)
{
    // Cancel ONLY from the top-left corner (the camera-close convention).
    // Tap-anywhere cancelled the scan every time the user's grip grazed the
    // glass — device testing found that out the hard way.
    //
    // This region now has a CANCEL pill drawn in it, so the target is visible
    // instead of folklore. The region stays slightly larger than the pill and
    // stays live: it costs nothing, and it keeps working for anyone who
    // learned the corner before the pill existed.
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
        scan_bzero(txt, sizeof txt);
        return;
    }
    if (!s_parser) return;
    int rc = qrt_parser_feed(s_parser, data, len);
    int seen = qrt_parser_seen(s_parser), total = qrt_parser_total(s_parser);
    if (rc != 0) {                             // some other QR in view: ignore
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
    if (s_prog) {
        char b[48];
        if (total > 1) snprintf(b, sizeof b, tr(STR_N_PARTS_FMT), seen, total);
        else snprintf(b, sizeof b, "%s", tr(STR_N_READING));
        lv_label_set_text(s_prog, b);
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
        scan_teardown();
        if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
        if (rrc == 0) { if (s_on_psbt) s_on_psbt(s_psbt, n, fmt); }
        else {
            // Backing out here looks to the user exactly like tapping cancel,
            // which is why an oversized PSBT reads as "the scanner quit".
            SCAN_LOG("REJECTED after assembly: rc %d (over %u-byte cap?)",
                     rrc, (unsigned)sizeof s_psbt);
            if (s_on_cancel) s_on_cancel();
        }
        scan_bzero(s_psbt, sizeof s_psbt);
    }
}

void wallet_scan_inject(const char *data, size_t len) { feed(data, len); }

static void poll_cb(lv_timer_t *t)
{
    (void)t;
#ifndef SIMULATOR
    size_t n = __atomic_load_n(&s_pend_len, __ATOMIC_ACQUIRE);
    if (n) {
        static char tmp[sizeof s_pend];        // feed() may tear the timer down
        memcpy(tmp, s_pend, n);
        // Keep the slot marked occupied until its contents are gone.
        scan_bzero(s_pend, sizeof s_pend);
        __atomic_store_n(&s_pend_len, 0, __ATOMIC_RELEASE);  // free the slot
        feed(tmp, n);
        scan_bzero(tmp, sizeof tmp);
        return;
    }
    if (camera_spike_check_died() && s_prog) {
        lv_label_set_text(s_prog, tr(STR_N_CAM_STOP));
        if (s_hint) lv_label_set_text(s_hint, tr(STR_N_RETRY));
        lv_obj_invalidate(lv_screen_active()); // video gone: repaint the LVGL screen
    }
#endif
}

static void scan_open_common(lv_obj_t *parent);

void wallet_scan_open(lv_obj_t *parent,
                      void (*on_psbt)(const uint8_t *, size_t, int),
                      void (*on_cancel)(void))
{
    if (s_scr) return;
    s_on_psbt = on_psbt;
    s_on_text = NULL;
    s_on_cancel = on_cancel;
    s_parser = qrt_parser_new();
    scan_open_common(parent);
}

void wallet_scan_open_raw(lv_obj_t *parent,
                          void (*on_text)(const char *, size_t),
                          void (*on_cancel)(void))
{
    if (s_scr) return;
    s_on_psbt = NULL;
    s_on_text = on_text;
    s_on_cancel = on_cancel;
    s_parser = NULL;                    // raw: no PSBT assembly
    scan_open_common(parent);
}

// ---- ADDENDUM-01 section 3 geometry ----
// The reticle column, and the card that answers the question nobody asks out
// loud: somebody new to this is pointing a camera at a code they cannot read,
// on a device holding their keys, wondering whether this is how people get
// robbed. The screen has the space and the dead time to answer it.
// 112 to WT_CONTENT_BOTTOM (398) is 286. The preview takes 208 of it and the
// two status lines under it take the rest, so both land above the action bar
// instead of behind it. The right column's own stack (caption, one CAN row,
// three CANNOT rows, the paragraph) is measured to the same floor.
#define SCN_CAM_X 48
#define SCN_CAM_Y 112
#define SCN_CAM_W 300
#define SCN_CAM_H 208
#define SCN_COL_X 396
#define SCN_COL_W 356

// One permission row: a glyph in `col` and a line of text beside it.
static void scan_perm(lv_obj_t *par, int y, const char *glyph, int str,
                      lv_color_t col)
{
    wt_lbl(par, glyph, 14, y, wt_font14(), col);
    lv_obj_t *t = wt_lbl(par, tr(str), 40, y, wt_font14(), WT_MUT);
    lv_obj_set_width(t, SCN_COL_W - 54);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
}

static void scan_psbt_help_cb(lv_event_t *e)
{
    (void)e;
    wallet_info_help_card_open(s_scr, tr(STR_N_PSBT_T), tr(STR_N_PSBT_B));
}

static void scan_open_common(lv_obj_t *parent)
{
    scan_bzero(s_pend, sizeof s_pend);
    scan_bzero(s_psbt, sizeof s_psbt);
    __atomic_store_n(&s_pend_len, 0, __ATOMIC_RELEASE);   // camera not started yet

    // wt_screen, not a bare container: this screen used to build its own 800x480
    // object and so was the one wallet surface that did not wear the card frame,
    // the title treatment or the action bar the rest of the device has.
    s_scr = wt_screen(parent, tr(STR_N_T), tr(STR_N_S));
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scr, cancel_cb, LV_EVENT_CLICKED, NULL);
#ifndef SIMULATOR
    lv_obj_add_event_cb(s_scr, zoom_drag_cb, LV_EVENT_ALL, NULL);
#endif

    // The preview column. An empty bordered panel, so the composition reads the
    // same in the simulator (which never runs a camera) as on the device, and so
    // the column is a viewport before the first frame lands in it.
    lv_obj_t *vp = lv_obj_create(s_scr);
    lv_obj_remove_style_all(vp);
    lv_obj_set_pos(vp, SCN_CAM_X, SCN_CAM_Y);
    lv_obj_set_size(vp, SCN_CAM_W, SCN_CAM_H);
    lv_obj_set_style_radius(vp, 10, 0);
    lv_obj_set_style_border_width(vp, 1, 0);
    lv_obj_set_style_border_color(vp, WT_EDGE, 0);
    lv_obj_remove_flag(vp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(vp, LV_OBJ_FLAG_SCROLLABLE);

    // Right column. One thing a scan CAN do, then the three it cannot.
    wt_section(s_scr, tr(STR_N_CAN_CAP), SCN_COL_X, SCN_CAM_Y);

    lv_obj_t *can = lv_obj_create(s_scr);
    lv_obj_remove_style_all(can);
    lv_obj_set_pos(can, SCN_COL_X, SCN_CAM_Y + 26);
    lv_obj_set_size(can, SCN_COL_W, 40);
    lv_obj_set_style_radius(can, 8, 0);
    lv_obj_set_style_border_width(can, 1, 0);
    lv_obj_set_style_border_color(can, WT_OK, 0);
    lv_obj_set_style_border_opa(can, 77, 0);      // ~30 percent
    lv_obj_set_style_bg_color(can, WT_OK, 0);
    lv_obj_set_style_bg_opa(can, 13, 0);          // ~5 percent
    lv_obj_remove_flag(can, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(can, LV_OBJ_FLAG_SCROLLABLE);
    scan_perm(can, 11, LV_SYMBOL_OK, STR_N_CAN, WT_OK);

    lv_obj_t *cant = lv_obj_create(s_scr);
    lv_obj_remove_style_all(cant);
    lv_obj_set_pos(cant, SCN_COL_X, SCN_CAM_Y + 76);
    lv_obj_set_size(cant, SCN_COL_W, 68);
    lv_obj_set_style_radius(cant, 8, 0);
    lv_obj_set_style_border_width(cant, 1, 0);
    lv_obj_set_style_border_color(cant, WT_HAIR, 0);
    lv_obj_set_style_bg_color(cant, WT_PANEL, 0);
    lv_obj_set_style_bg_opa(cant, LV_OPA_COVER, 0);
    lv_obj_remove_flag(cant, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(cant, LV_OBJ_FLAG_SCROLLABLE);
    // TWO crosses, not the three ADDENDUM-01 drew. It also listed "reach your
    // recovery words" and "put this device online", and the owner cut both:
    // nobody arriving at a scan screen fears either. The product is called an
    // airgapped signer and the home screen says so, so "cannot go online" tells
    // a reader something they already knew, and a QR reaching the recovery words
    // is not a worry anybody has until this card invents it. Padding a safety
    // card with risks the reader does not hold makes the one that matters read
    // like boilerplate. What is actually being asked, camera pointed at a code
    // you cannot read on a device holding your keys, is "can this spend my
    // money", and these two answer exactly that.
    scan_perm(cant, 10, LV_SYMBOL_CLOSE, STR_N_CANT_SPEND, WT_STOP);
    scan_perm(cant, 38, LV_SYMBOL_CLOSE, STR_N_CANT_SIGN,  WT_STOP);

    wt_note(s_scr, tr(STR_N_NOTHING_SIGNED), SCN_COL_X, SCN_CAM_Y + 154,
            SCN_COL_W - 44, 76);
    wt_help_chip(s_scr, 752, SCN_CAM_Y + 154, MUT_COL, scan_psbt_help_cb, NULL);

    // Status lives under the preview now, not in the middle of the screen: the
    // middle of the screen is the permissions card.
    s_prog = lv_label_create(s_scr);
    lv_label_set_text(s_prog, tr(STR_N_STARTING));
    lv_obj_set_style_text_color(s_prog, INK_COL, 0);
    lv_obj_set_style_text_font(s_prog, wt_font23(), 0);
    lv_obj_set_pos(s_prog, SCN_CAM_X, SCN_CAM_Y + SCN_CAM_H + 10);

    // The camera-failure path writes the driver status here, which is technical
    // metadata and belongs at font14.
    s_hint = lv_label_create(s_scr);
    lv_label_set_text(s_hint, "");
    lv_obj_set_style_text_color(s_hint, MUT_COL, 0);
    lv_obj_set_style_text_font(s_hint, wt_font14(), 0);
    lv_obj_set_width(s_hint, SCN_CAM_W);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_hint, SCN_CAM_X, SCN_CAM_Y + SCN_CAM_H + 40);

    // CANCEL in the action row, and the SD alternative NAMED beside it: the
    // person struggling to scan does not know they have another option, because
    // that choice was two screens ago. Muted text, not a button, because it is
    // not reachable from here without cancelling first.
    wt_pill(s_scr, tr(STR_C_CANCEL), 48, WT_ACTION_Y, 200, cancel_btn_cb, NULL);
    wt_lbl(s_scr, tr(STR_N_OR_SD), 268, WT_ACTION_Y + 16, wt_font14(), MUT_COL);

    s_tmr = lv_timer_create(poll_cb, 80, NULL);

#ifndef SIMULATOR
    camera_spike_set_preview_rect(SCN_CAM_X, SCN_CAM_Y, SCN_CAM_W, SCN_CAM_H);
    if (camera_scan_start(s_bus, decode_cb)) {
        if (s_prog) lv_label_set_text(s_prog, tr(STR_N_WAIT_QR));
    } else {
        lv_label_set_text(s_prog, tr(STR_C_CAM_UNAVAIL));
        lv_label_set_text(s_hint, camera_spike_status());
    }
#else
    lv_label_set_text(s_prog, tr(STR_N_WAIT_QR));
#endif
}
