// The ecash screens. See kiss_ecash_ui.h.
//
// Four screens, each one decision or one fact: the ecash key the vault page
// asked for, the spend under review with the slide that signs it, the answer
// the page reads back, and a refusal that says which rule the request broke.
// Every number on the review is what the swap moves: kiss_ecash_review proves
// the change and counts the rest.
#include "kiss_ecash_ui.h"

#include <stdio.h>
#include <string.h>

#include "i18n.h"
#include "kiss_crypto.h"
#include "kiss_ecash.h"
#include "kiss_theme.h"
#include "kiss_wipe.h"

// What the page shows to ask for the key, exactly. Anything else that is JSON
// is a spend request or nothing.
static const char KEY_REQUEST[] = "{\"v\":1,\"want\":\"key\"}";

static lv_obj_t *s_parent;
static lv_obj_t *s_scr;
static lv_obj_t *s_qr;
static lv_obj_t *s_slide_lbl;
static void (*s_done)(void);
static kiss_ecash_summary_t s_sum;
static char s_reply[256];
static bool s_signing;
static lv_timer_t *s_sign_timer;   // the 30 ms between the slide and the signature

bool kiss_ecash_ui_wants(const uint8_t *bytes, size_t len)
{
    size_t i = 0;
    while (i < len && (bytes[i] == ' ' || bytes[i] == '\n' || bytes[i] == '\r'))
        i++;
    return i < len && bytes[i] == '{';
}

bool kiss_ecash_ui_active(void) { return s_scr != NULL; }

static void drop_screen(void)
{
    // The answer QR carries nothing secret, but the kit's scrub is how every
    // QR here leaves, so nothing has to remember which ones mattered.
    if (s_qr) { wt_qr_scrub(s_qr); s_qr = NULL; }
    if (s_scr) { lv_obj_delete_async(s_scr); s_scr = NULL; }
    s_slide_lbl = NULL;
}

static void forget(void)
{
    kiss_ecash_review_clear();
    kiss_wipe(s_reply, sizeof s_reply);
    memset(&s_sum, 0, sizeof s_sum);
    // A close inside those 30 ms (the idle lock) must not leave a timer that
    // later draws a refusal over whatever screen comes next.
    if (s_sign_timer) { lv_timer_delete(s_sign_timer); s_sign_timer = NULL; }
    s_signing = false;
}

void kiss_ecash_ui_close(void)
{
    forget();
    drop_screen();
    s_done = NULL;
}

static void leave_cb(lv_event_t *e)
{
    (void)e;
    if (s_signing) return;
    void (*done)(void) = s_done;
    kiss_ecash_ui_close();
    if (done) done();
}

// Every ecash screen is reached from SIGN's scanner, and the strip row says
// so, the way KEYS names the conversation it is about. Each caller builds its
// own wt_chrome with a literal key, so the screen coverage gate can name it.
static void screen_done(void)
{
    char trail[48];
    snprintf(trail, sizeof trail, "%s / ECASH", tr(STR_S_T));
    wt_trail(s_scr, WT_ICON_QR, trail, false);
}

// ---- refusal ----
static const char *refusal_text(int rc)
{
    switch (rc) {
    case KISS_ECASH_E_LOCKED:
        return kiss_session_card() ? tr(STR_EC_NO_CARD) : tr(STR_C_LOCKED_B);
    case KISS_ECASH_E_VERSION:  return tr(STR_EC_NO_VERSION);
    case KISS_ECASH_E_NOT_OURS: return tr(STR_EC_NO_OURS);
    case KISS_ECASH_E_LOCK:     return tr(STR_EC_NO_LOCK);
    case KISS_ECASH_E_DUP:      return tr(STR_EC_NO_DUP);
    case KISS_ECASH_E_CHANGE:   return tr(STR_EC_NO_CHANGE);
    default:                    return tr(STR_EC_NO_FORMAT);
    }
}

