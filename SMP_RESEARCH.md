# SMP Encryption Research — Automated Farming

**Date:** 2026-06-12
**Session:** 4 (Cron — 6 searches, 10 fetched sources) 
**Session 3:** 2026-06-11 (38 URLs, donor LTK approach identified)
**Context:** BTstack BLE daemon on Pi 5 (LibreELEC 13) → Nintendo Switch 2 Pro Controller (E0:EF:BF:3B:C6:76)
**Blocker:** SMP Encryption doesn't complete → Pair-Commands get `Write Not Permitted (0x03)`

---

## 🔥🔥 CRITICAL BREAKTHROUGH — Session 4: FULL PROTOCOL DOCUMENTED (ndeadly)

**Source:** ndeadly/switch2_controller_research — THE authoritative protocol reference
**URLs:** Multiple raw.githubusercontent.com files (see All URLs below)

### Root Cause Confirmed (Updated)

The Switch 2 Pro Controller does NOT use standard SMP at all. Nintendo implemented a **proprietary pseudo-Out-Of-Band pairing protocol** over the HID command interface (characteristic `649d4ac9-8eb7-4e6c-af44-1ea54fe5f005`, handle 0x0014).

**Any attempt to use SMP will cause the controller to disconnect:**
> "Attempting to pair controllers using SMP (as many platforms do automatically) will cause the controller to terminate the connection." — ndeadly, bluetooth_interface.md

This explains why our BTstack setup never completed encryption — BTstack attempts SMP on connection, the controller drops the connection.

### CRITICAL: Pairing is OPTIONAL for basic communication
> "Pairing itself is optional for communicating with the controller and receiving notifications etc. via PC and other devices, but the Switch 2 console requires successful pairing in order to complete initialisation."

This means: for our use case (reading input from the controller on Pi 5), we may NOT need pairing at all IF the command characteristic (0x0014) accepts unencrypted writes. However, our tests show `Write Not Permitted (0x03)`, which suggests the controller DOES require encryption for the command characteristic — but this may be BTstack auto-triggering SMP first and the controller disconnecting before we even try the write.

### The Chicken-and-Egg Problem

To pair, we must send 0x15 subcommands via characteristic 0x0014.
But 0x0014 requires encryption.
To encrypt, we need SM pairing.
But SM pairing causes the controller to disconnect.

**AND YET** — ndeadly's Python viewer script works on unpaired controllers (it's a "connecting Nintendo Switch 2 controllers" tool). This means on platforms that DON'T auto-trigger SMP, the command characteristic CAN be written to without encryption.

**The real fix for BTstack:** Suppress automatic SMP pairing entirely. Connect with no security. Write commands to 0x0014 with `gatt_client_write_value_of_characteristic_without_response()`. Do NOT call `sm_request_pairing()` or `sm_set_request_security()`.

---

## 🔥 COMPLETE PROPRIETARY PAIRING PROTOCOL (Command 0x15)

Source: ndeadly/switch2_controller_research/commands.md

### Command Header Format (all commands)
| Offset | Size | Value           | Comment                                                     |
|--------|------|-----------------|-------------------------------------------------------------|
| 0x0    | 0x1  | Command ID      | 0x15 = Bluetooth Pairing                                    |
| 0x1    | 0x1  | Direction       | 0x91 = Host→Device (request), 0x01 = Device→Host (response) |
| 0x2    | 0x1  | Transport       | 0x00=USB, 0x01=Bluetooth                                    |
| 0x3    | 0x1  | Subcommand ID   | 0x01-0x04                                                   |
| 0x4    | 0x1  | Unknown         | -                                                           |
| 0x5    | 0x1  | Data Length/ACK | Request: length of data, Response: ACK                      |
| 0x6    | 0x2  | Reserved        | Always 0x0000                                               |

