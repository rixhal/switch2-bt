# Hardware Test Procedure — Switch 2 Pro Controller BLE

This document is a step-by-step checklist for validating the full BLE wireless
path from controller to uinput gamepad, using `switch2d.py` on Linux (Raspberry
Pi 4/5 or any BlueZ-capable host).

**Prerequisite:** Read [README.md](../README.md) for project overview, and have
[TROUBLESHOOTING.md](TROUBLESHOOTING.md) handy if any stage fails.

---

## OS Preflight (run once before testing)

```bash
sudo ./scripts/os-preflight.sh
```

This script brings the Bluetooth adapter and `/dev/uinput` into a known-good state:

1. Unblocks Bluetooth via `rfkill`
2. Restarts `bluetooth.service`
3. Powers on the adapter via `bluetoothctl`
4. Loads the `uinput` kernel module
5. Prints adapter info (`hciconfig -a` or `bluetoothctl show`)
6. Verifies `/dev/uinput` exists
7. Prints the suggested golden-run command

**No command fails hard** — optional tools (`rfkill`, `hciconfig`) are skipped
if missing. Run this before every hardware test session.

To capture raw HCI traffic for debugging, run in a separate terminal before
starting `switch2d.py`:

```bash
sudo ./scripts/capture-btmon.sh
```

This records everything the Bluetooth adapter sends/receives to
`btmon_golden_run.log`. Ctrl+C to stop after the test.

---

## Run Modes

```
# Full diagnostic (one-shot — recommended for first verification)
sudo python3 switch2d.py --diagnose --verbose

# Daemon mode (reconnect loop, uinput gamepad)
sudo python3 switch2d.py --daemon --uinput --max-reports 0 --verbose

# Read-only (skip init, just scan & connect)
sudo python3 switch2d.py --profile macos --verbose
```

---

## 1. PREFLIGHT

Check that the host is ready before the controller is even turned on.

### Checklist

- [ ] Python 3.8+ installed (`python3 --version`)
- [ ] bleak installed (`pip show bleak` or `python3 -c "import bleak"`)
- [ ] evdev installed if using uinput (`pip show evdev` or `python3 -c "import evdev"`)
- [ ] bluetoothd running (`systemctl status bluetooth` or `ps aux | grep bluetoothd`)
- [ ] Bluetooth adapter visible (`bluetoothctl list` shows at least one controller)
- [ ] Controller charged and nearby

### Expected Output (Preflight)

```
[0000.000] ▶ preflight
[0000.012]   info msg=bleak 0.22.0
[0000.016]   info msg=evdev available
[0000.030]   info msg=Bluetooth adapter accessible
[0000.031] ✅ preflight
```

### If This Fails

| Symptom | See TROUBLESHOOTING.md |
|---------|------------------------|
| `FATAL: bleak not installed` | Install: `pip install bleak` |
| `python-evdev not installed` | Install: `pip install evdev` |
| `Bluetooth adapter not accessible` | Check bluetoothd, hci0 state |

---

## 2. SCAN

Hold the controller SYNC button (top edge, recessed) until the home button LEDs
start cycling. The controller broadcasts BLE advertisements with Nintendo
manufacturer ID `0x0553`.

### Checklist

- [ ] SYNC button held (LEDs cycling/blinking)
- [ ] Controller appears in scan within `--scan-timeout` seconds (default 10)
- [ ] BDADDR captured (e.g. `E0:EF:BF:3B:C6:76`)
- [ ] Manufacturer data decoded: VID `0x057E`, PID `0x2069`
- [ ] Reconnect MAC parsed from manufacturer data

### Expected Output

```
[0000.050] ▶ scan timeout=10.0
[0002.143]   info msg=found E0:EF:BF:3B:C6:76 rssi=-52 vid=0x057e pid=0x2069 reconnect_mac=e0:ef:bf:3b:c6:76
[0002.144] ✅ scan address=E0:EF:BF:3B:C6:76 vid=0x057e pid=0x2069
```

### If This Fails

| Symptom | See TROUBLESHOOTING.md |
|---------|------------------------|
| "No controller found in scan" | *No controller found in scan* |
| Scan timeout before any device seen | *No controller found in scan* |
| Device found but wrong VID/PID | May not be a Switch 2 Pro Controller |

---

## 3. CONNECT

`switch2d.py` establishes a BLE connection via BlueZ/bleak. This uses the
standard MGMT path — no raw HCI is required, so it works on CYW43455 (Raspberry
Pi 4/5).

### Checklist

- [ ] BLE connection established within 20 seconds
- [ ] MTU size negotiated (logged after connect)
- [ ] No `Command Disallowed` or HCI-level errors (this path bypasses raw HCI)

