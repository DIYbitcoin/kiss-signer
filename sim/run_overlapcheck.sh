#!/bin/bash
# Run the text overlap gate over every locale (see sim/overlapcheck.c).
#
# One binary, one run per language, because the fault this looks for only
# appears in the languages nobody reads during development: content is placed at
# absolute y while the body font degrades 28 to 23 to 14 on how long the
# translation is, so English is the one locale guaranteed to look fine.
#
# Exit code is 0 while OVERLAPCHECK_STRICT is unset. That is how this landed in
# phase 0, reporting honestly against layout nobody had fixed yet. Phase 1 fixed
# it, desktop-tests.yml sets the variable, and a new overlap now fails the build.
# Running this by hand still only reports, which is what you want while working.
set -u
cd "$(dirname "$0")/.."

# ---- this run's own scratch ------------------------------------------------
#
# Everything a desktop build pretends is hardware -- the fake card, the seed
# files, the frames -- and the binary itself now hang off KISS_SIM_TMP
# (main/kiss_simpath.h). Unset it is /tmp, which is what a hand run wants.
#
# A gate run wants the opposite: its own directory, so two of these at once
# cannot delete each other's fixtures mid-walk. That collision is what a lock
# here used to work around, and a lock made everyone queue for no reason. A run
# whose fixtures vanish comes up short in a file list, taps rows that moved, and
# derails -- and then prints "clean" for every stop it never reached, so it
# reports a clean sweep AND a non-zero exit. The note at the bottom of this
# script has been blaming an interleaved run all along and was right.
#
# Honour an inherited value: CI may want the frames somewhere it can collect.
if [ -z "${KISS_SIM_TMP:-}" ]; then
    KISS_SIM_TMP=$(mktemp -d "/tmp/kiss-overlap-XXXXXX")
    trap 'rm -rf "$KISS_SIM_TMP"' EXIT INT TERM
fi
export KISS_SIM_TMP
mkdir -p "$KISS_SIM_TMP"

# A stale binary is worse than none: it passes its own self test, then the 24
# walks "verify" whatever was built last. Fail the run when the build fails.
bash sim/build_overlapcheck.sh || {
    echo "FAILED: sim/build_overlapcheck.sh" >&2
    exit 1
}

# WALL, CUT, LAYER and ROLE all report nothing on the current UI, so prove they
# can still
# report anything at all before trusting a clean run. See oc_selftest in
# sim/overlapcheck.c for why these two need that and the other five do not.
# The exit status is not enough on its own: the checks print a marker when
# they behave, so a run that exits 0 without them (or vice versa) also fails.
echo
st=$(OVERLAPCHECK_SELFTEST=1 "$KISS_SIM_TMP/kissoverlap" 2>&1)
if [ $? -ne 0 ] ||
    ! printf '%s\n' "$st" | grep -q 'CUT self test: 4 cases, all as expected' ||
    ! printf '%s\n' "$st" | grep -q 'INK self test: 2 cases, all as expected' ||
    ! printf '%s\n' "$st" | grep -q 'EXIT self test: 2 cases, all as expected' ||
    ! printf '%s\n' "$st" | grep -q 'VOID self test: 2 cases, all as expected' ||
    ! printf '%s\n' "$st" | grep -q 'FIT self test: 2 cases, all as expected' ||
    ! printf '%s\n' "$st" | grep -q 'TINY self test: 3 cases, all as expected' ||
    ! printf '%s\n' "$st" | grep -q 'RAGGED self test: 2 cases, all as expected' ||
    ! printf '%s\n' "$st" | grep -q 'AMBER self test: 4 cases, all as expected' ||
    ! printf '%s\n' "$st" | grep -q 'WALL self test: 2 cases, all as expected' ||
    ! printf '%s\n' "$st" | grep -q 'STALE self test: 2 cases, all as expected' ||
    ! printf '%s\n' "$st" | grep -q 'LAYER self test: 3 cases, all as expected' ||
    ! printf '%s\n' "$st" | grep -q 'ROLE self test: 4 cases, all as expected' ||
    ! printf '%s\n' "$st" | grep -q 'LADDER self test: 2 cases, all as expected' ||
    ! printf '%s\n' "$st" | grep -q 'READ self test: 3 cases, all as expected' ||
    ! printf '%s\n' "$st" | grep -q 'MARK self test: 6 cases, all as expected' ||
    ! printf '%s\n' "$st" | grep -q 'WIDOW self test: 2 cases, all as expected' ||
    ! printf '%s\n' "$st" | grep -q 'CLIPX self test: 2 cases, all as expected' ||
    ! printf '%s\n' "$st" | grep -q 'DOTS self test: 2 cases, all as expected' ||
    ! printf '%s\n' "$st" | grep -q 'TERM self test: 1 case, all as expected'; then
    echo
    echo "FAILED: the self test no longer reports its expected markers, so a"
    echo "clean run means nothing."
    exit 1
