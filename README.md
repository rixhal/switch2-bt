# Switch 2 Pro Controller — Bluetooth Daemon

> ⚠️ **STATUS: NOT WORKING (Wireless BLE)** — June 2026  
> USB kernel driver works. Wireless BLE connection **does not** work yet on
> Linux. The Cypress CYW43455 chip on Raspberry Pi blocks raw HCI LE Create
> Connection. BlueZ MGMT layer filters zero-byte scan responses. BTstack
> bypasses both but the controller rejects standard BLE SMP pairing.
>
> **Working paths (on Windows):** [joycon2cpp](https://github.com/TheFrano/joycon2cpp)
> by TheFrano successfully connects wirelessly via Windows BLE stack + CareyScott
> GATT protocol. We're porting this approach to Linux.
>
> See [RESEARCH.md](RESEARCH.md) for full timeline of dead ends and breakthroughs.

Raw-HCI / BlueZ-GATT research daemon for connecting a Nintendo Switch 2 Pro
Controller via Bluetooth Low Energy and exposing it as a standard Linux gamepad.

## The Problem

The Switch 2 Pro Controller (VID 0x057E, PID 0x2069) advertises via BLE with **zero-byte
scan responses** — no device name, no service UUIDs. BlueZ's management layer filters
these out entirely. Additionally, the controller's Bluetooth module is **disabled by
default** and requires USB-based activation commands before it will advertise at all.

## The Solution

**Raw HCI bypass** — talk directly to the Bluetooth controller socket, bypassing BlueZ
entirely:

1. **USB prep phase** — send `SetHCIState` (0x06) + `BluetoothManualPair` (0x01)
   subcommands via `/dev/hidraw0`, then physically disconnect USB
2. **LE Connection** — `hci_le_create_conn()` via libbluetooth to establish the link
3. **GATT probing** — discover services, characteristics, enable notifications
4. **UHID passthrough** — parse Nintendo input report 0x09, convert to standard HID

## Architecture

```
┌─────────────────────────────────────────┐
│  UHID input reports (gamepad emulation)  │
│  report09_to_uhid() parsing              │
├─────────────────────────────────────────┤
│  GATT layer: ATT protocol               │
│  MTU exchange, service/char discovery   │
│  CCCD notification enable (handle 0x000F)│
├─────────────────────────────────────────┤
│  ACL transport: raw read()/write() on fd │
│  HCI_ACLDATA_PKT framing, L2CAP header  │
├─────────────────────────────────────────┤
│  Connection: libbluetooth               │
│  hci_open_dev(0) → fd                   │
│  hci_le_create_conn() → handle          │
├─────────────────────────────────────────┤
│  Session: reconnect loop, USB prep       │
└─────────────────────────────────────────┘
```

## Key Design Decisions

| Decision | Choice | Why |
|---|---|---|
| HCI socket | `hci_open_dev(0)` | Kernel tracks connection ownership → ACL data routes correctly |
| ACL I/O | Raw `read()`/`write()` on same fd | Zero overhead, proven ATT framing, no libbluetooth buffering conflicts |
| HCI filter | Never touched after open | Default filter passes ACL + events; changing it breaks ACL routing |
| SMP pairing | Not initiated | Controller disconnects on pairing; raw GATT works without bonding |
| Connection params | scan_int=4, scan_win=4, int_min=15, int_max=15, timeout=3200 | Replicated from `hcitool lecc` strace — what actually works |

## Build

```bash
# Native (on Pi)
make

# Cross-compile for aarch64 (from x86_64 host)
make aarch64
```

Requires: `libbluetooth-dev` (for `<bluetooth/hci_lib.h>` and `-lbluetooth`)

## Usage

```bash
# Kill bluetoothd first (Raw HCI needs exclusive hci0 access)
systemctl stop bluetooth

# One-shot: prep controller via USB, then connect via BLE
./sw2d_final --usb-init --hidraw /dev/hidraw0

# Just connect (controller already advertising)
./sw2d_final

# Test without UHID (GATT probing only)
./sw2d_final --no-uhid
```

## Current Recommended Wireless Probe

The Windows `joycon2cpp` project proved a simpler wireless path: let the OS BLE
stack establish the link, then use custom GATT commands and notifications. The
Linux equivalent is now in `tools/switch2_ble_probe.py`.

```bash
python3 -m pip install bleak evdev

# Hold the controller Sync button until the LEDs blink, then run:
sudo python3 tools/switch2_ble_probe.py \
  --mode procon2 \
  --uinput \
  --scan-timeout 30 \
  --connect-retries 5 \
  --gatt-retries 10 \
  --notify-timeout 60 \
  --max-reports 200 \
  --no-sound \
  --dump-jsonl procon2_reports.jsonl
```

This scans for Nintendo manufacturer data `0x0553` with the Switch 2 prefix,
connects via BlueZ/Bleak, writes the joycon2cpp-proven Pro Controller 2 init
commands to `649d4ac9-8eb7-4e6c-af44-1ea54fe5f005`, subscribes to
`ab7de9be-89fe-49ad-828f-118f09df7fd2`, and decodes long Pro Controller reports.
With `--uinput`, it also creates a virtual Linux gamepad via `/dev/uinput`.

If GATT connects but no reports arrive, retry with notification subscription
before the init writes:

```bash
sudo python3 tools/switch2_ble_probe.py \
  --mode procon2 \
  --uinput \
  --notify-before-init \
  --scan-timeout 30 \
  --connect-retries 5 \
  --gatt-retries 10 \
  --notify-timeout 60 \
  --no-sound \
  --dump-jsonl procon2_reports_notify_first.jsonl
```

Successful reports should be at least `0x3c` bytes. The decoded layout is:

| Field | Offset |
|---|---|
| Buttons | bytes `3..8` as a 48-bit bitfield |
| Left stick | bytes `10..12`, 12-bit packed X/Y |
| Right stick | bytes `13..15`, 12-bit packed X/Y |
| Accel | bytes `0x30..0x35` |
| Gyro | bytes `0x36..0x3b` |

### USB Prep Workflow

1. Connect controller via USB
2. Run `./sw2d_final --usb-init`
3. **Physically unplug** USB within 8 seconds
4. Controller begins BLE advertising
5. Daemon connects, creates UHID gamepad

The controller must be physically disconnected after USB commands — it only switches
to BLE advertising mode on USB detach.

## Controller Details

- **BD_ADDR:** `E0:EF:BF:3B:C6:76` (random address, changes on power cycle)
- **USB VID/PID:** 0x057E / 0x2069
- **GATT handles:** Input report 0x09 on 0x000E, CCCD on 0x000F
- **Input format:** 11+ byte report starting with 0x09 (buttons + 4×12-bit axes)
- **24 buttons + 4 analog axes** (2 sticks, 2 triggers via UHID)

## Files

| File | Purpose |
|---|---|
| `sw2d_final.c` | **Production daemon** — libbluetooth + raw ACL + UHID |
| `sw2d_lib.c` | Proof of concept — first working libbluetooth connection |
| `sw2d.c` | Earlier iteration — raw sockets (kept for reference) |
| `sw2rawd.c` | Initial raw HCI daemon (pre-libbluetooth) |
| `hciletest.c` | `hci_le_create_conn()` test — validated libbluetooth approach |
| `acltest.c` | ACL/ATT data reception test |
| `hciraw*.c`, `hcisend*.c`, `hcitest.c` | HCI-level research tools |
| `gattdump.c` | Service/characteristic discovery |
| `sw2acl.c` | ACL packet capture |
| `Makefile` | Build configuration |

## Credits

- **[TheFrano/joycon2cpp](https://github.com/TheFrano/joycon2cpp)** — Windows wireless proof-of-concept
  using CareyScott GATT protocol (Pro Controller 2 init sequence, report format, button masks)
- **[CareyScott/switch2controllerpc](https://github.com/CareyScott/switch2controllerpc)** — GATT pairing
  protocol reverse-engineering (SetMAC, LTK1/LTK2/Finish, UUID discovery)
- **[bluekitchen/btstack](https://github.com/bluekitchen/btstack)** — Bluetooth Host stack used for
  HCI_CHANNEL_USER bypass on CYW43455 (port/linux)
- **SDL2** — `SDL_hidapi_switch2.c` for USB subcommand discovery (SetHCIState, BluetoothManualPair)
- **[Nohzockt/BlueRetro](https://github.com/Nohzockt/Switch2-Controllers)** — Switch 2 Pro Controller
  research, GATT UUIDs, init sequence (v1.8 release)
- **[ndeadly/MissionControl](https://github.com/ndeadly/MissionControl)** — Switch controller GATT
  research, UUIDs, button mapping, report format
- **BlueZ** — `hcilecreateconn` test program validated libbluetooth connection approach
- **[Akashem06/RPI_Bluetooth](https://github.com/Akashem06/RPI_Bluetooth)** — Raw-HCI host layer on RPi4
- **[bleno#225](https://github.com/noble/bleno/issues/225)** — HCI_CHANNEL_USER usage pattern on Linux
- **Claude** — Architectural review, code audit, root cause analysis of SMP error 0x13
- **Codex** — Initial sw2d.c and sw2d_final.c implementations
