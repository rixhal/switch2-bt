# switch2-bt

Bluetooth-to-UHID bridge daemon for Nintendo Switch 2 Pro Controller on Linux ARM64.

Enables wireless use of the Switch 2 Pro Controller via BLE/GATT on devices like
Raspberry Pi 5 running LibreELEC 13.

## Architecture

```
Switch 2 Pro (BLE)
       │
       ▼
  BlueZ D-Bus ──► switch2-bt (userspace daemon)
                       │
                       ▼
                  /dev/uhid ──► Kernel HID ──► /dev/input/event*
                                                      │
                       ┌──────────────────────────────┘
                       ▼
              switch2-bt-ff.ko (FF_RUMBLE kernel module)
```

## Features

- 24-button + 4-axis + D-pad HID gamepad via UHID
- Full button mapping verified against ndeadly/switch2_controller_research
- NWCP init sequence (Pro Controller 2)
- Rumble support via Physical Page HID descriptor + FF_RUMBLE kernel module
- Auto-reconnect with exponential backoff
- Static binary for minimal dependency footprint
- Cross-compiles for aarch64 (tested: RPi5, LE13 Kernel 6.18.32)

## Files

| File | Description |
|------|-------------|
| `switch2-bt.c` | UHID daemon (~590 lines C) |
| `switch2-bt-ff.c` | Force feedback kernel module (~130 lines) |
| `hid_report_descriptor.h` | HID report descriptor (93 bytes, 24-btn + 4-axis + rumble) |
| `switch2-bt.service` | systemd service unit |
| `Makefile` | Cross-compile for aarch64 |
| `Kbuild` | Kernel module build |

## Build

### Daemon
```bash
aarch64-linux-gnu-gcc -O2 -Wall -o switch2-bt switch2-bt.c \
  $(pkg-config --cflags --libs libsystemd)
```

### Kernel Module
```bash
cd /path/to/linux-6.18.32
make M=/path/to/switch2-bt ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
```

## Usage

```bash
# 1. Pair controller (one-time)
python3 switch2-bt.py --pair XX:XX:XX:XX:XX:XX

# 2. Load FF kernel module
insmod switch2-bt-ff.ko

# 3. Start daemon
./switch2-bt -a XX:XX:XX:XX:XX:XX -v

# 4. Test input
evtest /dev/input/event*
```

## References

- [ndeadly/switch2_controller_research](https://github.com/ndeadly/switch2_controller_research) — GATT protocol docs
- [ndeadly/switch2_input_viewer.py](https://gist.github.com/ndeadly/7d27aa63e2f653a902a2474dbcbc08b3) — Reference button mapping
- [Nohzockt/Switch2-Controllers](https://github.com/Nohzockt/Switch2-Controllers) — Xbox mapping reference

## License

MIT
