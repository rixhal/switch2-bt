#!/usr/bin/env bash
# os-preflight.sh — Prepare Linux Bluetooth + uinput for switch2d.py hardware test
#
# Usage:   sudo ./scripts/os-preflight.sh
# Purpose: Bring BT adapter and /dev/uinput into a known-good state before
#          running the golden-run diagnostic. Never fails hard — optional
#          tools (rfkill, bluetoothctl, hciconfig) are skipped if missing.
#
# This is an OS-only helper. It does NOT modify the Bluetooth architecture,
# raw-HCI, BTstack, kernel code, or protocol logic.

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
NC='\033[0m' # No Color

ok()   { echo -e "  ${GREEN}✓${NC} $*"; }
warn() { echo -e "  ${YELLOW}⚠${NC} $*"; }
fail() { echo -e "  ${RED}✗${NC} $*"; }
step() { echo -e "\n${BOLD}[$1/7]${NC} $2"; }

echo "=== Switch 2 Pro Controller — OS Preflight ==="
echo "Host: $(hostname) | Kernel: $(uname -r) | $(date)"

# ── [1/7] rfkill unblock bluetooth ────────────────────────────────

step 1 "rfkill unblock bluetooth"
if command -v rfkill &>/dev/null; then
    if rfkill list bluetooth | grep -q "Soft blocked: yes"; then
        rfkill unblock bluetooth && ok "Bluetooth unblocked via rfkill"
    else
        ok "Bluetooth not soft-blocked"
    fi
    echo "  rfkill status:"
    rfkill list bluetooth 2>/dev/null | sed 's/^/    /' || true
else
    warn "rfkill not found — skipping unblock check"
fi

# ── [2/7] restart bluetooth service ───────────────────────────────

step 2 "restart bluetooth service"
if command -v systemctl &>/dev/null && systemctl is-active --quiet bluetooth 2>/dev/null; then
    systemctl restart bluetooth && ok "bluetooth.service restarted"
elif command -v systemctl &>/dev/null; then
    if systemctl start bluetooth 2>/dev/null; then
        ok "bluetooth.service started"
    else
        warn "bluetooth.service could not be started"
    fi
elif [ -f /storage/.kodi/userdata/addon_data/service.system.docker/service.system.docker ]; then
    # LibreELEC — bluetoothctl auto-starts bluetoothd via D-Bus activation
    ok "LibreELEC detected — bluetoothd auto-managed via D-Bus"
else
    warn "systemctl not found — cannot manage bluetooth.service"
fi

# ── [3/7] bluetoothctl power on ───────────────────────────────────

step 3 "bluetoothctl power on"
if command -v bluetoothctl &>/dev/null; then
    if bluetoothctl show | grep -q "Powered: yes"; then
        ok "Bluetooth adapter already powered on"
    else
        if bluetoothctl power on 2>/dev/null; then
            ok "Bluetooth adapter powered on"
        else
            warn "bluetoothctl power on failed — check hci0 state"
        fi
    fi
else
    warn "bluetoothctl not found — skipping power check"
fi

# ── [4/7] load uinput module ──────────────────────────────────────

step 4 "load uinput module"
if lsmod 2>/dev/null | grep -q "^uinput"; then
    ok "uinput module already loaded"
elif command -v modprobe &>/dev/null; then
    if modprobe uinput 2>/dev/null; then
        ok "uinput module loaded"
    else
        warn "modprobe uinput failed — /dev/uinput may be unavailable"
    fi
else
    warn "modprobe not found — cannot load uinput"
fi

# ── [5/7] show adapter info ───────────────────────────────────────

step 5 "show adapter info"
if command -v hciconfig &>/dev/null; then
    hciconfig -a 2>/dev/null | sed 's/^/  /' || warn "hciconfig -a returned no output"
elif command -v bluetoothctl &>/dev/null; then
    echo "  (using bluetoothctl instead of hciconfig)"
    bluetoothctl show 2>/dev/null | sed 's/^/  /' || warn "bluetoothctl show returned no output"
else
    warn "neither hciconfig nor bluetoothctl found — cannot show adapter info"
fi

# ── [6/7] show /dev/uinput status ──────────────────────────────────

step 6 "show /dev/uinput status"
if [ -c /dev/uinput ]; then
    ls -l /dev/uinput 2>/dev/null | sed 's/^/  /'
    ok "/dev/uinput exists"
else
    fail "/dev/uinput NOT found — run: sudo modprobe uinput"
fi

# ── [7/7] ready ───────────────────────────────────────────────────

step 7 "ready for controller sync mode"
echo ""
echo -e "  ${BOLD}Controller preparation:${NC}"
echo "    1. Hold the SYNC button (top edge, recessed) for 3-5 seconds"
echo "    2. Home-button LEDs should start cycling/blinking"
echo "    3. Controller is now advertising BLE"
echo ""
echo -e "  ${BOLD}Golden-run command:${NC}"
echo ""
echo "    sudo python3 switch2d.py \\"
echo "      --diagnose \\"
echo "      --profile auto \\"
echo "      --uinput \\"
echo "      --dump-jsonl golden_run.jsonl \\"
echo "      --verbose"
echo ""
echo -e "  ${BOLD}To capture raw HCI for debugging:${NC}"
echo "    ./scripts/capture-btmon.sh  # (in another terminal, before the run)"
echo ""
echo "=== OS preflight complete ==="
