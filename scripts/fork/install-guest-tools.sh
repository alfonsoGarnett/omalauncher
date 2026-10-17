#!/usr/bin/env bash
# Installs the guest tools that ship with the launcher's feature branches (guest/ in this checkout, normally
# local/build) into this Linux system: guest/bin to /usr/local/bin, guest/share to ~/.local/share. A tool with a
# requirements.txt gets a Python venv next to it. scripts/fork/sync.sh runs this after building.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
[ -d guest ] || { echo "no guest/ in this checkout"; exit 0; }
for f in guest/bin/*; do
    install -m 755 "$f" /usr/local/bin/
    echo "installed /usr/local/bin/$(basename "$f")"
done
if [ -d guest/share ]; then
    cp -r guest/share/. "$HOME/.local/share/"
    find guest/share -name requirements.txt | while read -r req; do
        dir="$HOME/.local/share/$(dirname "${req#guest/share/}")"
        [ -x "$dir/venv/bin/python" ] || python3 -m venv "$dir/venv"
        "$dir/venv/bin/pip" install -q -r "$req"
        echo "venv ready in $dir/venv"
    done
fi
