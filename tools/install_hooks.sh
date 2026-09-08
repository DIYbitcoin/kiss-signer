#!/bin/sh
# Install the repository's git hooks into .git/hooks.
#
# Hooks are not tracked by git, so a fresh clone starts with none. Run this once
# after cloning. See the Attribution section of dev/HOUSE-RULES.md for what the commit-msg
# hook removes and why a written rule was not enough on its own.
#
# ONE GATE, and it is the one that cannot be caught later. A co-author trailer
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
#
# post-merge is the second file here and it is not a gate at all -- it refuses
# nothing, prints nothing and holds nothing up. It sweeps the worktrees a merge
# just finished with, because that is the moment they become finished and
# nothing in git notices. See tools/hooks/post-merge for what it will and will
# not remove.
set -e
cd "$(dirname "$0")/.."
DEST=$(git rev-parse --git-path hooks)
mkdir -p "$DEST"
for h in commit-msg post-merge; do
    cp "tools/hooks/$h" "$DEST/$h"
    chmod +x "$DEST/$h"
    echo "installed: $DEST/$h"
done

# Prove the one that can REFUSE still refuses, and still lets the right thing
# through. A gate defined by what it excuses says nothing when it passes, and
# this one is now the only thing in the repo that can hold a commit up -- so an
# install that silently put a dead file in place would be worse than no install.
# Four cases: the message that must be refused, the rewrite of it that must go
# through, the filename that used to be excused and is not any more, and a
# trailer that must be STRIPPED rather than refused.
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
printf 'x\n\nthe superpowers folder held ten files.\n'                     > "$T/refuse"
printf 'x\n\nTen design plans sat where Pages serves. They are design/ now.\n' > "$T/pass"
printf 'x\n\nCLAUDE.md names them now, beside the device compiler.\n'      > "$T/name"
# A real identity, because the strip only fires on one -- "A N Other" is not an
# agent and was not stripped, which is the check working and the CASE being
# wrong. This file and tools/hooks/* are excluded from the attribution lane's
# tracked-file grep for exactly this reason.
printf 'x\n\nbody.\n\nCo-Authored-By: Claude <noreply@anthropic.com>\n' > "$T/trailer"
fail=0
sh "$DEST/commit-msg" "$T/refuse"  >/dev/null 2>&1 && { echo "hook selftest: a message naming a tool was ACCEPTED"; fail=1; }
sh "$DEST/commit-msg" "$T/pass"    >/dev/null 2>&1 || { echo "hook selftest: the rewritten message was refused";   fail=1; }
sh "$DEST/commit-msg" "$T/name"    >/dev/null 2>&1 && { echo "hook selftest: the retired filename exception still excuses"; fail=1; }
sh "$DEST/commit-msg" "$T/trailer" >/dev/null 2>&1 || { echo "hook selftest: a trailer was refused, not stripped"; fail=1; }
grep -qi 'co-authored-by' "$T/trailer" && { echo "hook selftest: the trailer survived"; fail=1; }
[ "$fail" = 0 ] || { echo "hook selftest FAILED -- the installed hook is not doing its job"; exit 1; }
echo "hook selftest: 5 checks, 0 broken"
