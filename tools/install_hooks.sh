#!/bin/sh
# Install publication checks and the existing post-merge cleanup hook.
# Run after cloning. Tests use temporary local repositories, never a server.
# Direct API submissions still require the explicit review in dev/PUBLICATION.md.
set -e
cd "$(dirname "$0")/.."
PYTHONDONTWRITEBYTECODE=1 python3 tools/hooks/test_publication_guard.py
DEST=$(git rev-parse --git-path hooks)
mkdir -p "$DEST"
for h in commit-msg post-merge pre-push publication_guard.py; do
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
# The length check only warns, so its proof is what it prints. The 50-character
# subject is 52 bytes because of the em dash: a byte count would flag it, a
# character count must not. The 51-character one must be flagged AND go through.
printf 'Count this subject by characters — never by bytes!\n\nbody.\n'  > "$T/at50"
printf 'Count this subject by characters — never by bytes!!\n\nbody.\n' > "$T/at51"
fail=0
sh "$DEST/commit-msg" "$T/refuse"  >/dev/null 2>&1 && { echo "hook selftest: a message naming a tool was ACCEPTED"; fail=1; }
sh "$DEST/commit-msg" "$T/pass"    >/dev/null 2>&1 || { echo "hook selftest: the rewritten message was refused";   fail=1; }
sh "$DEST/commit-msg" "$T/name"    >/dev/null 2>&1 && { echo "hook selftest: the retired filename exception still excuses"; fail=1; }
sh "$DEST/commit-msg" "$T/trailer" >/dev/null 2>&1 || { echo "hook selftest: a trailer was refused, not stripped"; fail=1; }
grep -qi 'co-authored-by' "$T/trailer" && { echo "hook selftest: the trailer survived"; fail=1; }
sh "$DEST/commit-msg" "$T/at50" 2>&1 | grep -q 'subject is' && { echo "hook selftest: a 50-character subject was flagged"; fail=1; }
sh "$DEST/commit-msg" "$T/at51" 2>&1 | grep -q 'subject is 51' || { echo "hook selftest: a 51-character subject went unflagged"; fail=1; }
sh "$DEST/commit-msg" "$T/at51" >/dev/null 2>&1 || { echo "hook selftest: a long subject was refused, not warned"; fail=1; }
[ "$fail" = 0 ] || { echo "hook selftest FAILED -- the installed hook is not doing its job"; exit 1; }
echo "hook selftest: 8 checks, 0 broken"
