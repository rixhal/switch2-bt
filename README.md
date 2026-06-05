# Switch 2 Pro Controller — Linux Wireless Daemon

> **STATUS: BlueZ/bleak daemon ready for hardware testing** — June 2026

**`switch2d.py`** is a production BlueZ/bleak/uinput daemon for the Nintendo
Switch 2 Pro Controller (VID 0x057E, PID 0x2069). It scans, connects via the
Linux BLE stack, runs the joycon2cpp ProCon2 init sequence, subscribes to
input report notifications, and exposes them as a standard Linux gamepad via
`/dev/uinput`.

[joycon2cpp](https://github.com/TheFrano/joycon2cpp) (TheFrano) is the
reference Windows implementation. We port its CareyScott GATT protocol to
Linux. Predecessor paths (raw HCI, BTstack, BlueZ D-Bus) are documented in
[RESEARCH.md](RESEARCH.md) — 14 phases of dead ends and breakthroughs.

## Primary Path: `switch2d.py` — BlueZ/bleak → uinput

```
Controller (SYNC btn) ──BLE──► BlueZ (bluetoothd) ──► bleak (Python) ──► uinput
                              └─ connection mgmt      └─ GATT init        └─ gamepad
```

### Quick Start

```bash
# Install
./install.sh

# Diagnostic mode — verify everything works
sudo python3 switch2d.py --diagnose --verbose

# Daemon mode — run forever with auto-reconnect
sudo python3 switch2d.py --daemon --uinput --max-reports 0 --verbose
```

### Modes

| Flag | Behavior |
|------|----------|
| `--diagnose` | Run full connect→discover→init→subscribe cycle, verify reports, exit |
| `--daemon` | Run forever with exponential-backoff reconnect |
| (default) | Single session, exit after first connection |

### Stage Pipeline

```
PREFLIGHT → SCAN → CONNECT → DISCOVER → INIT → SUBSCRIBE → RUNNING
```

Each stage logs structured results (text or `--json` for machine parsing).
Exit codes 10-16 map to the failing stage.

### Report Format (0x3C bytes)

| Field | Offset |
|-------|--------|
| Packet ID (24-bit LE) | 0..2 |
| Buttons (48-bit BE) | 3..8 |
| Left stick (12-bit packed X,Y) | 10..12 |
| Right stick (12-bit packed X,Y) | 13..15 |
| Accel (3× s16 LE) | 0x30..0x35 |
| Gyro (3× s16 LE) | 0x36..0x3B |

21 buttons: A, B, X, Y, L, R, ZL, ZR, Home, Minus, Plus, L3, R3, D-Pad,
Screenshot, C, GL, GR

### Flags

```
--diagnose            Diagnostic mode: verify everything, then exit
--daemon              Daemon mode: run forever with reconnect
--max-reports N       0 = unlimited (default: 100)
--scan-timeout S      BLE scan timeout (default: 10s)
--address BDADDR      Filter scan to specific address
--mode procon2|none   Init mode (default: procon2)
--uinput              Create /dev/uinput gamepad
--verbose             Detailed output
--json                JSON-line structured logging
--quiet               Errors only
```

### Systemd

```bash
sudo ./install.sh --systemd
systemctl status switch2d
journalctl -u switch2d -f
```

---

## Docs

- [HARDWARE-TEST.md](docs/HARDWARE-TEST.md) — Step-by-step hardware validation checklist
- [TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) — Symptom→Diagnosis→Fix guide
- [RESEARCH.md](RESEARCH.md) — Full timeline of 14 research phases

## Tests

```bash
pip install -r requirements-dev.txt
python3 -m pytest tests/ -v
```

---

## Repo Structure

```
switch2d.py                  ← PRIMARY: BlueZ/bleak → uinput daemon
src/
  switch2_protocol.c/.h      ← CareyScott protocol (C reference)
tests/
  test_parsing.py            ← Stick/report/manufacturer-data parsing
  test_commands.py           ← Command builder payload verification
  test_report_decode.py      ← Full report decode with known samples
  test_pair_payloads.py      ← CareyScott pair payload byte-accuracy
docs/
  HARDWARE-TEST.md           ← Hardware validation checklist
  TROUBLESHOOTING.md         ← Troubleshooting guide
systemd/
  switch2d.service           ← systemd unit file
install.sh                   ← One-command installer
requirements.txt             ← Python deps (bleak, evdev)
requirements-dev.txt         ← Dev deps (pytest)
legacy/raw-hci/              ← Raw HCI experiments (archived)
btstack-upstream/            ← BTstack fork (static lib)
```

## Known Blockers

| Blocker | Where | Symptom |
|---------|-------|---------|
| BlueZ MGMT filter | bluetoothd | Zero-byte SCAN_RSP → device may not register |
| CYW43455 firmware | raw HCI only | `LE Create Connection` → `Command Disallowed` |
| SMP pairing rejected | raw HCI/BTstack | Controller terminates on standard SMP |

**The BlueZ/bleak path avoids CYW43455 and SMP blockers** by letting the kernel
MGMT layer handle the connection. This is the same approach joycon2cpp uses on
Windows.

## Credits

- **[TheFrano/joycon2cpp](https://github.com/TheFrano/joycon2cpp)** — Windows wireless proof-of-concept
- **[CareyScott/switch2controllerpc](https://github.com/CareyScott/switch2controllerpc)** — GATT pairing reverse-engineering
- **[bluekitchen/btstack](https://github.com/bluekitchen/btstack)** — HCI_CHANNEL_USER bypass reference
- **SDL2** — USB subcommand discovery (`SDL_hidapi_switch2.c`)
- **[Nohzockt/BlueRetro](https://github.com/Nohzockt/Switch2-Controllers)** — Switch 2 controller research
- **[ndeadly/MissionControl](https://github.com/ndeadly/MissionControl)** — Switch controller GATT research

Built on crackberry (Raspberry Pi 4) for crackbery5 (Raspberry Pi 5 / LibreELEC).
