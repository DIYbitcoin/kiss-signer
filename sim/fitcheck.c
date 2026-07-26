// Explainer fit report: does each body of copy render at font28 (readable at
// arm's length) or fall back to font14, in every locale?
//
// wt_body_font() makes that decision at runtime from lv_text_get_size in the
// ACTIVE locale's font, so guessing line widths by hand is worthless — a line
// one character too long wraps and silently costs a whole 37px row. This tool
// asks LVGL the same question the UI does, for all 21 locales at once, and
// prints the measured height against the budget so a translation can be
// shortened by exactly as much as it overflows.
//
// Build: bash sim/build_fitcheck.sh   Run: /tmp/kissfit [locale ...]
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "i18n.h"
#include "wallet_theme.h"

// Every wt_body_font() call site in the UI: the copy it measures and the
// (width, height) box it has to live inside. Keep in sync with the sources
// listed in the comment beside each entry.
typedef struct {
    const char *surface;   // where the user sees it
    int key;               // STR_* (or -1 for a composed body)
    int w, h;
    int may_be_small;      // 1 = font14 is the accepted outcome here
} slot_t;

static const slot_t SLOTS[] = {
    // main.c:1467 — home fingerprint card
    { "home/fp-card",     STR_H_FP_CARD_B,  720, 152 },
    // wallet_setup.c:279 — amber line under the word grid
    { "setup/paper-only", STR_W_PAPER_ONLY, 700,  40 },
    // wallet_info.c:73 — "?" cards (155 with a chip diagram, 225 without)
    { "wallet/?fp",       STR_I_H_FP_B,     720, 155 },
    { "wallet/?type",     STR_I_H_TYPE_B,   720, 225 },
    { "wallet/?pair",     STR_I_H_PAIR_B,   720, 155 },
    { "wallet/?addr",     STR_I_H_ADDR_B,   720, 225 },
    // wallet_info.c:263,326 — full-screen warnings
    { "wallet/sp-warn",   STR_R_SP_WARN_B,  700, 280 },
    { "wallet/words-warn",STR_I_WARN_B,     700, 270 },
    // wallet_sign.c:355,394,966
    { "sign/why",         -1,               720, 300 },   // composed below
    { "sign/rbf-on",      STR_S_RBF_B_ON,   720, 230 },
    { "sign/rbf-off",     STR_S_RBF_B_OFF,  720, 230 },
    { "sign/?coord",      STR_S_COORD_B,    720, 144 },
    // wallet_ui.c:469,769 — login warning + passphrase intro
    { "login/warn",       STR_L_WARN_B,     720, 200 },
    { "login/pp-intro",   STR_L_PPINTRO_B,  704, 280 },
    // wallet_setup.c — wizard explainers
    { "setup/entropy",    STR_W_RAND_B,     704, 274 },
    { "setup/checksum",   STR_W_CHECK_B,    704, 256 },
    { "setup/verify-in",  STR_W_VINTRO_B,   704, 274 },
    { "setup/verify-ok",  STR_W_VOK_B,      704, 190 },
    { "setup/verify-bad", STR_W_VBAD_B,     704, 190 },
    // wallet_settings.c: the two wipe overlays
    { "wipe/confirm",     STR_G_WIPEC_B,    704, 190 },
    { "wipe/erased",      STR_G_ERASED_B,   704, 160 },
    { "wipe/not-erased",  STR_G_NOERASE_B,  704, 160 },
    // amnesic mode: seed-QR import + passphrase-from-QR
    { "setup/qr-bad",     STR_W_QRBAD_B,    704, 240 },
    { "login/qr-warn",    STR_L_SCAN_WARN_B,704, 274 },
    // wallet_settings.c — the notes under each chooser. These sit in gaps
    // between controls, so 23 (not 28) is the realistic top rung; what matters
    // is that none of them falls to 14.
    { "set/net-main",     STR_G_MAINNET_NOTE, 340, 58 },
    { "set/net-test",     STR_G_TESTNET_NOTE, 340, 58 },
    { "set/ty-native",    STR_G_TY_NATIVE_NOTE, 360, 58 },
    { "set/ty-nested",    STR_G_TY_NESTED_NOTE, 360, 58 },
    { "set/ty-legacy",    STR_G_TY_LEGACY_NOTE, 360, 58 },
    { "set/separate",     STR_G_SEPARATE,     360, 58 },
    { "set/create-note",  STR_G_CREATE_NOTE,  340, 88 },
    { "set/wipe-note",    STR_G_WIPE_NOTE,    340, 88 },
    // wallet_recv.c / wallet_info.c — instructions the user has to act on
    // wt_screen() subtitles: one line, 704px wide, between title and content.
    { "sub/receive",      STR_R_S,            704, 30, 0 },
    { "sub/wallet",       STR_I_S,            704, 30, 0 },
    { "sub/verify",       STR_R_VS,           704, 30, 0 },
    { "sub/words-warn",   STR_I_WARN_S,       704, 30, 0 },
    { "sub/sp-export",    STR_R_SP_EXPORT_S,  704, 30, 0 },
    { "sub/sp-warn",      STR_R_SP_WARN_S,    704, 30, 0 },
    { "sub/pair",         STR_I_PAIR_S,       704, 30, 0 },
    // Procedural, read once with the device in hand, and wedged into a 360px
    // column beside a QR. They auto-fit like everything else, so they grow if
    // the copy is ever shortened -- but font14 is the accepted answer today.
    { "recv/verify",      STR_R_VERIFY_NOTE,  360, 90, 1 },
    { "pair/sparrow",     STR_I_NOTE_SPARROW, 360, 86, 1 },
    { "pair/bluewallet",  STR_I_NOTE_BW,      360, 86, 1 },
    { "pair/prove",       STR_I_PROVE,        360, 72, 1 },
    // wallet_info.c — the note under each action pill
    { "wallet/pair-note", STR_I_PAIR_BTN_NOTE,  340, 89 },
    { "wallet/words-note",STR_I_WORDS_BTN_NOTE, 340, 90 },
};
#define NSLOT ((int)(sizeof SLOTS / sizeof SLOTS[0]))

