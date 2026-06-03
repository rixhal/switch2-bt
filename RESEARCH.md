# Research Log — Switch 2 Pro Controller Bluetooth

Timeline of discoveries, dead ends, and breakthroughs during the development
of a raw-HCI BLE daemon for the Nintendo Switch 2 Pro Controller (0x057E:0x2069).

## The Problem

The Switch 2 Pro Controller's Bluetooth module is **disabled by default**.
When activated, it advertises with **zero-byte scan responses** — no device name,
no service UUIDs. BlueZ's management layer filters these devices entirely.

## Timeline

### Phase 1: USB Kernel Driver (Working)
- **hid-switch2.ko** — Linux kernel driver for USB mode
- 24 buttons, 4 analog axes, force feedback (FF_RUMBLE)
- Works perfectly on LibreELEC 13 (Kernel 6.18.32)
- **Limitation:** USB-only, no wireless

### Phase 2: BlueZ Pairing (Dead End)
- Controller visible via `hcitool lescan` but **not via bluetoothctl**
- Root cause: SCAN_RSP = 0 bytes → BlueZ `mgmt` layer filters device
- Attempts to force registration (modify `/var/lib/bluetooth/`, `ReverseServiceDiscovery=false`) — no effect
- SMP Pairing: controller **disconnects immediately** on any pairing attempt
- **Conclusion:** BlueZ is a dead end. Must bypass entirely.

### Phase 3: USB-Init Discovery (Breakthrough #1)
- Reverse-engineered from **SDL2 source code** (`SDL_hidapi_switch2.c`)
- Key subcommands sent via USB HID reports:
  - `0x06` **SetHCIState** — enables the Bluetooth module in the controller
  - `0x01` **BluetoothManualPair** — forces controller into BLE advertising with host BD_ADDR
  - `0x08` SetShipmentMode — for cleanup
  - `0x00` ExitShipmentMode — wake controller
- **Critical workflow:** Send commands → **physically disconnect USB** → controller begins BLE advertising
- Controller BD_ADDR: `E0:EF:BF:3B:C6:76` (random address, power-cycle dependent)

### Phase 4: hcitool LE Connection (Breakthrough #2)
- After USB-init + disconnect → `hcitool lescan` shows controller
- `hcitool lecc E0:EF:BF:3B:C6:76` → **successful LE connection**, Handle 0x0040
- Connection survived `hcitool` process exit (Handle stays live)
- `strace` revealed exact `writev()` parameters:
  ```
  scan_interval=0x0004  scan_window=0x0004
  conn_interval_min=0x000F  conn_interval_max=0x000F
  supervision_timeout=0x0C80  (3200 × 10ms = 32s)
  min_ce_length=0x0001  max_ce_length=0x0001
  ```
- **3 separate iov buffers** in `writev()`: type byte, header, params

### Phase 5: Raw HCI Daemon Attempt (Partial Success)
- Built `sw2d.c` (829 lines) — raw socket HCI bypass
- Manual HCI command construction with `writev()` (matching strace params)
- Full GATT probing, ATT protocol, UHID gamepad creation
- **Fatal flaw:** ACL data not routed to daemon's socket
- HCI commands sent via `writev()` on raw socket don't establish **connection ownership**
- Kernel routes ACL only to the socket that "owns" the connection
- `hcitool`'s socket got ACL data; daemon's socket got nothing

### Phase 6: libbluetooth Discovery (Breakthrough #3)
- `hci_open_dev(0)` + `hci_le_create_conn()` from libbluetooth
- Test program `sw2d_lib.c` (69 lines) — **successful!**
- Connection Handle 0x0040, MTU exchange, ACL data on daemon's socket
- **Root cause confirmed:** libbluetooth's `hci_le_create_conn()` properly registers
  connection ownership with the kernel. Raw `writev()` on `socket(AF_BLUETOOTH)` does not.