fi

# The locale codes are the names of the translation files, so a language added
# to i18n/ is covered here the same day without this script being edited.
#
# OVERLAPCHECK_LANGS overrides the list, space separated. While the UI is being
# rebuilt the house rule is ENGLISH ONLY -- the other twenty carry wording that
# is about to be replaced, so sweeping them proves nothing about the product and
# takes twenty times as long to say it:
#
#   OVERLAPCHECK_LANGS=en bash sim/run_overlapcheck.sh
#
# The full sweep is the translation pass's gate, not the daily one. It does
# catch real faults -- a walk needle that passed in English because two keys
# share a string there and differ in French was caught exactly that way -- and
# that class of bug waits until the wording settles.
langs=()
if [ -n "${OVERLAPCHECK_LANGS:-}" ]; then
    for b in $OVERLAPCHECK_LANGS; do
        [ -f "i18n/$b.json" ] || { echo "no such locale: $b" >&2; exit 1; }
        langs+=("$b")
    done
else
    for f in i18n/*.json; do
        b=$(basename "$f" .json)
        langs+=("$b")
    done
fi

echo
echo "text overlap gate: ${#langs[@]} locales"
echo

worst=0
died=""
# The per-locale ceiling file, and the list of locales that broke it. Unset
# OVERLAPCHECK_CEILINGS to run without one (which is what a bisect wants).
ceilfile="${OVERLAPCHECK_CEILINGS:-sim/overlap_ceilings.txt}"
over=""
unceiled=""
total=0
summary=""
# The LVGL heap watermark, out of runs this script already makes. Every one
# of these 24 walks prints an [lvheap] line at its end and every one of them
# was thrown away; the number was in the log of a gate nobody parsed, which is
# one level down from the reporting-only gate this file already complains
# about. max_used only: frag_pct is sampled at one instant, so ratcheting it
# would be ratcheting noise.
#
# The line is now REQUIRED, not optional: a missing or malformed line used to
# leave the heap verdict silently skipped, which read as "under the ceiling"
# while measuring nothing. total_size is not quite the pool: the TLSF walker
# sums block sizes, and splitting and merging free blocks moves the sum by a
# block header or two between runs (observed +/-24 on the 128K pool). The pool
# itself is a compile-time constant, so the checks are a tolerance against the
# first run's total and a floor: a run that measured a different pool (a 64K
# build, or a dead monitor) falls outside both.
heap_max=0; heap_total=""; heap_who=""; heap_peak_total=0; heap_frag=0
heap_note() {
    local who="$1" out="$2" line tot used
    line=$(printf '%s\n' "$out" | grep -m1 '^\[lvheap\]') || return 1
    [ -n "$line" ] || return 1
    # [lvheap] total %u used %u max_used %u frag %u%%: $3 is the pool, $7 the
    # peak. Words sit between them, so a "$2/$4" pickup reads words and
    # disables the ceiling silently.
    tot=$(printf '%s\n' "$line" | awk '{print $3}')
    used=$(printf '%s\n' "$line" | awk '{print $7}')
    # $9 is "57%" -- kept for the failure message only, never ratcheted.
    local fr; fr=$(printf '%s\n' "$line" | awk '{print $9}' | tr -d '%')
    case "$fr" in ''|*[!0-9]*) fr=0;; esac
    case "$used" in ''|*[!0-9]*) return 1;; esac
    case "$tot" in ''|*[!0-9]*) return 1;; esac
    if [ -n "$heap_total" ]; then
        local d=$(( tot > heap_total ? tot - heap_total : heap_total - tot ))
        if [ "$d" -gt 256 ]; then
            echo "heap pool differs in $who: $tot vs $heap_total" >&2
            return 1
        fi
    fi
    if [ "$tot" -lt 98304 ] || [ "$tot" -gt 131072 ]; then
        # A real 128K pool reads ~120K. The lower edge rejects a 64K/dead
        # monitor; the upper edge rejects a 256K sim that would make the same
        # device workload look artificially cheap.
        echo "heap pool out of band in $who: $tot" >&2
        return 1
    fi
    heap_total=$tot
    # The peak and ITS sample's own pool reading go together. total_size is
    # not quite the pool (the TLSF walker sums block sizes, and merging free
    # blocks moves it by a header or two between runs), so a percentage
    # computed against the LAST run's total would be off by that wobble; the
    # denominator that belonged to the peak is the honest one.
    if [ "$used" -le 0 ]; then
        echo "heap peak is not credible in $who: $used" >&2
        return 1
    fi
    # Keep the sample with the WORST ratio, not merely the largest absolute
    # byte count: TLSF's reported denominator wobbles by a header or two.
    if [ "$heap_peak_total" -eq 0 ] ||
       [ $(( used * heap_peak_total )) -gt $(( heap_max * tot )) ]; then
        heap_max=$used; heap_who=$who; heap_peak_total=$tot; heap_frag=$fr
    fi
    return 0
}

# A run proves nothing unless it rendered and measured. A dead walk prints no
# [overlap] summary at all, and a walk whose instrumentation vanished prints
# one with zero stops; both used to read as "clean" (missing summaries became
# zero findings). Each run must report its summary line and a stop count in
# the same ballpark as the walk this gate ships: the nominal walk makes 251
# stops, so a run that reports far fewer either derailed early or is an older
# binary answering a newer question. The floor is a factor of safety, not a
# ratchet: a stop dropped for good reason must be deliberate, not silent.
MIN_STOPS=200
summary_of() {
    printf '%s\n' "$1" | grep -m1 '^\[overlap\] .* distinct findings$'
}

for l in "${langs[@]}"; do
    # Each locale starts on a fresh card. The walk WRITES to /tmp/simsd -- it
    # signs files, and it exercises REMOVE ALL -- so 21 runs in a row hand each
    # other a card the next one did not expect. It only started mattering when
    # the walk began tapping actions by label: the coordinate taps had been
    # missing REMOVE ALL in some locales and silently doing nothing, which read
    # as "stable" and was really "not pressing the button".
    rm -rf "$KISS_SIM_TMP/simsd"
    out=$(SIM_LANG="$l" "$KISS_SIM_TMP/kissoverlap" 2>&1)
    rc=$?
    sline=$(summary_of "$out")
    n=$(printf '%s\n' "$sline" | sed -n \
        's/^\[overlap\] [^:]*: \([0-9][0-9]*\) stops checked, [0-9][0-9]* game frames skipped, \([0-9][0-9]*\) distinct findings$/\2/p')
    stops=$(printf '%s\n' "$sline" | sed -n \
        's/^\[overlap\] [^:]*: \([0-9][0-9]*\) stops checked.*/\1/p')
    if [ -z "$n" ] || [ -z "$stops" ] || [ "$stops" -lt "$MIN_STOPS" ]; then
        died="$died SIM_LANG=$l(no-summary)"
    fi
    if ! heap_note "$l" "$out"; then
        died="$died SIM_LANG=$l(no-heap)"
    fi
    total=$((total + ${n:-0}))
    [ "$rc" -gt "$worst" ] && worst=$rc
    if [ "$rc" -ne 0 ] && [ "${n:-0}" -eq 0 ]; then died="$died SIM_LANG=$l(rc=$rc)"; fi

    # An EMPTY $n is a locale whose run produced no summary line, which is a
    # walk that died rather than a walk that found nothing. It was reaching
    # this test unguarded -- two lines above, the same variable is read as
    # ${n:-0} -- so the shell errored with "integer expression expected" and
    # fell through to the else, printing the word CLEAN for a locale nothing
    # had successfully checked. died= already records it and the verdict at
    # the end is correct; this line was the one saying otherwise, and it is
    # the line a person reads.
    if [ -z "$n" ]; then
        printf '%-8s NO SUMMARY -- the walk did not finish\n' "$l"
    elif [ "$n" -gt 0 ]; then
        # SLACK and PORT were emitted by the binary and absent from the list
        # below, so the count said 2 and the run printed nothing under it --
        # a finding a person cannot read is one nobody can act on. Every name
        # the binary prints is here now.
        printf '%-8s %3d findings\n' "$l" "$n"
        printf '%s\n' "$out" | grep -E '^  (TEXT|CONTENT|GROWTH|CLIPPED|ROLE|BARE|WALL|FIT|CUT|TINY|AMBER|RAGGED|LAYER|TERM|READ|LADDER|PATH|WIDOW|CLIPX|DOTS|SLACK|PORT|STALE|INK|EXIT|VOID|MARK)' | sed 's/^/  /'
        echo
    else
        printf '%-8s clean\n' "$l"
    fi
    # The backlog lines, which this runner has never shown. Three checks keep
    # shrink-only exemption lists and the docs say "the run prints how many are
    # left" -- true of the binary, and not of this script, which captured the
    # output and printed only findings. An exemption nobody sees is an exemption
    # nobody removes. Non-zero counts and stale entries only, so a clean sweep
    # stays quiet.
    printf '%s\n' "$out" \
        | grep -E 'backlog entry .* never matched|: [1-9][0-9]* (screens|strings) still on the' \
        | sed 's/^\[overlap\] /  /'
    summary="${summary}${l}=${n} "
    # THE CEILING, and why this is a ceiling rather than a pass/fail. English
    # is 0 and stays 0; the twenty translations are not, and the reason is not
    # a bug list. A German row label is a compound word against a lane sized
    # for an English one -- VERSCHLUESSELUNG wants 268px of the 210px ENCRYPTION
    # fits in -- so closing this sweep is a copy project measured in days, not
    # a fix. Left as pass/fail it would be red for all of them on day one, and
    # a gate that is red for a reason nobody is acting on is a gate nobody
    # reads. That sentence is already written down about kissosd.
    #
    # So it ratchets instead, the same shape OC_BARE_BACKLOG and its two
    # siblings already use inside the binary: the number recorded here may
    # only go DOWN. A locale that gets worse fails; a locale that gets better
    # says so and asks for the file to be lowered.
    ceil=$(awk -v L="$l" '$1==L {print $2}' "$ceilfile" 2>/dev/null)
    if [ -n "$ceil" ]; then
        if [ "${n:-0}" -gt "$ceil" ]; then
            over="$over $l(${n}>${ceil})"
        elif [ "${n:-0}" -lt "$ceil" ]; then
            printf '  %s is down to %d from a ceiling of %d -- lower it in %s\n' \
                   "$l" "${n:-0}" "$ceil" "$ceilfile"
        fi
    elif [ "${n:-0}" -gt 0 ]; then
        # No ceiling means no allowance. English is the only locale that ships
        # without an entry, and English is the source copy: it has no excuse.
        unceiled="$unceiled $l(${n})"
    fi
