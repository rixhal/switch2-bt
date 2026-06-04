# Contributing to switch2-bt

Thanks for wanting to help get the Switch 2 Pro Controller working wirelessly on Linux!

## Quick Start

```bash
git clone git@github.com:rixhal/switch2-bt.git
cd switch2-bt
```

See [README.md](README.md) for architecture overview and [RESEARCH.md](RESEARCH.md) for the
full research log — read these first to understand what's been tried and what failed.

## What Needs Help

### 🔴 Critical (blocks all wireless progress)

1. **CYW43455 LE Create Connection blocked** — The Cypress BLE firmware on Raspberry Pi
   returns `Command Disallowed (0x0C)` for raw HCI `LE Create Connection`. Any bypass,
   firmware patch, kernel config, or alternative BLE controller approach would unblock
   the entire project.

2. **BTstack SMP rejected** — BTstack bypasses the CYW43455 issue (HCI_CHANNEL_USER)
   and connects successfully, but the controller terminates on SMP pairing with
   `ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION (0x13)`. We now skip SMP and attempt
   direct GATT discovery — untested on hardware.

3. **BlueZ MGMT scan filter** — The controller sends zero-byte scan responses, which
   BlueZ's management layer discards. A BlueZ patch or `main.conf` workaround would
   allow standard `bluetoothctl` / `bleak` connections.

### 🟠 High Impact

4. **Linux Wireless GATT daemon** — Port the working [joycon2cpp](https://github.com/TheFrano/joycon2cpp)
   Windows approach to Linux. The CareyScott GATT protocol is documented in
   `src/switch2_protocol.c` and `switch2d.py`. Use BlueZ D-Bus (via
   `bleak` or directly) to connect, then run the 2-step init and subscribe to
   input reports. See [JOYCON2CPP_COMPARISON.md](JOYCON2CPP_COMPARISON.md).

5. **Alternative BLE hardware** — Test with CSR8510, nRF52840, or ESP32 USB dongles
   that don't have the CYW43455 firmware restrictions.

### 🟡 Ongoing

6. **Test `switch2_ble_probe.py`** on hardware with controller in pairing mode (sync button)
7. **Test BTstack no-SMP bridge** (`switch2_btstack_bridge.c`) on crackbery5
8. **Improve `switch2-bt.c`** — currently uses wrong GATT UUIDs/protocol path (ndeadly
   instead of CareyScott). Needs full rewrite of GATT layer.

## Development

### Prerequisites

- Raspberry Pi 5 (LibreELEC) or Debian ARM64 host
- `aarch64-linux-gnu-gcc`, `libbluetooth-dev`, `libsystemd-dev`
- Python 3.10+ with `bleak`, `evdev` (`pip install bleak evdev`)

### Build

```bash
make                    # native
make aarch64            # cross-compile for RPi5
```

### Test

```bash
python3 -m pytest tests/ -v
```

### Run (on target hardware with controller)

```bash
# Python probe (BlueZ/bleak path)
sudo python3 switch2d.py --mode procon2 --verbose --connect-retries 5

# BTstack bridge (raw HCI path — stop BlueZ first!)
systemctl stop bluetooth && hciconfig hci0 down
sudo ./switch2_btstack_bridge
```

## Pull Requests

- Keep changes small and focused
- Reference the research log (RESEARCH.md) for context
- Test on hardware if your change touches BLE/connection code
- Run `python3 -m pytest tests/ -v` before submitting

## Questions?

Open an issue. We're on Telegram too (ask for invite).
