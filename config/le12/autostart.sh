#!/bin/sh
# LE12 autostart — clean, no LE13 cruft
rfkill unblock wlan

# Tailscale
TAILSCALE_BIN=/storage/tailscale
TAILSCALED_BIN=/storage/tailscaled
STATE_DIR=/storage/.config/tailscale
mkdir -p $STATE_DIR/logs
if ! pgrep -f tailscaled > /dev/null 2>&1; then
  $TAILSCALED_BIN \
    --state=$STATE_DIR/tailscaled.state \
    --socket=$STATE_DIR/tailscaled.sock \
    --port=41641 \
    > $STATE_DIR/logs/tailscaled.log 2>&1 &
  for i in $(seq 1 10); do
    [ -S $STATE_DIR/tailscaled.sock ] && break
    sleep 1
  done
fi
