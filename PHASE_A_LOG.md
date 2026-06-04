# Phase A Probe Results — 2026-06-03

## Summary

✅ **Scan successful**: Controller E0:EF:BF:3B:C6:76 found with Nintendo manufacturer 0x0553  
✅ **PID confirmed**: 0x2069 = Pro Controller 2  
✅ **Manufacturer data decode correct**: vendor=0x057E, product=0x2069
❌ **Connect failed**: BlueZ on crackberry (RPi4) kills BLE connection immediately (`le-connection-abort-by-local`)  

## Key Observations

### Manufacturer Advertisement Data
```
Hex: 01 00 03 7e 05 69 20 00 01 00 31 f8 94 55 e2 98 0f 00 00 00 00 00 00 00
Parse:
  bytes[3:5]  = 0x057E  → Nintendo Vendor ID ✅
  bytes[5:7]  = 0x2069  → Pro Controller 2 PID ✅
  bytes[10:16] = 31 f8 94 55 e2 98 → reconnect_mac field is non-zero
```

### BlueZ Interference
```
BlueZ log: "retry due to le-connection-abort-by-local"
```
The kernel/BlueZ stack on crackberry (standard Raspbian) immediately drops the BLE connection after it's established. This is the same `LE Read Remote Used Features` interference pattern seen with `sw2d_final.c` on LE.

### Controller Advertising Behavior
- Controller does NOT advertise in normal state
- Long press Sync button (recessed, top edge) → Player LEDs flash → BLE advertising starts
- Advertising stops after ~10-15 seconds if not connected
- Reconnect MAC field was non-zero in this capture; do not treat this log as
  proof of a fresh/unpaired state

## Exit Codes

| Code | Meaning | Hit? |
|------|---------|------|
| 1 | No Nintendo controller found | ✅ (first run, controller not in sync) |
| 2 | Connect failed | ✅ (BlueZ kill) |
| 3 | GATT characteristics missing | — (not reached) |
| 4 | Pair command write failed | — (not reached) |
| 5 | Notification subscription failed | — (not reached) |
| 0 | Notifications received | — (not reached) |

## Next Steps

1. **crackbery5 test**: LE has `bluetooth.disable_mgmt=1` → BlueZ shouldn't interfere
   - Need to install `bleak` on LE (pip blocked by read-only rootfs)
   - Options: pip in venv on /storage, or pre-built wheel
2. **crackberry BlueZ stop**: `systemctl stop bluetooth` before probe could work
3. **BTstack bypass**: Skip BlueZ entirely, use HCI_CHANNEL_USER like `sw2d_final.c`

## Files Created

- `tools/switch2_ble_probe.py` — Phase A probe (bleak + BlueZ backend)
- `tools/dump_reports.py` — JSONL report analyzer
- `src/switch2_protocol.h` — Protocol constants (UUIDs, PIDs, pair keys)
- `src/switch2_protocol.c` — Payload builder implementation
- `tests/test_pair_payloads.py` — 17 byte-accurate tests (all pass)

## Probe CLI
```bash
python3 tools/switch2_ble_probe.py \
  --address E0:EF:BF:3B:C6:76 \
  --manufacturer 0553 \
  --verbose \
  --scan-timeout 15 \
  --dump-jsonl reports.jsonl
```

Flags: `--no-pair` (only discover), `--pair-only` (pair + exit), `--max-reports N`