// The refusal is its own render, under the title of the flow it stopped,
// shaped like the locked scan key refusal: a card naming the state, then the
// reason. No QR and nothing that looks like one.
static void refusal_screen(int rc, bool key)
{
    forget();
    drop_screen();
    s_scr = key ? wt_chrome(s_parent, tr(STR_EC_KEY_T))
                : wt_chrome(s_parent, tr(STR_EC_SPEND_T));
    screen_done();
    lv_obj_t *card = wt_card(s_scr, WT_LANE_X, WT_LANE_Y + SY(14), WT_LANE_W,
                             SY(140));
    lv_obj_t *ic = wt_lbl(card, LV_SYMBOL_CLOSE, 0, 0, wt_font34(), WT_WARN);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, SX(28), 0);
    lv_obj_t *chip = wt_state_chip(card, tr(STR_EC_NO_T), WT_WARN);
    lv_obj_align(chip, LV_ALIGN_LEFT_MID, SX(92), 0);
    wt_note(s_scr, refusal_text(rc), WT_LANE_X, WT_LANE_Y + SY(176), WT_LANE_W,
            SY(90));
    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, SX(592), WT_ACTION_Y,
                    SX(160), true, leave_cb, NULL);
}

// ---- a QR, a few facts beside it, and one way out ----
#if KISS_NARROW
#define QR_CARD SQ(204)
#define QR_PX   SQ(190)
#else
#define QR_CARD SQ(270)
#define QR_PX   SQ(250)
#endif

static int qr_screen(const char *payload)
{
    screen_done();
    wt_qr_card(s_scr, &s_qr, WT_LANE_X, WT_LANE_Y, QR_CARD, QR_PX);
    if (!s_qr || wt_qr_update(s_qr, payload, (uint32_t)strlen(payload)) != LV_RESULT_OK)
        return 1;
    wt_arrow_action(s_scr, tr(STR_C_DONE), true, false, SX(592), WT_ACTION_Y,
                    SX(160), true, leave_cb, NULL);
    return 0;
}

// The column right of the code: x and width.
#define COL_X (WT_LANE_X + QR_CARD + SX(28))
#define COL_W (WT_LANE_X + WT_LANE_W - COL_X)

static void key_screen(void)
{
    char key[160];
    if (kiss_session_ecash_export(key, sizeof key) != 0) {
        refusal_screen(KISS_ECASH_E_LOCKED, true);
        return;
    }
    drop_screen();
    s_scr = wt_chrome(s_parent, tr(STR_EC_KEY_T));
    if (qr_screen(key)) {
        refusal_screen(KISS_ECASH_E_INTERNAL, true);
        return;
    }
    // The fingerprint and path the page will show after it reads the code,
    // so the owner can match the two.
    char fp[12];
    snprintf(fp, sizeof fp, "%.8s", key + 1);
    for (char *c = fp; *c; c++)
        if (*c >= 'a' && *c <= 'f') *c = (char)(*c - 'a' + 'A');
    wt_lbl(s_scr, fp, COL_X, WT_LANE_Y, wt_font_mono28(), WT_INK);
    wt_lbl(s_scr, "129373h/10h/0h/0h", COL_X, WT_LANE_Y + SY(40),
           wt_font_mono14(), WT_MUT);
    wt_note(s_scr, tr(STR_EC_KEY_NOTE), COL_X, WT_LANE_Y + SY(76), COL_W,
            SY(180));
}

static void answer_screen(void)
{
    drop_screen();
    s_scr = wt_chrome(s_parent, tr(STR_S_SIGNED_T));
    if (qr_screen(s_reply)) {
        refusal_screen(KISS_ECASH_E_INTERNAL, false);
        return;
    }
    wt_note(s_scr, tr(STR_EC_ANSWER_NOTE), COL_X, WT_LANE_Y, COL_W, SY(180));
}