### Subcommand 0x01 — Exchange Addresses
**Request:**
| Offset | Size        | Value        | Comment                                        |
|--------|-------------|--------------|------------------------------------------------|
| 0x0    | 0x1         | 0x00         | Unknown, always 0x00                           |
| 0x1    | 0x1         | Count        | Number of addresses (max 2)                    |
| 0x2    | 0x6 × Count | Address list | Host BDADDRs (reverse byte-order)              |

**Example (2 addresses, first byte-reversed):**
```
15 91 01 01 00 0e 00 00  00 02 81 eb 3a eb f1 48  80 eb 3a eb f1 48
```

**Response:**
| Offset | Size | Value    | Comment                                     |
|--------|------|----------|---------------------------------------------|
| 0x0    | 0x1  | 0x01     | Always 0x01                                 |
| 0x1    | 0x1  | Unknown  | -                                           |
| 0x2    | 0x1  | Count?   | Always 0x01                                 |
| 0x3    | 0x6  | Address  | Controller BDADDR (reverse byte-order)      |

### Subcommand 0x04 — Exchange Keys
**Request:** Host public key A1 (16 bytes, reverse byte-order)
**Request data:** `00 [16-byte host key A1, byte-reversed]`

**Example:** Host sends A1 = `3503e92982877124bea80c664615834b` (reversed for transmission)
```
15 91 01 04 00 11 00 00  00 35 03 e9 29 82 87 71 24 be a8 0c 66 46 15 83 4b
```

**Response:** Controller key B1 — **ALWAYS FIXED:**
```
15 01 01 04 10 78 00 00  01 5c f6 ee 79 2c df 05 e1 ba 2b 63 25 c4 1a 5f 10
```
**B1 (raw, not reversed): `5CF6EE792CDF05E1BA2B6325C41A5F10`**

### LTK Computation
```
LTK = A1 XOR B1   (both in native byte order)
```
The LTK is computed by XORing the host's public key with the controller's fixed public key.

### Subcommand 0x02 — Confirm LTK
**Request:** Challenge A2 (16 bytes, reverse byte-order)
**Request data:** `00 [16-byte challenge A2, byte-reversed]`

The controller computes: `B2 = AES_128_ECB_encrypt(A2[::-1], key=LTK[::-1])`
Both A2 and LTK are byte-reversed for the AES computation.

**Example:**
```
Request:  15 91 01 02 00 11 00 00  00 6f c6 df 8a d8 fe df 15 bb 8c 15 e9 1f 32 05 44
Response: 15 01 01 02 10 78 00 00  01 13 4c 97 f5 11 b9 b6 dd 4d 86 fd 40 f5 36 e9 ed
```

### Subcommand 0x03 — Finalise Pairing
**Request:** `00`
**Response:** `01`

Commits host addresses and LTK to controller flash at address 0x1FA000.

### Complete Working Python Code (from ndeadly)
```python
from Crypto.Cipher import AES

A1 = bytes.fromhex("3503e92982877124bea80c664615834b")  # Host public key
B1 = bytes.fromhex("5cf6ee792cdf05e1ba2b6325c41a5f10")  # Controller fixed public key
A2 = bytes.fromhex("6fc6df8ad8fedf15bb8c15e91f320544")  # Host challenge

LTK = bytes(a ^ b for a, b in zip(A1, B1))  # XOR
B2  = AES.new(LTK[::-1], AES.MODE_ECB).encrypt(A2[::-1])

print("LTK:", LTK.hex())
print("B2: ", B2.hex())
```

---

## 🔥 BYPASS PATH: Command 0x03 Subcommand 0x07 — Send Pairing Info

This allows COMPLETELY bypassing the 0x15 exchange:
1. Send `0x03/0x07`: Host BDADDR (6 bytes, byte-reversed) + LTK (16 bytes, byte-reversed)
2. Send `0x03/0x09`: Store Pairing Info
3. Controller now has the LTK for BLE-level encryption

**This is the preferred path for BTstack:** Pre-compute the LTK (pick random A1, XOR with known B1 = `5CF6EE792CDF05E1BA2B6325C41A5F10`), inject via 0x03/0x07, then use `sm_register_ltk_callback()` to provide the same LTK to BTstack for BLE encryption.

