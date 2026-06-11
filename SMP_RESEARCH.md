# SMP Encryption Research — Automated Farming

**Date:** 2026-06-11
**Session:** 3 (Automated Cron — 6 searches, 8 follow-up fetches)
**Context:** BTstack BLE daemon on Pi 5 (LibreELEC 13) → Nintendo Switch 2 Pro Controller (E0:EF:BF:3B:C6:76)
**Blocker:** SMP Encryption doesn't complete → Pair-Commands get `Write Not Permitted (0x03)`

---

## 🔥 CRITICAL FINDING: Switch 2 Pro Controller Uses Proprietary Pairing, NOT Standard SMP

**Source:** Leon's Notes — "Reverse-Engineering the Switch 2 Pro Controller's Bluetooth Protocol" (2026-04-04)
**URL:** https://leonsnotes.ca/2026/04/04/reverse-engineering-the-switch-2-pro-controllers-bluetooth-protocol/

### The 0x15 Problem (Root Cause)

When a real Pro Controller 2 pairs with a Switch 2 console for the first time, a **subcommand 0x15** handles the initial key exchange. This establishes the shared encryption key (LTK) between the controller and the console. The console never sees intermediate steps — the LTK computation happens inside the controller's firmware and gets stored on the controller's SPI flash.

**This is NOT standard SMP.** Standard SMP pairing (Just Works, Passkey Entry, Numeric Comparison) will never complete because the controller expects the proprietary 0x15 exchange instead of standard SMP key distribution.

### Connection Interval = 4 (Below BLE Spec Minimum)

The Switch 2 console uses a connection interval of **4** (5 milliseconds). The BLE spec defines a minimum interval of **6** (7.5 milliseconds). Every spec-compliant BLE stack (Zephyr, NimBLE, and potentially BTstack) **silently rejects** this. No error, no log, just silence.

**For BTstack:** This may need to be addressed in the HCI layer. BTstack may apply similar bounds checks during connection parameter validation.

### Known Workaround (Leon's Approach)

1. Extract **MAC address + LTK** from an already-paired real controller (donor)
2. Load them into the emulator/microcontroller
3. Console sees same MAC, receives same LTK → assumes same device
4. **No donor hardware = no encryption = no input**

### Security Model
- No challenge-response or anti-replay at application layer
- Nintendo's security relies **entirely on BLE-layer encryption** + physical possession of paired controller
- Application protocol trusts whatever comes through the encrypted pipe

### Zephyr-specific Key Storage Gotcha
- `BT_KEYS_LTK` — for centrals
- `BT_KEYS_PERIPH_LTK` — for peripherals
- Use wrong one → encryption silently fails with "PIN or Key Missing"

---

## BTstack-Specific Configuration & API

**Source:** `sm.h`, `sm_pairing_peripheral.c`, `bluetooth.h` from bluekitchen/btstack (master)

### Critical APIs for LTK Injection

| API | Purpose | Relevance |
|-----|---------|-----------|
| `sm_register_ltk_callback()` | Inject custom LTK on re-encryption | **🔥 Use this to inject donor LTK!** |
| `sm_allow_ltk_reconstruction_without_le_device_db_entry()` | Re-encrypt without LE Device DB | Enable for donor-style reconnect |
| `sm_set_secure_connections_only_mode(bool)` | Enable/disable SC-only | Disable for Legacy, Enable for SC |
| `sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT)` | Set IO caps | Matches Controller Just Works |
| `sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION)` | Set auth reqs | SC without bonding |
| `sm_request_pairing(con_handle)` | Manually trigger pairing | Call after connection |
| `sm_set_request_security(bool)` | Auto-request encryption on connect | Alternative to manual trigger |
| `sm_get_ltk(con_handle, ltk)` | Get LTK for encrypted connection | Verify after encryption |
| `sm_set_encryption_key_size_range(min, max)` | Set key size range | Default 16/16 |
| `gatt_client_set_required_security_level(LEVEL_2)` | Enforce encryption for GATT | Required for encrypted characteristics |

### SM Event Constants (btstack_defines.h)

