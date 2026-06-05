# SMP Encryption Research — Automated Farming (Session 2)
**Date:** 2026-06-05
**Researched by:** Hermes Enterprise (cron job)
**Sessions:** 2 automated farming runs consolidated below
**Sources:** web_search (14 queries), GitHub raw source extraction, leonsnotes.ca full article, Nadeflore switch2-controllers, CareyScott switch2controllerpc, BTstack upstream code

---

## 🚨 CRITICAL FINDING (from Session 1): Switch 2 Uses Proprietary GATT Pairing, NOT SMP

**The Switch 2 Pro Controller does NOT use standard BLE SMP pairing.** Our entire approach of trying to complete SMP encryption is on the wrong protocol layer. The controller uses a Nintendo-proprietary command protocol over GATT.

### Evidence
- **Source:** [Nadeflore/switch2-controllers](https://github.com/Nadeflore/switch2-controllers) — reverse-engineered Switch 2 controller protocol
- **Source:** [CareyScott/switch2controllerpc](https://github.com/CareyScott/switch2controllerpc) — Windows app using the protocol
- **Source:** [Leon's Notes](https://leonsnotes.ca/2026/04/04/reverse-engineering-the-switch-2-pro-controllers-bluetooth-protocol/) — Full reverse engineering writeup (Apr 4, 2026)
- **Source:** [BlueRetro Issue #1249](https://github.com/darthcloud/BlueRetro/issues/1249) — Switch 2 GATT service structure

---

## 🔥 NEW FINDING (Session 2): Connection Interval Violation — Silent Connection Failure

### The Switch 2 Console Uses Interval=4 (5ms) — Below BLE Spec Minimum of 6 (7.5ms)

**From Leon's Notes:** "Nintendo's console operates below the Bluetooth spec minimum. Every spec-compliant BLE stack — NimBLE, Zephyr, presumably others — silently rejects this. The connection is accepted at the radio, handed up one layer, and quietly dropped because the interval fails a bounds check. No error, no log. Just silence."

**This means:**
1. Even if the Switch 2 *console* initiates a connection from the home screen, our peripheral device must accept interval=4
2. Standard BTstack also validates connection intervals — we may need to override this
3. Home screen auto-connect fails silently because the controller rejects the connection parameters
4. "Change Grip/Order" menu works because it uses different connection parameters

### Implications for Our Pi Setup
- BTstack's `gap_set_connection_parameters()` may get rejected by the controller if the Switch 2 host proposes interval < 6
- Same issue with NimBLE/Zephyr: `ull_peripheral.c` rejects interval < 6
- Fix: In Zephyr, change `CONN_INTERVAL_MIN` constant; in BTstack, check if `hci.c` or `gap.c` has interval validation

---

## 🔥 NEW FINDING (Session 2): The 0x15 Subcommand is a Hardcoded LTK — Can't Be Generated

**From Leon's Notes:** "There's one part of the protocol I never cracked — subcommand 0x15 handles the initial key exchange. This establishes the shared encryption key (LTK) between controller and console. My emulator sidesteps this entirely by extracting the MAC address and LTK from an already-paired real controller and loading them into the microcontroller."

**Key points:**
- The LTK is stored on the controller's SPI flash AFTER pairing with a real Switch 2
- The pairing computation happens inside the controller's firmware — not on the network
- 0x15 subcommand triggers this firmware computation
- Result: Every emulator needs a donor controller (MAC + LTK extracted)
- Hardcoded LTKs in Nadeflore's code (PAIR_LTK1, PAIR_LTK2) are from a specific paired session

---

## Switch 2 GATT Characteristics (from reverse engineering)
```
INPUT_REPORT_UUID    = "ab7de9be-89fe-49ad-828f-118f09df7fd2"  ✅ matches ours
COMMAND_WRITE_UUID   = "649d4ac9-8eb7-4e6c-af44-1ea54fe5f005"  ✅ matches ours
COMMAND_RESPONSE_UUID = "c765a961-d9d8-4d36-a20a-5315b111836a"  ⚠️ we need this!
VIBRATION_WRITE_PRO  = "cc483f51-9258-427d-a939-630c31f72b05"
```

### Connection Handle Mapping (from Leon's Notes)
- Console writes commands on **handle 0x0016** (not necessarily 0x0014)
- Controller responds via notifications on **handle 0x001a**
- Console addresses characteristics by **handle number, not UUID** — handles must match exactly
- 14 characteristics across 2 custom GATT services, all behind 128-bit UUIDs

### Input Report Format (from Leon's sniffer)
- Input report type byte = **0x20** (NOT 0x0d which is the original Pro Controller)
- Input reports flow at **200Hz** on real controller (one per 5ms connection event)
- Activation response is **16 bytes** (not 9)

---

## Switch 2 Advertising Format
```
Manufacturer ID: 0x0553 (not USB VID 0x057e)
Manufacturer Data:
  [0:1]   unknown
  [1:3]   unknown
  [3:5]   vendor_id (0x057e)
  [5:7]   product_id (0x2069 = Pro Controller 2)
  [7:10]  unknown
  [10:16] reconnect_mac (6 bytes)
            - 0x000000000000 = pairing mode (sync button pressed)
            - matches host MAC = already paired → skip pairing
```

### Switch 2 Controller PID Values
```
JOYCON2_RIGHT_PID       = 0x2066
JOYCON2_LEFT_PID        = 0x2067
PRO_CONTROLLER2_PID     = 0x2069
NSO_GAMECUBE_CONTROLLER = 0x2073
```

---

## Switch 2 Pairing Protocol (Custom GATT Commands)
```
COMMAND_PAIR = 0x15

Step 1: SUBCOMMAND_PAIR_SET_MAC (0x01)
  Data: 0x00 0x02 + host_mac(6LE) + host_mac(6LE)
  Sets the host MAC address

Step 2: SUBCOMMAND_PAIR_LTK1 (0x04)
  Data: 0x00 ea bd 47 13 89 35 42 c6 79 ee 07 f2 53 2c 6c 31
  Hardcoded LTK from a specific donor controller session

Step 3: SUBCOMMAND_PAIR_LTK2 (0x02)
  Data: 0x00 40 b0 8a 5f cd 1f 9b 41 12 5c ac c6 3f 38 a0 73
  Hardcoded LTK from a specific donor controller session

Step 4: SUBCOMMAND_PAIR_FINISH (0x03)
  Data: 0x00
  Finalizes pairing
```

### Command Write Format (for COMMAND_WRITE_UUID)
```python
# write_command format:
# [cmd_id(1)] [0x91 0x01] [subcmd(1)] [0x00] [len(1)] [0x00 0x00] [data(N)]
command_buffer = cmd_id + b"\x91\x01" + subcmd_id + b"\x00" + len(data) + b"\x00\x00" + data

# Response comes on COMMAND_RESPONSE_UUID (notification):
# [cmd_id(1)] [0x01] [subcmd(1)] [0x00] [len(1)] [0x00 0x00] [response_data(N)]
```

### 19-Step Activation Sequence (from Leon's Sniffer)
1. Console reads device info
2. Pulls calibration data from virtual SPI flash
3. Configures various settings
4. Sends activation command
5. Heartbeats begin after activation accepted

Every response is a hardcoded byte array. No session tokens, no nonces, no timestamps. Protocol is entirely deterministic.

---

## BTstack-Specific Configuration

### ⚠️ Our Bridge Code Already Handles Encryption Change
The existing `switch2_btstack_bridge.c` at line 408 has:
```c
case HCI_EVENT_ENCRYPTION_CHANGE:
    if (app_state == STATE_WAIT_ENCRYPTION) {
        uint8_t enc_status = hci_event_encryption_change_get_status(packet);
        bridge_log("Encryption change: 0x%02x — starting GATT", enc_status);
        app_state = STATE_CHAR_DISCOVERING;
        // ... starts GATT discovery
    }
```

### Correct BTstack Approach
BTstack bridge is already moving in the right direction: **NO SMP, proprietary GATT pair instead**.

```c
// BTstack APIs needed:
sm_init();
sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION);
// sm_add_event_handler(&sm_event_callback_registration);  // SEPARATE handler
hci_add_event_handler(&hci_event_callback_registration);

// Auto-confirm Just Works
sm_just_works_confirm(con_handle);  // in SM_EVENT_JUST_WORKS_REQUEST handler
```

### SM Event Format Strings (from btstack_defines.h)
```
SM_EVENT_JUST_WORKS_REQUEST        H1B1    handle, addr_type, addr, secure_connection
SM_EVENT_PAIRING_COMPLETE          H1B111  handle, addr_type, addr, status, reason, ctkd_active
SM_EVENT_REENCRYPTION_COMPLETE     H1B1    handle, addr_type, addr, status
SM_EVENT_IDENTITY_CREATED          H1B1B2  handle, addr_type, addr, id_addr_type, id_addr, index
```

### 🔥 NEW: Check for Connection Interval Validation in BTstack
If BTstack rejects connection intervals < 6 (like Zephyr/NimBLE), the Switch 2 console's interval=4 will cause SILENT connection rejection at home screen. Search BTstack source for `CONN_INTERVAL_MIN` or `interval_min` validation in `hci.c`, `gap.c`, or port layer.

---

## External Examples

### BTstack sm_pairing_peripheral.c (canonical)
**File:** https://github.com/bluekitchen/btstack/blob/master/example/sm_pairing_peripheral.c

Complete SMP event handling in separate handlers — demonstrates correct API usage.

### Nadeflore switch2-controllers (ACTUAL WORKING IMPLEMENTATION)
**Repo:** https://github.com/Nadeflore/switch2-controllers
Complete implementation: BLE connection, Nintendo pairing protocol, input report parsing, vibration, LED control.

### CareyScott switch2controllerpc
**Repo:** https://github.com/CareyScott/switch2controllerpc
Wraps Nadeflore code with ViGEmBus virtual controller.

### Leon's Notes Reverse Engineering Writeup
**URL:** https://leonsnotes.ca/2026/04/04/reverse-engineering-the-switch-2-pro-controllers-bluetooth-protocol/
Full hardware/software reverse engineering of Switch 2 Pro Controller BLE protocol. nRF52840-based emulator.

### BlueRetro Issue #1249
**URL:** https://github.com/darthcloud/BlueRetro/issues/1249
Community tracking of Switch 2 controller GATT service structure and connection behavior.

### ProCon2Tool
**URL:** https://github.com/HandHeldLegend/handheldlegend.github.io/blob/master/procon2tool/index.html
Web tool for reading Pro Controller 2 SPI flash layout and command structure.

### Infineon: GATT Attribute Security Permissions in BLE
**URL:** https://community.infineon.com/t5/Knowledge-Base-Articles/Handling-GATT-attribute-security-permissions-requirements-in-Bluetooth-LE/ta-p/289525
"Read authentication required" / "Write authentication" means encryption OR authentication needed.

### Zephyr Kconfig: Encryption-Change Event Optional
**URL:** https://github.com/zephyrproject-rtos/zephyr/blob/main/subsys/bluetooth/host/Kconfig
"Enabling this option will make the central role not require the encryption-change event to be received before..."

---

## 🔥 Connection Interval Fix Reference
**Zephyr fix:** Change minimum connection interval constant in `subsys/bluetooth/controller/ll_sw/ull_peripheral.c` from 6 to 4.
**NimBLE fix:** Same constant in NimBLE link layer.
**BTstack:** Check `src/hci.c`, `src/gap.c`, and port layer for similar validation.

---

## Dead Ends Confirmed

1. **SMP-based pairing is unnecessary** — Switch 2 has proprietary GATT pairing
2. **gap_secure_connections_enable() does NOT affect BLE** — BR/EDR only; use `sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION)` for LE SC
3. **Trying to complete SMP encryption before sending PAIR commands** — Wrong order; controller's proprietary pairing enables encryption
4. **SM_AUTHREQ_NO_BONDING with SC enabled** — Creates confusion
5. **Forcing SC when controller may not support it** — Uses hardcoded LTKs, not real SC key generation
6. **Writing commands before pairing** — Always gets Write Not Permitted (0x03)
7. **Standard BLE Just Works pairing alone** — Controller doesn't complete; needs 0x15 subcommand sequences
8. **Connection at home screen** — Fails silently if interval < 6 is rejected by our stack

---

## Next Action: Immediate Code Changes

### Priority 1: Complete the GATT Pair Command Sequence
The bridge already transitions through `STATE_WAIT_ENCRYPTION` → `STATE_CHAR_DISCOVERING` → `STATE_PAIRING`. Ensure:
1. Discover COMMAND_RESPONSE_UUID (`c765a961`) and subscribe to notifications
2. Send all 4 PAIR commands in sequence
3. Wait for response on each before proceeding
4. Then enable INPUT_REPORT_UUID notifications

### Priority 2: Check BTstack Connection Interval Validation
Search BTstack source for interval minimum validation. If found and it rejects < 6, patch to allow interval=4 from Switch 2 console.

### Priority 3: Handle Donor Controller Requirement
Document that the hardcoded LTK values (PAIR_KEY_1, PAIR_KEY_2) need to match a specific paired session. Without a donor controller, we cannot generate these keys ourselves.

### After pairing succeeds:
- Read controller info via COMMAND_MEMORY (address 0x00013000, size 0x40)
- Enable features: FEATURE_MOTION=0x04, FEATURE_MOUSE=0x10
- Set LEDs via COMMAND_LEDS (0x09)

---

## All URLs Found

| # | URL | Description |
|---|-----|-------------|
| 1 | https://leonsnotes.ca/2026/04/04/reverse-engineering-the-switch-2-pro-controllers-bluetooth-protocol/ | **NEW SESSION 2** — Full hardware RE writeup: interval=4, 0x15 LTK, 19-step activation, nRF52840 emulator |
| 2 | https://github.com/Nadeflore/switch2-controllers | PRIMARY — Original Switch 2 controller reverse engineering (Python/bleak) |
| 3 | https://github.com/CareyScott/switch2controllerpc | Windows app with ViGEmBus, same protocol |
| 4 | https://raw.githubusercontent.com/Nadeflore/switch2-controllers/main/controller.py | Full controller.py source (467 lines) |
| 5 | https://raw.githubusercontent.com/Nadeflore/switch2-controllers/main/discoverer.py | Discovery/pairing flow (109 lines) |
| 6 | https://github.com/bluekitchen/btstack/blob/master/example/sm_pairing_peripheral.c | BTstack canonical SMP pairing example — full source |
| 7 | https://github.com/bluekitchen/btstack/blob/master/src/ble/sm.h | BTstack SM API header |
| 8 | https://github.com/bluekitchen/btstack/blob/master/src/btstack_defines.h | BTstack event definitions |
| 9 | https://github.com/bluekitchen/btstack/blob/master/src/gap.h | BTstack GAP functions |
| 10 | https://github.com/bluekitchen/btstack/blob/master/src/hci.c | BTstack HCI layer |
| 11 | https://github.com/bluekitchen/btstack/blob/master/CHANGELOG.md | BTstack changelog |
| 12 | https://github.com/darthcloud/BlueRetro/issues/1249 | Switch 2 GATT service structure |
| 13 | https://github.com/HandHeldLegend/handheldlegend.github.io/blob/master/procon2tool/index.html | ProCon2Tool — SPI flash layout |
| 14 | https://github.com/MarcanBat2a/gamecube-remote-mac/blob/main/NSO_GC_BLE_PROTOCOL.md | NSO GC controller BLE protocol (related) |
| 15 | https://github.com/zephyrproject-rtos/zephyr/blob/main/subsys/bluetooth/host/Kconfig | Zephyr encryption-change event optional |
| 16 | https://community.infineon.com/t5/Knowledge-Base-Articles/Handling-GATT-attribute-security-permissions-requirements-in-Bluetooth-LE/ta-p/289525 | GATT security permissions for BLE |
| 17 | https://groups.google.com/g/btstack-dev/c/N43oaguv7zg | BTstack-dev: LE Security Manager discussion |
| 18 | https://github.com/espressif/esp-idf/blob/master/examples/bluetooth/bluedroid/ble/gatt_security_server/tutorial/ | ESP-IDF GATT security tutorial |
| 19 | https://old.reddit.com/r/switch2/comments/1l42pe5/ | Reddit: Switch 2 Pro Controller PC connectivity |
| 20 | https://github.com/zephyrproject-rtos/zephyr/issues/37228 | Zephyr SMP bug: pairing not propagated |
| 21 | https://github.com/zephyrproject-rtos/zephyr/discussions/41907 | Zephyr: reconnect paired Just Works device |
| 22 | https://stackoverflow.com/questions/78767932 | SO: Missing SMP packet for BLE pairing |
| 23 | https://stackoverflow.com/questions/62147384 | SO: Raspberry BLE encryption/pairing |
| 24 | https://device.report/m/575be3b618223159318f30e0d2d1a7d8a8cca1682d93599712bea00f4b370379 | BTstack POSIX H4 port — SM encryption events |
| 25 | https://bluekitchen-gmbh.com/btstack/examples/generated/ | BTstack example docs |
| 26 | https://bluekitchen-gmbh.com/btstack/protocols/ | BTstack protocol docs |
| 27 | https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/ | Bluetooth Core 5.4 spec — Encryption Change event |
| 28 | https://bluez-peripheral.readthedocs.io/en/stable/ref/gatt/characteristic.html | BlueZ GATT characteristic security |
| 29 | https://bluekitchen-gmbh.com/btstack/examples/examples.html | BTstack examples index |

---

## Risk Assessment

- **Risk Level:** Low (read-only protocol research)
- **Impact:** Fundamental approach confirmed — proprietary GATT pairing, not SMP
- **New Risk (Session 2):** **Donor Controller Required** — The 0x15 LTK is hardcoded per controller session; cannot be generated without a real paired controller or firmware RE
- **New Finding (Session 2):** **Connection Interval=4** — BTstack may silently reject connections from Switch 2 console at home screen; needs interval validation check and patch
- **Effort:** Phase B bridge code already structured correctly — needs PAIR command completion + interval fix
- **Urgency:** High — this unblocks the entire project

---

*Generated automatically by Hermes Enterprise research farming cron job (2 sessions consolidated).*
