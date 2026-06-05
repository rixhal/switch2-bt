# Switch 2 Pro Controller — Linux Wireless Daemon

> **STATUS: Awaiting hardware validation** — NOT yet confirmed working on Linux
>
> The daemon compiles, passes 107 unit tests, and uses protocol constants
> verified by two independent *working* implementations (SPro2Win on Windows,
> switch2bridge-macos on macOS). But it has **not** been tested with the actual
> Switch 2 Pro Controller on a Raspberry Pi. See
> [docs/HARDWARE-TEST.md](docs/HARDWARE-TEST.md) for the validation procedure.

**`switch2d.py`** is a BlueZ/bleak/uinput daemon for the Nintendo Switch 2
Pro Controller (VID 0x057E, PID 0x2069). It scans for the controller, connects
via the Linux BLE stack, subscribes to input report notifications (no GATT
init required — this matches how the working macOS and Windows implementations
operate), and exposes decoded inputs as a standard Linux gamepad via
`/dev/uinput`.

Reference implementations (both WORKING):
- [SPro2Win](https://github.com/SquareDonut1/SPro2Win) — Windows, handle-45 notify
- [switch2bridge-macos](https://github.com/mlstr0m/switch2bridge-macos) — macOS, UUID `7492866c`

Predecessor paths (raw HCI, BTstack, BlueZ D-Bus) are documented in
[RESEARCH.md](RESEARCH.md).

## Primary Path: `switch2d.py` — BlueZ/bleak → notify → uinput

```
Controller (SYNC btn) ──BLE──► BlueZ (bluetoothd) ──► bleak (Python) ──► uinput
                              └─ connection mgmt      └─ notify sub       └─ gamepad
```

No GATT init sequence needed — the working implementations just subscribe to
notifications and input flows. This is the default. Use `--init` to opt into
the joycon2cpp ProCon2 feature-select sequence if needed.

### Quick Start

```bash
# Install
./install.sh

# Diagnostic mode — verify everything works, exit with JSON summary
sudo python3 switch2d.py --diagnose --verbose --json

# Daemon mode — run forever with auto-reconnect
sudo python3 switch2d.py --daemon --uinput --max-reports 0 --verbose

# Hardware golden run — save all reports for analysis
sudo python3 switch2d.py --diagnose --dump-jsonl reports.jsonl --verbose
```

### Modes

| Flag | Behavior |
|------|----------|
| `--diagnose` | Full pipeline, JSON summary, exit with stage-level exit code |
| `--daemon` | Run forever with exponential-backoff reconnect |
| (default) | Single session, exit after first connection |

### Stage Pipeline

```
PREFLIGHT → SCAN → CONNECT → DISCOVER → SUBSCRIBE → RUNNING
```

Init is **off by default** (matches working implementations). Each stage
logs structured results (text or `--json`).

### Diagnostic JSON Summary

`--diagnose` prints a final JSON object on stdout with these fields:

```json
{
  "exit_code": 0,
  "stages": {
    "scan": true, "connect": true, "discover": true,
    "subscribe": true, "reports": true, "uinput": false
  },
  "device": "E0:EF:BF:3B:C6:76",
  "input_uuid": "7492866c-ec3e-4619-8258-32755ffcc0f9",
  "input_handle": 45,
  "report_count": 12,
  "first_report_hex": "5b2300000079478585787f38...",
  "last_report_hex": "5c2400000079478585787f38..."
}
```

### Report Format

| Field | Offset |
|-------|--------|
| Timer (16-bit LE) | 0..1 |
| Right cluster (8-bit mask) | 2 |
| Left cluster (8-bit mask) | 3 |
| System + grip (8-bit mask) | 4 |
| Left stick (12-bit packed X,Y) | 5..7 |
| Right stick (12-bit packed X,Y) | 8..10 |
| Accel (3× s16 LE) | 0x30..0x35 (long reports only) |
| Gyro (3× s16 LE) | 0x36..0x3B (long reports only) |

Byte-level button layout verified by SPro2Win + switch2bridge-macos.

### Flags

```
--diagnose            Diagnostic mode with JSON summary, then exit
--daemon              Daemon mode with reconnect loop
--max-reports N       0 = unlimited (default: 100)
--scan-timeout S      BLE scan timeout (default: 10s)
--address BDADDR      Filter scan to specific address
--loose-scan          Accept any Nintendo device (default: PID 0x2069 only)
--mode procon2|none   Init mode (default: none)
--init                Opt-in: send ProCon2 init sequence
--spro2win            SPro2Win mode: subscribe-all, handle 45 filter
--uinput              Create /dev/uinput gamepad
--dump-jsonl PATH     Save all received reports to JSONL file
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
switch2d.py                  ← PRIMARY: BlueZ/bleak → notify → uinput daemon
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

The BlueZ/bleak path avoids CYW43455 and SMP blockers by letting the kernel
MGMT layer handle the connection. This is the same approach SPro2Win and
switch2bridge-macos use on their respective platforms.

## Credits

- **[SquareDonut1/SPro2Win](https://github.com/SquareDonut1/SPro2Win)** — Windows BLE driver (byte-level parsing, handle-45)
- **[mlstr0m/switch2bridge-macos](https://github.com/mlstr0m/switch2bridge-macos)** — macOS bridge (UUID `7492866c`, PID-byte matching)
- **[TheFrano/joycon2cpp](https://github.com/TheFrano/joycon2cpp)** — ProCon2 init sequence
- **[CareyScott/switch2controllerpc](https://github.com/CareyScott/switch2controllerpc)** — GATT pairing reverse-engineering
- **[dekuNukem/Nintendo_Switch_Reverse_Engineering](https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering)** — Switch Joy-Con protocol reference

Built on crackberry (Raspberry Pi 4) for crackbery5 (Raspberry Pi 5 / LibreELEC).