done

echo
echo "totals: $summary"
echo "text overlap gate: $total findings across ${#langs[@]} locales"

# The ROLE check asks about colour, and colour does not change with language, so
# sweeping it over 21 locales would be 21 identical answers. It changes with the
# ACCENT instead, which the locale sweep never varies: those runs are all MONO,
# where the accent is WT_INK and the check has nothing to look at. So the same
# walk runs once per themed accent, in English.
echo
echo "theme role + stale gate: 3 accents"
echo
roletotal=0
stale_seen=""          # set -u: the accent loop appends to it
for a in GREEN CYPHERPINK ORANGE; do
    # A fresh card here too. The locale loop above has done this since the walk
    # started tapping actions by label, and this loop never did -- so each accent
    # run inherited whatever the run before it left on /tmp/simsd: a signature
    # it wrote, a file REMOVE ALL took away. That is a walk derailing on a row
    # that moved, and a derailed walk prints "clean" for every stop it never
    # reached, which is the exact failure the message at the bottom of this
    # script warns about and blames on somebody else's interleaved run.
    rm -rf "$KISS_SIM_TMP/simsd"
    out=$(SIM_ACCENT="$a" "$KISS_SIM_TMP/kissoverlap" 2>&1)
    rc=$?
    # Each run says which STALE backlog entries IT matched. The verdict is
    # taken after the loop; see the block below for why it cannot be taken here.
    stale_seen="$stale_seen
