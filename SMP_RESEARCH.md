# SMP Encryption Research — Automated Farming
**Date:** 2026-06-05
**Researched by:** Hermes Enterprise (cron job)
**Sources:** web_search (8 searches, 42 results), GitHub raw source code extraction, Nadeflore switch2-controllers reverse engineering

---

## 🚨 CRITICAL FINDING: Switch 2 Uses Proprietary GATT Pairing, NOT SMP

**The Switch 2 Pro Controller does NOT use standard BLE SMP pairing.** Our entire approach of trying to complete SMP encryption is on the wrong protocol layer. The controller uses a Nintendo-proprietary command protocol over GATT.

### Evidence
- **Source:** [Nadeflore/switch2-controllers](https://github.com/Nadeflore/switch2-controllers) — reverse-engineered Switch 2 controller protocol
- **Source:** [CareyScott/switch2controllerpc](https://github.com/CareyScott/switch2controllerpc) — Windows app using the protocol
- **File:** `controller.py` lines 30-53, 306-384 — complete pairing flow

### Switch 2 GATT Characteristics (from reverse engineering)
```
INPUT_REPORT_UUID    = "ab7de9be-89fe-49ad-828f-118f09df7fd2"  ✅ matches ours
COMMAND_WRITE_UUID   = "649d4ac9-8eb7-4e6c-af44-1ea54fe5f005"  ✅ matches ours
COMMAND_RESPONSE_UUID = "c765a961-d9d8-4d36-a20a-5315b111836a"  ⚠️ we need this!
VIBRATION_WRITE_PRO  = "cc483f51-9258-427d-a939-630c31f72b05"
```

### Nintendo Advertising Format
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

### Switch 2 Pairing Protocol (Custom GATT Commands)
```
COMMAND_PAIR = 0x15

Step 1: SUBCOMMAND_PAIR_SET_MAC (0x01)
  Data: 0x00 0x02 + host_mac(6LE) + host_mac(6LE)
  Sets the host MAC address (Switch 2 sends two copies — 
  Switch console has 2 BT adapters)

Step 2: SUBCOMMAND_PAIR_LTK1 (0x04)
  Data: 0x00 ea bd 47 13 89 35 42 c6 79 ee 07 f2 53 2c 6c 31
  Hardcoded! Same for all controllers

Step 3: SUBCOMMAND_PAIR_LTK2 (0x02)
  Data: 0x00 40 b0 8a 5f cd 1f 9b 41 12 5c ac c6 3f 38 a0 73
  Hardcoded! Same for all controllers

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
# Check: response[0] == cmd_id and response[1] == 0x01
```

### Connection Flow (from discoverer.py)
1. Scan for BLE advertisements with Nintendo manufacturer ID (0x0553)
2. Connect via BLE (standard GATT connection — NO SMP pairing needed!)
3. Read COMMAND_RESPONSE_UUID notifications
4. Check reconnect_mac in advertisement:
   - If 0 → controller in pairing mode: run `pair()` protocol
   - If matches host MAC → already paired: skip pairing
5. Read controller info via memory read (COMMAND_MEMORY)
6. Enable INPUT_REPORT_UUID notifications → input data flows!

---

## BTstack-Specific Configuration (What We Should Be Doing)

### Correct Approach for BTstack Central Role
The Switch 2 controller does NOT require SMP encryption to communicate. The `write_command` on the bleak Python library succeeds because the OS Bluetooth stack (WinRT on Windows, BlueZ on Linux) handles whatever encryption handshake the controller needs — likely Just Works with the controller accepting any connection.

**Our BTstack approach should:**
1. Connect to controller via GAP LE (this works ✅)
2. Do NOT initiate SMP pairing — it's not needed and causes issues
3. Instead, send the Nintendo COMMAND_PAIR sequence over the command characteristic
4. The controller's write permission may require link encryption — but this is handled at the HCI level, not SMP

### Key BTstack APIs for GATT Client (Central Role)
```c
// From btstack_defines.h — SM events for reference
#define SM_EVENT_PAIRING_STARTED          0xD4u
#define SM_EVENT_PAIRING_COMPLETE         0xD5u  // status codes:
                                                //   ERROR_CODE_SUCCESS
                                                //   ERROR_CODE_CONNECTION_TIMEOUT
                                                //   ERROR_CODE_AUTHENTICATION_FAILURE
#define SM_EVENT_REENCRYPTION_STARTED     0xD6u
#define SM_EVENT_REENCRYPTION_COMPLETE    0xD7u

// Gap functions (from gap.h)
void gap_secure_connections_enable(bool enable);  // NOTE: BR/EDR only!
void gap_delete_bonding(bd_addr_type_t addr_type, bd_addr_t addr);
void gap_drop_link_key_for_bd_addr(bd_addr_t addr);

// SM setup (from sm_pairing_peripheral.c example)
sm_init();
sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION);  // LE SC
// OR for legacy:
sm_set_secure_connections_only_mode(false);
sm_set_authentication_requirements(0);  // Just Works, no SC

// Event handlers
sm_add_event_handler(&sm_event_callback_registration);  // SEPARATE handler!
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

---

## Protocol Insights

### Why SMP Was Failing
1. **gap_secure_connections_enable() is for BR/EDR only** — it doesn't affect BLE LE Secure Connections at all
2. For BLE LE SC, use `sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION)`
3. The Switch 2 controller may accept SMP pairing but its primary mechanism is the proprietary command protocol
4. The "Write Not Permitted (0x03)" error happens because the controller's GATT server requires link-layer encryption for the command characteristic, BUT the controller manages encryption as part of its proprietary pairing, not SMP

### Bluetooth LE Security Levels
- LEVEL_1: No security (open)
- LEVEL_2: Encryption with unauthenticated pairing (Just Works)
- LEVEL_3: Encryption with authenticated pairing (MITM)
- LEVEL_4: LE Secure Connections with authenticated pairing

The controller likely requires LEVEL_2 for command writes, achieved through its internal encryption handshake triggered by the PAIR commands, not SMP.

### Switch 2 Controller PID Values
```python
JOYCON2_RIGHT_PID       = 0x2066
JOYCON2_LEFT_PID        = 0x2067
PRO_CONTROLLER2_PID     = 0x2069
NSO_GAMECUBE_CONTROLLER = 0x2073
```

---

## External Examples

### BTstack sm_pairing_peripheral.c (canonical SMP example)
**File:** https://github.com/bluekitchen/btstack/blob/master/example/sm_pairing_peripheral.c

Shows complete SMP event handling:
- SM_EVENT_JUST_WORKS_REQUEST → `sm_just_works_confirm(handle)`
- SM_EVENT_PAIRING_COMPLETE → check status codes
- SM_EVENT_REENCRYPTION_COMPLETE → handle bonding info missing
- SM_EVENT_IDENTITY_CREATED / RESOLVING_*

Configuration examples for LE Legacy Just Works:
```c
sm_set_secure_connections_only_mode(false);
sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
sm_set_authentication_requirements(0);  // no bonding, no SC, no MITM
```

### Nadeflore switch2-controllers (ACTUAL WORKING IMPLEMENTATION)
**Repo:** https://github.com/Nadeflore/switch2-controllers
**File:** controller.py (467 lines)

This is the gold-standard reference. Complete implementation of:
- BLE connection (via bleak/WinRT)
- Nintendo pairing protocol (COMMAND_PAIR sequence)
- Controller memory read (for calibration, serial number)
- Input report parsing (buttons, sticks, gyro, accelerometer, magnometer, battery)
- Vibration control
- LED control
- Feature enable (motion, mouse)

### CareyScott switch2controllerpc (Packaged Windows App)
**Repo:** https://github.com/CareyScott/switch2controllerpc

Wraps Nadeflore code with ViGEmBus virtual controller support. Uses the same GATT UUIDs and pairing protocol. Includes gyro mouse, Joy-Con split/merge.

### Zephyr SMP Issues (for general SMP debugging reference)
- [Zephyr #37228](https://github.com/zephyrproject-rtos/zephyr/issues/37228): SMP pairing never propagated to app layer → 30s timeout with unspecified failure
- [Zephyr #41907](https://github.com/zephyrproject-rtos/zephyr/discussions/41907): Failing to reconnect to paired Just Works device
- [Zephyr #4044](https://github.com/zephyrproject-rtos/zephyr/issues/4044): Livelock in SMP pairing failed scenario

Common SMP failure pattern: remote device (nRF5340) completes pairing/encryption at controller level but pairing success never reaches application layer → 30s disconnect.

---

## Dead Ends Confirmed

1. **SMP-based pairing is unnecessary** — The Switch 2 controller has its own pairing protocol over GATT
2. **gap_secure_connections_enable() does NOT affect BLE** — it's for BR/EDR (Classic) only; use sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION) for LE SC
3. **Trying to complete SMP encryption before sending PAIR commands** — Wrong order. The controller's proprietary pairing is what enables encryption, not SMP
4. **SM_AUTHREQ_NO_BONDING with SC enabled** — Creates confusion; Just Works with NoInputNoOutput is best baseline
5. **Forcing SC when controller may not support it** — The controller uses hardcoded LTK values, suggesting it doesn't do real SC key generation
6. **Writing commands to 649d4ac9... before pairing** — Will always get Write Not Permitted; must send PAIR sequence first (the controller encrypts as part of this)

---

## Next Action: Immediate Code Change

**STOP trying to complete SMP encryption. Implement the Nintendo COMMAND_PAIR protocol instead:**

### Step-by-step for BTstack:
1. **Connect** via GAP LE (already working ✅)
2. **Discover GATT characteristics** (already working ✅ — handles 0x000a input, 0x0014 cmd)
3. **Also discover COMMAND_RESPONSE_UUID** (`c765a961-d9d8-4d36-a20a-5315b111836a`) and subscribe to notifications
4. **Send PAIR sequence** via `gatt_client_write_value_of_characteristic_without_response()` to command handle:
   ```
   // PAIR SET MAC (host BDADDR = pi's hci1 address)
   [0x15] [0x91 0x01] [0x01] [0x00] [0x0E] [0x00 0x00] [0x00 0x02] [host_mac_6LE] [host_mac_6LE]
   
   // PAIR LTK1
   [0x15] [0x91 0x01] [0x04] [0x00] [0x11] [0x00 0x00] [0x00 ea bd 47 13 89 35 42 c6 79 ee 07 f2 53 2c 6c 31]
   
   // PAIR LTK2
   [0x15] [0x91 0x01] [0x02] [0x00] [0x11] [0x00 0x00] [0x00 40 b0 8a 5f cd 1f 9b 41 12 5c ac c6 3f 38 a0 73]
   
   // PAIR FINISH
   [0x15] [0x91 0x01] [0x03] [0x00] [0x01] [0x00 0x00] [0x00]
   ```
5. **Wait for responses** on COMMAND_RESPONSE_UUID notification
6. **Then** enable INPUT_REPORT_UUID notification to get input data
7. **Input report format**: See ControllerInputData class (60 bytes, starts with 4-byte timestamp, 4-byte buttons)

### After pairing succeeds:
- Read controller info via COMMAND_MEMORY (address 0x00013000, size 0x40)
- Enable features via COMMAND_FEATURE (0x0c): FEATURE_MOTION=0x04, FEATURE_MOUSE=0x10
- Set LEDs via COMMAND_LEDS (0x09)

---

## All URLs Found

| # | URL | Description |
|---|-----|-------------|
| 1 | https://github.com/Nadeflore/switch2-controllers | **PRIMARY** — Original Switch 2 controller reverse engineering (Python/bleak) |
| 2 | https://github.com/CareyScott/switch2controllerpc | Wrapped Windows app with ViGEmBus, same protocol |
| 3 | https://raw.githubusercontent.com/Nadeflore/switch2-controllers/main/controller.py | Full controller.py source (467 lines) — pairing, commands, input parsing |
| 4 | https://raw.githubusercontent.com/Nadeflore/switch2-controllers/main/discoverer.py | Discovery/pairing flow (109 lines) — ad parsing, reconnect_mac logic |
| 5 | https://github.com/bluekitchen/btstack/blob/master/example/sm_pairing_peripheral.c | BTstack canonical SMP pairing example |
| 6 | https://github.com/bluekitchen/btstack/blob/master/src/ble/sm.h | BTstack SM API header — all sm_* functions |
| 7 | https://github.com/bluekitchen/btstack/blob/master/src/btstack_defines.h | BTstack event definitions incl. SM_EVENT_* and HCI_EVENT_ENCRYPTION_CHANGE |
| 8 | https://github.com/bluekitchen/btstack/blob/master/src/gap.h | BTstack GAP functions: gap_secure_connections_enable, gap_delete_bonding |
| 9 | https://github.com/bluekitchen/btstack/blob/master/src/hci.c | BTstack HCI layer — event handler registration order |
| 10 | https://github.com/bluekitchen/btstack/blob/master/CHANGELOG.md | BTstack changelog — SM fixes, encryption changes |
| 11 | https://bluekitchen-gmbh.com/btstack/examples/generated/ | BTstack example docs — GATT client setup |
| 12 | https://bluekitchen-gmbh.com/btstack/protocols/ | BTstack protocol docs — SMP/encryption overview |
| 13 | https://github.com/espressif/esp-idf/blob/master/examples/bluetooth/bluedroid/ble/gatt_security_server/tutorial/ | ESP-IDF GATT security tutorial — pairing events flow |
| 14 | https://github.com/zephyrproject-rtos/zephyr/issues/37228 | Zephyr SMP bug: pairing complete not propagated to app |
| 15 | https://github.com/zephyrproject-rtos/zephyr/discussions/41907 | Zephyr: failing to reconnect paired Just Works device |
| 16 | https://stackoverflow.com/questions/78767932 | SO: Missing SMP packet for BLE pairing to finish |
| 17 | https://stackoverflow.com/questions/62147384 | SO: Raspberry BLE encryption/pairing |

---

## Risk Assessment

- **Risk Level:** Low (read-only protocol research)
- **Impact:** Fundamental approach change needed — replace SMP pairing with Nintendo command protocol
- **Effort:** Medium — new command state machine, command response handling, input report parsing
- **Urgency:** High — this unblocks the entire project

---

*Generated automatically by Hermes Enterprise research farming cron job.*
