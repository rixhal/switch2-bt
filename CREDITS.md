# SW2D BTstack Bridge — Credits

## Donor LTKs & Init Command Format
**Nadeflore/switch2-controllers** ([GitHub](https://github.com/Nadeflore/switch2-controllers)) — Python/bleak Switch 2 controller driver. Provided the two donor LTKs (controller.py:380-383) and the `write_command()` byte format specification.

## Reference Implementation
**Joypad OS** ([joypad-ai/joypad-os](https://github.com/joypad-ai/joypad-os)) — Open-source BTstack firmware stack. `src/bt/bthid/devices/vendors/nintendo/switch2_ble.c` proves BTstack successfully communicates with Switch 2 Pro Controllers. Used for button mapping and axis calibration reference.

## BLE Stack
**BTstack** ([BlueKitchen GmbH](https://github.com/bluekitchen/btstack)) — Embedded BLE stack. SM API (`sm.h`), example code (`sm_pairing_central.c`), and LTK callback mechanism are the foundation of this bridge.

## Protocol Reverse Engineering
**Leon's Notes** ([leonsnotes.ca](https://leonsnotes.ca/2026/04/04/reverse-engineering-the-switch-2-pro-controllers-bluetooth-protocol/)) — Detailed Switch 2 BLE protocol breakdown: connection interval=4, 0x15 proprietary pair sequence, donor LTK approach, advertising data quirks.

## BLE Implementation Confirmation
**BlueRetro** ([darthcloud/BlueRetro](https://github.com/darthcloud/BlueRetro)) — Multi-console Bluetooth adapter. Issue #1249 and beta build confirm Switch 2 BLE support is achievable.

---

*2026-06-10.*
