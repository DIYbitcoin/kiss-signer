#!/bin/bash
# Everything the desktop CI lane checks, in one command, before the push.
#
# WHY THIS EXISTS. sim/check_sd_psbts.sh compiles main/kiss_psbt.c. When
# 468aa2a6 moved the signature fingerprint onto cUR's streaming sha256,
# sim/build_test.sh gained the include and the source; check_sd_psbts.sh did
# not, and died on the header. It ran only in CI, so nobody working by hand
# could have hit it -- and it then sat red across THIRTEEN consecutive pushes,
# about ten hours, because a red CI is a thing somebody has to go and look at.
#
# The by-hand gate block is the working list and it is fifteen commands
# long. Fifteen commands is a list people run most of. This runs all of them
# plus the four the block never listed, which is exactly where the break was.
#
# It does NOT stop at the first failure. A run that quits early tells you about
# one thing and hides the rest, and the whole point is the summary at the end.
#
#   bash tools/preflight.sh              # everything
#   bash tools/preflight.sh -q           # only the failures, then the table
#
# What it deliberately leaves out: the ESP-IDF container build, which needs
# docker and takes minutes, and the 21-locale sweeps, which the i18n
# rule says not to run while the screens are still moving. CI runs the container
# build in its own lane and that is the one to read after the push.
set -uo pipefail
cd "$(dirname "$0")/.."

QUIET=0
[ "${1:-}" = "-q" ] && QUIET=1

# Its own scratch, so this can run beside a walk somebody else started. The
# walk lock in main/kiss_simpath.h refuses two runs on one root, and that
# refusal is the failure this avoids rather than reports.
export KISS_SIM_TMP="${KISS_SIM_TMP:-/tmp/kiss-preflight-$$}"
mkdir -p "$KISS_SIM_TMP"

# Warnings are errors for everything this script builds. Every sim/build_*.sh
# is clean today and the desktop CI lane sets the same variable, so a warning
# that appears here is one somebody is about to push. sim/sim_tmp.sh says why
# it is opt in rather than always on.
export KISS_WERROR=1
LOGS="$KISS_SIM_TMP/preflight-logs"

