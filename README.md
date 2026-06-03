# Switch 2 Pro Controller — Bluetooth Daemon

Raw-HCI daemon that connects a Nintendo Switch 2 Pro Controller via Bluetooth Low Energy
and exposes it as a standard UHID gamepad. No BlueZ pairing needed.

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

Built on crackberry (Raspberry Pi 4 + LibreELEC) for crackbery5 (Raspberry Pi 5).
Nintendo subcommands from SDL2 source code and Nohzockt/ndeadly implementations.
