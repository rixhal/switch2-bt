#!/usr/bin/env bash
set -euo pipefail

# --- Parse flags ---
SYSTEMD=false
INSTALL_DIR="/opt/switch2d"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --systemd) SYSTEMD=true; shift ;;
        --install-dir) INSTALL_DIR="$2"; shift 2 ;;
        *) echo "Unknown flag: $1"; exit 1 ;;
    esac
done

echo "=== Switch 2 Pro Controller Daemon Installer ==="

# --- 1. Check Python 3.10+ ---
echo "[1/4] Checking Python version..."
PYTHON_BIN=""
for candidate in python3.13 python3.12 python3.11 python3.10 python3; do
    if command -v "$candidate" &>/dev/null; then
        ver=$("$candidate" -c 'import sys; print(".".join(map(str, sys.version_info[:2])))')
        major=$(echo "$ver" | cut -d. -f1)
        minor=$(echo "$ver" | cut -d. -f2)
        if [[ "$major" -eq 3 && "$minor" -ge 10 ]]; then
            PYTHON_BIN="$candidate"
            break
        fi
    fi
done

if [[ -z "$PYTHON_BIN" ]]; then
    echo "ERROR: Python 3.10 or newer is required but was not found."
    echo "       Install it with: sudo apt install python3.11 (or similar)"
    exit 1
fi
echo "  Found: $PYTHON_BIN ($ver)"

# --- 2. Install pip dependencies ---
echo "[2/4] Installing Python dependencies..."
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
"$PYTHON_BIN" -m pip install --upgrade pip
"$PYTHON_BIN" -m pip install -r "$SCRIPT_DIR/requirements.txt"

# --- 3. Check for /dev/uinput ---
echo "[3/4] Checking /dev/uinput..."
if [[ ! -c /dev/uinput ]]; then
    echo "  WARNING: /dev/uinput not found."
    echo "  Run: sudo modprobe uinput"
    echo "  To make it permanent: echo 'uinput' | sudo tee /etc/modules-load.d/uinput.conf"
else
    echo "  /dev/uinput is available."
fi

# --- 4. systemd install (optional) ---
if $SYSTEMD; then
    echo "[4/4] Installing systemd service..."
    if [[ ! -d /etc/systemd/system ]]; then
        echo "  ERROR: /etc/systemd/system not found — systemd not available?"
        exit 1
    fi

    # Ensure install directory exists
    sudo mkdir -p "$INSTALL_DIR"

    # Copy daemon script if it exists alongside install.sh
    if [[ -f "$SCRIPT_DIR/switch2d.py" ]]; then
        sudo cp "$SCRIPT_DIR/switch2d.py" "$INSTALL_DIR/switch2d.py"
        echo "  Copied switch2d.py to $INSTALL_DIR/"
    else
        echo "  WARNING: switch2d.py not found next to install.sh — make sure the daemon is at $INSTALL_DIR/switch2d.py"
    fi

    sudo cp "$SCRIPT_DIR/systemd/switch2d.service" /etc/systemd/system/switch2d.service
    sudo systemctl daemon-reload
    sudo systemctl enable switch2d.service
    sudo systemctl start switch2d.service
    echo "  Service installed, enabled, and started."
    echo "  Check status: sudo systemctl status switch2d"
else
    echo "[4/4] Skipping systemd (use --systemd to install the service)."
fi

echo "=== Install complete ==="
