#!/bin/sh
# Install the repository's git hooks into .git/hooks.
#
# Hooks are not tracked by git, so a fresh clone starts with none. Run this once
# after cloning. See the Attribution section of CLAUDE.md for what the commit-msg
# hook removes and why a written rule was not enough on its own.
set -e
cd "$(dirname "$0")/.."
DEST=$(git rev-parse --git-path hooks)
mkdir -p "$DEST"
cp tools/hooks/commit-msg "$DEST/commit-msg"
chmod +x "$DEST/commit-msg"
echo "installed: $DEST/commit-msg"