```
SM_EVENT_JUST_WORKS_REQUEST         0xC8
SM_EVENT_PASSKEY_DISPLAY_NUMBER     0xC9
SM_EVENT_PASSKEY_DISPLAY_CANCEL     0xCA
SM_EVENT_PASSKEY_INPUT_NUMBER       0xCB
SM_EVENT_NUMERIC_COMPARISON_REQUEST 0xCC
SM_EVENT_IDENTITY_RESOLVING_STARTED 0xCD
SM_EVENT_IDENTITY_RESOLVING_FAILED  0xCE
SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED 0xCF
SM_EVENT_AUTHORIZATION_REQUEST      0xD0
SM_EVENT_AUTHORIZATION_RESULT       0xD1
SM_EVENT_KEYPRESS_NOTIFICATION      0xD2
SM_EVENT_IDENTITY_CREATED           0xD3
SM_EVENT_PAIRING_STARTED            0xD4
SM_EVENT_PAIRING_COMPLETE           0xD5
SM_EVENT_REENCRYPTION_STARTED       0xD6
SM_EVENT_REENCRYPTION_COMPLETE      0xD7
HCI_EVENT_ENCRYPTION_CHANGE         0x08
HCI_EVENT_ENCRYPTION_CHANGE_V2      0x59
```

### Auth Requirement Flags (bluetooth.h)

```
SM_AUTHREQ_NO_BONDING        0x00
SM_AUTHREQ_BONDING           0x01
SM_AUTHREQ_MITM_PROTECTION   0x04
SM_AUTHREQ_SECURE_CONNECTION 0x08
SM_AUTHREQ_KEYPRESS          0x10
SM_AUTHREQ_CT2               0x20
```

### IO Capabilities (bluetooth.h)

```
IO_CAPABILITY_DISPLAY_ONLY      = 0
IO_CAPABILITY_DISPLAY_YES_NO    = 1
IO_CAPABILITY_KEYBOARD_ONLY     = 2
IO_CAPABILITY_NO_INPUT_NO_OUTPUT = 3
```

### sm_pairing_peripheral.c — Event Handler Architecture

**CRITICAL:** The example shows two separate event handlers:
1. `hci_packet_handler` — registered via `hci_add_event_handler()` — handles HCI events including connection complete
2. `sm_packet_handler` — registered via `sm_add_event_handler()` — handles SM events (pairing, re-encryption, identity)

Both handlers can trigger `sm_request_pairing()` on connection complete. In the example it's commented out — you must explicitly call it or trigger via GATT client request.

### SM_EVENT_PAIRING_COMPLETE status codes:
- `ERROR_CODE_SUCCESS` — pairing complete, success
- `ERROR_CODE_CONNECTION_TIMEOUT` — failed, timeout
- `ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION` — failed, disconnected
- `ERROR_CODE_AUTHENTICATION_FAILURE` — failed, with reason code

### SM_EVENT_REENCRYPTION_COMPLETE status codes:
- `ERROR_CODE_SUCCESS` — re-encryption complete
- `ERROR_CODE_PIN_OR_KEY_MISSING` — bonding info missing → delete local bonding, allow new pairing

---

## Protocol Insights

### Standard SMP Flow (from BTstack sm_pairing_peripheral.c)
1. Connection Complete → `sm_request_pairing(con_handle)`
2. SM_EVENT_PAIRING_STARTED
3. SM_EVENT_JUST_WORKS_REQUEST → `sm_just_works_confirm(con_handle)`
4. SM_EVENT_PAIRING_COMPLETE (status=SUCCESS)
5. (or) SM_EVENT_REENCRYPTION_STARTED → SM_EVENT_REENCRYPTION_COMPLETE

### Why Standard SMP Fails with Switch 2 Controller

1. Controller expects **subcommand 0x15** for key exchange, not standard SMP key distribution
2. Even when SC is enabled, the controller won't complete the exchange because it's waiting for the proprietary handshake
3. The `Write Not Permitted (0x03)` on the command GATT characteristic confirms: the controller requires an encrypted link, which it won't establish via standard SMP

