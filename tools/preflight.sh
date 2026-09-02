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
# The gate block in CLAUDE.md is the by-hand list and it is fifteen commands
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
# docker and takes minutes, and the 21-locale sweeps, which CLAUDE.md's i18n
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
LOGS="$KISS_SIM_TMP/preflight-logs"
mkdir -p "$LOGS"

NAMES=()
RESULTS=()
FAILED=0
N=0

run() {
    local name="$1"; shift
    N=$((N + 1))
    local log
    log="$LOGS/$(printf '%02d' "$N")-$(echo "$name" | tr ' /' '__').log"
    printf '  %-46s ' "$name"
    if bash -c "$*" >"$log" 2>&1; then
        echo "ok"
        NAMES+=("$name"); RESULTS+=("ok")
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
run "a checker nothing runs" "GATECHECK_SELFTEST=1 python3 tools/check_gates.py"
run "a measurement before its layout" \
    "python3 tools/check_layout_reads.py --selftest && python3 tools/check_layout_reads.py"
run "the sim's LVGL config vs the device's" "LVCONF_SELFTEST=1 python3 tools/check_lv_conf.py"
run "the decisions index vs the comments" "python3 tools/gen_decisions.py --check"
run "the published wasm vs the tree" "python3 tools/check_sim_fresh.py"
run "installer artifacts vs VERSION" "python3 tools/check_installer_version.py"
# Both --check only: they read and report, they do not regenerate. That is the
# whole reason they can sit here rather than in the skip list.
run "every picture resolves to a frame" "python3 tools/gen_docs_shots.py --check"
run "the offline installer packs what the page loads" "python3 tools/make_offline_zip.py --check"
run "how far the pictures trail the screens" \
    "python3 tools/check_docs_fresh.py --selftest && python3 tools/check_docs_fresh.py"

# --- the things that compile ---------------------------------------------
run "unit tests" "bash sim/build_test.sh && \"\$KISS_SIM_TMP/kisstest\""

# NOT in CLAUDE.md's gate block until now, and the reason this file exists: it
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

run "text fit" "bash sim/build_fitcheck.sh && SIM_LANG=en \"\$KISS_SIM_TMP/kissfit\""
run "accent vs status colour" "bash sim/build_themecheck.sh && \"\$KISS_SIM_TMP/kisstheme\""
run "on-video overlay text" "bash sim/build_osdcheck.sh && SIM_LANG=en \"\$KISS_SIM_TMP/kissosd\""

# The walk last: it is the slowest, and every check above tells you something
# useful about a tree the walk would only derail on.
run "screen walk (en)" \
    "bash sim/build_sim.sh && OVERLAPCHECK_LANGS=en bash sim/run_overlapcheck.sh"
run "screens no gate sees" "python3 tools/check_screen_coverage.py"
# AFTER the walk, and not with the other pure python above it: it compares the
# frames the walk saves, and preflight gives every run a fresh KISS_SIM_TMP, so
# ahead of the walk the scratch is empty and it exits 1 every time. CI puts it
# here for the same reason, in the comment on its own smoke walk step.
run "a walk tap that hits nothing" "python3 tools/check_sim_taps.py"

# --- the table ------------------------------------------------------------
echo
echo "  ------------------------------------------------------------"
for i in "${!NAMES[@]}"; do
    printf '  %-46s %s\n' "${NAMES[$i]}" "${RESULTS[$i]}"
done
echo "  ------------------------------------------------------------"
echo

if [ "$FAILED" -gt 0 ]; then
    echo "preflight: $FAILED of $N failed. Logs in $LOGS"
    echo "Do not push. CI runs these plus the container build and the installer checks."
    exit 1
fi

echo "preflight: $N of $N passed."
echo "Still unread by anything here: the ESP-IDF container build, and CI itself."
echo "Read the run after the push -- desktop tests has been red for ten hours before now."