// Pill labels. A button must never be smaller than the note beside it, and
// notes cap at 23, so font14 here is a FAILURE: it means the box is too narrow
// for that translation and the pill needs widening (or the word shortening).
typedef struct { const char *surface; int key; int w, h, primary; } pill_t;
static const pill_t PILLS[] = {
    { "sign/hold",        STR_S_HOLD_TO_SIGN, 272, 52, 1 },
    { "sign/back",        STR_C_BACK,         140, 52, 0 },
    { "wallet/pair",      STR_I_PAIR_T,       340, 52, 1 },
    { "wallet/words",     STR_I_WORDS_BTN,    340, 52, 0 },
    { "set/create",       STR_G_CREATE_NEW,   340, 52, 0 },
    { "set/wipe",         STR_G_WIPE,         340, 52, 0 },
    { "recv/verify",      STR_R_VERIFY,       148, 52, 0 },
    { "recv/next",        STR_R_NEXT,         130, 52, 0 },
    { "recv/sp",          STR_S_SP_BADGE,     220, 52, 0 },
    { "recv/fresh",       STR_R_FRESH,        124, 44, 0 },
    { "pair/scankey",     STR_R_SP_SCAN_BTN,  190, 60 - 22, 0 },
    { "pair/desktop",     STR_I_DESKTOP,      175, 60 - 22, 0 },
    { "pair/mobile",      STR_I_MOBILE,       175, 60 - 22, 0 },
    { "common/back",      STR_C_BACK,         140, 44, 0 },
    { "common/done",      STR_C_DONE,         140, 52, 0 },
    { "common/ok",        STR_C_OK,           200, 52, 0 },
    { "common/cancel",    STR_C_CANCEL,       140, 52, 0 },
};
#define NPILL ((int)(sizeof PILLS / sizeof PILLS[0]))

