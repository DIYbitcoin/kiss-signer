#!/usr/bin/env bash
# Rebuild the documentation screenshots from the simulator.
#
# The pictures in docs/shots/ are frames this firmware actually rendered, so
# they are regenerated rather than recaptured: run this after any UI change
# that the walkthrough shows, and commit whatever moves.
#
#   bash tools/gen_docs_shots.sh
#
# The simulator writes ~100 frames to /tmp; tools/gen_docs_shots.py picks the
# sixteen the walkthrough uses and writes docs/walkthrough.md alongside them.
set -euo pipefail

cd "$(dirname "$0")/.."

echo "== building the simulator =="
bash sim/build_sim.sh

# Axis passes run BEFORE the canonical pass so /tmp ends holding canonical
# frames and the reveal GIF is built from them. save() prefixes by SIM_LANG
# only, never by SIM_ACCENT, so a green run silently overwrites the MONO .ppm
# files in /tmp under the same names; the ordering here is the fix for that.
echo "== rendering review axes =="
for axis in $(python3 tools/gen_docs_shots.py --axis-list); do
    env $(python3 tools/gen_docs_shots.py --axis-env "$axis") \
        /tmp/fruitsim > "/tmp/sim_axis_$axis.log" 2>&1 || {
        echo "simulator failed on axis $axis, tail of /tmp/sim_axis_$axis.log:" >&2
        tail -20 "/tmp/sim_axis_$axis.log" >&2
        exit 1
    }
    python3 tools/gen_docs_shots.py --review "$axis"
done
python3 tools/gen_docs_shots.py --review-index

echo "== rendering frames =="
/tmp/fruitsim > /tmp/sim_docs.log 2>&1 || {
    echo "simulator failed, tail of /tmp/sim_docs.log:" >&2
    tail -20 /tmp/sim_docs.log >&2
    exit 1
}
tail -1 /tmp/sim_docs.log

echo "== writing docs/shots + docs/walkthrough.md =="
python3 tools/gen_docs_shots.py