$(printf '%s\n' "$out" | grep -o 'STALE backlog entry [a-z]* |.*|')"
    sline=$(summary_of "$out")
    n=$(printf '%s\n' "$sline" | sed -n \
        's/^\[overlap\] [^:]*: \([0-9][0-9]*\) stops checked, [0-9][0-9]* game frames skipped, \([0-9][0-9]*\) distinct findings$/\2/p')
    stops=$(printf '%s\n' "$sline" | sed -n \
        's/^\[overlap\] [^:]*: \([0-9][0-9]*\) stops checked.*/\1/p')
    if [ -z "$n" ] || [ -z "$stops" ] || [ "$stops" -lt "$MIN_STOPS" ]; then
        died="$died SIM_ACCENT=$a(no-summary)"
    fi
    if ! heap_note "$a" "$out"; then
        died="$died SIM_ACCENT=$a(no-heap)"
    fi
    # STALE is counted HERE and not in the locale loop, because it is the only
    # check that needs the accent to CHANGE and the accent sweep is the only
    # place it does. It also cannot see anything in the MONO run, whose accent
    # is WT_INK -- half the device is legitimately WT_INK, so the old colour
    # would match everything.
    r=$(printf '%s\n' "$out" | grep -cE '^  (ROLE|STALE)')
    roletotal=$((roletotal + r))
    [ "$rc" -gt "$worst" ] && worst=$rc
    if [ "$rc" -ne 0 ] && [ "$r" -eq 0 ]; then died="$died SIM_ACCENT=$a(rc=$rc)"; fi

    if [ "$r" -gt 0 ]; then
        printf '%-12s %3d findings\n' "$a" "$r"
        printf '%s\n' "$out" | grep -E '^  (ROLE|STALE)' | sed 's/^/  /'
        echo
    else
        printf '%-12s clean\n' "$a"
    fi
