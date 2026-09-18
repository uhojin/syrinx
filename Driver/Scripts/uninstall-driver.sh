#!/bin/bash
# Removes SyrinxLoopback.driver from /Library/Audio/Plug-Ins/HAL/ and
# restarts coreaudiod. Safe to run any time, including if the driver
# was never approved/loaded.
set -euo pipefail

DEST="/Library/Audio/Plug-Ins/HAL/SyrinxLoopback.driver"

if [ ! -d "$DEST" ]; then
    echo "SyrinxLoopback.driver is not installed. Nothing to do."
    exit 0
fi

echo "Removing $DEST ..."
echo "(this restarts coreaudiod — expect a brief, harmless audio blip)"

osascript <<OSA
do shell script "rm -rf '$DEST' && killall coreaudiod || true" with administrator privileges with prompt "Syrinx wants to remove its virtual loopback audio driver."
OSA

echo "==> uninstalled. A reboot fully clears any trace from CoreAudio,"
echo "    but the running system should stop listing the device immediately."
