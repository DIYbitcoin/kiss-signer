// See wallet_fw_ui.h. Four screens: what is on the card, what it costs to take
// it, the write, and how it ended.
//
// The shape is lifted from the storage move in wallet_settings.c rather than
// invented, because this is the same kind of act: SD backed, irreversible in
// the moment, and worth a hold rather than a tap. Copying it means the two
// screens in this product that rewrite something permanent behave identically,
// which is worth more than any improvement either could make alone.
#include "wallet_fw_ui.h"

#include <stdio.h>
#include <string.h>

#include "wallet_fw.h"
#include "wallet_theme.h"
#include "i18n.h"

#ifndef SIMULATOR
#include "esp_system.h"
#endif

static lv_obj_t *s_scr;
static lv_obj_t *s_parent;
static void (*s_done)(void);
static wfw_image_t s_img;
static lv_obj_t *s_pct_card;

// The pair geometry from the project rules, shared with the fingerprint reveal,
// the passphrase intro and the backup check. Not re-derived here.
#define BLK_Y   232
#define BLK_W   344
#define BLK_L_X  48
#define BLK_R_X 408
#define BLK_H   (WT_CONTENT_BOTTOM - BLK_Y)
// The font14 heading wt_why_block draws above the body, allowing for one
// that wraps to two lines. Same budget wallet_setup.c uses.
#define WT_FW_HEAD_ROOM 46

static void fw_screen(void);

static void close_cb(lv_event_t *e)
{
    (void)e;
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_pct_card = NULL;
    if (s_done) s_done();
}

static void fresh(const char *title, const char *sub)
{
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_pct_card = NULL;
    s_scr = wt_screen(s_parent, title, sub);
}

// Bytes as something a person reads off glass. Whole MB: a firmware image is
// megabytes and the third decimal place of one is not a fact anybody acts on.
static void size_str(char *out, size_t n, size_t bytes)
{
    unsigned mb10 = (unsigned)((bytes * 10 + 524288) / 1048576);
    snprintf(out, n, "%u.%u MB", mb10 / 10, mb10 % 10);
}

// ---- 4. result -------------------------------------------------------------

#ifndef SIMULATOR
static void restart_cb(lv_event_t *e) { (void)e; esp_restart(); }
#else
static void restart_cb(lv_event_t *e) { close_cb(e); }
#endif

static void result_back_cb(lv_event_t *e)
{
    (void)e;
    fw_screen();
}

static void result_screen(int rc)
{
    const char *title, *body;
    lv_color_t col;

    if (rc == WFW_OK) {
        title = tr(STR_G_FW_OK_T);
        body  = tr(STR_G_FW_OK_B);
        col   = WT_OK;
    } else {
        title = tr(STR_G_FW_FAIL_T);
        col   = WT_STOP;
        // Each failure gets its own sentence. "It did not work" on a screen
        // that just spent a minute writing to flash tells the owner nothing
        // about whether the device still boots, which is the only thing they
        // want to know.
        body = rc == WFW_ERR_REJECTED  ? tr(STR_G_FW_FAIL_SIG_B)
             : rc == WFW_ERR_CARD_GONE ? tr(STR_G_FW_FAIL_CARD_B)
             : rc == WFW_ERR_UNSIGNED  ? tr(STR_G_FW_UNSIGNED_B)
             :                           tr(STR_G_FW_FAIL_WRITE_B);
    }

    fresh(title, NULL);
    lv_obj_set_style_text_color(wt_screen_title(s_scr), col, 0);
    // The rule colour carries the outcome, so the block agrees with the title
    // rather than sitting grey under a green or red heading.
    wt_why_body(s_scr, body, 136, col, true);

    if (rc == WFW_OK) {
        lv_obj_t *p = wt_pill(s_scr, tr(STR_G_FW_RESTART), 300, WT_ACTION_Y, 200,
                              restart_cb, NULL);
        wt_pill_primary(p);
    } else {
        wt_pill(s_scr, tr(STR_C_OK), 300, WT_ACTION_Y, 200, result_back_cb, NULL);
    }
}

// ---- 3. writing ------------------------------------------------------------

static void progress_cb(int pct, void *ud)
{
    (void)ud;
    if (!s_pct_card) return;
    char v[16];
    snprintf(v, sizeof v, "%d%%", pct);
    // Set the value, do not rebuild the card. This used to delete and recreate
    // it on every percent, so a write dirtied a 300x90 rectangle a hundred
    // times while the LVGL task was already blocked by flash erase -- reported
    // from the bench as the screen flashing and looking like shit, and no gate
    // can see it because the simulator writes no flash and the desktop
    // display has no framebuffer to starve.
    wt_value_card_set(s_pct_card, v);
    // The write blocks this task, so nothing else will pump LVGL. Without this
    // the bar would jump from 0 to 100 when the whole thing finished.
    lv_refr_now(NULL);
}

