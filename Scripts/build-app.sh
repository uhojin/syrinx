#!/bin/bash
# Builds the SPM executable and wraps it into a real, ad-hoc-signed
# Syrinx.app bundle (no Xcode project needed).
set -euo pipefail
cd "$(dirname "$0")/.."

CONFIG="${1:-release}"
APP_NAME="Syrinx"
BUILD_DIR=".build/$CONFIG"

# Persisted, auto-incrementing build number (CFBundleVersion) — bumped
# on every release build so it's always exact, no manual editing.
# Seeded at 60 as a rough estimate of prior iterations before this
# counter existed; exact history before that point isn't tracked.
BUILD_NUMBER_FILE=".build-number"
if [ "$CONFIG" = "release" ]; then
    PREVIOUS_BUILD_NUMBER=$(cat "$BUILD_NUMBER_FILE" 2>/dev/null || echo 59)
    BUILD_NUMBER=$((PREVIOUS_BUILD_NUMBER + 1))
    echo "$BUILD_NUMBER" > "$BUILD_NUMBER_FILE"
else
    BUILD_NUMBER=$(cat "$BUILD_NUMBER_FILE" 2>/dev/null || echo 60)
fi
echo "==> build number: $BUILD_NUMBER"

echo "==> swift build -c $CONFIG"
swift build -c "$CONFIG"

APP_BUNDLE="$APP_NAME.app"
rm -rf "$APP_BUNDLE"
mkdir -p "$APP_BUNDLE/Contents/MacOS"
mkdir -p "$APP_BUNDLE/Contents/Resources"

cp "$BUILD_DIR/$APP_NAME" "$APP_BUNDLE/Contents/MacOS/$APP_NAME"
cp "Resources/Info.plist" "$APP_BUNDLE/Contents/Info.plist"
plutil -replace CFBundleVersion -string "$BUILD_NUMBER" "$APP_BUNDLE/Contents/Info.plist"

if [ -f "Resources/AppIcon.icns" ]; then
    echo "==> bundling AppIcon.icns"
    cp "Resources/AppIcon.icns" "$APP_BUNDLE/Contents/Resources/AppIcon.icns"
else
    echo "==> no Resources/AppIcon.icns yet — app will use the generic icon"
fi

echo "==> bundling Driver/ (source + build/install scripts) as a resource"
mkdir -p "$APP_BUNDLE/Contents/Resources/Driver"
cp -R Driver/SyrinxLoopback.c Driver/Info.plist Driver/Scripts "$APP_BUNDLE/Contents/Resources/Driver/"

echo "==> codesign (ad-hoc)"
codesign --force --deep --sign - "$APP_BUNDLE"

echo "==> built $APP_BUNDLE"
