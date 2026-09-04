#!/usr/bin/env bash
# Rebuild the documentation screenshots from the simulator.
#
# The pictures in docs/shots/ are frames this firmware actually rendered, so
# they are regenerated rather than recaptured: run this after any UI change
# that the walkthrough shows, and commit whatever moves.
#
#   bash tools/gen_docs_shots.sh
#
# The simulator writes ~100 frames to the scratch root; tools/gen_docs_shots.py
# picks the sixteen the walkthrough uses and writes docs/walkthrough.md
# alongside them.
set -euo pipefail

cd "$(dirname "$0")/.."

# The scratch root, from the same place every sim build script gets it.
#
# This script used to say "$KISS_SIM_TMP/fruitsim" outright, while sim/build_sim.sh
# already honoured KISS_SIM_TMP and tools/gen_docs_shots.py already read its
# frames from it. So with the variable set -- which the house rules tell you to
# do whenever two things run in one checkout -- this built a new binary into
# the scratch root and then ran a DIFFERENT, older one left behind in /tmp, and
# the Python half read those frames as current. Nothing failed. The pictures
# were simply of whatever code that stale binary had been built from.
. sim/sim_tmp.sh

echo "== building the simulator =="
bash sim/build_sim.sh

# Axis passes run BEFORE the canonical pass so the scratch root ends holding
# canonical frames and the reveal GIF is built from them. save() prefixes by
# SIM_LANG only, never by SIM_ACCENT, so a green run silently overwrites the
# MONO .ppm files under the same names; the ordering here is the fix for that.
echo "== rendering review axes =="
for axis in $(python3 tools/gen_docs_shots.py --axis-list); do
    env $(python3 tools/gen_docs_shots.py --axis-env "$axis") \
        "$KISS_SIM_TMP/fruitsim" > "$KISS_SIM_TMP/sim_axis_$axis.log" 2>&1 || {
        echo "simulator failed on axis $axis, tail of $KISS_SIM_TMP/sim_axis_$axis.log:" >&2
        tail -20 "$KISS_SIM_TMP/sim_axis_$axis.log" >&2
        exit 1
    }
    python3 tools/gen_docs_shots.py --review "$axis"
done
python3 tools/gen_docs_shots.py --review-index

echo "== rendering frames =="
"$KISS_SIM_TMP/fruitsim" > "$KISS_SIM_TMP/sim_docs.log" 2>&1 || {
    echo "simulator failed, tail of $KISS_SIM_TMP/sim_docs.log:" >&2
    tail -20 "$KISS_SIM_TMP/sim_docs.log" >&2
    exit 1
}
tail -1 "$KISS_SIM_TMP/sim_docs.log"

echo "== writing docs/shots + docs/walkthrough.md =="
python3 tools/gen_docs_shots.py
