#!/usr/bin/env python3
"""Snapshot files from this system into branches, as described in scripts/fork/snapshots.conf.

Each branch named there gets a commit holding exactly the listed files (a file dropped from the list, or from a
listed directory, disappears from the next snapshot), on top of its previous snapshot; a branch that doesn't exist
yet starts without history. Nothing is checked out: the files go straight into a new commit, so this can run on
any branch with any working tree.

    python3 scripts/fork/save-snapshots.py            (scripts/fork/sync.sh runs it first)
    python3 scripts/fork/save-snapshots.py --dry-run  list what each branch would hold

The list itself is read from fork/twin-app, whatever is checked out.
"""
import os
import subprocess
import sys
import tempfile

CONF = "fork/twin-app:scripts/fork/snapshots.conf"
SKIP_DIRS = {"__pycache__", ".cache", ".git"}


def git(*args, env=None, input=None):
    return subprocess.run(["git", *args], check=True, capture_output=True, text=input is None or isinstance(input, str),
                          env=env, input=input).stdout


def is_binary_program(path):
    with open(path, "rb") as f:
        return f.read(4) == b"\x7fELF"


def entries():
    """(branch, source, path in branch) for every line of the conf."""
    for line in git("show", CONF).splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        branch, source, target = line.split()
        yield branch, source, target


def files_of(source, target):
    """(path in branch, mode, bytes) for a source: a file, a directory, or git:<branch>:<path>."""
    if source.startswith("git:"):
        _, rev, path = source.split(":", 2)
        data = subprocess.run(["git", "show", f"{rev}:{path}"], check=True, capture_output=True).stdout
        mode = "100755" if path.endswith(".sh") else "100644"
        yield target, mode, data
        return
    source = os.path.expanduser(source)
    if not os.path.exists(source):
        print(f"  missing, skipped: {source}", file=sys.stderr)
        return
    if os.path.isfile(source):
        candidates = [(source, target)]
    else:
        candidates = []
        for root, dirs, names in os.walk(source):
            dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
            for name in sorted(names):
                full = os.path.join(root, name)
                candidates.append((full, os.path.join(target, os.path.relpath(full, source))))
    for full, path in candidates:
        if os.path.islink(full) or not os.path.isfile(full) or full.endswith(".pyc") or is_binary_program(full):
            continue
        mode = "100755" if os.access(full, os.X_OK) else "100644"
        with open(full, "rb") as f:
            yield path, mode, f.read()


def main():
    dry = "--dry-run" in sys.argv
    os.chdir(git("rev-parse", "--show-toplevel").strip())
    branches = {}
    for branch, source, target in entries():
        branches.setdefault(branch, []).extend(files_of(source, target))
    for branch, files in branches.items():
        if dry:
            print(f"{branch}: {len(files)} files")
            for path, _, _ in files:
                print(f"  {path}")
            continue
        with tempfile.NamedTemporaryFile() as index:
            env = dict(os.environ, GIT_INDEX_FILE=index.name)
            git("read-tree", "--empty", env=env)
            for path, mode, data in files:
                blob = subprocess.run(["git", "hash-object", "-w", "--stdin"], check=True, capture_output=True,
                                      input=data).stdout.decode().strip()
                git("update-index", "--add", "--cacheinfo", f"{mode},{blob},{path}", env=env)
            tree = git("write-tree", env=env).strip()
        old = subprocess.run(["git", "rev-parse", "--verify", "-q", f"refs/heads/{branch}"],
                             capture_output=True, text=True).stdout.strip()
        if old and git("rev-parse", f"{old}^{{tree}}").strip() == tree:
            print(f"{branch}: up to date ({len(files)} files)")
            continue
        parents = ["-p", old] if old else []
        new = git("commit-tree", tree, *parents, "-m", f"Snapshot of {branch} ({len(files)} files)").strip()
        git("update-ref", f"refs/heads/{branch}", new, *([old] if old else []))
        print(f"{branch}: saved {new[:7]} ({len(files)} files)")


if __name__ == "__main__":
    main()