// The write itself, one LVGL tick after the screen that announces it.
//
// Deferred rather than called inline, for two reasons that happen to be the
// same reason. On the device, an inline call blocks the LVGL task from the
// moment the hold completes, so the writing screen would only appear because
// lv_refr_now forces it out mid erase -- the screen would be painted by the
// work rather than before it. And the screen walk saves a frame between
// pump() calls, so a screen that is drawn and replaced inside one callback is
// a screen no gate can ever measure: WRITING was the one stop overlapcheck
// could not reach.
static void install_now(lv_timer_t *t)
{
    lv_timer_delete(t);
    int rc = wallet_fw_install(&s_img, progress_cb, NULL);
    result_screen(rc);
}

static void writing_apply(void *ud)
{
    (void)ud;
    fresh(tr(STR_G_FW_WRITING_T), NULL);
    // No BACK and no CANCEL: past this point the receiving slot is being erased,
    // and the honest options are finish or lose power, neither of which is a
    // button.
    s_pct_card = wt_value_card(s_scr, tr(STR_G_FW_PCT), "0%", 250, 170, 300, true);
    wt_why_block(s_scr, NULL, tr(STR_G_FW_RISK_B), BLK_L_X, 300, 704,
                 WT_CONTENT_BOTTOM - 300, NULL, WT_WARN);

    lv_timer_t *t = lv_timer_create(install_now, 30, NULL);
    lv_timer_set_repeat_count(t, 1);
}

// ---- 2. confirm ------------------------------------------------------------

static void confirm_cancel_cb(lv_event_t *e)
{
    (void)e;
    fw_screen();
}

static void confirm_screen(void)
{
    bool down = s_img.cmp < 0;
    fresh(tr(STR_G_FW_CONFIRM_T), s_img.version);

    // Two claims, not one paragraph: how it works on the left in the accent,
    // where it goes wrong on the right in WT_WARN. A downgrade swaps the left
    // block for the one that says so, because on that path the interesting
    // claim is not how the check works but that this goes backwards.
    const char *lh = down ? tr(STR_G_FW_DOWN_H) : tr(STR_G_FW_WHY_H);
    const char *lb = down ? tr(STR_G_FW_DOWN_B) : tr(STR_G_FW_WHY_B);
    const char *rh = tr(STR_G_FW_RISK_H);
    const char *rb = tr(STR_G_FW_RISK_B);
    // One shared size across the pair, measured the way the proven pairs do it:
    // BLK_W - 14 because wt_why_block spends that on its rule and padding, and
    // BLK_H - HEAD_ROOM - 8 because the heading is drawn above the body at
    // font14 and the box's own metrics cost a few pixels on top. Measuring
    // against the full BLK_W picked a size for a wider box than the text
    // actually gets, so it wrapped to an extra line and ran 7px past
    // WT_CONTENT_BOTTOM in Japanese and Swedish.
    const lv_font_t *f = wt_body_font2(lb, rb, BLK_W - 14, BLK_H - WT_FW_HEAD_ROOM - 8);
    wt_why_block(s_scr, lh, lb, BLK_L_X, BLK_Y, BLK_W, BLK_H, f,
                 down ? WT_WARN : wt_accent());
    wt_why_block(s_scr, rh, rb, BLK_R_X, BLK_Y, BLK_W, BLK_H, f, WT_WARN);

    // The tall row, 1500 ms, matching the storage move exactly. CANCEL takes the
    // tall geometry too, because a row whose pills differ in height stops
    // looking like a row.
    wt_hold_pill(s_scr, tr(STR_G_FW_HOLD), 48, WT_ACTION_Y_TALL, 330,
                 WT_ACTION_H_TALL, 1500, writing_apply, NULL);
    lv_obj_t *cancel = wt_pillh(s_scr, tr(STR_C_CANCEL), 585, WT_ACTION_Y_TALL,
                                165, WT_ACTION_H_TALL, confirm_cancel_cb, NULL);
    lv_obj_set_ext_click_area(cancel, 10);
}

static void install_cb(lv_event_t *e)
{
    (void)e;
    confirm_screen();
}

// ---- 1. firmware -----------------------------------------------------------

// The states where there is nothing to install. Each is two claims rather than
// one paragraph, so the screen never becomes a wall of text, and each says what
// to DO rather than only what went wrong.
static void nothing_to_install(int rc)
{
    const char *lh, *lb;
    switch (rc) {
    case WFW_ERR_NO_CARD:    lh = tr(STR_G_FW_NOCARD_H);   lb = tr(STR_G_FW_NOCARD_B);   break;
    case WFW_ERR_NO_FILE:    lh = tr(STR_G_FW_NOFILE_H);   lb = tr(STR_G_FW_NOFILE_B);   break;
    case WFW_ERR_TOO_BIG:    lh = tr(STR_G_FW_BIG_H);      lb = tr(STR_G_FW_BIG_B);      break;
    case WFW_ERR_UNSIGNED:   lh = tr(STR_G_FW_UNSIGNED_H); lb = tr(STR_G_FW_UNSIGNED_B); break;
    case WFW_ERR_SAME:       lh = tr(STR_G_FW_SAME_H);     lb = tr(STR_G_FW_SAME_B);     break;
    default:                 lh = tr(STR_G_FW_BAD_H);      lb = tr(STR_G_FW_BAD_B);      break;
    }
    const char *rh = tr(STR_G_FW_WHERE_H);
    const char *rb = tr(STR_G_FW_WHERE_B);
    const lv_font_t *f = wt_body_font2(lb, rb, BLK_W - 14, BLK_H - WT_FW_HEAD_ROOM - 8);
    wt_why_block(s_scr, lh, lb, BLK_L_X, BLK_Y, BLK_W, BLK_H, f,
                 rc == WFW_ERR_SAME ? wt_accent() : WT_WARN);
    wt_why_block(s_scr, rh, rb, BLK_R_X, BLK_Y, BLK_W, BLK_H, f, wt_accent());
}