### BlueZ Reference — Successful Encryption Change Event (Issue #811)
```
> HCI Event: Encryption Change (0x08) plen 4
  Status: Success (0x00)
  Handle: 12
  Encryption: Enabled with E0 (0x01)
> HCI Event: Command Complete (0x0e) plen 7
  Read Encryption Key Size (0x05|0x0008)
  Status: Success (0x00)
  Key size: 16
```
This shows what a successful encryption completion looks like. The Encryption Change event fires with `Encryption: Enabled with E0 (0x01)` or `AES-CCM (0x02)` for LE Secure Connections.

---

## External Examples

### CareyScott/switch2controllerpc (Windows, Python)
- **URL:** https://github.com/CareyScott/switch2controllerpc
- Uses `bleak` BLE library for scanning (Nintendo manufacturer ID `0x0553`)
- Own pairing path (not Windows Settings Bluetooth) — "faster, lower-latency"
- ViGEmBus for virtual Xbox 360 / PS4 controller emulation
- Source code not directly accessible (404 on raw paths) — repo uses a different structure
- This is a consumer app using Windows Bluetooth stack, NOT BTstack

### aureliendesert/switch2bridge-macos (macOS, Python)
- **URL:** https://github.com/aureliendesert/switch2bridge-macos
- Bluetooth bridge for Switch 2 Pro Controller on macOS
- Source structure: `switch2bridge/` package
- Specific source files not accessible (404 on raw paths)

### BetterJoy Issue #1223 — Switch 2 Pro Controller Feature Request
- **URL:** https://github.com/Davidobot/BetterJoy/issues/1223
- Confirms: Pro Controller 2 via USB → BetterJoy doesn't recognize it
- Active feature request, not yet resolved

### Reddit Confirmation — No Standard PC/Mac Support
- r/switch2: "Switch 2 Pro Controller won't connect to PC or external devices"
- r/macgaming: "Switch 2 Pro controller doesn't seem to work on the Mac"
- r/Switch: "Is the Switch 2 Pro Controller compatible with anything besides the Switch 2?"
- **Consensus:** Controller uses proprietary protocol; standard Bluetooth pairing doesn't work

---

## Dead Ends Confirmed

| Approach | Status | Reason |
|----------|--------|--------|
| Standard SMP Just Works (no SC) | ❌ Dead | Authentication Failure (0x05) |
| Standard SMP Just Works (with SC) | ❌ Dead | Pairing runs but encryption never completes — controller expects 0x15 |
| Standard SMP Passkey Entry | ❌ Dead | Controller has NoInputNoOutput — not applicable |
| sm_set_secure_connections_only_mode(true) | ❌ Not Enough | Doesn't address the proprietary 0x15 handshake |
| sm_set_secure_connections_only_mode(false) | ❌ Not Enough | Legacy pairing also won't complete |
| Waiting for Encryption Change event | ❌ Stuck | Event never fires because SMP doesn't complete |

---

## Next Action — DONOR LTK APPROACH

### The ONLY known path forward:
1. **Obtain a donor Switch 2 Pro Controller** (the real hardware)
2. **Extract the MAC address (BDADDR) and LTK** from the paired controller
   - Leon's method: extract from controller's SPI flash or intercept during pairing with a sniffer
3. **Spoof the BDADDR** in BTstack to match the donor
4. **Inject the LTK** using `sm_register_ltk_callback()` in BTstack:
   ```c
   static bool my_ltk_callback(hci_con_handle_t con_handle, uint8_t addr_type, 
                                bd_addr_t addr, uint8_t *ltk) {
       // Hardcode the donor LTK
       memcpy(ltk, donor_ltk, 16);
       return true; // LTK was modified
   }
   
   // In setup:
   sm_register_ltk_callback(&my_ltk_callback);
   sm_allow_ltk_reconstruction_without_le_device_db_entry(1);
   ```
5. **On connection**, BTstack should try re-encryption with the injected LTK
6. **SM_EVENT_REENCRYPTION_COMPLETE** should fire with SUCCESS
7. Then GATT writes to the command characteristic should work

### Alternative (if donor extraction works):
- `sm_set_secure_connections_only_mode(false)` + `sm_set_authentication_requirements(SM_AUTHREQ_NO_BONDING)`
- Inject the LTK via callback — BTstack will use it for encryption without full SMP negotiation
- This bypasses the need for the 0x15 subcommand entirely

