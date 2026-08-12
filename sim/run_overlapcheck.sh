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

bash sim/build_overlapcheck.sh

# WALL and ROLE both report nothing on the current UI, so prove they can still
# report anything at all before trusting a clean run. See oc_selftest in
# sim/overlapcheck.c for why these two need that and the other five do not.
echo
if ! OVERLAPCHECK_SELFTEST=1 /tmp/kissoverlap; then
    echo
    echo "FAILED: a self test check no longer behaves, so a clean run means nothing."
    exit 1
fi

# The locale codes are the names of the translation files, so a language added
# to i18n/ is covered here the same day without this script being edited.
langs=()
for f in i18n/*.json; do
    b=$(basename "$f" .json)
    langs+=("$b")
done

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
heap_max=0; heap_total=0; heap_who=""
heap_note() {
    local who="$1" out="$2" line used tot
    line=$(printf '%s\n' "$out" | grep -m1 '^\[lvheap\]') || return 0
    [ -n "$line" ] || return 0
    tot=$(printf '%s\n' "$line" | awk '{print $3}')
    used=$(printf '%s\n' "$line" | awk '{print $7}')
    case "$used" in ''|*[!0-9]*) return 0;; esac
    [ "$used" -gt "$heap_max" ] && { heap_max=$used; heap_who=$who; }
    heap_total=$tot
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
    n=$(printf '%s\n' "$out" | sed -n 's/.*, \([0-9]*\) distinct findings/\1/p' | tail -1)
    [ -z "$n" ] && n=0
    total=$((total + n))
    [ "$rc" -gt "$worst" ] && worst=$rc
    if [ "$rc" -ne 0 ] && [ "$n" -eq 0 ]; then died="$died SIM_LANG=$l(rc=$rc)"; fi
    heap_note "$l" "$out"

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
    out=$(SIM_ACCENT="$a" /tmp/kissoverlap 2>&1)
    heap_note "$a" "$out"
    rc=$?
    n=$(printf '%s\n' "$out" | grep -c '^  ROLE')
    roletotal=$((roletotal + n))
    [ "$rc" -gt "$worst" ] && worst=$rc
    if [ "$rc" -ne 0 ] && [ "$n" -eq 0 ]; then died="$died SIM_ACCENT=$a(rc=$rc)"; fi

    if [ "$n" -gt 0 ]; then
        printf '%-12s %3d findings\n' "$a" "$n"
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
heap_pct=0
if [ "$heap_total" -gt 0 ]; then
    heap_pct=$(( heap_max * 100 / heap_total ))
    echo
    echo "LVGL heap: peak $heap_max of $heap_total bytes (${heap_pct}%), worst in $heap_who"
fi

# Two different failures share this exit code and must not share a message.
# kissoverlap returns non-zero in exactly one case of its own: findings, with
# OVERLAPCHECK_STRICT set. So non-zero WITHOUT findings is the walk dying, and
# it is the more dangerous of the two, because that run printed "clean" for
# every locale it never reached. A gate is allowed to fail; it is not allowed
# to say clean about a screen it never rendered.
if [ -n "$died" ]; then
    echo
    echo "FAILED: the walk exited non-zero having reported nothing:$died"
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
HEAP_MAX_PCT="${HEAP_MAX_PCT:-90}"
if [ "$heap_pct" -gt "$HEAP_MAX_PCT" ]; then
    echo
    echo "FAILED: LVGL heap peaked at ${heap_pct}% of the 128K pool (ceiling ${HEAP_MAX_PCT}%)."
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
