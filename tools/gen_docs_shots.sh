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

echo "== rendering frames =="
/tmp/fruitsim > /tmp/sim_docs.log 2>&1 || {
    echo "simulator failed, tail of /tmp/sim_docs.log:" >&2
    tail -20 /tmp/sim_docs.log >&2
    exit 1
}
tail -1 /tmp/sim_docs.log

echo "== writing docs/shots + docs/walkthrough.md =="
python3 tools/gen_docs_shots.py
