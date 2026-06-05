# Troubleshooting Guide — Switch 2 Pro Controller BLE

Symptom → Diagnosis → Fix reference for `switch2d.py` (BlueZ/bleak → uinput).

See [HARDWARE-TEST.md](HARDWARE-TEST.md) for the step-by-step validation
checklist. See [README.md](../README.md) for project overview and all CLI flags.

---

## No controller found in scan

**Symptom:** `stage_scan` returns ❌ with `reason=no Nintendo controller found`.

**Diagnosis:**
- Is the controller in pairing/advertising mode? LEDs must be cycling.
- Hold the SYNC button (small recessed button on top edge) for 3+ seconds until
  the home-button LEDs start blinking.
- If controller was previously paired via USB to a Switch 2, it may not
  advertise. Connect it to USB first (which initializes Bluetooth), then
  disconnect and try again.
- Verify the Bluetooth adapter sees *any* BLE advertisements:

```bash
sudo bluetoothctl scan le
# or
sudo hcitool lescan
```

**Fix:**
1. Hold SYNC button firmly until LEDs cycle.
2. Try USB-init: plug controller into Switch 2 dock or PC via USB-C briefly,
   unplug, then SYNC.
3. Increase scan timeout: `--scan-timeout 30`
4. If `bluetoothctl scan le` also shows nothing, the host Bluetooth adapter or
   antenna may be the problem — not the controller.

---

## Connect timeout

**Symptom:** `stage_connect` returns ❌ with `reason=all retries exhausted`.
Log shows `connect timeout` or `connect failed`.

**Diagnosis:**
- Is `bluetoothd` running?

```bash
systemctl status bluetooth
ps aux | grep bluetoothd
```

- Is the BLE address correct? Run `--diagnose --verbose` to see the address
  found during scan — it must match.
- On Raspberry Pi with CYW43455: raw HCI `LE Create Connection` is blocked by
  firmware (`Command Disallowed 0x0C`). `switch2d.py` uses BlueZ/bleak (MGMT
  path) specifically to bypass this — verify you're running `switch2d.py`, not
  the raw-HCI alternatives.
- Try connecting manually with `bluetoothctl`:

```bash
bluetoothctl
[bluetooth]# connect E0:EF:BF:3B:C6:76
```

**Fix:**
1. Start bluetoothd if stopped: `systemctl start bluetooth`
2. Verify address: `--address E0:EF:BF:3B:C6:76` (use the one from your scan)
3. Increase retries: `--connect-retries 5`
4. If `bluetoothctl connect` also fails, `bluetoothd` may need a restart:
   `systemctl restart bluetooth`
5. On Raspberry Pi: ensure you're using `switch2d.py` (BlueZ path), not any of
   the raw-HCI programs (`legacy/raw-hci/sw2d_final.c`, `sw2d.c`).

---

## GATT service discovery returns empty

**Symptom:** `stage_discover` returns ❌ with `reason=no services discovered`.
Services list is empty or `client.services` raises an exception.

**Diagnosis:**
- This is an **encryption gate** issue. The Switch 2 Pro Controller may require
  pairing/bonding before exposing GATT services. However, `switch2d.py` does
  **not** perform SMP pairing — the CareyScott protocol works without it.
- If the controller was previously paired to another host, it may be waiting
  for an encrypted connection. Remove any stale bonds:

```bash
bluetoothctl
[bluetooth]# remove E0:EF:BF:3B:C6:76
[bluetooth]# power off && power on
```

- Check if services are accessible with a generic GATT tool:

```bash
# Using gatttool (deprecated but useful for diagnosis)
gatttool -b E0:EF:BF:3B:C6:76 --primary

# Using bluetoothctl GATT
bluetoothctl
[bluetooth]# menu gatt
[bluetooth]# list-attributes
```

- Controller state: if the controller is asleep or not in the right mode, GATT
  discovery may return empty or partial results. Ensure LEDs are still on
  (solid or cycling) after connection.

**Fix:**
1. Remove stale bonds: `bluetoothctl remove <BDADDR>`
2. Power-cycle controller: hold power button until off, then SYNC again.
3. Increase GATT retries: `--gatt-retries 20`
4. Try with `--profile macos` first (skip init) to isolate whether discovery alone
   works. If services appear with `--profile macos` but not with `--profile
   joycon2cpp`, the issue is post-connect controller state.
5. As a last resort, try the BTstack bridge (`switch2_btstack_bridge.c`) which
   handles GATT discovery differently (see README § "Alternative Paths").

---

## Command write characteristic not found

**Symptom:** `stage_discover` returns ❌ with
`reason=command write UUID 649d4ac9-... not found`.

**Diagnosis:**
- Is the controller in the right mode? The CareyScott/joycon2cpp UUIDs are
  specific to **Pro Controller 2** (PID `0x2069`). Joy-Con (original Switch)
  uses different UUIDs (ndeadly/MissionControl protocol).
