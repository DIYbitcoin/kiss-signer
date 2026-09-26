// The in-place definition's arithmetic (main/kiss_defrow.h). Pure integers,
// proven here so a screen never has to: an open row plus its ghosts must give
// the lane back exactly, and the closed pitch may leave only the floor's
// remainder at the bottom edge -- slack, never a fractional row.
#include <stdio.h>

#include "kiss_defrow.h"

static int dfails;

static void dchk(const char *name, int ok)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) dfails++;
}

int test_defrow(void)
{
    // The spec's own worked examples, verbatim, for the wide lane; the
    // 3.5in's lane is 178 with a 22 ghost (kiss_defrow.h says why it is not
    // two thirds of 284) and its numbers are worked the same way.
#if KISS_NARROW
    dchk("n=4 closed is 44", wt_def_h_closed(4) == 44);
    dchk("n=4 open is 112", wt_def_h_open(4) == 112);
    dchk("n=3 closed is 59", wt_def_h_closed(3) == 59);
    dchk("n=3 open is 134", wt_def_h_open(3) == 134);
    dchk("a ghost is 22", wt_def_h_ghost() == 22);
#elif defined(KISS_BOARD_JC1060)
    // The 7in's lane is SY(284) = 355 with an SY(34) = 42 ghost, worked the
    // same way.
    dchk("n=4 closed is 88", wt_def_h_closed(4) == 88);
    dchk("n=4 open is 229", wt_def_h_open(4) == 229);
    dchk("n=3 closed is 118", wt_def_h_closed(3) == 118);
    dchk("n=3 open is 271", wt_def_h_open(3) == 271);
    dchk("a ghost is 42", wt_def_h_ghost() == 42);
#else
    dchk("n=4 closed is 71", wt_def_h_closed(4) == 71);
    dchk("n=4 open is 182", wt_def_h_open(4) == 182);
    dchk("n=3 closed is 94", wt_def_h_closed(3) == 94);
    dchk("n=3 open is 216", wt_def_h_open(3) == 216);
    dchk("a ghost is 34", wt_def_h_ghost() == 34);
#endif

    for (int n = 2; n <= 5; n++) {
        char name[64];
        // The open row takes exactly what the ghosts leave.
        snprintf(name, sizeof name, "n=%d open+ghosts fills the lane", n);
        dchk(name, wt_def_h_open(n) + WT_DEF_GHOST * (n - 1) == WT_DEF_LANE);
        // Closed rows floor; the remainder is bottom slack, under one row's
        // worth of pixels and under n.
        int slack = WT_DEF_LANE - wt_def_h_closed(n) * n;
        snprintf(name, sizeof name, "n=%d closed slack %d is under n", n, slack);
        dchk(name, slack >= 0 && slack < n);
        // A ghost never outgrows a closed row, or opening a row would make a
        // sibling TALLER.
        snprintf(name, sizeof name, "n=%d ghost fits under closed", n);
        dchk(name, wt_def_h_ghost() <= wt_def_h_closed(n));
        // An open row always gains on its closed self, or the tap did nothing.
        snprintf(name, sizeof name, "n=%d open grows", n);
        dchk(name, wt_def_h_open(n) > wt_def_h_closed(n));
    }
    return dfails;
}
