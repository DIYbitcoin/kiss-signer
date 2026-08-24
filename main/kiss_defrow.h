// The in-place definition's arithmetic, in one place. Six shapes and a dozen
// screens divide the same 284px lane, and they must agree to the pixel: a row
// opens by taking every pixel the ghosts give up, so any second copy of this
// sum is a future off-by-one between two screens that look identical today.
//
// From design_handoff_system/README.md Part 3:
//
//   closed:  LANE / n              every row equal
//   open:    LANE - GHOST*(n-1)    the open one
//   ghost:   GHOST                 every other one
//
// LANE/n floors, so up to n-1 px of slack sits at the LANE's bottom edge when
// all rows are closed (n=3 leaves 2). The slack is never distributed: rows on
// an uneven pitch stop reading as a list, and the open sum is exact anyway.
//
// Pure integers, no LVGL, so kisstest can prove the identities on desktop.
#pragma once

#define WT_DEF_LANE  284   // the chrome contract's content lane height
#define WT_DEF_GHOST 34    // a collapsed row

static inline int wt_def_h_closed(int n) { return WT_DEF_LANE / n; }
static inline int wt_def_h_open(int n)
{
    return WT_DEF_LANE - WT_DEF_GHOST * (n - 1);
}
static inline int wt_def_h_ghost(void) { return WT_DEF_GHOST; }
