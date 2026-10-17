#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NDK="/opt/android-sdk/ndk/28.2.13676358"
SYSROOT="$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot"
OUT="$ROOT/app/src/main/jniLibs/arm64-v8a/libwlbridge.so"
SRC="$ROOT/app/src/main/cpp/wlbridge.c"

echo "Compiling wlbridge.c for arm64-v8a..."
/usr/bin/clang -O3 -fPIC -target aarch64-linux-android29 \
  --sysroot="$SYSROOT" \
  -c "$SRC" -o /tmp/wlbridge.o

echo "Linking libwlbridge.so..."
/usr/bin/ld.lld -shared -o "$OUT" /tmp/wlbridge.o \
  -L"$SYSROOT/usr/lib/aarch64-linux-android/29" \
  -landroid -llog -lc

echo "Built $OUT successfully:"
ls -lh "$OUT"
