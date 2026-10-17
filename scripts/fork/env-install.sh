#!/usr/bin/env bash
# install.sh: install the Omarchy desktop for Samsung DeX from this branch: a Sway session (with Waybar, Wofi and
# Mako) inside Omalauncher's Arch Linux, shown on the DeX monitor, plus its helper tools.
#
#   ./install.sh
#
# Copies bin/ to /usr/local/bin, local/bin/ to ~/.local/bin, config/* to ~/.config/ and local/share/* to
# ~/.local/share/, keeping any file it replaces as <file>.bak, then builds the GTK tools (local/bin/*.c).
# Then start the desktop with `dex-omarchy` (see the shortcuts with omarchy-cheatsheet).
#
# Needs: pacman -S --needed sway waybar wofi mako foot python gtk3 gcc pkgconf jq
# Several tools talk to Android over adb on localhost (`droid`): turn on Wireless debugging for those.
# Waybar themes (~/.config/waybar-themes.base) aren't included; omarchy-waybar-theme lists whichever are installed.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"

copy() {  # copy <from-dir> <to-dir>: every file, keeping a .bak of anything it replaces
    local from="$1" to="$2"
    [ -d "$from" ] || return 0
    (cd "$from" && find . -type f) | while read -r f; do
        mkdir -p "$to/$(dirname "$f")"
        if [ -e "$to/$f" ] && ! cmp -s "$from/$f" "$to/$f"; then cp -p "$to/$f" "$to/$f.bak"; fi
        cp -p "$from/$f" "$to/$f"
    done
}

copy "$here/bin" /usr/local/bin
copy "$here/local/bin" "$HOME/.local/bin"
copy "$here/config" "$HOME/.config"
copy "$here/local/share" "$HOME/.local/share"

for src in "$HOME"/.local/bin/*.c; do
    [ -e "$src" ] || continue
    if grep -q "gtk/gtk.h" "$src"; then
        gcc -O2 -o "${src%.c}" "$src" $(pkg-config --cflags --libs gtk+-3.0) -lm && echo "built $(basename "${src%.c}")"
    fi
done
echo "Installed. Start the desktop on DeX with: dex-omarchy"
