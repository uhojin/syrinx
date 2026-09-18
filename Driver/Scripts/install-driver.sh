#!/bin/bash
# Installs SyrinxLoopback.driver into /Library/Audio/Plug-Ins/HAL/ and
# restarts coreaudiod so it picks up the new plugin. Requires admin
# privileges (prompts via a native macOS authorization dialog) since
# that directory is root-owned system territory.
set -euo pipefail
cd "$(dirname "$0")/.."

DRIVER_NAME="SyrinxLoopback.driver"
SRC="$(pwd)/$DRIVER_NAME"
DEST="/Library/Audio/Plug-Ins/HAL/$DRIVER_NAME"

if [ ! -d "$SRC" ]; then
    echo "error: $SRC not found — run Scripts/build-driver.sh first" >&2
    exit 1
fi

echo "Installing $DRIVER_NAME to /Library/Audio/Plug-Ins/HAL/ ..."
echo "(this restarts coreaudiod — expect a brief, harmless audio blip)"

osascript <<OSA
do shell script "rm -rf '$DEST' && cp -R '$SRC' '$DEST' && chown -R root:wheel '$DEST' && chmod -R 755 '$DEST' && killall coreaudiod || true" with administrator privileges with prompt "Syrinx wants to install its virtual loopback audio driver."
OSA

echo "==> installed."
echo "    If System Settings > Privacy & Security shows a blocked system"
echo "    extension banner, click Allow there, then reboot — the driver"
echo "    only becomes selectable after that reboot."