### Expected Output

```
[0002.200] ▶ connect address=E0:EF:BF:3B:C6:76
[0005.012] ✅ connect mtu=247 address=E0:EF:BF:3B:C6:76
```

### If This Fails

| Symptom | See TROUBLESHOOTING.md |
|---------|------------------------|
| "connect timeout" | *Connect timeout* |
| "connect failed" with exception | *Connect timeout* |
| MTU very small (e.g. 23) | May still work, but report packets may be fragmented |

---

## 4. DISCOVER

GATT service discovery walks the controller's service/characteristic tree,
looking for the three CareyScott/joycon2cpp UUIDs.

### Checklist

- [ ] GATT services discovered (non-empty list)
- [ ] Input Report UUID `ab7de9be-89fe-49ad-828f-118f09df7fd2` found (notify)
- [ ] Command Write UUID `649d4ac9-8eb7-4e6c-af44-1ea54fe5f005` found (write-without-response)
- [ ] Characteristic handles logged

### Expected Output (verbose)

```
[0005.100] ▶ discover start
[0005.250]   info msg=  char 00001800-0000-1000-8000-00805f9b34fb [read]
[0005.251]   info msg=  char 00002a00-0000-1000-8000-00805f9b34fb [read]
...
[0005.400]   info msg=  → input report: handle=36 props=notify
[0005.401]   info msg=  → command write: handle=39 props=write-without-response
[0005.402] ✅ discover services=5 chars=15 input_handle=36 cmd_handle=39
```

### If This Fails

| Symptom | See TROUBLESHOOTING.md |
|---------|------------------------|
| "no services discovered" | *GATT service discovery returns empty* |
| Input report UUID not found | *GATT service discovery returns empty* |
| Command write UUID not found | *Command write characteristic not found* |

---

## 5. INIT

Send the joycon2cpp-proven init sequence (feature-select 0x02 → 0x04 → LED →
Sound) via WriteWithoutResponse on the command write characteristic.

### Checklist

- [ ] Feature-select 0x02 acknowledged (no write error)
- [ ] Feature-select 0x04 acknowledged
- [ ] LED command sent (sets player 1)
- [ ] Sound command sent (Nintendo connect sound)

### Commands Sent (joycon2cpp profile)

```
0c 91 01 02 00 04 00 00 ff 00 00 00    # feature-select 0x02 → 500ms
0c 91 01 04 00 04 00 00 ff 00 00 00    # feature-select 0x04 → 700ms (500ms + 200ms barrier)
09 91 01 07 00 08 00 00 01 00 00 00 00 00 00 00  # LED player 1 → 50ms
0a 91 01 02 00 08 00 00 04 00 00 00 00 00 00 00  # Sound → 50ms
```

### Expected Output

```
[0005.500] ▶ init start profile=joycon2cpp
[0005.501]   info msg=  → feature-select 0x02: 0c 91 01 02 00 04 00 00 ff 00 00 00
[0005.550]   info msg=  ← feature-select 0x02: OK
[0006.050]   info msg=  → feature-select 0x04: 0c 91 01 04 00 04 00 00 ff 00 00 00
[0006.750]   info msg=  ← feature-select 0x04: OK
[0006.800]   info msg=  → set LED: 09 91 01 07 00 08 00 00 01 00 00 00 00 00 00 00
[0006.810]   info msg=  ← set LED: OK
[0006.860]   info msg=  → sound: 0a 91 01 02 00 08 00 00 04 00 00 00 00 00 00 00
[0006.870]   info msg=  ← sound: OK
[0006.871] ✅ init
```

### If This Fails

| Symptom | See TROUBLESHOOTING.md |
|---------|------------------------|
| Write fails with exception | *Command write characteristic not found* |
| `skipped profile=macos has no init` | Use `--profile joycon2cpp` for init |

---

## 6. SUBSCRIBE

Enable BLE notifications (CCCD write to `0x0002`) on the input report
characteristic. After this, the controller streams input reports automatically.

### Checklist

- [ ] `start_notify` call succeeds (no CCCD write error)
- [ ] `subscribe` stage logs `ok`

### Expected Output

```
[0005.900] ▶ subscribe start
[0005.950] ✅ subscribe handle=36 uuid=ab7de9be-89fe-49ad-828f-118f09df7fd2
```

### If This Fails

| Symptom | See TROUBLESHOOTING.md |
|---------|------------------------|
| "subscribe failed" with CCCD error | *Notification subscription fails* |
| Subscribe succeeds but no reports | *No input reports received* |

---

## 7. RUNNING

Input reports arrive as BLE notifications. `switch2d.py` decodes buttons (48-bit
bitfield) and analog sticks (12-bit packed X/Y) from 0x3C-byte reports.

