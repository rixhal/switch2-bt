# Joycon2cpp vs switch2_ble_probe.py — Deep Init Sequence Comparison

## joycon2cpp Source (TheFrano)

File: `testapp/src/testapp.cpp`
Lines 966-1075 (ProController path), 337-370 (SendGenericCommand, SendCustomCommands)

## Complete Connection Flows Side-by-Side

### joycon2cpp Pro Controller 2 Connection Flow

```
1. BleConnect():
   - Scan for mfg_id=0x0553 + prefix 01 00 03 7E
   - BLE connect (Windows BLE stack)
   - GATT services: 10 retries × 500ms = up to 5s
   - Find inputChar (ab7de9be-...) and writeChar (649d4ac9-...)
   - RequestPreferredConnectionParameters(ThroughputOptimized)  ← NOT IN OUR PROBE
   - Return ConnectedJoyCon structure

2. SendCustomCommands(writeChar):          ← SAME BYTES, different delays
   [0c 91 01 02 00 04 00 00 ff 00 00 00]  ← WriteWithoutResponse
   sleep(500ms)                            ← SAME
   [0c 91 01 04 00 04 00 00 ff 00 00 00]  ← WriteWithoutResponse
   sleep(500ms)                            ← SAME

3. sleep(200ms)                            ← **EXTRA DELAY — NOT IN OUR PROBE**

4. SetPlayerLEDs(writeChar, 0x01):
   [09 91 01 07 00 08 00 00 01 00 00 00 00 00 00 00]  ← WriteWithoutResponse
   sleep(50ms) via SendGenericCommand

5. EmitSound(writeChar):
   [0a 91 01 02 00 08 00 00 04 00 00 00 00 00 00 00]  ← WriteWithoutResponse
   sleep(50ms) via SendGenericCommand

6. inputChar.ValueChanged = handler        ← Register callback

7. CCCD Write (Notify)                     ← **NOTIFICATIONS ENABLED LAST**
```

### switch2_ble_probe.py (default --mode procon2)

```
1. _scan(): mfg_id=0x0553, vendor=0x057E, PID=0x2069
2. _connect_with_retries(): BLE connect (BlueZ/bleak)
3. _discover_characteristics(): GATT services
4. _procon2_init():
   [0c 91 01 02 00 04 00 00 ff 00 00 00]  ← WriteWithoutResponse
   sleep(500ms)
   [0c 91 01 04 00 04 00 00 ff 00 00 00]  ← WriteWithoutResponse
   sleep(500ms)
   (optional) [09 91 01 07 ... LED]       ← WriteWithoutResponse, 50ms delay
   (optional) [0a 91 01 02 ... SOUND]     ← WriteWithoutResponse, 50ms delay

5. _subscribe(): start_notify(inputChar)   ← **NOTIFICATIONS ENABLED**
```

## ALL DIFFERENCES FOUND

### DIFFERENCE 1: Notification Enable Order (CRITICAL)

| Aspect | joycon2cpp | Our Probe (default) |
|--------|-----------|---------------------|
| Init writes sent | First | First |
| LED/Sound | Before notify | Before notify (default) |
| CCCD Notify enable | **LAST** (after all writes + handler registration) | After init writes |
| Handler registered | Before CCCD | During start_notify() |
| **Impact** | Controller sees all init before any reports flow | Controller could send reports while init still pending |

**Note:** Our `--notify-before-init` flag inverts this — subscribe FIRST, then init. This is OPPOSITE of joycon2cpp.

### DIFFERENCE 2: Extra 200ms Delay Between Init Commands and LED/Sound

| joycon2cpp | Our Probe |
|------------|-----------|
| Init commands → **sleep(200ms)** → LED → Sound | Init commands → (optional LED → Sound with 50ms each) |

The 200ms gap in joycon2cpp creates a clear separation between "feature select" commands (0x0c) and "output" commands (LED/Sound). Our probe runs LED and Sound immediately after the init commands with only 50ms delays.

### DIFFERENCE 3: LED/Sound Are MANDATORY in joycon2cpp

| joycon2cpp | Our Probe |
|------------|-----------|
| ALWAYS sends LED + Sound for ALL controller types | Optional behind `--no-led` / `--no-sound` flags |
| Part of the connection ritual | Considered optional "nice-to-have" |

### DIFFERENCE 4: Connection Parameter Tuning

| joycon2cpp | Our Probe |
|------------|-----------|
| `RequestPreferredConnectionParameters(ThroughputOptimized)` | Nothing |
| Requests higher throughput from BLE stack | Relies on default connection parameters |
| **Impact** | May improve report latency/reliability |

### DIFFERENCE 5: SendGenericCommand Internal Delay

