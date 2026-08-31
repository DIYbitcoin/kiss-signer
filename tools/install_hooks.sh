#!/bin/sh
# Install the repository's git hooks into .git/hooks.
#
# Hooks are not tracked by git, so a fresh clone starts with none. Run this once
# after cloning. See the Attribution section of CLAUDE.md for what the commit-msg
# hook removes and why a written rule was not enough on its own.
#
# ONE HOOK, and it is the one that cannot be caught later. A co-author trailer
# that reaches GitHub is permanent -- refs/pull/*/head is written by GitHub and
# never rewritten -- so the only lane that matters is the one before the commit
# is written. Everything else CI can say four minutes afterwards.
#
# There WAS a pre-push hook running the whole gate suite, and it is gone. Every
# gate it ran, .github/workflows/desktop-tests.yml runs as well, along with a
# fuzz pass, a sanitized walk and the installer checks the hook never touched --
# so it was a duplicate that held a push for three minutes behind a UI with
# nowhere to print why. Run the gates while you are working, which is where
# they are useful; read the CI result before calling something done, which is
# the habit the hook was standing in for.
set -e
cd "$(dirname "$0")/.."
DEST=$(git rev-parse --git-path hooks)
mkdir -p "$DEST"
for h in commit-msg; do
    cp "tools/hooks/$h" "$DEST/$h"
    chmod +x "$DEST/$h"
    echo "installed: $DEST/$h"
done
