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

bool wallet_scan_active(void) { return s_scr != NULL; }
void wallet_scan_set_bus(void *bus) { s_bus = bus; }

static void scan_teardown(void)
{
    if (s_tmr) { lv_timer_delete(s_tmr); s_tmr = NULL; }
#ifndef SIMULATOR
    camera_scan_stop();
#endif
    if (s_parser) { qrt_parser_free(s_parser); s_parser = NULL; }
    s_on_text = NULL;                  // raw mode never survives a teardown
    s_prog = NULL; s_hint = NULL;
}

void wallet_scan_close(void)   // idle auto-lock: no on_cancel (caller locks next)
{
    scan_teardown();
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
}

static void cancel_cb(lv_event_t *e)
{
    // Cancel ONLY from the top-left corner (the camera-close convention).
    // Tap-anywhere cancelled the scan every time the user's grip grazed the
    // glass — device testing found that out the hard way.
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        if (p.x >= 200 || p.y >= 110) return;
    }
    scan_teardown();
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    if (s_on_cancel) s_on_cancel();
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
        return;
    }
    if (!s_parser) return;
    int rc = qrt_parser_feed(s_parser, data, len);
    int seen = qrt_parser_seen(s_parser), total = qrt_parser_total(s_parser);
    if (rc != 0) return;                       // some other QR in view: ignore
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
        scan_teardown();
        if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
        if (rrc == 0) { if (s_on_psbt) s_on_psbt(s_psbt, n, fmt); }
        else if (s_on_cancel) s_on_cancel();   // oversized/corrupt: back out
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
        __atomic_store_n(&s_pend_len, 0, __ATOMIC_RELEASE);  // free the slot
        feed(tmp, n);
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

    lv_obj_t *cap = lv_label_create(s_scr);
    lv_label_set_text(cap, tr(STR_N_T));
    lv_obj_set_style_text_color(cap, INK_COL, 0);
    lv_obj_set_style_text_font(cap, wt_font28(), 0);
    lv_obj_set_style_text_letter_space(cap, 3, 0);
    lv_obj_set_pos(cap, 48, 30);

    lv_obj_t *sub = lv_label_create(s_scr);
    lv_label_set_text(sub, tr(STR_N_S));
    lv_obj_set_style_text_color(sub, MUT_COL, 0);
    lv_obj_set_style_text_font(sub, wt_font14(), 0);
    lv_obj_set_pos(sub, 48, 68);

    s_prog = lv_label_create(s_scr);
    lv_label_set_text(s_prog, tr(STR_N_STARTING));
    lv_obj_set_style_text_color(s_prog, INK_COL, 0);
    lv_obj_set_style_text_font(s_prog, wt_font28(), 0);
    lv_obj_align(s_prog, LV_ALIGN_CENTER, 0, -20);

    s_hint = lv_label_create(s_scr);
    lv_label_set_text(s_hint, tr(STR_N_TAP_CANCEL));
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
