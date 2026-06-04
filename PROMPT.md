# Switch 2 Pro Controller Raw-HCI Daemon — Build Prompt

## Goal
Build a production-quality C daemon (`sw2d`) that connects a Nintendo Switch 2 Pro Controller to a Raspberry Pi 5 over BLE, bypassing BlueZ entirely, and exposes it as a standard Linux gamepad via UHID.

## Target Platform
- Raspberry Pi 5, LibreELEC 13 Nightly, kernel 6.18.32
- Cypress CYW43455 Bluetooth chip
- BlueZ/bluetoothd CANNOT be used (kernel MGMT layer drops the device)
- L2CAP sockets (AF_BLUETOOTH/SOCK_SEQPACKET) fail with EINVAL
- The ONLY working approach: raw HCI socket + ACL data
- The daemon MUST run as root (needs raw HCI and /dev/uhid)

## Hardware Info
- Controller BD_ADDR: E0:EF:BF:3B:C6:76 (Public address type)
- Controller VID: 0x057E, PID: 0x2069
- Pi5 BD_ADDR: D8:3A:DD:E5:69:ED (hci0)

## Critical: HCI Filter (EXACT 16 bytes — verified working via strace of hcitool)
```c
uint8_t hci_filter[16] = {
    0x10, 0x00, 0x00, 0x00,  // type_mask = HCI_EVENT_PKT
    0x02, 0xC0, 0x00, 0x00,  // event_mask[0] = events 0x01, 0x0E, 0x0F
    0x00, 0x00, 0x00, 0x40,  // event_mask[1] = event 0x3E (LE Meta)
    0x0D, 0x20, 0x00, 0x00   // opcode = 0x200D (LE Create Connection Cmd Complete)
};
setsockopt(hci_sock, SOL_HCI, HCI_FILTER, hci_filter, 16);
```
Without this EXACT filter, the HCI socket receives ZERO events.

## HCI LE Create Connection Parameters (matching hcitool)
```
scan_interval: 0x0010 (10ms)
scan_window: 0x0010 (10ms)
filter_policy: 0x00
peer_addr_type: 0x00 (Public)
peer_addr: E0:EF:BF:3B:C6:76 (6 bytes, network byte order)
own_addr_type: 0x00 (Public)
conn_interval_min: 0x0018 (30ms)
conn_interval_max: 0x0028 (50ms)
conn_latency: 0x0000
supervision_timeout: 0x002A (420ms)
min_ce_length: 0x0004
max_ce_length: 0x0006
```

## ACL Packet Format for sending ATT over HCI
```c
// HCI ACL header: [ptype=0x02][handle(12bit)|PB(2bit=10)|BC(2bit=00)][data_len(16bit)]
// L2CAP B-frame: [payload_len(16bit)][cid=0x0004(16bit)][ATT_PDU...]
```

## GATT Handles (Pro Controller, from ndeadly research)
- Service: 0x0008-0x002A (UUID ab7de9be-...-7fd0)
- Input Report Characteristic: handle 0x000E (UUID 7492866c-ec3e-4619-8258-32755ffcc0f8)
  - Sends Report 0x09 (63 bytes): buttons, sticks, motion data
- CCCD for Input: handle 0x000F (write 0x0001 to enable notifications)
- Output Report: handle 0x0012 (UUID cc483f51-...-72b05) — for rumble/vibration

## ATT/GATT Sequence
1. Exchange MTU: ATT opcode 0x02 (request), expect 0x03 (response). Request 517 bytes.
2. Read By Group Type: ATT opcode 0x10 with UUID 0x2800 (Primary Service), 16-bit first, then 128-bit if error 0x0A
3. Read By Type: ATT opcode 0x08 with UUID 0x2803 (Characteristic) on service range
4. Write Request: ATT opcode 0x12 to handle 0x000F with data 0x0001 (enable CCCD notifications)
5. Handle Value Notification: ATT opcode 0x1B will arrive on handle 0x000E with 63-byte input reports

