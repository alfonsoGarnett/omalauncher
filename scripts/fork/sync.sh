#!/usr/bin/env bash
# Sync the fork with upstream (SimonSchubert/omalauncher) and rebuild local/build.
#
#   scripts/fork/sync.sh            fetch upstream, move main to it, rebase every topic branch, recreate local/build
#   scripts/fork/sync.sh --build    ... then build the debug APK
#   scripts/fork/sync.sh --install  ... then build and install it over adb (restarts the launcher and any Linux
#                                   session inside it, so run it last)
#   scripts/fork/sync.sh --backup   ... and push the branches to GitHub: code to the public fork (remote "fork"),
#                                   fork/docs and fork/personal to the private repo (remote "private")
#
# First, scripts/fork/save-snapshots.py saves the files listed in scripts/fork/snapshots.conf into their branches
# (fork/docs, fork/personal, env/omarchy-dex). After a build, scripts/fork/install-guest-tools.sh installs the
# guest/ tools of local/build into this system.
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
build=0; install=0; backup=0
for a in "$@"; do case "$a" in --build) build=1 ;; --install) build=1; install=1 ;; --backup) backup=1 ;; *) echo "unknown option $a" >&2; exit 2 ;; esac; done

[ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "Commit or stash your changes first." >&2; exit 1; }
git config rerere.enabled true; git config rerere.autoUpdate true

# Our documents, personal files and the DeX desktop, saved into their branches.
python3 <(git show fork/twin-app:scripts/fork/save-snapshots.py)

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
if [ $build = 1 ]; then
  bash scripts/fork/install-guest-tools.sh
fi

if [ $backup = 1 ]; then
  # Branches with an open upstream PR are left alone: pushing a rebased one would change the PR. Push those on purpose.
  in_pr=$(gh pr list --repo SimonSchubert/omalauncher --author @me --state open --json headRefName -q '.[].headRefName' 2>/dev/null || true)
  public=(main fork/twin-app local/build env/omarchy-dex)
  while read -r b base; do public+=("$b"); done <<< "$topics"
  echo "== backing up to the public fork"
  for b in "${public[@]}"; do
    case "$b" in fork/docs|fork/personal) echo "refusing to push $b to the public fork" >&2; exit 1 ;; esac
    if grep -qx "$b" <<< "$in_pr"; then echo "  $b: open PR, not pushed"; continue; fi
    git rev-parse --verify -q "refs/heads/$b" >/dev/null || continue
    git push -q --force-with-lease fork "refs/heads/$b:refs/heads/$b" && echo "  $b"
  done
  echo "== backing up to the private repo"
  for b in fork/docs fork/personal; do
    git rev-parse --verify -q "refs/heads/$b" >/dev/null || continue
    git push -q --force-with-lease private "refs/heads/$b:refs/heads/$b" && echo "  $b"
  done
fi

if [ $install = 1 ]; then
  dev=$(droid | awk '{print $1}')
  echo "== installing on $dev (the launcher restarts)"
  adb -s "$dev" install -r app/build/outputs/apk/debug/app-debug.apk
fi