static void fw_screen(void)
{
    int rc = wallet_fw_scan(&s_img);
    // A build that cannot check a signature has nothing to say about whatever
    // is on the card, so that answer outranks the scan's.
    if (wallet_fw_available() != WFW_OK)
        rc = WFW_ERR_UNSIGNED;

    char sub[64];
    snprintf(sub, sizeof sub, tr(STR_G_FW_RUNNING_FMT), wallet_fw_running_version());
    fresh(tr(STR_G_FW_T), sub);

    bool have_image = s_img.version[0] != 0;
    bool installable = (rc == WFW_OK || rc == WFW_ERR_OLDER);

    // The framed subject whenever there is one: the version on the card, which
    // is what the whole screen is about. A pill would be the action, not the
    // subject.
    if (have_image)
        wt_value_card(s_scr, tr(STR_G_FW_ON_CARD), s_img.version,
                      WT_LIST_L_X, WT_LIST_Y(0), WT_LIST_W, true);

    if (installable) {
        // Facts as rows on the list grid, every one with a mark. The right
        // column, so the value card keeps the left.
        char sz[24];
        size_str(sz, sizeof sz, s_img.size);
        const char *dir = s_img.cmp > 0 ? tr(STR_G_FW_NEWER) : tr(STR_G_FW_OLDER);

        // The file name is the row's LABEL, not its value, and there is no
        // "File" word in front of it. As a value it had to share the row with a
        // label and wrapped into it: the walk caught 22 characters of
        // kiss-signer-99.0.0.bin overlapping "File" by 5x28px in every locale,
        // and a real release name is longer than the fixture's. As the label it
        // gets the row's whole width, and the card mark already says what kind
        // of name it is, which is the rule about preferring a mark to a word.
        wt_row_x(s_scr, WT_ICON_SD, s_img.name, NULL, NULL,
                 NULL, NULL, WT_INK, false,
                 WT_LIST_R_X, WT_LIST_Y(0), WT_LIST_W, WT_ROW_H, NULL, NULL);
        wt_row_x(s_scr, LV_SYMBOL_DOWNLOAD, tr(STR_G_FW_ROW_SIZE), NULL, NULL,
                 sz, NULL, WT_INK, false,
                 WT_LIST_R_X, WT_LIST_Y(1), WT_LIST_W, WT_ROW_H, NULL, NULL);
        lv_obj_t *sig = wt_row_x(s_scr, LV_SYMBOL_OK, tr(STR_G_FW_ROW_SIG),
                                 NULL, NULL, tr(STR_G_FW_SIG_OK), NULL, WT_INK,
                                 false, WT_LIST_R_X, WT_LIST_Y(2), WT_LIST_W,
                                 WT_ROW_H, NULL, NULL);
        wt_row_sev(sig, WT_SEV_OK);

        // The direction sits under the card it describes, coloured by what it
        // means: forward is ordinary, backward is not.
        lv_obj_t *v = wt_row_x(s_scr, WT_ICON_REPLACE, tr(STR_G_FW_ROW_VER),
                               NULL, NULL, dir, NULL,
                               s_img.cmp < 0 ? WT_WARN : WT_INK, false,
                               WT_LIST_L_X, WT_LIST_Y(2), WT_LIST_W, WT_ROW_H,
                               NULL, NULL);
        if (s_img.cmp < 0) wt_row_sev(v, WT_SEV_WARN);

        lv_obj_t *p = wt_pill_icon(s_scr, LV_SYMBOL_DOWNLOAD, tr(STR_G_FW_INSTALL),
                                   48, WT_ACTION_Y, 240, WT_ACTION_H,
                                   install_cb, NULL);
        if (rc == WFW_OK) wt_pill_primary(p);
    } else {
        // Nothing to take. The rows are dropped rather than shown greyed: they
        // would sit where the explanation has to go, and a file name is not
        // what the owner needs when the answer is no.
        nothing_to_install(rc);
    }

    wt_pill(s_scr, tr(STR_C_BACK), WT_BACK_X, WT_ACTION_Y, 140, close_cb, NULL);
}

void wallet_fw_ui_open(lv_obj_t *parent, void (*done_cb)(void))
{
    s_parent = parent;
    s_done = done_cb;
    s_scr = NULL;
    fw_screen();
}