| joycon2cpp | Our Probe |
|------------|-----------|
| `SendGenericCommand` has built-in **50ms sleep after every write** | No equivalent helper; each command specifies its own delay |
| Used by SetPlayerLEDs, EmitSound | LED/Sound delays set manually in PROCON2_INIT_COMMANDS list |

This is functionally equivalent for the commands we send, but joycon2cpp's pattern ensures ALL generic commands have at least 50ms post-write.

### DIFFERENCE 6: Advertising Filter Precision

| joycon2cpp | Our Probe |
|------------|-----------|
| mfg_id=0x0553 + prefix `01 00 03 7E` | mfg_id=0x0553 + vendor=0x057E + PID=0x2069 |
| Prefix matches bytes 0-3 of manufacturer data | Parses vendor_id at offset 3-4, PID at 5-6 |
| Functionally equivalent for Pro Controller 2 (0x057E:0x2069) | ✅ Same target |

### DIFFERENCE 7: GATT Discovery Retry Details

| joycon2cpp | Our Probe |
|------------|-----------|
| 10 retries, 500ms between, on GetGattServicesAsync | 10 retries, 500ms between, on client.services access |
| Functionally identical | ✅ |

### DIFFERENCE 8: ValueChanged Handler Registration

| joycon2cpp | Our Probe |
|------------|-----------|
| InputChar.ValueChanged = lambda (C++/WinRT event) | start_notify(char, callback) |
| Explicit two-step: register handler, then WriteCCCD(Notify) | bleak merges both into one call |
| Functionally equivalent | ✅ |

## NON-DIFFERENCES (Confirmed Equivalent)

- ✅ UUIDs match exactly: `ab7de9be-89fe-49ad-828f-118f09df7fd2` and `649d4ac9-8eb7-4e6c-af44-1ea54fe5f005`
- ✅ Init command bytes match exactly (0x0c 0x91 0x01 sub=0x02 and sub=0x04)
- ✅ LED command bytes match (0x09 0x91 0x01 0x07, data[0]=0x01)
- ✅ Sound command bytes match (0x0a 0x91 0x01 0x02, data[0]=0x04)
- ✅ Both use WriteWithoutResponse for all init commands
- ✅ Both use 500ms delay between the two init commands
- ✅ Both use 50ms delay after LED and Sound commands
- ✅ joycon2cpp does NOT send any pairing commands (no 0x15 SetMAC/Key1/Key2/Finish)
- ✅ joycon2cpp does NOT enable notifications BEFORE init writes
- ✅ joycon2cpp does NOT send any additional commands beyond what we capture

## RECOMMENDED CODE CHANGES

### 1. DEFAULT to notify-AFTER-init (CRITICAL)

Current behavior: subscribe after init (good — matches joycon2cpp)
But `--notify-before-init` flag exists and should be documented as divergent from working path.

### 2. Add 200ms delay between feature-select commands and LED/Sound

```python
# In _procon2_init(), after the two init commands:
commands = list(PROCON2_INIT_COMMANDS)
if not self.args.no_led or not self.args.no_sound:
    commands.append(("--- barrier ---", None, 0.200))  # 200ms gap
if not self.args.no_led:
    commands.append(("Set LED 1", PROCON2_LED_COMMAND, 0.05))
if not self.args.no_sound:
    commands.append(("Emit sound", PROCON2_SOUND_COMMAND, 0.05))
```

### 3. ALWAYS send LED + Sound by default

Remove `--no-led` / `--no-sound` flags or keep them but note they diverge from joycon2cpp's working path.

### 4. Add connection parameter tuning

```python
# After connect, before GATT discovery:
# BlueZ/bleak equivalent of RequestPreferredConnectionParameters(ThroughputOptimized)
# This may require direct D-Bus calls to BlueZ for connection parameter update
```

## TIMING DIAGRAM (joycon2cpp working path)

```
Connect ──► GATT discover ──► [cmd 0x0c/02] ──500ms──► [cmd 0x0c/04] ──500ms──►
──200ms──► [LED 0x09/07] ──50ms──► [Sound 0x0a/02] ──50ms──► 
──► Register ValueChanged handler ──► Write CCCD (Notify) ──► REPORTS FLOW 🎮
```

## TIMING DIAGRAM (Our current default)

```
Connect ──► GATT discover ──► [cmd 0x0c/02] ──500ms──► [cmd 0x0c/04] ──500ms──►
──► [LED 0x09/07] ──50ms──► [Sound 0x0a/02] ──50ms──► 
──► start_notify() ──► REPORTS FLOW 🎮
```

Key missing: the 200ms barrier between init group and output group.