### Phase 7: Merged Daemon (Final)
- Combined libbluetooth connection handshake with raw ACL/ATT I/O
- Architecture: `hci_open_dev()` → `hci_le_create_conn()` → raw `read()`/`write()` for everything else
- Full GATT probing, CCCD notification enable, UHID gamepad, input report parsing
- Reconnect loop with exponential backoff (1s → 16s)
- Cross-compiled on crackberry (RPi4) for crackbery5 (RPi5 aarch64)

## Key Technical Findings

### Controller GATT Layout
| Handle | Purpose |
|--------|---------|
| 0x000E | Input Report 0x09 (notifications) |
| 0x000F | CCCD for 0x000E (write `0x0100` to enable) |
| 0x0012 | Rumble Output |
| 0x0014 | Command Output |
| 0x0016 | Command Data |

### Input Report 0x09 Format (11+ bytes)
```
Offset  Size  Field
0       1     Report ID (0x09)
1       2     Button bitmap (24 buttons)
3       2     Left stick X (12-bit)
5       2     Left stick Y (12-bit)
7       2     Right stick X (12-bit)
9       2     Right stick Y (12-bit)
```

### UUIDs Tested
- **Nohzockt UUIDs:**
  - Service: `ab7de9be-...` (not used — we use raw GATT)
  - Input: `ab7de9be-...` (not needed — we use handle 0x000E directly)
- **ndeadly UUIDs:**
  - Input: `7492866c-...0f9` (reference)
  - Output: `7492866c-ec3e-4619-8258-32755ffcc0f8` (reference)

### HCI Filter Fiasco
- `setsockopt(SOL_HCI, HCI_FILTER)` after `hci_open_dev()` **breaks ACL routing**
- The default filter from `hci_open_dev()` is correct — never touch it
- Custom filters caused the exact bug that made raw sockets fail

### Connection Parameters
- Default LE params work, but `conn_interval=15 (18.75ms)` is slow for gaming
- Connection update requested: 7.5ms interval (0x0006 → 6 × 1.25ms)
- Controller must accept; some reject fast params for power saving

## Dead Ends

| Attempt | Why It Failed |
|---------|---------------|
| BlueZ/bluetoothctl pairing | SCAN_RSP=0 bytes → mgmt filter discards device |
| `ReverseServiceDiscovery=false` in main.conf | No effect — filtering happens before SDP |
| Manual `/var/lib/bluetooth/` cache injection | BlueZ validates via kernel; forged entries rejected |
| SMP LE Secure Connections pairing | Controller disconnects on every pairing request |
| Raw `socket(AF_BLUETOOTH)` + `writev()` HCI | Kernel doesn't track connection ownership |
| L2CAP sockets (`socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP)`) | Needs BlueZ connection management — same filter problem |
| `hcitool lecc` as connection anchor | ACL data goes to hcitool's socket, not daemon's |

## Build Environment

```
Host:    crackberry (Raspberry Pi 4, Debian)
Target:  crackbery5 (Raspberry Pi 5, LibreELEC 13 Nightly)
Kernel:  6.18.32 (target), 6.12.87 (host)
Arch:    aarch64 (both)
Compiler: aarch64-linux-gnu-gcc
Deps:    libbluetooth-dev (bluez-libs)
```

## Credits

- **SDL2** — `SDL_hidapi_switch2.c` for USB subcommand discovery
- **Nohzockt** — Switch 2 Pro Controller research, GATT UUIDs, report format
- **ndeadly** — MissionControl Switch controller research
- **BlueZ** — `hcilecreateconn` test program validated libbluetooth approach
- **Claude** — Architectural review confirming raw I/O + libbluetooth handshake design
- **Codex** — Built sw2d.c and sw2d_final.c

## Related Files

| File | Description |
|------|-------------|
| `sw2d_final.c` | Production daemon (753 lines) |
| `sw2d_lib.c` | Proof of concept (69 lines) |
| `sw2d.c` | Raw socket iteration (829 lines, archived) |
| `hciletest.c` | libbluetooth connection test |
| `acltest.c` | ACL data reception test |
| `hciraw*.c` | HCI filter and event research |
| `gattdump.c` | Service/characteristic discovery |
| `switch2-bt.c` | USB kernel driver |
| `switch2-bt-ff.c` | Force feedback kernel module |