done

echo
echo "theme role + stale gate: $roletotal findings across 3 accents"
total=$((total + roletotal))

# Whether a STALE exemption is still earning its place is the SWEEP's verdict,
# never one run's. The check needs the accent to CHANGE, so a live entry goes
# unmatched in passes where its screen is not repainted: the single entry on
# the list today matches under CYPHERPINK and ORANGE and NOT under GREEN. A per
# run "never matched a stop" message -- which is what every other backlog in
# overlapcheck.c prints -- would have told somebody to delete a live exemption
# in one run out of three.
#
# This exists because s_stale_hit was set and never read, so this backlog was
# the one that could never be collected. Printed, not failed, exactly like the
# others: a list of excuses going stale is a thing to see, not a build break.
stale_dead=$(printf '%s\n' "$stale_seen" |
    sed -n 's/^STALE backlog entry [a-z]* |\(.*\)|$/\1/p' | sort -u |
    while IFS= read -r e; do
        [ -n "$e" ] || continue
        printf '%s\n' "$stale_seen" | grep -qF "STALE backlog entry matched |$e|" || printf '%s\n' "$e"
    done)
stale_n=$(printf '%s\n' "$stale_seen" |
    sed -n 's/^STALE backlog entry [a-z]* |\(.*\)|$/\1/p' | sort -u | grep -c .)
if [ -n "$stale_dead" ]; then
    echo
    echo "STALE backlog entries that matched under NO accent -- cut them:"
    printf '%s\n' "$stale_dead" | sed 's/^/  /'
fi
echo "STALE backlog: $stale_n entry(s), $(printf '%s\n' "$stale_dead" | grep -c .) excusing nothing" 