## Input Report 0x09 Format (63 bytes, from Nohzockt code)
```
Offset 0:  Report ID (0x09)
Offset 1-2: Button bitfield (LE16)
  Bit 0: A, Bit 1: B, Bit 2: X, Bit 3: Y
  Bit 4: L, Bit 5: R, Bit 6: ZL, Bit 7: ZR
  Bit 8: Minus, Bit 9: Plus, Bit 10: L-Stick, Bit 11: R-Stick
  Bit 12: Home, Bit 13: Capture, Bit 14: SL, Bit 15: SR
Offset 3: Left Stick X (12-bit, 0-4095, neutral ~2048)
Offset 5: Left Stick Y (12-bit)
Offset 7: Right Stick X (12-bit)
Offset 9: Right Stick Y (12-bit)
... vibration/IMU data follows
```

## UHID Gamepad Descriptor
```c
// Standard gamepad with 24 buttons, 4 analog axes (2 sticks), D-pad, 2 triggers
// Report ID 0x01, 6 bytes: buttons(3) + LX(12bit) + LY(12bit) + RX(12bit) + RY(12bit) + D-pad(4bit) + LT(8bit) + RT(8bit)
// Total: 12 bytes
```

## USB Preparation Phase (required before BLE connection)
The controller's BT radio is OFF by default. Must send these USB HID subcommands BEFORE unplugging:
1. SetHCIState (subcmd 0x06, arg 0x01) — enables BT chip
2. SetShipmentMode (subcmd 0x08, arg 0x00) — exit shipment mode
3. BluetoothManualPair (subcmd 0x01, arg = host BD_ADDR 6 bytes) — enables pairing

USB HID output report format (64 bytes):
```
[0]=0x01(report_id), [1]=counter, [2-9]=rumble(zeros), [10]=subcmd, [11+]=data
```

## Architecture
```
sw2d daemon:
  1. USB Phase: Open /dev/hidraw0, send subcmds 0x06/0x08/0x01, signal user to unplug
  2. HCI Phase: Open raw HCI socket, set 16-byte filter, send LE Create Connection
  3. Wait for LE Connection Complete event → get connection handle
  4. ACL/ATT Phase: Exchange MTU, discover GATT services, enable CCCD on 0x000F
  5. UHID Phase: Create UHID device with gamepad descriptor
  6. Main Loop: Receive ACL packets, parse Handle Value Notifications, convert to UHID input
  7. Cleanup: Destroy UHID, close sockets on disconnect

Command-line: sw2d [bdaddr] [--usb-init] [--no-uhid]
```

## Additional Requirements
- LE Connection Update after connect: request 7.5ms interval (Spec minimum, controller wants 5ms but that's below spec)
- Handle disconnection gracefully: attempt reconnect with backoff
- Log to stdout/stderr, use syslog optional
- Compile with: gcc -O2 -s -Wall -o sw2d sw2d.c
- Single .c file, no external dependencies beyond libc and Linux headers
- include <linux/uhid.h> for UHID structures
- Must work with bluetoothd KILLED (sudo killall -9 bluetoothd)
- Include Makefile

## Key Pitfalls to Avoid
- Do NOT use L2CAP sockets (they fail with EINVAL on this kernel)
- Do NOT use any BlueZ D-Bus API
- Do NOT attempt SMP pairing (controller disconnects)
- HCI filter MUST be exactly 16 bytes and exactly match the bytes above
- ACL data must be sent as HCI ACL packets wrapped around L2CAP B-frames
- USB init MUST happen before BLE connection attempt
- The HCI socket must be bound to hci0 (device index 0)

## Test Files Available
- gattdump.c: L2CAP ATT test (works for reference, but don't use L2CAP approach)
- hciraw3.c: HCI filter verification test
- hcitest.c: HCI event reader test
- hcisend.c, hcisend2.c: HCI command send tests
- l2test_c.c: L2CAP connect format tests

Build the complete, working daemon in sw2d.c. Focus on reliability over features — get the gamepad working first, vibration can come later.
