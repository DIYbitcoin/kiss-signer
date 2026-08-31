#!/usr/bin/env bash
# Remove the worktrees that are finished with: branch fully merged into a base,
# tree clean, not the main checkout, not the one in use, untouched for a while.
#
# A second checkout is cheap to make and easy to forget -- kiss-signer-b sat in
# Documents/Github for two days after session-b landed in develop, carrying its
# own build-docker/. Nothing in git removes one, so this does.
#
# Prints a JSON systemMessage naming what went, so it can be wired to a hook;
# silent when it removes nothing. WORKTREE_GC_DRY=1 to list without removing.
set -u

DRY=${WORKTREE_GC_DRY:-0}
IDLE_MIN=${WORKTREE_GC_IDLE_MIN:-60}

cd "${CLAUDE_PROJECT_DIR:-$PWD}" 2>/dev/null || exit 0
git rev-parse --git-dir >/dev/null 2>&1 || exit 0

common=$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null) || exit 0
main_wt=$(cd "$(dirname "$common")" 2>/dev/null && pwd -P) || exit 0
top=$(git rev-parse --show-toplevel 2>/dev/null)
cur=$(cd "$top" 2>/dev/null && pwd -P)

git -C "$main_wt" worktree prune 2>/dev/null

bases=""
for b in develop main master origin/develop origin/main origin/master; do
  git -C "$main_wt" rev-parse --verify -q "$b" >/dev/null 2>&1 && bases="$bases $b"
done
[ -n "$bases" ] || exit 0

removed=""
wt=""; head=""; branch=""; locked=0

consider() {
  local path="$wt" h="$head" br="$branch" lk="$locked" merged=0 b
  wt=""; head=""; branch=""; locked=0
  [ -n "$path" ] || return 0
  [ "$lk" = 1 ] && return 0
  [ "$path" = "$main_wt" ] && return 0
  [ "$path" = "$cur" ] && return 0
  [ -d "$path" ] || return 0
  # in use: anything touched in the last IDLE_MIN minutes stays
  [ -n "$(find "$path" -maxdepth 1 -mmin "-$IDLE_MIN" -print -quit 2>/dev/null)" ] && return 0
  # uncommitted work stays (ignored files are not dirt)
  [ -z "$(git -C "$path" status --porcelain 2>/dev/null)" ] || return 0
  for b in $bases; do
    if git -C "$main_wt" merge-base --is-ancestor "$h" "$b" 2>/dev/null; then merged=1; break; fi
  done
  [ "$merged" = 1 ] || return 0

  if [ "$DRY" = 1 ]; then
    removed="$removed $path"
    return 0
  fi
  git -C "$main_wt" worktree remove "$path" >/dev/null 2>&1 || return 0
  [ -n "$br" ] && git -C "$main_wt" branch -d "$br" >/dev/null 2>&1
  removed="$removed $path"
}

while IFS= read -r line; do
  case "$line" in
    "worktree "*) consider; wt=${line#worktree } ;;
    "HEAD "*)     head=${line#HEAD } ;;
    "branch "*)   branch=${line#branch refs/heads/} ;;
    locked*)      locked=1 ;;
    detached)     branch="" ;;
  esac
done < <(git -C "$main_wt" worktree list --porcelain 2>/dev/null)
consider

[ -n "$removed" ] || exit 0
names=$(for p in $removed; do basename "$p"; done | paste -sd', ' -)
printf '{"systemMessage":"worktree gc removed: %s","suppressOutput":true}\n' "$names"