- GATT discovery may have returned only a partial service tree. Check the
  `services=N chars=N` fields in the fail log — if `chars` is very low (<10),
  the controller may not have fully exposed its GATT tree.
- Run with `--verbose` to see every characteristic discovered. Look for any
  UUID containing `649d4ac9`.

**Fix:**
1. Verify you have a Switch 2 Pro Controller (not original Switch Pro
   Controller, not Joy-Con). The scan should show PID `0x2069`.
2. Retry GATT discovery: `--gatt-retries 20`
3. If input report UUID *is* found but command write is not, the controller
   may be in a read-only mode. Try USB-init (plug into USB briefly, unplug,
   SYNC again).
4. Run with `--profile macos` to verify discovery works without init — this at
   least confirms the BLE path is functional.

---

## Notification subscription fails

**Symptom:** `stage_subscribe` returns ❌ with `reason=all subscribe attempts
failed`.

**Diagnosis:**
- CCCD (Client Characteristic Configuration Descriptor) write failed. This is a
  write to handle `input_report_handle + 1` with value `0x0100` (enable
  notifications).
- Common causes:
  - GATT server rejected the CCCD write (permissions / state issue).
  - Connection dropped between discovery and subscribe.
  - Controller requires init commands before it accepts subscriptions — ensure
    `--mode procon2` (default), not `--mode none`.
- Try subscribing manually with `bluetoothctl`:

```bash
bluetoothctl
[bluetooth]# menu gatt
[bluetooth]# select-attribute /org/bluez/hci0/dev_E0_EF_BF_3B_C6_76/serviceXXXX/charYYYY
[bluetooth]# notify on
```

**Fix:**
2. Ensure init stage succeeded before subscribe. If init failed, fix init first
   (check `--profile joycon2cpp` — macos and spro2win profiles have no init).
2. Increase GATT retries: `--gatt-retries 20`
3. Try `--profile macos` — if subscribe works without init, the init sequence
   may be interfering. This is a diagnostic step, not a fix.
4. If `bluetoothctl notify on` also fails, the controller's GATT server is
   rejecting notification setup — possible firmware/state issue.

---

## No input reports received

**Symptom:** Subscribe succeeds but no reports arrive. After `--notify-timeout`
seconds (default 30), the session disconnects with a timeout warning.

**Diagnosis:**
- Is the controller awake? Press a button — reports may only flow when the
  controller is actively communicating. Some controllers go idle and stop
  sending reports when no buttons are pressed.
- Check if the controller's home-button LEDs are solid (indicating active
  connection) vs. cycling (still in pairing mode).
- Run with `--verbose` to see every notification attempt. The report handler in
  `switch2d.py` increments `report_count` — if it stays at 0, no BLE
  notifications arrived.
- Verify BLE notifications are actually enabled:

```bash
bluetoothctl
[bluetooth]# menu gatt
[bluetooth]# list-attributes
# Look for "Notify: yes" on the input report characteristic
```

**Fix:**
1. Press buttons on the controller. Move sticks. Wake it up.
2. Increase notify timeout: `--notify-timeout 60`
3. Run `--max-reports 0 --verbose` to observe indefinitely.
4. If reports flow on Windows (joycon2cpp) but not Linux, check:
   - BlueZ version: `bluetoothd --version` (5.64+ recommended)
   - Kernel version: `uname -r` (5.15+ recommended for BLE stability)
   - Try with `--profile macos` (skip init) — if reports appear, init sequence
     needs adjustment.

---

## uinput device not created

**Symptom:** `setup_uinput` fails or `evtest` lists no "Nintendo Switch 2 Pro
Controller" device.

**Diagnosis:**
- Is the `uinput` kernel module loaded?

```bash
lsmod | grep uinput
ls -la /dev/uinput
```

- If `/dev/uinput` does not exist:

```bash
sudo modprobe uinput
```

- Permissions — does the user have write access?

```bash
ls -la /dev/uinput
# Expected: crw-rw---- 1 root input ...
```

**Fix:**
1. Load uinput: `sudo modprobe uinput`
2. Run as root: `sudo python3 switch2d.py --uinput ...`
3. Add a udev rule for persistent permissions (see below).
4. Check for conflicts: if another uinput device with the same name exists, the
   new one may not be created. Run `evtest` without args to list all devices.

---

## Controller disconnects after 30s

**Symptom:** Connection appears stable at first, then disconnects after
approximately `--notify-timeout` seconds (default 30). Log shows
`no reports for 30s`.

**Diagnosis:**
- This is the **notification watchdog**. `switch2d.py` expects input reports to
  arrive continuously. If no reports arrive for `--notify-timeout` seconds, it
  disconnects (assuming the controller went to sleep or BLE notifications
  stopped).
- The controller may stop sending reports when idle. Press and hold a button to
  keep it awake.
- Check if the controller's idle timeout is shorter than `--notify-timeout`.

