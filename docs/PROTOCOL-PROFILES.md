# Protocol Profiles — Source Documentation

`switch2d.py` v5.0 implements four protocol profiles (three production + one experimental), each derived from a
documented working implementation on another platform. This document explains
where each profile came from and what is implemented.

## Profile Comparison

| Profile | Source | Platform | Strategy | Input UUID | Init | Handle Filter |
|---------|--------|----------|----------|-----------|------|---------------|
| `macos` | [switch2bridge-macos](https://github.com/mlstr0m/switch2bridge-macos) | macOS (CoreBluetooth) | Selected notify | `7492866c-ec3e-4619-8258-32755ffcc0f9` | None | None |
| `spro2win` | [SPro2Win](https://github.com/SquareDonut1/SPro2Win) | Windows (bleak) | Subscribe-all | None (any notify char) | None | Handle 45 |
| `joycon2cpp` | [joycon2cpp](https://github.com/TheFrano/joycon2cpp) | Windows (bleak) | Selected notify + init | `ab7de9be-89fe-49ad-828f-118f09df7fd2` | Feature-select 0x02, 0x04 + LED + Sound | None |
| `joycon2cpp-pair` ⚠️ | CareyScott + Nadeflore + joycon2cpp | Linux (bleak, EXPERIMENTAL) | COMMAND_PAIR 0x15 + init + notify | `ab7de9be-89fe-49ad-828f-118f09df7fd2` | SetMAC→LTK1→LTK2→Finish + joycon2cpp init | None |

## Profile: `macos`

**Source**: Aurélien Desert's [switch2bridge-macos](https://github.com/mlstr0m/switch2bridge-macos), the first working BLE bridge for Switch 2 Pro Controller on macOS. Published June 2026, MIT license.

**What it does**:
- Connects via CoreBluetooth (macOS BLE stack)
- Reads the manufacturer data from BLE advertisements, matches Company ID `0x057E` and PID bytes `\x69\x20` (0x2069 LE)
- Discovers GATT services and finds the characteristic with UUID `7492866c-ec3e-4619-8258-32755ffcc0f9`
- Subscribes to notifications on that UUID — no init commands needed
- Parses input reports as 11+ byte frames with byte-level button bitmasks (bytes 2-4) and 12-bit packed sticks (bytes 5-10)
- Maps inputs to keyboard keys via pynput for Ryujinx emulator

**What we implement**:
- Same UUID (`7492866c-...0f9`)
- Same no-init approach
- Same byte-level report layout
- Selected notify strategy (subscribe only to matched UUID)
- uinput gamepad instead of keyboard (platform difference)

**What we differ on**:
- macOS uses CoreBluetooth; we use BlueZ/bleak
- macOS maps to keyboard (Ryujinx); we map to uinput gamepad
- macOS has a menubar UI; we are a CLI daemon

## Profile: `spro2win`

**Source**: SquareDonut1's [SPro2Win](https://github.com/SquareDonut1/SPro2Win), the first working Windows BLE driver for Switch 2 Pro Controller. Published October 2025, MIT license. Version 0.2.0.

**What it does**:
- Scans for Nintendo Manufacturer ID `1363` (0x0553) in BLE advertisements
- Connects via bleak on Windows
- **Subscribes to ALL notify characteristics** — no UUID filtering
- Filters incoming notifications by handle: only processes handle **45**
- No init commands needed — input flows immediately after subscribe
- Parses input reports with identical byte layout to macOS bridge
- Maps to XInput (ViGEmBus) for universal game compatibility

**What we implement**:
- Subscribe-all strategy: subscribes to every notify characteristic
- Handle 45 filter: drops notifications from any other handle
- Same no-init approach
- Same byte-level report layout
- uinput gamepad instead of XInput (platform difference)

**What we differ on**:
- Windows uses ViGEmBus XInput; we use uinput gamepad
- SPro2Win uses `COMMAND_CHANNEL_UUID = "3dacbc7e-6955-40b5-8eaf-6f9809e8b379"` as a reference but doesn't write to it
- SPro2Win reports 60ms report interval (controller limitation)

## Profile: `joycon2cpp`

**Source**: TheFrano's [joycon2cpp](https://github.com/TheFrano/joycon2cpp), a Windows wireless proof-of-concept for Switch 2 Pro Controller. Uses the CareyScott GATT protocol for init.

**What it does**:
- Connects via bleak on Windows
- Discovers GATT services and finds characteristic with UUID `ab7de9be-89fe-49ad-828f-118f09df7fd2` for input
- Writes init commands to command UUID `649d4ac9-8eb7-4e6c-af44-1ea54fe5f005`
- Init sequence (Write Without Response, with delays matching joycon2cpp timing):
  - `0c 91 01 02 00 04 00 00 ff 00 00 00` (feature-select 0x02, 500ms delay)
  - `0c 91 01 04 00 04 00 00 ff 00 00 00` (feature-select 0x04, 700ms delay = 500ms init + 200ms barrier)
  - `09 91 01 07 00 08 00 00 01 00 00 00 00 00 00 00` (LED player 1, 50ms delay)
  - `0a 91 01 02 00 08 00 00 04 00 00 00 00 00 00 00` (Nintendo connect sound, 50ms delay)
- Subscribes to notifications on matched UUID
- Decodes ProCon2 0x3C-byte reports (21 buttons, 4 analog axes, accel, gyro)

**What we implement**:
- Same input UUID (`ab7de9be-...fd2`)
- Same command UUID (`649d4ac9-...f005`)
- Same four init writes with delays (matching joycon2cpp timing)
- Selected notify strategy
- Full ProCon2 report decoder (buttons + sticks + accel/gyro when present)

**What we differ on**:
- Windows uses ViGEmBus; we use uinput
- joycon2cpp has dedicated ProCon2 report decoder for 0x3C-byte reports; we use the unified byte-level decoder that works with 11-byte and 0x3C-byte reports

## Auto-Detect Mode

When `--profile auto` (default for `--diagnose`), profiles are tried in order:

1. **macos** — try first (simplest, no init, selected notify)
2. **spro2win** — try if macos fails (subscribe-all, handle 45)
3. **joycon2cpp** — try last (init writes required, most complex)

Each profile attempt re-scans and re-connects. The first profile that
receives input reports wins. The diagnostic JSON summary includes an
`attempted_profiles` array with success/failure per profile and
`winning_profile`.

## Common Elements

All profiles share:
- **BLE scan**: Company IDs 0x0553 + 0x057E, strict PID 0x2069 (or `--loose-scan`)
- **Report format**: byte-level button bitmasks at bytes 2-4, 12-bit packed sticks at bytes 5-10
- **Report decoder**: `decode_report()` — unified function, 11-byte minimum, optional IMU at 0x30+
- **uinput gamepad**: 21 buttons + 6 axes when `--uinput` is set
- **JSONL dump**: `--dump-jsonl` for hardware golden runs

## ⚠️ Report Layout Warning

The report layouts used by SPro2Win/macOS and joycon2cpp/Leon's Notes **may differ**.
Until real JSONL reports are captured from actual hardware, the parser uses a unified
layout that matches the SPro2Win + macOS bridge implementations (bytes 2-4 = buttons,
bytes 5-10 = sticks).

**Leon's Notes + joycon2cpp report layout** (from reverse engineering):
- Minimum length: `0x3c` (60 bytes)
- Button state: bytes 3-8 as a 48-bit big-endian bitfield
- Left stick: bytes 10-12, 12-bit packed X/Y
- Right stick: bytes 13-15, 12-bit packed X/Y
- Accel: 0x30-0x35, Gyro: 0x36-0x3b

**SPro2Win/macOS report layout** (current parser):
- Minimum length: 11 bytes
- Buttons: bytes 2-4 as bitmasks
- Sticks: bytes 5-10 as 12-bit packed pairs

These layouts disagree on button offset (byte 2 vs byte 3) and stick offset
(byte 5 vs byte 10). The parser must **not** be changed until real hardware
JSONL records are captured so the actual report format can be verified.

## Future Profile: `joycon2cpp-pair`

A pairing-aware profile is planned but **not yet enabled**. When implemented,
it would add the Nintendo 0x15 COMMAND_PAIR GATT sequence before init writes:

1. `SUBCOMMAND_PAIR_SET_MAC` (0x01) — send host MAC
2. `SUBCOMMAND_PAIR_LTK1` (0x04) — hardcoded LTK value
3. `SUBCOMMAND_PAIR_LTK2` (0x02) — hardcoded LTK value
4. `SUBCOMMAND_PAIR_FINISH` (0x03) — finalize pairing

The profile would wait for responses on `COMMAND_RESPONSE_UUID`
(`c765a961-d9d8-4d36-a20a-5315b111836a`) between each step.

**⚠️ WARNING — Do not enable by default:** The hardcoded LTK values in the
reference implementations (Nadeflore/switch2-controllers, CareyScott/switch2controllerpc)
are **donor/session-specific** — extracted from an already-paired real Switch 2
Pro Controller's SPI flash. These values may not work for other controllers or
pairing sessions. Without a donor controller, LTK generation is not possible.
See `SMP_RESEARCH.md` for full context.

## References

| Project | Platform | Status | Profile |
|---------|----------|--------|---------|
| [switch2bridge-macos](https://github.com/mlstr0m/switch2bridge-macos) | macOS | ✅ Working | `macos` |
| [SPro2Win](https://github.com/SquareDonut1/SPro2Win) | Windows | ✅ Working | `spro2win` |
| [joycon2cpp](https://github.com/TheFrano/joycon2cpp) | Windows | ✅ Working | `joycon2cpp` |
| [CareyScott/switch2controllerpc](https://github.com/CareyScott/switch2controllerpc) | Windows | ✅ Working | Pair keys reference |
| [dekuNukem/Nintendo_Switch_Reverse_Engineering](https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering) | Switch 1 | Reference | Protocol research |
| **switch2d.py** | **Linux** | **Awaiting hardware** | All three |
