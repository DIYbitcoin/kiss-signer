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

# A stale binary is worse than none: it passes its own self test, then the 24
# walks "verify" whatever was built last. Fail the run when the build fails.
bash sim/build_overlapcheck.sh || {
    echo "FAILED: sim/build_overlapcheck.sh" >&2
    exit 1
}

# WALL and ROLE both report nothing on the current UI, so prove they can still
# report anything at all before trusting a clean run. See oc_selftest in
# sim/overlapcheck.c for why these two need that and the other five do not.
# The exit status is not enough on its own: the checks print a marker when
# they behave, so a run that exits 0 without them (or vice versa) also fails.
echo
st=$(OVERLAPCHECK_SELFTEST=1 /tmp/kissoverlap 2>&1)
if [ $? -ne 0 ] ||
    ! printf '%s\n' "$st" | grep -q 'WALL self test: 2 cases, all as expected' ||
    ! printf '%s\n' "$st" | grep -q 'ROLE self test: 4 cases, all as expected'; then
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
heap_max=0; heap_total=""; heap_who=""; heap_peak_total=0
heap_note() {
    local who="$1" out="$2" line tot used
    line=$(printf '%s\n' "$out" | grep -m1 '^\[lvheap\]') || return 1
    [ -n "$line" ] || return 1
    # [lvheap] total %u used %u max_used %u frag %u%%: $3 is the pool, $7 the
    # peak. Words sit between them, so a "$2/$4" pickup reads words and
    # disables the ceiling silently.
    tot=$(printf '%s\n' "$line" | awk '{print $3}')
    used=$(printf '%s\n' "$line" | awk '{print $7}')
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
        heap_max=$used; heap_who=$who; heap_peak_total=$tot
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
    # the walk began tapping pills by label: the coordinate taps had been
    # missing REMOVE ALL in some locales and silently doing nothing, which read
    # as "stable" and was really "not pressing the button".
    rm -rf /tmp/simsd
    out=$(SIM_LANG="$l" /tmp/kissoverlap 2>&1)
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

    if [ "$n" -gt 0 ]; then
        printf '%-8s %3d findings\n' "$l" "$n"
        printf '%s\n' "$out" | grep -E '^  (TEXT|CONTENT|GROWTH|CLIPPED|ROLE|BARE|WALL)' | sed 's/^/  /'
        echo
    else
        printf '%-8s clean\n' "$l"
    fi
    summary="${summary}${l}=${n} "
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
echo "theme role gate: 3 accents"
echo
roletotal=0
for a in GREEN CYPHERPINK ORANGE; do
    # A fresh card here too. The locale loop above has done this since the walk
    # started tapping pills by label, and this loop never did -- so each accent
    # run inherited whatever the run before it left on /tmp/simsd: a signature
    # it wrote, a file REMOVE ALL took away. That is a walk derailing on a row
    # that moved, and a derailed walk prints "clean" for every stop it never
    # reached, which is the exact failure the message at the bottom of this
    # script warns about and blames on somebody else's interleaved run.
    rm -rf /tmp/simsd
    out=$(SIM_ACCENT="$a" /tmp/kissoverlap 2>&1)
    rc=$?
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
    r=$(printf '%s\n' "$out" | grep -c '^  ROLE')
    roletotal=$((roletotal + r))
    [ "$rc" -gt "$worst" ] && worst=$rc
    if [ "$rc" -ne 0 ] && [ "$r" -eq 0 ]; then died="$died SIM_ACCENT=$a(rc=$rc)"; fi

    if [ "$r" -gt 0 ]; then
        printf '%-12s %3d findings\n' "$a" "$r"
        printf '%s\n' "$out" | grep -E '^  ROLE' | sed 's/^/  /'
        echo
    else
        printf '%-12s clean\n' "$a"
    fi
done

echo
echo "theme role gate: $roletotal findings across 3 accents"
total=$((total + roletotal))

# The heap verdict, AFTER the sweep and with its own message. Never through
# kissoverlap's exit code: that code already means two things (findings, and a
# dead walk), and a third meaning would be read as one of the first two.
# The ceiling is enforced by cross multiplication, never by a pre-divided
# percent: heap_max * 1000 cannot truncate, while a percent out of an integer
# division could round 90.9% down to 90% and pass the very ceiling it exists
# to stop. The denominator is the pool reading of the run that SET the peak,
# not the last run's (heap_peak_total, recorded in heap_note).
HEAP_MAX_PCT="${HEAP_MAX_PCT:-90}"
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
    echo "Re-run it on its own: /tmp/simsd and the /tmp frames are shared with"
    echo "kisstest and the screen walk, and an interleaved run kills it."
    exit 1
fi
if [ "$worst" -ne 0 ]; then
    echo
    echo "FAILED: OVERLAPCHECK_STRICT is set and the gate found something."
    exit 1
fi
# A ceiling, not a ratchet. max_used is deterministic, but it moves with every
# screen, label and font this project adds, so an exact-match ratchet turns
# every UI commit red. The pool is 128K and a failed lv_malloc is an LVGL
# assert -- an infinite loop on the device -- so the margin is the point.
if [ "$heap_ok" -eq 0 ]; then
    echo
    echo "FAILED: LVGL heap peaked above the ${HEAP_MAX_PCT}% ceiling (peak ${heap_pct}%, $heap_max of $heap_peak_total, worst in $heap_who)."
    echo "A failed lv_malloc mid-render is an LVGL assert, which on the device"
    echo "is an infinite loop. Reduce what a screen builds, or raise the pool"
    echo "in BOTH sim/lv_conf.h and CONFIG_LV_MEM_SIZE_KILOBYTES deliberately."
    exit 1
fi

if [ "$total" -gt 0 ]; then
    echo
    echo "Reported without failing: OVERLAPCHECK_STRICT is unset (CI sets it)."
fi
exit 0