// sign/why is built at runtime from up to three reasons plus the footer; the
// worst case (all three flagged) is what has to fit.
static void compose_why(char *out, size_t cap)
{
    snprintf(out, cap, "%s\n%s\n%s\n\n%s",
             tr(STR_S_WHY_HIGHFEE), tr(STR_S_WHY_DUSTIN),
             tr(STR_S_WHY_TINYCH), tr(STR_S_WHY_FOOT));
}

int main(int argc, char **argv)
{
    lv_init();
    static uint8_t buf[800 * 40 * 2];
    lv_display_t *d = lv_display_create(800, 480);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, buf, NULL, sizeof buf, LV_DISPLAY_RENDER_MODE_PARTIAL);

    int total_small = 0;
    for (int l = 0; l < I18N_LANG_N; l++) {
        const i18n_lang_t *li = i18n_lang_info(l);
        if (argc > 1) {                       // only the locales asked for
            int want = 0;
            for (int a = 1; a < argc; a++)
                if (strcmp(argv[a], li->code) == 0) want = 1;
            if (!want) continue;
        }
        i18n_set_lang(l);

        int small = 0;
        char lines[NSLOT][160];
        for (int i = 0; i < NSLOT; i++) {
            char composed[1024];
            const char *txt;
            if (SLOTS[i].key < 0) { compose_why(composed, sizeof composed); txt = composed; }
            else                  { txt = tr(SLOTS[i].key); }

            const lv_font_t *f = wt_body_font(txt, SLOTS[i].w, SLOTS[i].h);
            // Only font14 counts as a failure now. 23 is a real reading size,
            // and the notes wedged between controls can never reach 28.
            int rung = f == wt_font28() ? 28 : f == wt_font23() ? 23 : 14;
            int bad  = rung == 14 && !SLOTS[i].may_be_small;
            if (bad) small++;
            // how far the copy overflows at 23 is what a translator must delete
            lv_point_t sz;
            lv_text_get_size(&sz, txt, wt_font23(), 0, 0, SLOTS[i].w, LV_TEXT_FLAG_NONE);
            int over = sz.y - SLOTS[i].h;
            snprintf(lines[i], sizeof lines[i], "  %-18s font%-2d  %3dpx / %3dpx%s",
                     SLOTS[i].surface, rung, (int)sz.y, SLOTS[i].h,
                     bad && over > 0 ? "  cut " : "");
            if (bad && over > 0) {
                char n[24];
                snprintf(n, sizeof n, "%d row(s) @23", (over + 28) / 29);
                strncat(lines[i], n, sizeof lines[i] - strlen(lines[i]) - 1);
            }
        }
        int pbad = 0;
        char plines[NPILL][160];
        for (int i = 0; i < NPILL; i++) {
            const char *txt = tr(PILLS[i].key);
            const lv_font_t *f = wt_pill_font(txt, PILLS[i].w, PILLS[i].h,
                                              PILLS[i].primary);
            int rung = f == wt_font28() ? 28 : f == wt_font23() ? 23 : 14;
            lv_point_t sz;
            lv_text_get_size(&sz, txt, wt_font23(), 1, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
            if (rung == 14) pbad++;
            snprintf(plines[i], sizeof plines[i],
                     "  pill %-15s font%-2d  %3dpx / %3dpx%s", PILLS[i].surface,
                     rung, (int)sz.x, PILLS[i].w - 28,
                     rung == 14 ? "  WIDEN" : "");
        }
        printf("%-6s %-22s %2d/%d at font14, %d/%d pills\n", li->code, li->native,
               small, NSLOT, pbad, NPILL);
        for (int i = 0; i < NSLOT; i++)
            if (strstr(lines[i], "cut ")) puts(lines[i]);
        for (int i = 0; i < NPILL; i++)
            if (strstr(plines[i], "WIDEN")) puts(plines[i]);
        total_small += small + pbad;
    }
    printf("\ntotal at font14: %d\n", total_small);
    return 0;
}