---

## RECOMMENDED APPROACH FOR BTSTACK

### Phase 1: Bypass SMP, Just Write Commands (Try First)
```c
// In HCI packet handler on connection complete:
// DO NOT call sm_request_pairing()
// DO NOT call sm_set_request_security()

// Just discover GATT and write commands to handle 0x0014
gatt_client_write_value_of_characteristic_without_response(
    callback, connection_handle, 0x0014, length, data);
```

This may work because:
- ndeadly's Python viewer works on unpaired controllers
- The "Write Not Permitted" we saw may have been BTstack disconnecting due to SMP attempt, not the controller rejecting the write
- If the controller accepts unencrypted command writes, we're done — no encryption needed for input reports either

### Phase 2: If 0x0014 Requires Encryption

If Phase 1 fails (command writes rejected), implement the pairing bypass:

1. **Compute LTK without controller interaction:**
   - Pick any random 16-byte A1
   - B1 = `5CF6EE792CDF05E1BA2B6325C41A5F10` (fixed, known)
   - LTK = A1 XOR B1

2. **Inject LTK into BTstack via callback:**
   ```c
   static bool my_ltk_callback(hci_con_handle_t con_handle, uint8_t addr_type,
                                bd_addr_t addr, uint8_t *ltk) {
       // MAC check for our controller
       static const uint8_t target_mac[] = {0x76, 0xC6, 0x3B, 0xBF, 0xEF, 0xE0};
       if (memcmp(addr, target_mac, 6) == 0) {
           memcpy(ltk, precomputed_ltk, 16);
           return true;
       }
       return false;
   }
   
   sm_register_ltk_callback(&my_ltk_callback);
   sm_allow_ltk_reconstruction_without_le_device_db_entry(1);
   ```

3. **Send pairing info to controller via 0x03/0x07:**
   ```
   Header: 03 91 01 07 00 16 00 00
   Data:   [BDADDR byte-reversed 6 bytes] [LTK byte-reversed 16 bytes]
   Total:  22 bytes data
   ```

4. **Then send 0x03/0x09 to store:**
   ```
   Header: 03 91 01 09 00 00 00 00
   Data:   (none)
   ```

5. **After pairing info stored, re-encrypt the BLE link:**
   - BTstack should use the injected LTK for encryption
   - SM_EVENT_REENCRYPTION_COMPLETE should fire with SUCCESS

### Phase 3: No-Donor Approach (Self-Generated LTK)

The key insight: **we don't need a donor controller**. We can generate our own LTK:
1. Generate random A1
2. XOR with known B1 to get LTK
3. Inject into BTstack
4. Send to controller via 0x03/0x07
5. Both sides now have the same LTK → encrypted link works

---

## Protocol Insights

### Connection Interval = 4 (Below BLE Spec)
The Switch 2 console uses connection interval 4 (5ms), below BLE spec minimum 6 (7.5ms). BTstack may apply bounds checking that rejects this. Set connection parameters explicitly.

### Controller Fixed Key
B1 = `5CF6EE792CDF05E1BA2B6325C41A5F10` — this is hardcoded in controller firmware and NEVER changes. Verified by transmission capture.

### Security Model
- No challenge-response anti-replay at application layer
- Security relies entirely on BLE-layer encryption + physical possession
- Application protocol trusts whatever comes through encrypted pipe
- Pairing stores host BDADDRs + LTK at flash 0x1FA000

### Pairing is NOT Required for:
- Receiving input report notifications (0x000A, 0x000E)
- Reading device info characteristics
- Basic controller operation on PC platforms

### Pairing IS Required for:
- Auto-reconnection to Switch 2 console
- Wake-from-sleep functionality
- Writing to command characteristic 0x0014 (may require encryption)
- Full initialization by console

---

## BTstack-Specific Configuration