### If No Donor Hardware Available:
- **No known path exists.** The 0x15 subcommand is the gatekeeper for LTK establishment.
- Hardware reverse engineering (soldering debug wires, stepping through controller firmware) would be required.
- This is a different skill set and project entirely.

---

## All URLs

| # | URL | Description |
|---|-----|-------------|
| 1 | https://leonsnotes.ca/2026/04/04/reverse-engineering-the-switch-2-pro-controllers-bluetooth-protocol/ | **PRIMARY:** Complete RE of Switch 2 BLE protocol — 0x15 subcommand, interval=4, donor LTK approach |
| 2 | https://github.com/CareyScott/switch2controllerpc | Windows app using bleak + ViGEmBus for Switch 2 controllers on PC |
| 3 | https://github.com/aureliendesert/switch2bridge-macos | macOS Bluetooth bridge for Switch 2 Pro Controller |
| 4 | https://github.com/bluekitchen/btstack/blob/master/example/sm_pairing_peripheral.c | BTstack SM peripheral example — all pairing configs + event handling |
| 5 | https://github.com/bluekitchen/btstack/blob/master/src/ble/sm.h | BTstack SM API header — sm_register_ltk_callback, sm_request_pairing, etc. |
| 6 | https://github.com/bluekitchen/btstack/blob/master/src/btstack_defines.h | BTstack event constants — SM_EVENT_*, HCI_EVENT_ENCRYPTION_CHANGE |
| 7 | https://github.com/bluekitchen/btstack/blob/master/src/bluetooth.h | BTstack auth flags — SM_AUTHREQ_*, IO_CAPABILITY_* |
| 8 | https://github.com/bluekitchen/btstack/blob/master/src/hci.c | BTstack HCI layer — bonding flags, event handler management |
| 9 | https://github.com/bluez/bluez/issues/811 | BlueZ: Pairing fails on Read encryption size (BT 5.3) — shows successful Encryption Change trace |
| 10 | https://github.com/bluez/bluez/issues/371 | BlueZ: Unable to pair with BLE device — SMP trace showing Pairing Confirm/Random exchange |
| 11 | https://github.com/zephyrproject-rtos/zephyr/discussions/41907 | Zephyr: Failing to reconnect to paired JUST_WORKS BLE device |
| 12 | https://github.com/zephyrproject-rtos/zephyr/issues/4044 | Zephyr: Livelock in SMP pairing failed scenario — Pairing Response trace |
| 13 | https://github.com/zephyrproject-rtos/zephyr/issues/57980 | Zephyr: ESP32C3 BLE peripheral pairing fails with iOS — attribute permissions fix |
| 14 | https://github.com/espressif/esp-idf/blob/master/examples/bluetooth/bluedroid/ble/gatt_security_server/tutorial/Gatt_Security_Server_Example_Walkthrough.md | ESP-IDF GATT Security Server guide — attribute permissions + pairing flow |
| 15 | https://github.com/espressif/esp-idf/issues/1082 | ESP32: HCI Set Connection Encryption fails — reconnection link key timing issue |
| 16 | https://github.com/arduino-libraries/ArduinoBLE/pull/156 | ArduinoBLE: BLE Pairing and Encryption PR — auth req bits in pairing response |
| 17 | https://github.com/Davidobot/BetterJoy/issues/1223 | BetterJoy feature request: Support Switch 2 Pro Controller |
| 18 | https://github.com/ndeadly/MissionControl | MissionControl: Use controllers from other consoles on Switch |
| 19 | https://stackoverflow.com/questions/78767932 | SO: What SMP Packet is missing for BLE Pairing to finish? |
| 20 | https://stackoverflow.com/questions/62147384 | SO: Raspberry BLE Encryption / Pairing |
| 21 | https://stackoverflow.com/questions/77132455 | SO: Linux BlueZ BLE GATT Service encryption + indications |
| 22 | https://stackoverflow.com/questions/78615609 | SO: Bluetooth Pairing via BlueZ Just Works fails — Confirm Value Failed |
| 23 | https://stackoverflow.com/questions/68959377 | SO: Are GATT Event notifications possible without pairing? |
| 24 | https://stackoverflow.com/questions/67575000 | SO: In BLE GATT, detect when Long Term Keys are invalid? |
| 25 | https://docs.silabs.com/bluetooth/2.13/bluetooth-code-examples-stack-features-security/ | Silicon Labs: Pairing Processes Example |
| 26 | https://software-dl.ti.com/simplelink_cc2640r2_latest/docs/blestack/ble_user_guide/html/ble-stack-3.x/gapbondmngr.html | TI: GAP Bond Manager and LE Secure Connections |
| 27 | https://community.infineon.com/t5/Knowledge-Base-Articles/Handling-GATT-attribute-security-permissions-requirements-in-Bluetooth-LE/ta-p/289525 | Infineon: GATT attribute security permissions |
| 28 | https://community.infineon.com/t5/Smart-Bluetooth/Is-pairing-encryption-mandatory-to-allow-a-peer-to-write-in-GATT/td-p/110265 | Infineon: Is pairing/encryption mandatory for GATT write? |
| 29 | https://bluekitchen-gmbh.com/btstack/how_to/ | BTstack Manual — How to configure BTstack |
| 30 | https://bluekitchen-gmbh.com/btstack/examples/examples.html | BTstack Manual — Examples overview |
| 31 | https://bluekitchen-gmbh.com/btstack/v1.0/protocols/ | BTstack Manual v1.0 — Supported Protocols (SMP flow) |
| 32 | https://old.reddit.com/r/switch2/comments/1l42pe5/ | Reddit: Switch 2 Pro Controller won't connect to PC |
| 33 | https://old.reddit.com/r/macgaming/comments/1l4udry/ | Reddit: Switch 2 Pro controller doesn't work on Mac |
| 34 | https://old.reddit.com/r/Switch/comments/1se4hlv/ | Reddit: Is Switch 2 Pro Controller compatible with anything else? |
| 35 | https://steamcommunity.com/groups/SteamClientBeta/discussions/3/604158712017323997/ | Steam: Support for Switch 2 Pro Controller (beta discussion) |
| 36 | https://wiki.st.com/stm32mcu/wiki/Connectivity:STM32WB-WBA_BLE_security_-_out_of_band_pairing | STM32: BLE OOB pairing — P-256 public key event mask |
| 37 | https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host/security-manager-specification.html | Bluetooth Core Spec 5.4 — Security Manager Specification |
| 38 | https://www.pcgamer.com/how-to-use-a-nintendo-switch-pro-controller-on-pc/ | PC Gamer: Switch/Switch 2 Pro Controller on PC guide |