// ---- review and sign ----
static void sign_now(lv_timer_t *t)
{
    lv_timer_delete(t);
    s_sign_timer = NULL;
    int rc = kiss_ecash_sign_reviewed(s_reply, sizeof s_reply);
    s_signing = false;
    if (rc != KISS_ECASH_OK) {
        refusal_screen(rc, false);
        return;
    }
    answer_screen();
}

static void slide_done(void *ud)
{
    (void)ud;
    if (s_signing) return;
    s_signing = true;
    if (s_slide_lbl) lv_label_set_text(s_slide_lbl, tr(STR_S_SIGNING));
    s_sign_timer = lv_timer_create(sign_now, 30, NULL);   // let the word paint first
}

#if KISS_NARROW
#define ROW_H SY(66)
#else
#define ROW_H SY(70)
#endif

static void spend_screen(void)
{
    drop_screen();
    s_scr = wt_chrome(s_parent, tr(STR_EC_SPEND_T));
    screen_done();
    char sending[48], back[48];
    // The scheme is the one part of a mint's address that is never the
    // question, and dropping it is what lets the name fit the row.
    const char *mint = s_sum.mint;
    if (!strncmp(mint, "https://", 8)) mint += 8;
    snprintf(sending, sizeof sending, "%llu %s",
             (unsigned long long)s_sum.leaving, s_sum.unit);
    snprintf(back, sizeof back, "%llu %s",
             (unsigned long long)s_sum.change, s_sum.unit);
    int y = WT_LANE_Y;
    lv_obj_t *r = wt_line_row(s_scr, WT_LANE_X, y, WT_LANE_W, ROW_H,
                              tr(STR_EC_SENDING), sending, wt_font_mono28(),
                              WT_INK, tr(STR_EC_SENDING_SUB), NULL, NULL, NULL);
    wt_line_row_stage(r, 0);
    wt_line_rule(s_scr, WT_LANE_X, y + ROW_H, WT_LANE_W);
    y += ROW_H;
    r = wt_line_row(s_scr, WT_LANE_X, y, WT_LANE_W, ROW_H, tr(STR_EC_BACK),
                    back, wt_font_mono28(), WT_INK, tr(STR_EC_BACK_SUB), NULL,
                    NULL, NULL);
    wt_line_row_stage(r, 1);
    wt_line_rule(s_scr, WT_LANE_X, y + ROW_H, WT_LANE_W);
    y += ROW_H;
    r = wt_line_row(s_scr, WT_LANE_X, y, WT_LANE_W, ROW_H, tr(STR_EC_MINT),
                    mint, wt_font23(), WT_INK, tr(STR_EC_MINT_SUB), NULL,
                    NULL, NULL);
    wt_line_row_stage(r, 2);

    wt_slide_t sg = {
        .txt  = tr(STR_S_HOLD_TO_SIGN),
        .held = tr(STR_G_FW_KEEP_HOLDING),
        .x    = SX(48),
        .y    = WT_ACTION_Y_SLIDE,
        .w    = SX(310),
        .done = slide_done,
        // the scanner's last frame can land as a press on the new screen
        .deaf_ms = 400,
    };
    lv_obj_t *bar = wt_slide(s_scr, &sg);
    s_slide_lbl = wt_slide_label(bar);
    wt_arrow_action(s_scr, tr(STR_C_BACK), true, false, SX(592), WT_ACTION_Y,
                    SX(160), true, leave_cb, NULL);
}

void kiss_ecash_ui_open(lv_obj_t *parent, const uint8_t *bytes, size_t len,
                        void (*done)(void))
{
    kiss_ecash_ui_close();
    s_parent = parent;
    s_done = done;
    if (len == sizeof KEY_REQUEST - 1 && memcmp(bytes, KEY_REQUEST, len) == 0) {
        key_screen();
        return;
    }
    int rc = kiss_ecash_review((const char *)bytes, len, &s_sum);
    if (rc != KISS_ECASH_OK) {
        refusal_screen(rc, false);
        return;
    }
    spend_screen();
}