# The heap verdict, AFTER the sweep and with its own message. Never through
# kissoverlap's exit code: that code already means two things (findings, and a
# dead walk), and a third meaning would be read as one of the first two.
# The ceiling is enforced by cross multiplication, never by a pre-divided
# percent: heap_max * 1000 cannot truncate, while a percent out of an integer
# division could round 90.9% down to 90% and pass the very ceiling it exists
# to stop. The denominator is the pool reading of the run that SET the peak,
# not the last run's (heap_peak_total, recorded in heap_note).
# 88, not 90, and the two points are the whole gate.
#
# 90 passed a tree that could not build a screen. On 2026-08-31 adding sixteen
# objects to the twenty input SIGN > DETAILS page did not fail: lv_obj_create
# handed back NULL and the next create segfaulted, or lv_refr_now spun at 100%
# CPU forever with no output. The peak at the time was 110968 of 126344, which
# is 87.8% -- comfortably under a 90% ceiling, and comfortably unable to build
# the page. A ceiling above the observed failure is not a ceiling.
#
# Fragmentation is why 12% free was not 12% usable. frag_pct sits at 57% and
# the comment above is right that it is a single-instant sample, so it is
# still not ratcheted -- but it IS printed in the failure, because "peak 87%"
# on its own suggests headroom that a 57% fragmented TLSF pool does not have.
#
# 85 is the number this should be and the tree cannot hold it yet. Getting
# there is object count on whatever screen owns the peak, not a bigger pool:
# the pool matches the device and raising it here would only move the assert
# onto hardware.
HEAP_MAX_PCT="${HEAP_MAX_PCT:-88}"
heap_ok=1
if [ "${heap_peak_total:-0}" -gt 0 ]; then
    heap_pct=$(( heap_max * 1000 / heap_peak_total / 10 ))
    echo
    echo "LVGL heap: peak $heap_max of $heap_peak_total bytes (${heap_pct}%), worst in $heap_who"
    if [ $(( heap_max * 1000 )) -gt $(( heap_peak_total * HEAP_MAX_PCT * 10 )) ]; then
        heap_ok=0
    fi
fi

# Two different failures share this exit code and must not share a message.
# kissoverlap returns non-zero in exactly one case of its own: findings, with
# OVERLAPCHECK_STRICT set. So non-zero WITHOUT findings is the walk dying, and
# it is the more dangerous of the two, because that run printed "clean" for
# every locale it never reached. A gate is allowed to fail; it is not allowed
# to say clean about a screen it never rendered.
if [ -n "$died" ]; then
    echo
    echo "FAILED: a run exited non-zero, or reported no valid summary, stop"
    echo "count or heap line:$died"
    echo "A dead walk prints 'clean' for every stop it never reached, so this"
    echo "run proves nothing -- it is not a finding, and not a clean sweep."
    echo "Re-run it on its own. This run had its own scratch, so an"
    echo "interleaved run is no longer the explanation it used to be."
    exit 1
fi
if [ -n "$over" ]; then
    echo
    echo "FAILED: a locale is over its ceiling:$over"
    echo "The ceiling in $ceilfile may only go DOWN. Cut the copy the findings"
    echo "above name, or if the screen genuinely grew, say so in the commit and"
    echo "raise it deliberately -- never as a side effect."
    exit 1
fi
# STRICT is the contract for any locale WITHOUT a ceiling, which is English
# and only English. The translations answer to the ceiling above instead.
if [ -n "$unceiled" ]; then
    echo
    echo "FAILED: OVERLAPCHECK_STRICT is set and a locale with no ceiling"
    echo "found something:$unceiled"
    exit 1
fi
# A ceiling, not a ratchet. max_used is deterministic, but it moves with every
# screen, label and font this project adds, so an exact-match ratchet turns
# every UI commit red. The pool is 128K and a failed lv_malloc is an LVGL
# assert -- an infinite loop on the device -- so the margin is the point.
if [ "$heap_ok" -eq 0 ]; then
    echo
    echo "FAILED: LVGL heap peaked above the ${HEAP_MAX_PCT}% ceiling (peak ${heap_pct}%, $heap_max of $heap_peak_total, worst in $heap_who, frag ${heap_frag}%)."
    echo "Past the edge LVGL does not report anything: lv_obj_create hands back"
    echo "NULL and the next create segfaults, or lv_refr_now spins at 100% CPU"
    echo "forever. Both were reproduced at 87.8% of this pool, so the free"
    echo "percentage above is not headroom -- read it with the fragmentation."
    echo "Reduce what a screen BUILDS. Raising the pool in sim/lv_conf.h and"
    echo "CONFIG_LV_MEM_SIZE_KILOBYTES only moves the assert onto hardware."
    exit 1
fi

if [ "$total" -gt 0 ]; then
    echo
    echo "Reported without failing: OVERLAPCHECK_STRICT is unset (CI sets it)."
fi
exit 0
