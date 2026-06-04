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

### Phase 8: Peer Address Type Fix (Fixed)
- Claude review identified `peer_bdaddr_type=public` mismatch with random-address controller
- Added `--peer-addr-type public|random|auto`, `--auto-scan`, `--bdaddr` CLI flags
- `scan_for_peer()` parses LE Advertising Report to detect actual address type
- **Result:** Both `public` and `random` connect successfully (Handle 0x0040)
- Controller address changes per power cycle: `68:47:2E:..`, `16:34:39:..`, `E0:EF:BF:..`

### Phase 9: GATT Timeout Bug (Fixed)
- `read_full()` in `hci_read_packet()` blocked forever on partial HCI events
- Kernel delivered 8/62 bytes (plen=0x3E) — `read_full` hung waiting for remaining 54
- **Fix:** Replace `read_full` with `poll()+read()`, accept partial reads
- `read_acl_att()` added deadline-based total timeout (reset per-poll was infinite loop)
- GATT probing now gracefully times out after 2s per request
- **Result:** MTU exchange fails but daemon reconnects cleanly

### Phase 10: Kernel mgmt Interference (Fixed)
- **btmon discovery:** Kernel sends `LE Read Remote Used Features` (#5) simultaneously with daemon's ATT MTU request (#4), causing controller disconnect (0x3E)
- **Root cause:** `hci_open_dev()` uses `HCI_CHANNEL_RAW` — kernel mgmt shares the connection
- **Fix:** `hci_user_open()` with `HCI_CHANNEL_USER` (exclusive access) + `hci_le_create_conn_raw()` (manual LE Create Connection via `writev`)
- **Remaining:** Controller still drops connection before ATT — needs protocol analysis
- Reconnect loop: ✅ 1s → 2s → 4s → 8s backoff, handles disconnects gracefully

### Phase 11: HCI_CHANNEL_USER Kernel Routing Bug (2026-06-04)

- **btmon confirmed:** Connection Complete events are visible on HCI_CHANNEL_MONITOR
  but never delivered to HCI_CHANNEL_USER socket on Kernel 6.12.56 (Cypress CYW43455)
- Advertising Reports arrive correctly — only connection-level events are affected
- `setsockopt(HCI_FILTER)` returns `EBADF` on HCI_CHANNEL_USER (filters are
  HCI_CHANNEL_RAW-only)
- `ioctl(HCIDEVUP)` returns `EBADF` on HCI_CHANNEL_USER (uses separate control path)
- **Workaround:** `system("hciconfig hci0 up")` after bind brings the device up
- bind sequence: `hciconfig hci0 down` → `bind(HCI_CHANNEL_USER)` → `hciconfig hci0 up`
  (from **bleno#225** — sandeepmistry recommendation)
- Scan interference: `hciconfig hci0 up` triggers lingering scan; `LE Set Scan Enable`
  (disable) must be sent before `LE Create Connection`
- Connection parameter tuning: 50-70ms interval + 20s timeout (matching working
  bleno#225 btmon trace); original 18.75ms was too aggressive
- **Root cause:** Kernel 6.12.56 mgmt layer claims hci0 at boot; even with
  `bluetooth.service` masked, the in-kernel mgmt holds a reference that blocks
  HCI_CHANNEL_USER event delivery
- **Solution:** Kernel parameter `bluetooth.disable_mgmt=1` in `/flash/cmdline.txt`
  disables the in-kernel mgmt layer, allowing HCI_CHANNEL_RAW to operate without
  `LE Read Remote Used Features` interference
- Reboot required for kernel parameter to take effect

### Phase 12: LE Connection Complete Peer Mismatch (Current Blocker)

- With corrected HCI event parsing, `LE Connection Complete` can now be decoded
  reliably from the raw payload
- Latest failing trace:
  ```
  LE_CONN_COMPLETE st=0xff handle=0x007f
  raw: 01 ff 7f 00 00 db 34 b6 d7 82 de 1b 43 f4 01 00 00 00 00
  ```
- Earlier notes decoded the event peer one byte off. Per HCI LE Connection
  Complete offsets, byte 5 is `Peer_Address_Type`; the peer address starts at
  byte 6. The event peer from this failed event is `1B:DE:82:D7:B6:34`, and
  `Peer_Address_Type=0xdb` is invalid. Because `status != 0`, the peer fields
  should be treated as diagnostic only, not as a trustworthy remote identity.
- `E0:...` has top bits `11`, so it looks like a static random address; the
  address type from the advertising report must be used for LE Create
  Connection.
- Current hypothesis: stale rotated BDADDR, wrong peer address type, or expired
  advertising window
- Debug rule for next hardware run: use `--auto-scan --peer-addr-type auto` so
  scan and connect use the same HCI_CHANNEL_USER socket and the current
  advertising address/type.
- Code now logs the peer address and peer type from `LE Connection Complete` and
  warns when the event peer differs from the requested peer

### Phase 13: Debug Surface Reduction (2026-06-04)

- Weakness found: `sw2d_final.c` briefly used libbluetooth/BlueZ RAW scanning
  before switching back to HCI_CHANNEL_USER for connection. On CYW43455 this can
  leave controller state behind and undermines the BlueZ-bypass architecture.
- Fix: auto-scan now sends LE Set Scan Parameters/Enable over the same
  HCI_CHANNEL_USER socket used for LE Create Connection.
- Weakness found: synthetic advertising tests did not cover the real Switch 2
  Pro manufacturer payload.
- Fix: `test_adv_parse` now checks the Phase A payload (`0x0553`, `0x057E`,
  `0x2069`) and rejects other Nintendo PIDs.
- Remaining open question: after a clean raw-HCI connection, determine whether
  reports require Bluetooth SMP, vendor-specific GATT pairing (`0x15` sequence),
  or both in a specific order.

### External References

- **[bleno#225](https://github.com/noble/bleno/issues/225):** HCI_CHANNEL_USER usage
  pattern, btmon traces of successful BLE bonding, SMP flow analysis
- **[NXP SMP Pairing](https://community.nxp.com/t5/Wireless-MCU/Bluetooth-Low-Energy-SMP-Pairing/m-p/376931):**
  BLE SMP protocol reference — confirms normal connection flow (no
  `LE Read Remote Used Features` in standard pairing)
- **[RPI_Bluetooth](https://github.com/Akashem06/RPI_Bluetooth):** Validates raw-HCI
  host layer approach on Raspberry Pi 4

## Operational Notes

- **bluetoothd must be stopped** before running the daemon. Raw HCI requires
  exclusive access to `hci0`. Run `systemctl stop bluetooth` first.
- Controller BD_ADDR is **random per power cycle**. `E0:EF:BF:3B:C6:76` was the
  address during development. The daemon accepts BDADDR as a CLI argument, but
  `--auto-scan --peer-addr-type auto` is preferred before each hardware test.
- ADV_IND packets contain Nintendo Manufacturer Data (`0x0553`) and Flags `0x06`.
  This was confirmed via `btmon` capture during Phase 4 testing.

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
