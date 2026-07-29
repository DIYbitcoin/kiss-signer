// Theme safety gate (ADDENDUM-02).
//
// Four themed accents, three status colours that are never themed. The status
// colours carry meaning -- WT_OK is verified, WT_WARN is caution, WT_STOP is
// refused -- and an accent that lands on top of one of them does not break
// that meaning, it breaks the HIERARCHY: accent chrome starts reading as
// status, and status stops standing out from ordinary decoration.
//
// This does not try to ban collisions. GREEN's accent is 0x35D07F and WT_OK is
// 0x35D07F, byte identical, and that is not an accident: a green theme is
// supposed to be green. What it bans is an UNDECLARED collision. Every pair
// that renders close enough to be confused has to be listed below with the
// reason it is safe, and the list has to stay true in both directions:
//
//   * a colliding pair that is NOT declared fails, so a fifth accent, or a
//     nudge to an existing one, cannot land on a status colour unnoticed;
//   * a declared pair that no longer collides ALSO fails, so the exemptions
//     cannot rot into a list of things that used to be true.
//
// Distance is CIE76 dE in Lab, not RGB: two colours can be far apart in RGB
// and still read the same to an eye at arm's length.
//
// Running it settled one of the addendum's claims against it. ADDENDUM-02
// names three collisions: GREEN/WT_OK, CYPHERPINK/WT_STOP and ORANGE/WT_WARN.
// Measured, only the first is real. CYPHERPINK had already been moved to
// 0xC45CE8 and now sits 83.8 from WT_STOP, and ORANGE measures 30.1 from
// WT_WARN, which is over an order of magnitude past the ~2.3 where a
// difference stops being visible at all. Only GREEN/WT_OK is a collision, and
// it is a total one: dE 0.0.
//
// This gate checks the PALETTE. Whether a rendered element wears an accent and
// a status colour at the same time is rule 1 of the same addendum, and that
// question needs a live widget tree, so it is asked by sim/overlapcheck.c.
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "wallet_theme.h"
#include "colour_de.h"

typedef struct { const char *name; uint32_t hex; } named_t;

static const named_t STATUS[] = {
    { "WT_OK",   0x35D07F },
    { "WT_WARN", 0xF2B84B },
    { "WT_STOP", 0xFF4D5E },
};
#define NSTATUS ((int)(sizeof STATUS / sizeof STATUS[0]))

// The declared collisions, and why each one is safe. "Safe" always means the
// same thing: on every screen where that status colour appears, the accent is
// not also in play, so the two never have to be told apart.
static const struct {
    const char *accent, *status, *why;
} DECLARED[] = {
    { "GREEN", "WT_OK",
      "byte identical by design. Every screen carrying WT_OK gives the accent's "
      "job to WT_INK instead (wallet_info.c sp_permissions), so the two never "
      "share a screen region." },
};
#define NDECL ((int)(sizeof DECLARED / sizeof DECLARED[0]))

static int declared_at(const char *accent, const char *status)
{
    for (int i = 0; i < NDECL; i++)
        if (!strcmp(DECLARED[i].accent, accent) &&
            !strcmp(DECLARED[i].status, status))
            return i;
    return -1;
}

int main(void)
{
    lv_init();
    int fail = 0, hit[NDECL];
    memset(hit, 0, sizeof hit);

    printf("accent vs status, CIE76 dE (same below %.0f)\n\n", DE_SAME);
    printf("%-12s", "");
    for (int s = 0; s < NSTATUS; s++) printf("%10s", STATUS[s].name);
    printf("\n");

    for (int a = 0; a < WT_ACC_N; a++) {
        wt_accent_set(a);
        const char *an = wt_accent_name();
        lv_color_t ac = wt_accent();
        uint32_t ahex = ((uint32_t)ac.red << 16) | ((uint32_t)ac.green << 8) | ac.blue;

        printf("%-12s", an);
        for (int s = 0; s < NSTATUS; s++)
            printf("%10.1f", cde_delta_e(ahex, STATUS[s].hex));
        printf("   #%06X\n", (unsigned)ahex);

        for (int s = 0; s < NSTATUS; s++) {
            double de = cde_delta_e(ahex, STATUS[s].hex);
            int d = declared_at(an, STATUS[s].name);
            if (de < DE_SAME && d < 0) {
                printf("\nFAIL: %s accent (#%06X) collides with %s (#%06X), dE %.1f.\n"
                       "      Either move the accent or declare the pair in "
                       "sim/themecheck.c with the reason it is safe.\n",
                       an, (unsigned)ahex, STATUS[s].name,
                       (unsigned)STATUS[s].hex, de);
                fail++;
            }
            if (d >= 0) {
                hit[d] = 1;
                if (de >= DE_SAME) {
                    printf("\nFAIL: %s / %s is declared as a collision but "
                           "measures dE %.1f.\n"
                           "      The colours moved apart. Drop the entry.\n",
                           an, STATUS[s].name, de);
                    fail++;
                }
            }
        }
    }
    wt_accent_set(WT_ACC_MONO);

    for (int i = 0; i < NDECL; i++)
        if (!hit[i]) {
            printf("\nFAIL: declared pair %s / %s matches no accent or status "
                   "that exists.\n", DECLARED[i].accent, DECLARED[i].status);
            fail++;
        }

    printf("\ndeclared collisions:\n");
    for (int i = 0; i < NDECL; i++)
        printf("  %s / %s\n    %s\n", DECLARED[i].accent, DECLARED[i].status,
               DECLARED[i].why);

    if (fail) { printf("\ntheme gate: %d failure(s)\n", fail); return 1; }
    printf("\ntheme gate: %d accents x %d status colours, %d declared, 0 undeclared\n",
           WT_ACC_N, NSTATUS, NDECL);
    return 0;
}
