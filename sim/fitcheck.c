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
    { "wipe/erased",      STR_G_ERASED_B,   704, 160 },
    { "wipe/not-erased",  STR_G_NOERASE_B,  704, 160 },
    // amnesic mode: seed-QR import + passphrase-from-QR
    { "setup/qr-bad",     STR_W_QRBAD_B,    704, 240 },
    { "login/qr-warn",    STR_L_SCAN_WARN_B,704, 274 },
};
#define NSLOT ((int)(sizeof SLOTS / sizeof SLOTS[0]))

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
            lv_point_t sz;
            lv_text_get_size(&sz, txt, wt_font28(), 0, 0, SLOTS[i].w, LV_TEXT_FLAG_NONE);
            int big = (f == wt_font28());
            if (!big) small++;
            // rows over budget is what a translator actually has to delete
            int over = sz.y - SLOTS[i].h;
            snprintf(lines[i], sizeof lines[i], "  %-18s %s  %3dpx / %3dpx%s",
                     SLOTS[i].surface, big ? "BIG " : "small",
                     (int)sz.y, SLOTS[i].h,
                     big ? "" : (over > 0 ? "  cut " : ""));
            if (!big && over > 0) {
                char n[24];
                snprintf(n, sizeof n, "%d row(s)", (over + 36) / 37);
                strncat(lines[i], n, sizeof lines[i] - strlen(lines[i]) - 1);
            }
        }
        printf("%-6s %-22s %2d/%d small\n", li->code, li->native, small, NSLOT);
        for (int i = 0; i < NSLOT; i++)
            if (strstr(lines[i], "small")) puts(lines[i]);
        total_small += small;
    }
    printf("\ntotal small: %d\n", total_small);
    return 0;
}
