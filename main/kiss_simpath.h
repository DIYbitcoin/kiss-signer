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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

// ---- one walk per scratch -------------------------------------------------
// Everything above hands a run its own directory when KISS_SIM_TMP is set. It
// does not stop two runs from sharing one, and sharing one is not a slow race
// -- it is a fabricated failure report. A walk deletes and rewrites the fake
// card as it goes, so a second walk on the same root finds files missing that
// it just wrote, taps rows that have moved, and derails; the stops after that
// photograph whatever is on screen. That is the failure kiss_simpath.h itself
// was written for, and it came back the day this was added: a foreground walk
// run beside a background locale sweep printed twenty findings that were not
// in the product, on a tree whose own gates were green a minute earlier.
//
// The docs say to set KISS_SIM_TMP and nobody remembers at the moment it
// matters, so it is a LOCK rather than a paragraph. Refuse, name the holder,
// and print the one line that fixes it.
//
// A stale lock cannot wedge anyone: a killed run leaves its file behind, so
// the pid is checked and a dead one is taken over. Host only, and the process
// is single threaded, which is what makes open(O_EXCL) enough.
static inline void kiss_sim_unlock(void)
{
    char p[224];
    kiss_sim_path(p, sizeof p, "walk.lock");
    unlink(p);
}

static inline void kiss_sim_lock(const char *who)
{
    char p[224];
    kiss_sim_path(p, sizeof p, "walk.lock");
    for (int attempt = 0; attempt < 2; attempt++) {
        int fd = open(p, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd >= 0) {
            char line[128];
            int n = snprintf(line, sizeof line, "%ld %s\n", (long)getpid(),
                             who ? who : "?");
            if (n > 0) { ssize_t w = write(fd, line, (size_t)n); (void)w; }
            close(fd);
            atexit(kiss_sim_unlock);
            return;
        }
        if (errno != EEXIST) return;      /* unwritable scratch: not our fight */

        long pid = 0;
        char held[128] = {0};
        FILE *f = fopen(p, "r");
        if (f) {
            if (fscanf(f, "%ld %63s", &pid, held) < 1) pid = 0;
            fclose(f);
        }
        if (pid > 0 && kill((pid_t)pid, 0) == 0) {
            fprintf(stderr,
                    "\nREFUSED: %s is already walking this scratch (pid %ld, %s)\n"
                    "  scratch: %s\n"
                    "  Two walks on one scratch rewrite each other's fake card,\n"
                    "  and the loser reports findings about somebody else's run.\n"
                    "  Give this one its own:\n\n"
                    "    export KISS_SIM_TMP=/tmp/kiss-$$\n\n",
                    held[0] ? held : "another run", pid, who ? who : "?",
                    kiss_sim_root());
            exit(1);
        }
        unlink(p);                        /* stale: the holder is gone */
    }
}

// ---- a run does not inherit the last run's pictures ----------------------
// The lock above hands concurrent runs separate scratches, and its own advice
// is "export KISS_SIM_TMP=/tmp/kiss-$$" -- a fresh directory every time. That
// is right for two runs at once and wrong for the far commoner case, one run
// repeated after a fix, because nothing ever emptied the directory it left.
//
// A walk writes ~600 frames at 800x480 in binary P6, which is 1.15MB each and
// no compression: 660MB per walk. Nine walks in one afternoon is 5.9GB, and
// the measured cost of not having this was 41GB of near identical pictures
// across a fortnight of sessions.
//
// So a scratch is reusable instead of disposable: the frames from last time
// go, and nothing else does. Fixtures, the fake card, the seed files and the
// logs all survive -- sweeping those is the reset that drops the device to
// first boot and derails the walk on a random locale, which is a separate
// scar and not one to reopen. Prefix and extension both have to match, so a
// hand saved picture keeps its place as long as it is not called sim_*.ppm.
//
// KISS_SIM_KEEP=1 turns it off for the rare comparison against the previous
// run's frames. Nothing in the gates sets it.
static inline int kiss_sim_sweep_frames(void)
{
    const char *keep = getenv("KISS_SIM_KEEP");
    if (keep && *keep && strcmp(keep, "0") != 0) return 0;

    const char *root = kiss_sim_root();
    DIR *d = opendir(root);
    if (!d) return 0;                     /* no scratch yet: nothing to sweep */

    int swept = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n < 9) continue;                                  /* sim_x.ppm */
        if (strncmp(e->d_name, "sim_", 4) != 0) continue;
        if (strcmp(e->d_name + n - 4, ".ppm") != 0) continue;

        char p[224];
        if (snprintf(p, sizeof p, "%s/%s", root, e->d_name) >= (int)sizeof p)
            continue;
        if (remove(p) == 0) swept++;
    }
    closedir(d);
    return swept;
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
