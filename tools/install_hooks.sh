#!/bin/sh
# Install the repository's git hooks into .git/hooks.
#
# Hooks are not tracked by git, so a fresh clone starts with none. Run this once
# after cloning. See the Attribution section of CLAUDE.md for what the commit-msg
# hook removes and why a written rule was not enough on its own.
#
# pre-push runs the gates CI runs, before the push rather than four minutes
# after it. desktop-tests sat red on develop for four days because the only
# thing that reads a red run is somebody who goes and looks; two of the breaks
# were one line each and both would have failed locally in under a minute.
set -e
cd "$(dirname "$0")/.."
DEST=$(git rev-parse --git-path hooks)
mkdir -p "$DEST"
for h in commit-msg pre-push; do
    cp "tools/hooks/$h" "$DEST/$h"
    chmod +x "$DEST/$h"
    echo "installed: $DEST/$h"
done
