#!/usr/bin/env bash
# Saves this clone's agent instructions and fork log (AGENTS.md and FORK-LOG.md at the repo root, untracked so
# they stay put on every branch) into docs/fork/ on fork/twin-app, as a commit, when they changed. Works without
# checking anything out: the files are written straight into a new commit on top of fork/twin-app.
#
#   scripts/fork/save-docs.sh        (scripts/fork/sync.sh runs it first)
#
# To get them back in a fresh clone: git show fork/twin-app:docs/fork/AGENTS.md > AGENTS.md (same for FORK-LOG.md).
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
branch=fork/twin-app

if git worktree list --porcelain | grep -qx "branch refs/heads/$branch"; then
  echo "$branch is checked out in a worktree; commit docs/fork there instead." >&2
  exit 1
fi

index=$(mktemp); trap 'rm -f "$index"' EXIT
export GIT_INDEX_FILE=$index
git read-tree "$branch"
for f in AGENTS.md FORK-LOG.md; do
  [ -f "$f" ] || continue
  git update-index --add --cacheinfo "100644,$(git hash-object -w "$f"),docs/fork/$f"
done
tree=$(git write-tree)
if [ "$tree" = "$(git rev-parse "$branch^{tree}")" ]; then
  echo "docs/fork on $branch is up to date"
  exit 0
fi
old=$(git rev-parse "$branch")
new=$(git commit-tree "$tree" -p "$old" -m "docs(fork): save AGENTS.md and FORK-LOG.md")
git update-ref "refs/heads/$branch" "$new" "$old"
echo "saved to $branch: $(git log -1 --format='%h %s' "$new")"
