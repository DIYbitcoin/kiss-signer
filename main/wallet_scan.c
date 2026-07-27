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
    scan_teardown();
    scan_bzero(s_psbt, sizeof s_psbt);
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    if (s_on_cancel) s_on_cancel();
}

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

static void scan_open_common(lv_obj_t *parent)
{
    scan_bzero(s_pend, sizeof s_pend);
    scan_bzero(s_psbt, sizeof s_psbt);
    __atomic_store_n(&s_pend_len, 0, __ATOMIC_RELEASE);   // camera not started yet

    s_scr = lv_obj_create(parent);
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_size(s_scr, 800, 480);
    lv_obj_set_style_bg_color(s_scr, BG_COL, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_move_foreground(s_scr);
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scr, cancel_cb, LV_EVENT_CLICKED, NULL);
#ifndef SIMULATOR
    lv_obj_add_event_cb(s_scr, zoom_drag_cb, LV_EVENT_ALL, NULL);
#endif

    // Use the same localized CLOSE wording as the baked camera framebuffer
    // overlay. On hardware the live camera bypasses LVGL; in the simulator this
    // pill is the equivalent visible control.
    wt_pillh(s_scr, tr(STR_C_OSD_CLOSE), 48, 24, 150, 56,
             cancel_btn_cb, NULL);

    lv_obj_t *cap = lv_label_create(s_scr);
    lv_label_set_text(cap, tr(STR_N_T));
    lv_obj_set_style_text_color(cap, INK_COL, 0);
    lv_obj_set_style_text_font(cap, wt_font28(), 0);
    lv_obj_set_style_text_letter_space(cap, 3, 0);
    lv_obj_set_pos(cap, 220, 24);

    lv_obj_t *sub = wt_note(s_scr, tr(STR_N_S), 220, 62, 530, 29);
    lv_obj_set_style_text_color(sub, MUT_COL, 0);

    s_prog = lv_label_create(s_scr);
    lv_label_set_text(s_prog, tr(STR_N_STARTING));
    lv_obj_set_style_text_color(s_prog, INK_COL, 0);
    lv_obj_set_style_text_font(s_prog, wt_font28(), 0);
    lv_obj_align(s_prog, LV_ALIGN_CENTER, 0, -20);

    // Was STR_N_TAP_CANCEL, "tap the top-left corner to cancel" -- a sentence
    // that only existed because the control was invisible. The pill says it
    // now. The label stays because the camera-failure path writes the driver
    // status into it, which is technical metadata and belongs at font14.
    s_hint = lv_label_create(s_scr);
    lv_label_set_text(s_hint, "");
    lv_obj_set_style_text_color(s_hint, MUT_COL, 0);
    lv_obj_set_style_text_font(s_hint, wt_font14(), 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 24);

    s_tmr = lv_timer_create(poll_cb, 80, NULL);

#ifndef SIMULATOR
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
