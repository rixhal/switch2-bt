# Switch 2 Pro Controller — Scope & Limitations (v0.1)

## In Scope: Gameplay Input

`switch2d.py` v0.1 provides **gameplay input only** — buttons, D-Pad, and
analog sticks. This matches the scope of the working reference implementations
SPro2Win (Windows) and switch2bridge-macos (macOS).

| Feature | Status | Notes |
|---------|--------|-------|
| Buttons (A/B/X/Y/L/R/ZL/ZR/+/-/Home/L3/R3/C/GL/GR/Screenshot) | ✅ In scope | Byte-level bitmask on bytes 2-4, verified by SPro2Win + macOS bridge |
| D-Pad (Up/Down/Left/Right) | ✅ In scope | Same byte-level layout as buttons |
| Analog sticks (Left, Right) | ✅ In scope | 12-bit packed at bytes 5-10, normalized to -1..1 |
| uinput gamepad | ✅ In scope | 17 buttons + 6 axes (X/Y/RX/RY/Z/RZ) |
| BLE scan + connect | ✅ In scope | BlueZ/bleak, strict PID 0x2069 matching |
| Multi-UUID input probing | ✅ In scope | Primary: `7492866c`, Secondary: `ab7de9be` |
| SPro2Win subscribe-all mode | ✅ In scope | `--spro2win` flag, handle 45 filter |
| Auto-notify-fallback | ✅ In scope | Diagnostic mode retries with alternate UUIDs + subscribe-all |
| Reconnect loop | ✅ In scope | Exponential backoff (1s..30s), `--daemon` mode |
| Diagnostic JSON summary | ✅ In scope | Stage booleans, telemetry, report hex samples |
| JSONL report dump | ✅ In scope | `--dump-jsonl` for hardware golden runs |
| systemd service | ✅ In scope | `install.sh --systemd` |

## Out of Scope: Advanced Features (v0.1)

These features are documented by the reference implementations (Switch 1 research
by dekuNukem, CareyScott GATT protocol, ndeadly MissionControl) but are
**explicitly excluded** from v0.1:

| Feature | Why Out of Scope |
|---------|-----------------|
| **HD Rumble** | Proprietary protocol. Neither SPro2Win nor macOS bridge have it. Output characteristic `7492866c-...0f8` documented but not responding. Likely requires Joy-Con-style handshake via command UUID `649d4ac9`. |
| **Gyro / Motion controls** | Data bytes identified (0x30-0x3B in 0x3C-byte reports) but not decoded or exposed to uinput. SPro2Win explicitly marks this as not implemented. |
| **Battery level reporting** | SPro2Win explicitly lists this as a limitation. Controller sends data but protocol for interpreting it is unknown. |
| **LED control** | Output characteristic does not respond to standard commands. macOS bridge reports this as not working. |
| **NFC / Amiibo** | Not present in any working wireless implementation. |
| **Pairing persistence** | Controller uses proprietary CareyScott GATT pair keys (`SetMAC` → `Key1` → `Key2` → `Finish`). Pairing is not persistent across sessions on Linux — BLE bond is lost on disconnect. |
| **USB mode** | USB kernel driver (`hid-switch2.ko`) exists as a separate path at `~/switch2-raw/`. Not integrated into `switch2d.py`. Switch 2 Pro Controller uses different USB HID subcommand protocol than Switch 1 (no subcommand ACKs). |
| **Audio / headset** | Out of scope for gamepad functionality. |
| **IR camera** | Only present on Joy-Con 2 (Right), not Pro Controller 2. |

## Reference Implementations

| Project | Platform | Status | What It Does |
|---------|----------|--------|-------------|
| [SPro2Win](https://github.com/SquareDonut1/SPro2Win) | Windows | ✅ Working | BLE → ViGEmBus XInput. Handle 45, byte-level parsing. |
| [switch2bridge-macos](https://github.com/mlstr0m/switch2bridge-macos) | macOS | ✅ Working | BLE → pynput keyboard. UUID `7492866c`. |
| [joycon2cpp](https://github.com/TheFrano/joycon2cpp) | Windows | ✅ Working | ProCon2 init sequence, report decoder. |
| [CareyScott/switch2controllerpc](https://github.com/CareyScott/switch2controllerpc) | Windows | ✅ Working | GATT pair keys (SetMAC/Key1/Key2/Finish). |
| [dekuNukem/Nintendo_Switch_Reverse_Engineering](https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering) | Switch 1 | Reference | Joy-Con SPI, subcommands, CRC-8, button layout. |
| **switch2d.py (this project)** | **Linux** | **Awaiting hardware validation** | BLE → uinput. Combines SPro2Win + macOS bridge approaches. |

## Version History

| Version | Date | Changes |
|---------|------|---------|
| v0.1 | June 2026 | Initial release. Gameplay input only. Awaiting hardware validation. |
