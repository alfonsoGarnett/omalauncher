#!/usr/bin/env bash
# Sync the fork with upstream (SimonSchubert/omalauncher) and rebuild local/build.
#
#   scripts/fork/sync.sh            fetch upstream, move main to it, rebase every topic branch, recreate local/build
#   scripts/fork/sync.sh --build    ... then build the debug APK
#   scripts/fork/sync.sh --install  ... then build and install it over adb (restarts the launcher and any Linux
#                                   session inside it, so run it last)
#
# main mirrors upstream and never gets commits of its own. Topic branches (scripts/fork/topics) are rebased onto
# it; local/build is recreated as main plus a --no-ff merge of each, in that file's order. git rerere replays
# conflict resolutions recorded before, so a conflict only needs resolving once. On a conflict this stops and
# says what to do; the old local/build stays as it was until a sync finishes.
set -euo pipefail

# Runs from a copy: checking out branches changes this file under it.
if [ -z "${FORK_SYNC_COPY:-}" ]; then
  copy=$(mktemp); cp "$0" "$copy"
  FORK_SYNC_COPY=1 exec bash "$copy" "$@"
fi

cd "$(git rev-parse --show-toplevel)"
build=0; install=0
for a in "$@"; do case "$a" in --build) build=1 ;; --install) build=1; install=1 ;; *) echo "unknown option $a" >&2; exit 2 ;; esac; done

[ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "Commit or stash your changes first." >&2; exit 1; }
git config rerere.enabled true; git config rerere.autoUpdate true

topics=$(git show fork/twin-app:scripts/fork/topics | grep -v '^\s*#' | grep -v '^\s*$')
stamp=$(date +%Y%m%d-%H%M)

echo "== fetching upstream"
git fetch origin
old_main=$(git rev-parse main)
git checkout -q --detach
git branch -f main origin/main
echo "main: $(git log -1 --format='%h %s' main)"

# Each branch's tip before the sync, for --onto rebases of stacked branches.
declare -A before
while read -r b base; do before[$b]=$(git rev-parse "$b"); git tag -f "backup/sync-$stamp/$b" "$b" >/dev/null; done <<< "$topics"
git tag -f "backup/sync-$stamp/local/build" local/build >/dev/null
echo "backups: backup/sync-$stamp/*"

while read -r b base; do
  if [ -n "${base:-}" ]; then
    onto=$base; from=${before[$base]}
  else
    onto=main; from=$(git merge-base "${before[$b]}" "$old_main")
  fi
  echo "== rebasing $b onto $onto"
  if ! git rebase -q --onto "$onto" "$from" "$b"; then
    if [ -z "$(git diff --name-only --diff-filter=U)" ]; then
      # rerere resolved every file: carry on.
      GIT_EDITOR=true git rebase --continue || true
    fi
    if [ -d "$(git rev-parse --git-path rebase-merge)" ] || [ -d "$(git rev-parse --git-path rebase-apply)" ]; then
      echo
      echo "Conflict while rebasing $b. Resolve the files git lists, git add them, and run git rebase --continue;"
      echo "then run this script again (it starts over from the fetched upstream; finished branches rebase as no-ops)."
      exit 1
    fi
  fi
done <<< "$topics"

echo "== recreating local/build"
git checkout -q -B local/build-next main
while read -r b base; do
  if ! git merge -q --no-ff --no-edit -m "Merge $b into local/build" "$b"; then
    if [ -n "$(git diff --name-only --diff-filter=U)" ]; then
      echo
      echo "Conflict merging $b into local/build-next. Resolve, git add, git commit, then merge the remaining"
      echo "branches from scripts/fork/topics by hand and run: git branch -f local/build local/build-next"
      exit 1
    fi
    git commit -q --no-edit
  fi
done <<< "$topics"
git branch -f local/build local/build-next
git checkout -q local/build
git branch -D local/build-next >/dev/null
echo "local/build: $(git log -1 --format='%h' local/build), upstream $(git log -1 --format='%h' main)"

if [ $build = 1 ]; then
  echo "== building"
  bash scripts/build-wlbridge.sh
  ./gradlew assembleDebug --console=plain -q
  ls -la app/build/outputs/apk/debug/app-debug.apk
fi
if [ $install = 1 ]; then
  dev=$(droid | awk '{print $1}')
  echo "== installing on $dev (the launcher restarts)"
  adb -s "$dev" install -r app/build/outputs/apk/debug/app-debug.apk
fi
