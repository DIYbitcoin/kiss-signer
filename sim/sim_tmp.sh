# Sourced by every sim build script. Sets the scratch root and reaps stale ones.
#
# KISS_SIM_TMP is the root the binaries, the fake card, the seed files and the
# captured frames all hang off (main/kiss_simpath.h). Unset it is /tmp, exactly
# as before, so every command in the house rules is unchanged.
#
# The reap is here because nothing owned the other half of the contract. The
# rules say to set KISS_SIM_TMP when two things run at once, people set it to a
# label they can recognise -- kiss-p4, kiss-qr, kiss-plan1 -- and then the
# session ends. 83 roots reached 28G that way, which is a seventh of the disk,
# and every byte of it rebuilds in seconds.
# KISS_WERROR -- every sim/build_*.sh turns its warnings into errors when this
# is set, and none of them does when it is not. Every one of them is clean
# today, so this costs nothing and holds that.
#
# Opt in rather than always on, because the same clang invocation compiles 463
# vendored LVGL sources and the amalgamated libwally. An Apple clang update
# adding one warning to somebody else's code must never be able to block the
# daily build here -- only to redden the lane where a person is reading a
# verdict. tools/preflight.sh and the desktop CI lane set it; a plain
# `bash sim/build_sim.sh` does not.
#
#   KISS_WERROR=1 bash sim/build_test.sh
#
KISS_SIM_TMP="${KISS_SIM_TMP:-/tmp}"
mkdir -p "$KISS_SIM_TMP"

# Is a scratch root still owned by a running walk? kiss_simpath.h writes
# "<pid> <who>" into walk.lock and unlinks it at exit, so a file naming a live
# pid is the only evidence of use that does not depend on mtime -- and mtime is
# exactly what a long quiet walk does not update. No file, an unreadable one or
# a dead pid all mean finished.
_kiss_root_is_live() {
    local lock pid
    lock="$1/walk.lock"
    [ -f "$lock" ] || return 1
    pid=$(awk '{print $1; exit}' "$lock" 2>/dev/null) || return 1
    case "$pid" in ''|*[!0-9]*) return 1 ;; esac
    kill -0 "$pid" 2>/dev/null
}

# Finished roots go; roots with a walk still in them never do, at any age.
#
# mtime used to be the whole test, at 3 days. It is the wrong test twice over.
# A walk that is running but quiet does not touch its root, so age alone can
# condemn a root in use -- and the current one is skipped by name, which covers
# this build and nothing running beside it. In the other direction 3 days is
# far too patient for a root nobody holds: nine finished roots and 5.9G piled
# up inside one afternoon, none of them close to 3 days old, all of them dead
# within minutes of being written.
#
# So liveness decides whether to look at age at all, and walk.lock is what
# liveness reads. KISS_SIM_TMP_MINS is the age for a finished root, default 4
# hours: long enough to come back to this morning's frames, short enough that
# repeating a walk after each fix stops paying for every earlier try.
# KISS_SIM_TMP_DAYS still works and still means days. KISS_SIM_KEEP=1 turns
# the whole thing off.
_kiss_reap_sim_tmp() {
    [ -z "${KISS_SIM_KEEP:-}" ] || return 0

    local mins scan self n
    # One age, expressed in minutes. The days knob predates it and is kept
    # working rather than deprecated in a file nobody rereads.
    if [ -n "${KISS_SIM_TMP_DAYS:-}" ]; then
        mins=$(( KISS_SIM_TMP_DAYS * 1440 ))
    else
        mins="${KISS_SIM_TMP_MINS:-240}"
    fi

    # A root under a shared parent has its siblings beside it; the bare
    # default (/tmp) holds them itself.
    case "$KISS_SIM_TMP" in
        /tmp|/private/tmp) scan="$KISS_SIM_TMP" ;;
        *)                 scan="$(dirname "$KISS_SIM_TMP")" ;;
    esac
    self="$(cd "$KISS_SIM_TMP" 2>/dev/null && pwd -P)" || return 0

    n=0
    # Every directory, not a name glob. The names people pick are not a
    # pattern -- kiss-p4, kissfin, kw3-en and kw2-pl were all scratch roots,
    # and a glob written for the first two walked straight past the others.
    # What a root always has is something a sim build put in it, so that is
    # the test, and it is the safety check as well: a directory holding none
    # of these is somebody else's and stays.
    for d in "$scan"/*; do
        [ -d "$d" ] || continue
        [ "$(cd "$d" && pwd -P)" != "$self" ] || continue

        # Never delete a directory that is not ours.
        local mine=""
        for f in fruitsim kisstest kissfit kisstheme kissosd kissoverlap simsd; do
            [ -e "$d/$f" ] && { mine=1; break; }
        done
        [ -n "$mine" ] || continue

        # Somebody is walking in there. Age is not the question.
        ! _kiss_root_is_live "$d" || continue

        [ -n "$(find "$d" -maxdepth 0 -mmin "+$mins" 2>/dev/null)" ] || continue

        rm -rf "$d" && n=$((n + 1))
    done

    [ "$n" -gt 0 ] && echo "sim: reaped $n finished scratch root(s) idle over ${mins}m under $scan"
    return 0
}
_kiss_reap_sim_tmp || true

# What tree this build came from, printed once per build.
#
# Two runs of check_screen_coverage.py 29 seconds apart gave exit 0 with no
# failures and exit 1 with twelve, and the twelve were read as a flaky gate and
# reported to the owner as one. They were not. Somebody else was editing the
# same checkout: a settings tab had gained a third row and the walk's def_go(n,
# i) call sites, which take the ROW COUNT, had not been bumped yet. The tap did
# not miss -- def_go(2, 1) computes a y inside what is now the third row, so it
# hit a real control and opened a real page, and every save() after it
# photographed the wrong screen until a needle failed somewhere unrelated.
#
# KISS_SIM_TMP above stops two runs sharing a fake card. Nothing stopped two
# people sharing the SOURCE, and a gate failing out of a half-saved tree looks
# exactly like a regression in the reader's own diff. One line cannot prevent
# that; it can stop the failure being attributed to the wrong tree, which is
# where the whole cost was.
#
# main/, sim/ and i18n/ only. docs/ is regenerated constantly and is not
# compiled into anything here, so counting it would print "dirty" on every run
# and mean nothing by the second day.
_kiss_tree_stamp() {
    command -v git >/dev/null 2>&1 || return 0
    git rev-parse --git-dir >/dev/null 2>&1 || return 0
    local head n
    head=$(git rev-parse --short HEAD 2>/dev/null) || return 0
    n=$(git status --porcelain -- main sim i18n 2>/dev/null | grep -c . || true)
    if [ "${n:-0}" -gt 0 ]; then
        echo "sim: built from $head with $n uncommitted file(s) under main/ sim/ i18n/"
        echo "sim: a failure below may belong to that edit and not to your own"
    else
        echo "sim: built from $head, main/ sim/ i18n/ clean"
    fi
}
_kiss_tree_stamp || true