| API | Purpose | When to Use |
|-----|---------|------------|
| `sm_register_ltk_callback()` | Inject custom LTK for re-encryption | Phase 2+3 |
| `sm_allow_ltk_reconstruction_without_le_device_db_entry()` | Re-encrypt without device DB | Phase 2+3 |
| `sm_set_request_security(false)` | Prevent auto security request | Phase 1 |
| DO NOT call `sm_request_pairing()` | Suppress SMP | Phase 1 |
| `gatt_client_write_value_of_characteristic_without_response()` | Write to 0x0014 without encryption | Phase 1 |

### SM Events to Watch
```
SM_EVENT_PAIRING_STARTED        0xD4  — Should NOT fire (no SMP)
SM_EVENT_REENCRYPTION_STARTED   0xD6  — Should fire in Phase 2+3
SM_EVENT_REENCRYPTION_COMPLETE  0xD7  — SUCCESS = encryption working
HCI_EVENT_ENCRYPTION_CHANGE     0x08  — BLE-layer encryption change
HCI_EVENT_ENCRYPTION_CHANGE_V2  0x59  — Extended version
```

---

## External Examples (New in Session 4)

### ndeadly/switch2_controller_research — Authoritative Protocol Reference
- **URL:** https://github.com/ndeadly/switch2_controller_research
- **Status:** ACTIVE research repo, updated regularly
- **Contents:** bluetooth_interface.md, commands.md, hid_reports.md, memory_layout.md, descriptors.md
- **Key:** Developer of MissionControl (Switch homebrew controller support), authoritative source
- **Value:** ⭐⭐⭐⭐⭐ Complete protocol spec with byte-level detail

### ndeadly Gist — Working Python Viewer
- **URL:** https://gist.github.com/ndeadly/7d27aa63e2f653a902a2474dbcbc08b3
- **Status:** Functional Qt5 viewer using bleak
- **Value:** ⭐⭐⭐⭐ Shows connection + command sending on unpaired controllers (no SMP)

### bitaxislabs/Switch2BLE — iOS Reference App
- **URL:** https://github.com/bitaxislabs/Switch2BLE
- **Status:** Implements full proprietary BLE pairing + input parsing
- **Value:** ⭐⭐⭐⭐ iOS implementation shows CoreBluetooth approach

### alexvnesta/switch2controller — Wake from Sleep
- **URL:** https://github.com/alexvnesta/switch2controller
- **Status:** Implements wake packet + cites ndeadly as authoritative source
- **Value:** ⭐⭐⭐ Cross-references ndeadly protocol

---

## Dead Ends Confirmed

| Approach | Status | Reason |
|----------|--------|--------|
| Standard SMP (any auth req) | ❌ DEAD | Controller disconnects on SMP attempt |
| `sm_set_secure_connections_only_mode()` | ❌ IRRELEVANT | SMP is entirely the wrong protocol |
| Waiting for HCI_EVENT_ENCRYPTION_CHANGE from SMP | ❌ DEAD | Will never fire — controller doesn't do SMP |
| `gap_secure_connections_enable()` | ❌ IRRELEVANT | Same — SMP is wrong protocol |
| Donor controller extraction (old approach) | ⚠️ UNNECESSARY | B1 is fixed and known; no donor needed |

---

## Next Actions (Priority Order)

1. **TRY PHASE 1 FIRST:** Suppress BTstack SMP entirely. Connect with no security. Try writing to 0x0014. If it works, profit — no encryption needed for basic input reading.
2. **IF PHASE 1 FAILS (Write Not Permitted):** Implement Phase 2 — pre-compute LTK from known B1, inject via `sm_register_ltk_callback()`, send to controller via 0x03/0x07.
3. **Verification:** After encryption established, send command 0x0C/0x00 (Feature Select) to enable button/stick/IMU reporting. Check input report notifications on 0x000A and 0x000E.
4. **Connection parameters:** Set CI=6 (minimum spec-compliant) — console uses CI=4 which may be rejected by BTstack.

## All URLs (Consolidated: Sessions 1-4)

