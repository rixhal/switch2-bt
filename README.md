# Switch 2 Pro Controller — Linux Wireless Daemon

> **STATUS: Awaiting hardware validation** — NOT yet confirmed working on Linux
>
> The daemon passes 122 unit tests and uses protocol profiles verified by three
> independent *working* implementations (SPro2Win on Windows, switch2bridge-macos
> on macOS, joycon2cpp on Windows). But it has **not** been tested with the actual
> Switch 2 Pro Controller on a Raspberry Pi. See
> [docs/HARDWARE-TEST.md](docs/HARDWARE-TEST.md) for the validation procedure.

**`switch2d.py`** is a BlueZ/bleak/uinput daemon for the Nintendo Switch 2
Pro Controller (VID 0x057E, PID 0x2069). It scans for the controller, connects
via the Linux BLE stack, and exposes decoded inputs as a standard Linux gamepad
via `/dev/uinput`. Three protocol profiles (macos, spro2win, joycon2cpp) are
derived from documented *working* implementations on other platforms.

Reference implementations (all WORKING):
- [switch2bridge-macos](https://github.com/mlstr0m/switch2bridge-macos) — macOS, UUID `7492866c`
- [SPro2Win](https://github.com/SquareDonut1/SPro2Win) — Windows, handle-45 notify
- [joycon2cpp](https://github.com/TheFrano/joycon2cpp) — Windows, init + UUID `ab7de9be`

Predecessor paths (raw HCI, BTstack, BlueZ D-Bus) are archived in `legacy/`
and documented in [RESEARCH.md](RESEARCH.md).

## Primary Path: `switch2d.py` — BlueZ/bleak → profile → notify → uinput

```
Controller (SYNC btn) ──BLE──► BlueZ (bluetoothd) ──► bleak ──► uinput
                              └─ connection mgmt      └─ profile-driven notify
```

No raw HCI, no BTstack, no kernel module needed. The daemon uses BlueZ's
MGMT layer for connection management (same approach as the working macOS
and Windows implementations) and a profile system to match the controller's
GATT layout.

### Quick Start

```bash
# OS preflight — bring Bluetooth + uinput into known-good state
sudo ./scripts/os-preflight.sh

# Diagnostic mode — auto-detect best profile, print JSON summary, exit
sudo python3 switch2d.py --diagnose --profile auto --uinput --verbose

# Daemon mode — run forever with auto-reconnect and uinput gamepad
sudo python3 switch2d.py --daemon --profile auto --uinput --max-reports 0 --verbose

# Hardware golden run — save all reports for analysis
sudo python3 switch2d.py --diagnose --profile auto --uinput \
  --dump-jsonl golden_run.jsonl --verbose
```

### Protocol Profiles (v5.0)

Three profiles from documented working implementations. Use `--profile auto`
to try them sequentially.

| Profile | Source | Strategy | Input UUID | Init |
|---------|--------|----------|-----------|------|
| `macos` | switch2bridge-macos | Selected notify | `7492866c-...0f9` | None |
| `spro2win` | SPro2Win | Subscribe-all | Any notify char | None |
| `joycon2cpp` | joycon2cpp | Selected notify + init | `ab7de9be-...fd2` | Feature-select 0x02/0x04 + LED + Sound |

See [docs/PROTOCOL-PROFILES.md](docs/PROTOCOL-PROFILES.md) for full details.

### Modes

| Flag | Behavior |
|------|----------|
| `--diagnose` | Full pipeline, JSON summary, exit with stage-level exit code |
| `--daemon` | Run forever with exponential-backoff reconnect |
| (default) | Single session, exit after first connection |

### Stage Pipeline

```
PREFLIGHT → SCAN → CONNECT → DISCOVER → INIT → SUBSCRIBE → RUNNING
```

Init is **profile-dependent** — macos and spro2win skip it, joycon2cpp sends
feature-select + LED + Sound. Each stage logs structured results.

### Diagnostic JSON Summary

`--diagnose` prints a final JSON object:

```json
{
  "exit_code": 0,
  "mode": "auto",
  "attempted_profiles": [
    {"profile": "macos", "success": false, "failure_reason": "subscribed but zero reports"},
    {"profile": "spro2win", "success": true, "report_count": 12}
  ],
  "winning_profile": "spro2win",
  "stages": {
    "scan": true, "connect": true, "discover": true,
    "subscribe": true, "reports": true, "uinput": true
  },
  "device": "E0:EF:BF:3B:C6:76",
  "input_uuid": "7492866c-ec3e-4619-8258-32755ffcc0f9",
  "input_handle": 45,
  "report_count": 12,
  "first_report_hex": "5b2300000079478585787f38...",
  "last_report_hex": "5c2400000079478585787f38...",
  "telemetry": {
    "length_min": 11, "length_avg": 27.3, "length_max": 60,
    "interval_min_ms": 8.2, "interval_avg_ms": 15.1, "interval_max_ms": 32.7
  }
}
```

### Report Format (byte-level, verified by SPro2Win + macOS bridge)

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

### Flags

```
--diagnose              Diagnostic mode with JSON summary, then exit
--daemon                Daemon mode with reconnect loop
--profile auto|macos|spro2win|joycon2cpp
                        Protocol profile (auto = sequential fallback)
--max-reports N         0 = unlimited (default: 100)
--scan-timeout S        BLE scan timeout (default: 10s)
--address BDADDR        Filter scan to specific address
--loose-scan            Accept any Nintendo device (default: PID 0x2069 only)
--uinput                Create /dev/uinput gamepad (21 buttons + 6 axes)
--dump-jsonl PATH       Save all received reports to JSONL file
--notify-timeout S      Max seconds without reports (default: 30)
--connect-retries N     Connection attempts (default: 3)
--gatt-retries N        GATT discovery retries (default: 10)
--verbose               Detailed output
--json                  JSON-line structured logging
--quiet                 Errors only
```

### Install

```bash
# Quick install (Python deps only)
./install.sh

# Full install with systemd service
sudo ./install.sh --systemd
systemctl status switch2d
journalctl -u switch2d -f
```

---

## Scripts

| Script | Purpose |
|--------|---------|
| `scripts/os-preflight.sh` | 7-step OS readiness: rfkill, bluetoothd, uinput, adapter info |
| `scripts/capture-btmon.sh` | Record HCI trace (`btmon -w`) during golden run |

---

## Docs

- [HARDWARE-TEST.md](docs/HARDWARE-TEST.md) — Step-by-step hardware validation checklist
- [TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) — Symptom→Diagnosis→Fix guide (10 scenarios)
- [PROTOCOL-PROFILES.md](docs/PROTOCOL-PROFILES.md) — Profile sources and comparison
- [SCOPE.md](docs/SCOPE.md) — v0.1 scope (gameplay input only)
- [RESEARCH.md](RESEARCH.md) — Full timeline of 14 research phases

## Tests

```bash
pip install -r requirements-dev.txt
python3 -m pytest tests/ -v   # 122 tests
```

---

## Repo Structure

```
switch2d.py                  ← PRIMARY: BlueZ/bleak → profile → notify → uinput daemon
scripts/
  os-preflight.sh            ← OS readiness helper
  capture-btmon.sh           ← HCI trace recorder
tests/
  test_parsing.py            ← Stick/report/manufacturer-data parsing (27 tests)
  test_commands.py           ← Command builder payload verification (10 tests)
  test_report_decode.py      ← Full report decode with known samples (37 tests)
  test_init_commands.py      ← joycon2cpp init byte sequences + profile structure (15 tests)
  test_pair_payloads.py      ← CareyScott pair payload byte-accuracy (20 tests)
docs/
  HARDWARE-TEST.md           ← Hardware validation checklist
  TROUBLESHOOTING.md         ← Troubleshooting guide
  PROTOCOL-PROFILES.md       ← Profile sources and comparison
  SCOPE.md                   ← v0.1 scope
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
MGMT layer handle the connection.

## Credits

- **[mlstr0m/switch2bridge-macos](https://github.com/mlstr0m/switch2bridge-macos)** — macOS bridge (UUID `7492866c`, byte-level parsing)
- **[SquareDonut1/SPro2Win](https://github.com/SquareDonut1/SPro2Win)** — Windows BLE driver (subscribe-all, handle 45)
- **[TheFrano/joycon2cpp](https://github.com/TheFrano/joycon2cpp)** — ProCon2 init sequence (feature-select + LED + Sound)
- **[CareyScott/switch2controllerpc](https://github.com/CareyScott/switch2controllerpc)** — GATT pairing reverse-engineering
- **[dekuNukem/Nintendo_Switch_Reverse_Engineering](https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering)** — Switch Joy-Con protocol reference

Built on crackberry (Raspberry Pi 4) for crackbery5 (Raspberry Pi 5 / LibreELEC).
