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
total=0
summary=""
for l in "${langs[@]}"; do
    out=$(SIM_LANG="$l" /tmp/kissoverlap 2>&1)
    rc=$?
    n=$(printf '%s\n' "$out" | sed -n 's/.*, \([0-9]*\) distinct findings/\1/p' | tail -1)
    [ -z "$n" ] && n=0
    total=$((total + n))
    [ "$rc" -gt "$worst" ] && worst=$rc

    if [ "$n" -gt 0 ]; then
        printf '%-8s %3d findings\n' "$l" "$n"
        printf '%s\n' "$out" | grep -E '^  (TEXT|CONTENT|GROWTH|CLIPPED)' | sed 's/^/  /'
        echo
    else
        printf '%-8s clean\n' "$l"
    fi
    summary="${summary}${l}=${n} "
done

echo
echo "totals: $summary"
echo "text overlap gate: $total findings across ${#langs[@]} locales"

if [ "$worst" -ne 0 ]; then
    echo
    echo "FAILED: OVERLAPCHECK_STRICT is set and the gate found overlapping text."
    exit 1
fi
if [ "$total" -gt 0 ]; then
    echo
    echo "Reported without failing: OVERLAPCHECK_STRICT is unset (CI sets it)."
fi
exit 0