**Fix:**
1. Press buttons regularly to keep the report stream alive.
2. Increase timeout: `--notify-timeout 120`
3. Set to 0 to disable the watchdog (not recommended — you'll need to manually
   Ctrl+C to exit): `--notify-timeout 0`
4. Run in daemon mode so it reconnects automatically:
   `--daemon --notify-timeout 30`

---

## Reconnect fails

**Symptom:** After a disconnect, the daemon scans but doesn't find the
controller again. Session fails and backoff timer increases.

**Diagnosis:**
- The controller's **advertising window** after disconnect is brief. It may
  only advertise for a few seconds before stopping. If the daemon's scan
  misses that window, it won't find the controller.
- The controller may need a button press to re-enter advertising mode.
- Check if the controller is still powered on. LEDs off = controller asleep.

**Fix:**
1. Press SYNC button or any face button to wake the controller.
2. Increase scan timeout: `--scan-timeout 30`
3. Run USB-init cycle: plug controller into USB briefly, unplug, press SYNC.
4. Use `--address <BDADDR>` to skip discovery and connect directly to known
   address. This is faster and avoids the advertising window entirely.
5. Run in daemon mode with `--connect-retries 10` for persistent reconnection.

---

## Permission denied on /dev/uinput

**Symptom:** `setup_uinput` fails with `PermissionError` or `OSError: [Errno
13] Permission denied: '/dev/uinput'`.

**Diagnosis:**
- The user running `switch2d.py` does not have write permission to
  `/dev/uinput`.
- Default permissions on most distros: `crw------- 1 root root`.

```bash
ls -la /dev/uinput
```

**Fix:**

### Quick fix (temporary):
```bash
sudo chmod 666 /dev/uinput
sudo python3 switch2d.py --uinput ...
```

### Persistent fix — udev rule:

Create `/etc/udev/rules.d/99-uinput.rules`:

```udev
SUBSYSTEM=="misc", KERNEL=="uinput", MODE="0660", GROUP="input"
```

Then reload udev and trigger:

```bash
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=misc
sudo modprobe -r uinput && sudo modprobe uinput
```

Add your user to the `input` group:

```bash
sudo usermod -a -G input $USER
# Log out and back in for group membership to take effect
```

After this, `switch2d.py --uinput` can run without `sudo`.

---

## Diagnostic Quick Reference

| Exit Code | Stage | Quick Check |
|-----------|-------|-------------|
| 10 | Preflight | `pip install bleak evdev`; `systemctl start bluetooth` |
| 11 | Scan | Hold SYNC button; `--scan-timeout 30`; try USB-init |
| 12 | Connect | `systemctl status bluetooth`; verify address; use MGMT path |
| 13 | Discover | Remove stale bonds; `--gatt-retries 20`; power-cycle controller |
| 14 | Init | Verify PID 0x2069; try `--profile macos` for diagnosis |
| 15 | Subscribe/Reports | Press buttons; `--notify-timeout 60`; check CCCD |
| 16 | Running/Disconnect | Keep controller awake; use `--daemon` for reconnect |

### CLI Flags Summary

| Flag | Purpose |
|------|---------|
| `--diagnose` | One-shot full pipeline test with exit codes |
| `--daemon` | Infinite reconnect loop |
| `--profile auto\|macos\|spro2win\|joycon2cpp` | Protocol profile (auto = sequential fallback) |
| `--uinput` | Create Linux gamepad device at `/dev/uinput` |
| `--verbose` | Show all characteristics, reports, stick values |
| `--json` | Structured JSON-line log output |
| `--address <BDADDR>` | Skip scan, connect directly |
| `--scan-timeout <N>` | BLE scan duration (default: 10s) |
| `--connect-retries <N>` | Connection attempts (default: 3) |
| `--gatt-retries <N>` | GATT discovery retries (default: 10) |
| `--notify-timeout <N>` | Max seconds without reports (default: 30) |
| `--max-reports <N>` | Max reports before exit (0=unlimited) |
| `--loose-scan` | Accept any Nintendo device (not just ProCon2) |
| `--quiet` | Minimal output |

---

## Still Stuck?

1. Run a full diagnostic and capture output:
   ```bash
   sudo python3 switch2d.py --diagnose --verbose 2>&1 | tee diagnostic.log
   ```

2. Collect system info:
   ```bash
   uname -a
   bluetoothd --version
   pip show bleak evdev
   lsmod | grep -E 'uinput|bluetooth'
   hciconfig -a
   ```

3. Check the controller works via USB (confirmed path):
   ```bash
   # If hid-switch2.ko is available:
   sudo insmod hid-switch2.ko
   evtest  # should see the controller
   ```

4. Check [RESEARCH.md](../README.md) for known blockers on your platform (e.g.,
   CYW43455 raw HCI blocking, BlueZ MGMT filtering, SMP pairing issues).

If the controller works via USB but not BLE, the issue is in the BLE stack
path — not the controller. Focus on the `switch2d.py` diagnostics in this
document.
