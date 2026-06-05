#!/usr/bin/env bash
# capture-btmon.sh — Record btmon HCI trace for hardware golden-run debugging
#
# Usage:   sudo ./scripts/capture-btmon.sh [output_file]
#          (run in a separate terminal BEFORE starting switch2d.py)
#
# Default output: btmon_golden_run.log in current directory.

set -euo pipefail

OUTPUT="${1:-btmon_golden_run.log}"

if ! command -v btmon &>/dev/null; then
    echo "btmon not found."
    echo ""
    echo "Install:"
    echo "  Debian/Ubuntu/Raspberry Pi OS:  sudo apt install bluez"
    echo "  LibreELEC:                      btmon is included in the base image"
    echo ""
    exit 1
fi

echo "=== btmon HCI trace capture ==="
echo "Output:  $OUTPUT"
echo "Stop:    Ctrl+C when golden run is complete"
echo ""

exec btmon -w "$OUTPUT"