---

## Provider Effectiveness

| Provider | Rating | Notes |
|----------|--------|-------|
| web_search | ★★★★☆ | Good surface-level discovery — found Leon's Notes, CareyScott, Reddit, BTstack sources |
| GitHub Raw Source (curl raw.githubusercontent.com) | ★★★★★ | Excellent for BTstack API headers, examples. High value. |
| web_extract | ★☆☆☆☆ | **Failed on every attempt** — "SearXNG is a search-only backend and cannot extract URL content" |
| curl direct fetch | ★★★☆☆ | Works but returns raw HTML/CSS for JS-heavy pages (Leon's Notes, GitHub) — needs parsing |
| GitHub API | ★★★☆☆ | Works but requires separate parsing step due to pipe-to-python security restriction |

---

## Summary

**8 new leads discovered** — 38 URLs total across 6 primary searches and 8 follow-up fetches.

**Root cause confirmed:** The Switch 2 Pro Controller uses a proprietary 0x15 subcommand for LTK establishment, which is NOT standard SMP. Standard SMP pairing will never complete because the controller expects this custom exchange. The ONLY known workaround is Leon's donor-controller approach: extract BDADDR + LTK from a real paired controller and inject them via `sm_register_ltk_callback()`.

**Next step:** Obtain donor controller, extract LTK, inject via BTstack callback, verify SM_EVENT_REENCRYPTION_COMPLETE fires with SUCCESS, then test GATT write to command characteristic.