### Checklist

- [ ] Reports arriving (watch `report #N` counter climb)
- [ ] Buttons decoded correctly — press each button and verify it appears
- [ ] Analog sticks decoded — move sticks to extremes, verify normalized values
- [ ] Reports arrive at consistent rate (typically 60-120 Hz)
- [ ] No unexpected disconnects

### Expected Output (verbose)

```
[0005.960] ▶ running max_reports=100
[0005.980]   report #1: none L=(0.00,0.00) R=(0.00,0.00)
[0006.015]   report #2: a,b L=(0.12,-0.45) R=(0.00,0.00)
[0006.031]   report #3: a,b,l,zr L=(0.12,-0.45) R=(-0.83,0.33)
...
```

### Quick Button Map

| Switch 2 Button | Report Label |
|-----------------|-------------|
| A | `a` |
| B | `b` |
| X | `x` |
| Y | `y` |
| L / ZL | `l` / `zl` |
| R / ZR | `r` / `zr` |
| D-Pad | `dpad_up`, `dpad_down`, `dpad_left`, `dpad_right` |
| Home | `home` |
| – (Minus) | `back` |
| + (Plus) | `start` |
| Left/Right Stick Click | `l3`, `r3` |
| GL / GR (paddles) | `gl`, `gr` |
| Capture | `screenshot` |
| C (Campus) | `c` |

### If This Fails

| Symptom | See TROUBLESHOOTING.md |
|---------|------------------------|
| No reports after subscribe | *No input reports received* |
| Disconnect after ~30s | *Controller disconnects after 30s* |
| Reports with wrong button labels | Verify controller is Pro Controller 2 (PID 0x2069) |

---

## 8. UINPUT

When `--uinput` is passed, `switch2d.py` creates a Linux uinput device at
`/dev/uinput` and maps decoded reports to standard gamepad events.

### Checklist

- [ ] `/dev/uinput` device created (check `ls -la /dev/uinput`)
- [ ] `evtest` shows events from the gamepad device
- [ ] Verify with `evtest`:

```bash
# List input devices; find "Nintendo Switch 2 Pro Controller"
sudo evtest

# Select the device number to watch live events
sudo evtest /dev/input/eventX
```

- [ ] Buttons produce `EV_KEY` events with value 1 (press) / 0 (release)
- [ ] Analog sticks produce `EV_ABS` events (ABS_X, ABS_Y, ABS_RX, ABS_RY)

### Expected Output (stage log)

```
[0005.870] ✅ uinput name=Nintendo Switch 2 Pro Controller
```

### Expected evtest Output

```
Event: time 1686001234.567890, type 1 (EV_KEY), code 305 (BTN_EAST), value 1   # A pressed
Event: time 1686001234.567890, -------------- SYN_REPORT ------------
Event: time 1686001234.584210, type 3 (EV_ABS), code 0 (ABS_X), value -8192    # Left stick X
Event: time 1686001234.584210, type 3 (EV_ABS), code 1 (ABS_Y), value 16384    # Left stick Y
Event: time 1686001234.584210, -------------- SYN_REPORT ------------
```

### If This Fails

| Symptom | See TROUBLESHOOTING.md |
|---------|------------------------|
| "uinput setup failed" | *uinput device not created* |
| `Permission denied` on `/dev/uinput` | *Permission denied on /dev/uinput* |
| evtest doesn't see device | Check `modprobe uinput`, run as root, verify udev |

---

## Diagnostic Run Quick Reference

```bash
# One-liner full validation
sudo python3 switch2d.py --diagnose --verbose

# Expected exit codes:
#   0  — All stages passed, reports received
#  10  — Preflight failed (missing deps or permissions)
#  11  — Scan failed (no controller found)
#  12  — Connect failed
#  13  — GATT discover failed (missing characteristics)
#  14  — Init failed (feature-select writes failed)
#  15  — Subscribe or no reports
#  16  — Disconnected unexpectedly
#  99  — Fatal error (unhandled exception)
```

---

## Checkpoint Summary

After a successful test, you should have seen:

1. ✅ Preflight: bleak + evdev + bluetoothd all available
2. ✅ Scan: Controller BDADDR with VID 0x057E, PID 0x2069
3. ✅ Connect: BLE connection with MTU ≥247
4. ✅ Discover: Input report + command write handles found
5. ✅ Init: Both feature-selects acknowledged
6. ✅ Subscribe: Notifications enabled on input report UUID
7. ✅ Running: Input reports decoded (buttons + sticks)
8. ✅ Uinput: `/dev/uinput` gamepad producing evdev events

If all checkpoints pass, the BLE wireless path is fully operational.
