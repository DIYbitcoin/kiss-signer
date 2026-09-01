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
KISS_SIM_TMP="${KISS_SIM_TMP:-/tmp}"
mkdir -p "$KISS_SIM_TMP"

# Only roots untouched for this many days go. A root in use is a root being
# written to, so mtime is the whole test; the current one is skipped by name as
# well, because a build that reaped its own output would be a very confusing
# bug. KISS_SIM_KEEP=1 turns it off.
_kiss_reap_sim_tmp() {
    [ -z "${KISS_SIM_KEEP:-}" ] || return 0

    local days scan self n
    days="${KISS_SIM_TMP_DAYS:-3}"

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

        [ -z "$(find "$d" -maxdepth 0 -mtime "+$days" 2>/dev/null)" ] && continue

        rm -rf "$d" && n=$((n + 1))
    done

    [ "$n" -gt 0 ] && echo "sim: reaped $n scratch root(s) idle over ${days}d under $scan"
    return 0
}
_kiss_reap_sim_tmp || true