# PREFLIGHT_LANGS -- which locales the three locale aware gates look at.
#
# Default en, because the i18n rule pins the daily lane to English
# while the screens are still moving. That rule names the cost it accepts:
# the full sweep catches real faults and they wait for the translation pass.
#
# The translation pass is now happening, so the wait is over for any locale
# that has landed, and this is how it gets checked as it lands rather than at
# the end:
#
#   PREFLIGHT_LANGS="tr vi" bash tools/preflight.sh    # the two just swept
#   PREFLIGHT_LANGS=all     bash tools/preflight.sh    # all 21, the slow one
#
# fit and osd take ONE locale each through SIM_LANG, so a list loops them; the
# walk takes the whole list at once because run_overlapcheck.sh parses it. For
# "all" every one of the three is left UNSET, which is what each of them reads
# as the full sweep -- not a list this file would have to keep in step with
# i18n/.
PREFLIGHT_LANGS="${PREFLIGHT_LANGS:-en}"
if [ "$PREFLIGHT_LANGS" = "all" ]; then
    PF_LANG_LIST=""
    for f in i18n/*.json; do
        b=$(basename "$f" .json)
        PF_LANG_LIST="$PF_LANG_LIST $b"
    done
    PF_SWEEP=1
else
    PF_LANG_LIST="$PREFLIGHT_LANGS"
    PF_SWEEP=0
    for b in $PF_LANG_LIST; do
        [ -f "i18n/$b.json" ] || { echo "preflight: no such locale: $b" >&2; exit 2; }
    done
fi

mkdir -p "$LOGS"

NAMES=()
RESULTS=()
FAILED=0
NOTED=0
N=0

# A THIRD STATE, because two were not enough to describe what these scripts do.
#
# This table maps exit code to ok/FAILED, and check_docs_fresh exits 0 while
# printing "11 commits have changed a screen since". That is not a bug in the
# gate: it is ADVISORY on develop by design and exits nonzero only under
# --strict, which CI uses on the way to main, where the stale picture is what
# ships. But the table printed "ok" over it, so the run said nothing was wrong
# in the same breath as the gate saying something was -- and it did so in the
# same run that correctly caught check_sim_fresh beside it, which does exit
# nonzero.
#
# Reading the totals line does not catch this one. Only reading the gate's own
# output does, which is the thing a summary table exists to save somebody from.
#
# So a runner may name a pattern meaning "this passed, and it is still telling
# you something". It prints NOTE, it does not fail the run, and it is counted
# separately at the bottom, so the last line of a green run cannot quietly sit
# on top of a gate with something to say.
run() {
    local name="$1"
    local note_re=""
    if [ "$1" = "--note-if" ]; then note_re="$2"; name="$3"; shift 3
    else shift
    fi
    N=$((N + 1))
    local log
    log="$LOGS/$(printf '%02d' "$N")-$(echo "$name" | tr ' /' '__').log"
    printf '  %-46s ' "$name"
    if bash -c "$*" >"$log" 2>&1; then
        if [ -n "$note_re" ] && grep -qE "$note_re" "$log"; then
            echo "NOTE"
            NAMES+=("$name"); RESULTS+=("NOTE")
            NOTED=$((NOTED + 1))
        else
            echo "ok"
            NAMES+=("$name"); RESULTS+=("ok")
        fi
    else
        echo "FAILED"
        NAMES+=("$name"); RESULTS+=("FAILED")
        FAILED=$((FAILED + 1))
        [ "$QUIET" = 1 ] && { echo "    --- $log"; tail -15 "$log" | sed 's/^/    /'; }
    fi
    [ "$QUIET" = 0 ] && tail -3 "$log" | sed 's/^/      /'
    return 0
}

echo
echo "preflight: scratch is $KISS_SIM_TMP"
echo

# --- does this file still cover the CI lane -------------------------------
# First, because every other line below is only worth what its coverage is.
run "this script still covers the CI lane" "CHECKPREFLIGHT_SELFTEST=1 python3 tools/check_preflight.py"

# --- the words and the keys, cheapest first ------------------------------
# gen_i18n.py has no --check: it WRITES. CI can regenerate and diff because its
# tree is disposable; this one is shared, and the first run of this script
# regenerated 444 lines of main/i18n_tables.c on top of somebody else's
# uncommitted i18n/it.json. So it restores what it wrote, and when i18n/ is
# already dirty it does not run at all -- the answer would be about their edit
# and not about the tree.
run "i18n generator is not stale" '
    if [ -n "$(git status --porcelain -- i18n)" ]; then
        echo "i18n/ is dirty -- somebody is mid edit. Skipped rather than answered wrong."
        exit 0
    fi
    python3 tools/gen_i18n.py
    rc=0
    git diff --exit-code -- main/i18n_keys.h main/i18n_tables.c "tools/fonts/glyphs_*.txt" || rc=1
    git checkout -- main/i18n_keys.h main/i18n_tables.c "tools/fonts/glyphs_*.txt" 2>/dev/null || true
    exit $rc'
run "keys nothing references" "python3 tools/check_i18n_orphans.py"
run "the words on screen and in the docs" "python3 tools/check_vocab.py"
run "a refusal with no words" "python3 tools/check_stop_reasons.py"
run "an icon with no glyph" "GLYPHCHECK_SELFTEST=1 python3 tools/check_glyphs.py"
run "the same, for the mono faces" "python3 tools/check_mono_glyphs.py"
# A STRING with no glyph, rather than an icon. It was named by the by-hand
# gate list and by nothing else, so when that list stopped being tracked it
# became a checker nothing ran -- which is the exact failure check_gates.py
# exists to catch, and it caught it.
run "a string with no glyph" "python3 tools/check_text_glyphs.py"
run "a checker nothing runs" "GATECHECK_SELFTEST=1 python3 tools/check_gates.py"
run "a measurement before its layout" \
    "python3 tools/check_layout_reads.py --selftest && python3 tools/check_layout_reads.py"
run "the sim's LVGL config vs the device's" "LVCONF_SELFTEST=1 python3 tools/check_lv_conf.py"
run "the decisions index vs the comments" "python3 tools/gen_decisions.py --check"
run "the published wasm vs the tree" \
    "python3 tools/check_sim_fresh.py --selftest && python3 tools/check_sim_fresh.py"
run "installer artifacts vs VERSION" \
    "python3 tools/check_installer_version.py --selftest && python3 tools/check_installer_version.py"
# Both --check only: they read and report, they do not regenerate. That is the
# whole reason they can sit here rather than in the skip list.
run "every picture resolves to a frame" "python3 tools/gen_docs_shots.py --check"
run "the offline installer packs what the page loads" "python3 tools/make_offline_zip.py --check"
run --note-if "commits have changed a screen since" \
    "how far the pictures trail the screens" \
    "python3 tools/check_docs_fresh.py --selftest && python3 tools/check_docs_fresh.py"

# --- the things that compile ---------------------------------------------
run "unit tests" "bash sim/build_test.sh && \"\$KISS_SIM_TMP/kisstest\""

# NOT in the by-hand gate block until now, and the reason this file exists: it
# compiles main/kiss_psbt.c with its own include list, which is what drifted.
run "SD PSBT fixtures reach their verdicts" \
    "rm -rf \"\$KISS_SIM_TMP/sdfix\" && mkdir -p \"\$KISS_SIM_TMP/sdfix\" \
     && bash sim/mk_sd_psbts.sh \"\$KISS_SIM_TMP/sdfix\" \
     && bash sim/check_sd_psbts.sh \"\$KISS_SIM_TMP/sdfix\""

run "NVS forensic scanner" \
    "python3 tools/test_nvs_seed_scan.py | tee \"\$KISS_SIM_TMP/nvs.log\" \
     && grep -q 'PASS: recovery survives damaged NVS metadata' \"\$KISS_SIM_TMP/nvs.log\" \
     && bash tools/test_nvs_seed_check.sh | tee \"\$KISS_SIM_TMP/nvsc.log\" \
     && grep -q 'PASS: nvs_seed_check.sh plumbing' \"\$KISS_SIM_TMP/nvsc.log\""

run "parser fuzz (ASAN + UBSAN)" \
    "bash sim/build_fuzz.sh && \"\$KISS_SIM_TMP/kissfuzz\" | tee \"\$KISS_SIM_TMP/fuzz.log\" \
     && grep -q 'ALL FUZZ PASS' \"\$KISS_SIM_TMP/fuzz.log\""

run "text fit (build)" "bash sim/build_fitcheck.sh"
if [ "$PF_SWEEP" = 1 ]; then
    run "text fit (all 21)" "FITCHECK_SELFTEST=1 \"\$KISS_SIM_TMP/kissfit\""
else
    for L in $PF_LANG_LIST; do
        run "text fit ($L)" "FITCHECK_SELFTEST=1 SIM_LANG=$L \"\$KISS_SIM_TMP/kissfit\""
    done
fi
run "accent vs status colour" "bash sim/build_themecheck.sh && \"\$KISS_SIM_TMP/kisstheme\""
run "on-video overlay text (build)" "bash sim/build_osdcheck.sh"
if [ "$PF_SWEEP" = 1 ]; then
    run "on-video overlay text (all 21)" "\"\$KISS_SIM_TMP/kissosd\""
else
    for L in $PF_LANG_LIST; do
        run "on-video overlay text ($L)" "SIM_LANG=$L \"\$KISS_SIM_TMP/kissosd\""
    done
fi

# The walk last: it is the slowest, and every check above tells you something
# useful about a tree the walk would only derail on.
if [ "$PF_SWEEP" = 1 ]; then
    run "screen walk (all 21)" \
        "bash sim/build_sim.sh && bash sim/run_overlapcheck.sh"
else
    run "screen walk ($PF_LANG_LIST)" \
        "bash sim/build_sim.sh && OVERLAPCHECK_LANGS=\"$PF_LANG_LIST\" bash sim/run_overlapcheck.sh"
fi
run "screens no gate sees" "python3 tools/check_screen_coverage.py"
# AFTER the walk, and not with the other pure python above it: it compares the
# frames the walk saves, and preflight gives every run a fresh KISS_SIM_TMP, so
# ahead of the walk the scratch is empty and it exits 1 every time. CI puts it
# here for the same reason, in the comment on its own smoke walk step.
run "a walk tap that hits nothing" \
    "python3 tools/check_sim_taps.py --selftest && python3 tools/check_sim_taps.py"

# --- the table ------------------------------------------------------------
echo
echo "  ------------------------------------------------------------"
for i in "${!NAMES[@]}"; do
    printf '  %-46s %s\n' "${NAMES[$i]}" "${RESULTS[$i]}"
done
echo "  ------------------------------------------------------------"
echo

if [ "$FAILED" -gt 0 ]; then
    if [ "$NOTED" -gt 0 ]; then
        echo "preflight: $FAILED of $N failed, $NOTED with a NOTE. Logs in $LOGS"
    else
        echo "preflight: $FAILED of $N failed. Logs in $LOGS"
    fi
    echo "Do not push. CI runs these plus the container build and the installer checks."
    exit 1
fi

if [ "$NOTED" -gt 0 ]; then
    echo "preflight: $N of $N passed, $NOTED with a NOTE -- read those logs."
else
    echo "preflight: $N of $N passed."
fi
echo "Still unread by anything here: the ESP-IDF container build, and CI itself."
echo "Read the run after the push -- desktop tests has been red for ten hours before now."
