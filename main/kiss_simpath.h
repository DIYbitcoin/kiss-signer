// Where a desktop build puts the things it is pretending are hardware: the fake
// SD card, the files standing in for NVS, and every captured frame.
//
// All of it was under /tmp, flat, with the same names every time -- and /tmp is
// shared. Two builds on one machine wrote each other's fixtures, and a run
// whose fake card was wiped mid-walk came up short in a file list, tapped rows
// that had moved, and derailed. The walk then printed "clean" for every stop it
// never reached, so the run reported a clean sweep AND a non-zero exit. That is
// worse than a red run: it is a green one that measured nothing.
//
// Reproduced rather than guessed: deleting the card on a 100ms loop underneath
// a walk gives exactly the failure signature this was showing.
//
// KISS_SIM_TMP is the root everything hangs off. Unset it stays /tmp, so a
// single person on a single checkout sees no change at all and every path in
// the docs still resolves. Set it, and a run owns its own scratch -- which is
// what the gate scripts do, and what anyone running two of anything at once
// wants.
//
// Host only. Nothing here compiles into firmware; the device has a real card
// and real NVS, and neither of them lives in a directory.
#pragma once

#ifndef ESP_PLATFORM

#include <stdio.h>
#include <stdlib.h>

static inline const char *kiss_sim_root(void)
{
    const char *r = getenv("KISS_SIM_TMP");
    return (r && *r) ? r : "/tmp";
}

static inline const char *kiss_sim_path(char *buf, size_t n, const char *name)
{
    snprintf(buf, n, "%s/%s", kiss_sim_root(), name);
    return buf;
}

// A small ring of buffers behind the accessors below, so a call site that was
// `rename(SEED_TMP, SEED_FILE)` against two literals still reads that way
// against two calls. Four is two more than any expression here needs. The host
// build is single threaded and every use is immediate, which is the whole
// reason a ring is honest rather than a trap.
#define KISS_SIM_PATH_FN(fn, name)                          \
    static const char *fn(void)                             \
    {                                                       \
        static char buf[4][192];                            \
        static int slot;                                    \
        slot = (slot + 1) & 3;                              \
        return kiss_sim_path(buf[slot], sizeof buf[slot], (name)); \
    }

#endif  // !ESP_PLATFORM
