# Switch 2 Pro Controller — Linux Wireless Daemon

> ⚠️ **STATUS: NOT WORKING (Wireless BLE)** — June 2026  
> USB kernel driver works. Wireless BLE connection **does not** work yet on
> Linux. The Cypress CYW43455 chip on Raspberry Pi blocks raw HCI `LE Create
> Connection`. BlueZ MGMT layer filters zero-byte scan responses. BTstack
> bypasses both but the controller rejects standard BLE SMP pairing.
>
> **[joycon2cpp](https://github.com/TheFrano/joycon2cpp)** (TheFrano) works on
> Windows — we're porting that CareyScott GATT protocol to Linux. This is now
> the **primary development path**.
>
> See [RESEARCH.md](RESEARCH.md) for the full timeline (14 phases) of dead ends
> and breakthroughs.

## Primary Path: `switch2d.py` — BlueZ/bleak → uinput

Let the Linux BLE stack handle the connection (just like joycon2cpp does on
Windows), then run the CareyScott GATT init sequence and pipe input reports
to `/dev/uinput` as a standard gamepad.

```
Controller (SYNC btn) ──BLE──► BlueZ (bluetoothd) ──► bleak (Python) ──► uinput
                              └─ connection mgmt      └─ GATT init        └─ gamepad
```

### Quick Start

```bash
# Install deps
pip install bleak evdev

# Hold controller SYNC button (top edge, recessed) until LEDs blink
sudo python3 switch2d.py --mode procon2 --uinput --verbose
```

### What It Does

1. **Scan** for Nintendo manufacturer ID `0x0553` + VID `0x057E` + PID `0x2069`
2. **Connect** via BlueZ/bleak (no raw HCI — uses kernel mgmt path CYW43455 allows)
3. **Discover** GATT services → find input notify UUID and command write UUID
4. **Init** — 2-step joycon2cpp feature-select writes (WriteWithoutResponse):
   ```
   0c 91 01 02 00 04 00 00 ff 00 00 00    (200ms gap)
   0c 91 01 04 00 04 00 00 ff 00 00 00
   ```
5. **Subscribe** to input report notifications
6. **Decode** ProCon2 0x3C-byte reports → 21 buttons + 4 analog axes
7. **Expose** via `/dev/uinput` as standard Linux gamepad

### Key GATT UUIDs (CareyScott / joycon2cpp)

| Function | UUID |
|----------|------|
| Input Report (notify) | `ab7de9be-89fe-49ad-828f-118f09df7fd2` |
| Command Write | `649d4ac9-8eb7-4e6c-af44-1ea54fe5f005` |
| Command Response | `c765a961-d9d8-4d36-a20a-5315b111836a` |

### Report Format (0x3C bytes, Pro Controller 2)

| Field | Offset |
|-------|--------|
| Buttons (48-bit BE) | bytes 3..8 |
| Left stick (12-bit X/Y) | bytes 10..12 |
| Right stick (12-bit X/Y) | bytes 13..15 |
| Accel | bytes 0x30..0x35 |
| Gyro | bytes 0x36..0x3b |

### Hardware Run Checklist

When testing on hardware, collect these log milestones:

- [ ] **Scan:** Found Nintendo controller? BD_ADDR + manufacturer data decoded?
- [ ] **Connect:** BLE connection successful? Handle?
- [ ] **Services:** GATT services discovered? Count?
- [ ] **Command UUID:** `649d4ac9-...` found? Handle?
- [ ] **Notify UUID:** `ab7de9be-...` found? Handle?
- [ ] **Init write:** Both feature-select commands acknowledged?
- [ ] **Subscribe:** CCCD notification enabled?
- [ ] **Reports:** Input reports received? Count? Hexdump first 3?

Log each step. If anything fails, capture the exact error and surrounding context.

---

## Alternative Paths

### BTstack Bridge (`switch2_btstack_bridge.c`)

Bypasses BlueZ entirely via `HCI_CHANNEL_USER`. BTstack handles the BLE
connection and GATT client internally. Works around the CYW43455 `Command
Disallowed` blocking but hits the SMP pairing wall (error 0x13).

```bash
# Build
cd btstack-upstream/port/linux/build && cmake .. && make switch2_btstack_bridge

# Run (must stop BlueZ first!)
systemctl stop bluetooth && hciconfig hci0 down
sudo ./switch2_btstack_bridge
```

### USB Kernel Driver (`hid-switch2.ko`)

Working USB path — 21 buttons, 4 axes, force feedback. Deploy to LibreELEC:

```bash
scp *.ko root@crackbery5:/lib/modules/$(uname -r)/extra/
ssh root@crackbery5 insmod /lib/modules/$(uname -r)/extra/hid-switch2.ko
```

---

## Repo Structure

```
switch2d.py                  ← 🔥 PRIMARY: BlueZ/bleak → uinput daemon
switch2-bt.c                 ← BlueZ D-Bus GATT daemon (needs rewrite)
switch2-bt-ff.c              ← Force feedback kernel module
switch2_btstack_bridge.c     ← BTstack no-SMP GATT bridge
src/
  switch2_protocol.c/.h      ← CareyScott protocol: pair keys, command builder
tests/
  test_pair_payloads.py      ← 17 byte-accurate payload tests
legacy/raw-hci/              ← Dead-end experiments (archived)
  sw2d_final.c               ← libbluetooth + raw ACL (CYW43455 blocks)
  sw2d.c                     ← raw socket attempt (no connection ownership)
  dump_reports.py            ← JSONL report analyzer
btstack-upstream/            ← BTstack fork (static lib, custom bridge)
btstack_build/               ← BTstack build config + artifacts
```

---

## Known Blockers

| Blocker | Where | Symptom |
|---------|-------|---------|
| CYW43455 firmware | raw HCI | `LE Create Connection` → `Command Disallowed (0x0C)` |
| BlueZ MGMT filter | bluetoothd | Zero-byte SCAN_RSP → device never registered |
| SMP pairing rejected | BTstack | Controller terminates with `0x13` on any Pairing Request |
| `switch2-bt.c` wrong protocol | BlueZ D-Bus daemon | Uses ndeadly UUIDs instead of CareyScott/joycon2cpp |

## Credits

- **[TheFrano/joycon2cpp](https://github.com/TheFrano/joycon2cpp)** — Windows wireless proof-of-concept
  using CareyScott GATT protocol (Pro Controller 2 init sequence, report format, button masks)
- **[CareyScott/switch2controllerpc](https://github.com/CareyScott/switch2controllerpc)** — GATT pairing
  protocol reverse-engineering (SetMAC, LTK1/LTK2/Finish, UUID discovery)
- **[bluekitchen/btstack](https://github.com/bluekitchen/btstack)** — Bluetooth Host stack used for
  HCI_CHANNEL_USER bypass on CYW43455 (port/linux)
- **SDL2** — `SDL_hidapi_switch2.c` for USB subcommand discovery
- **[Nohzockt/BlueRetro](https://github.com/Nohzockt/Switch2-Controllers)** — Switch 2 Pro Controller
  research, GATT UUIDs, init sequence (v1.8 release)
- **[ndeadly/MissionControl](https://github.com/ndeadly/MissionControl)** — Switch controller GATT
  research, UUIDs, button mapping, report format
- **BlueZ** — `hcilecreateconn` test program validated libbluetooth connection approach
- **[Akashem06/RPI_Bluetooth](https://github.com/Akashem06/RPI_Bluetooth)** — Raw-HCI host layer on RPi4
- **[bleno#225](https://github.com/noble/bleno/issues/225)** — HCI_CHANNEL_USER usage pattern on Linux
- **Claude** — Architectural review, code audit, root cause analysis of SMP error 0x13
- **Codex** — Initial sw2d.c and sw2d_final.c implementations

Built on crackberry (Raspberry Pi 4) for crackbery5 (Raspberry Pi 5 / LibreELEC).
