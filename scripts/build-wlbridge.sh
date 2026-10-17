#!/usr/bin/env bash
# Builds the Wayland bridge (app/src/main/cpp/wlbridge.c) into the ignored app/src/main/jniLibs, on an ARM64
# Linux host where CMake's NDK toolchain (x86-64 only) can't run: the host's clang and lld, against the sysroot
# of the NDK upstream pins in scripts/fetch-proot.sh. Skips the build when wlbridge.c hasn't changed since the
# last one (a hash next to the .so); --force builds anyway.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NDK_VERSION=$(grep -m1 '^NDK_VERSION=' "$ROOT/scripts/fetch-proot.sh" | cut -d= -f2)
NDK="${ANDROID_NDK:-/opt/android-sdk/ndk/$NDK_VERSION}"
SYSROOT="$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot"
SRC="$ROOT/app/src/main/cpp/wlbridge.c"
OUT="$ROOT/app/src/main/jniLibs/arm64-v8a/libwlbridge.so"
STAMP="$OUT.src-sha256"

hash=$(sha256sum "$SRC" | cut -d' ' -f1)
if [ "${1:-}" != "--force" ] && [ -f "$OUT" ] && [ "$(cat "$STAMP" 2>/dev/null)" = "$hash" ]; then
  echo "libwlbridge.so is up to date with wlbridge.c"
  exit 0
fi
[ -d "$SYSROOT" ] || { echo "No NDK sysroot at $SYSROOT (NDK $NDK_VERSION, from scripts/fetch-proot.sh)." >&2; exit 1; }
command -v clang >/dev/null && command -v ld.lld >/dev/null || {
  echo "wlbridge.c changed and needs clang and lld: pacman -S --needed clang lld" >&2; exit 1; }

echo "Compiling wlbridge.c for arm64-v8a (NDK $NDK_VERSION sysroot)..."
obj=$(mktemp --suffix=.o)
clang -O3 -fPIC -target aarch64-linux-android29 --sysroot="$SYSROOT" -c "$SRC" -o "$obj"
mkdir -p "$(dirname "$OUT")"
ld.lld -shared -o "$OUT" "$obj" -L"$SYSROOT/usr/lib/aarch64-linux-android/29" -landroid -llog -lc
rm -f "$obj"
echo "$hash" > "$STAMP"
ls -lh "$OUT"