| # | URL | Description | Session |
|---|-----|-------------|---------|
| 1 | https://github.com/ndeadly/switch2_controller_research/blob/master/bluetooth_interface.md | Complete BLE interface spec (GATT table, advertisements, pairing protocol) | S4 ⭐ |
| 2 | https://github.com/ndeadly/switch2_controller_research/blob/master/commands.md | Complete command reference (all subcommands, 0x15 pairing, 0x03 bypass) | S4 ⭐ |
| 3 | https://github.com/ndeadly/switch2_controller_research/blob/master/hid_reports.md | Complete HID report formats for all controller types | S4 ⭐ |
| 4 | https://github.com/ndeadly/switch2_controller_research/blob/master/README.md | Repo overview, Discord link | S4 |
| 5 | https://gist.github.com/ndeadly/7d27aa63e2f653a902a2474dbcbc08b3 | Working Python BLE viewer for Switch 2 controllers | S4 ⭐ |
| 6 | https://github.com/bitaxislabs/Switch2BLE | iOS reference app implementing proprietary BLE pairing | S4 |
| 7 | https://github.com/alexvnesta/switch2controller | Wake-from-sleep + protocol cross-reference | S4 |
| 8 | https://leonsnotes.ca/2026/04/04/reverse-engineering-the-switch-2-pro-controllers-bluetooth-protocol/ | Original RE article — 0x15 subcommand, interval=4, donor approach | S3 |
| 9 | https://github.com/bluekitchen/btstack/blob/master/example/sm_pairing_peripheral.c | BTstack SM peripheral example | S3 |
| 10 | https://github.com/bluekitchen/btstack/blob/master/src/ble/sm.h | BTstack SM API header | S3 |
| 11 | https://github.com/bluekitchen/btstack/blob/master/src/btstack_defines.h | BTstack event constants | S3 |
| 12 | https://github.com/bluekitchen/btstack/blob/master/src/bluetooth.h | BTstack auth flags | S3 |
| 13 | https://github.com/CareyScott/switch2controllerpc | Windows bleak+ViGEmBus for Switch 2 | S3 |
| 14 | https://github.com/aureliendesert/switch2bridge-macos | macOS Bluetooth bridge | S3 |
| 15 | https://github.com/Davidobot/BetterJoy/issues/1223 | BetterJoy S2 Pro Controller feature request | S3 |
| 16 | https://github.com/Nohzockt/Switch2-Controllers | Python tool for Switch controllers (Switch 1 PIDs) | S3 |
| 17 | https://github.com/ndeadly/MissionControl | MissionControl — Switch homebrew controller support | S3 |
| 18-38 | [See Session 3 report for URLs 18-38] | BlueZ issues, Zephyr, ESP32, Stack Overflow, BT spec | S1-3 |

---

## Provider Effectiveness

| Provider | Rating | Notes |
|----------|--------|-------|
| web_search | ★★★★☆ | Session 4: Found ndeadly repos, bitaxislabs, alexvnesta — excellent discovery |
| GitHub Raw Source (curl) | ★★★★★ | Essential — pulled ndeadly protocol docs directly. Highest value source. |
| web_extract | ★☆☆☆☆ | Still broken — "SearXNG is a search-only backend" |
| Direct URL fetch | ★★★★☆ | Required for raw.githubusercontent.com content |

## Session 4 Summary

**Massive breakthrough.** ndeadly's switch2_controller_research provides the COMPLETE proprietary protocol specification. The pairing protocol (Command 0x15) is fully documented with byte-level detail, working Python code, and captured transmissions. The controller's fixed public key B1 is known and never changes. A bypass path (0x03/0x07) exists to inject LTK directly without the full 0x15 exchange. Most critically: **SMP is the wrong protocol** — the controller disconnects when SMP is attempted. The fix is to suppress BTstack's SMP entirely and either (a) write commands unencrypted (Phase 1), or (b) pre-compute LTK and inject via callback (Phase 2).

**New leads: 7** (ndeadly research repo + gist, bitaxislabs, alexvnesta, plus detailed protocol documentation)
**Total URLs across all sessions: 38+7 = 45**
