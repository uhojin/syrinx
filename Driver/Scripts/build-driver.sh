#!/bin/bash
# Compiles SyrinxLoopback.c into a loadable HAL bundle and assembles
# SyrinxLoopback.driver. Not built through SPM/Xcode: a HAL plugin is a
# raw Mach-O bundle (MH_BUNDLE, not MH_DYLIB), which needs a direct
# clang -bundle invocation.
set -euo pipefail
cd "$(dirname "$0")/.."

DRIVER_NAME="SyrinxLoopback"
BUNDLE="$DRIVER_NAME.driver"
BUILD_DIR=".build"

mkdir -p "$BUILD_DIR"

echo "==> compiling $DRIVER_NAME.c"
clang \
    -arch arm64 -arch x86_64 \
    -mmacosx-version-min=13.0 \
    -bundle \
    -O2 -g \
    -Wall -Wextra \
    -framework CoreAudio \
    -framework CoreFoundation \
    -o "$BUILD_DIR/$DRIVER_NAME" \
    "$DRIVER_NAME.c"

echo "==> assembling $BUNDLE"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/Contents/MacOS"
cp "$BUILD_DIR/$DRIVER_NAME" "$BUNDLE/Contents/MacOS/$DRIVER_NAME"
cp "Info.plist" "$BUNDLE/Contents/Info.plist"

echo "==> codesign (ad-hoc)"
codesign --force --sign - "$BUNDLE"

echo "==> built $BUNDLE"
