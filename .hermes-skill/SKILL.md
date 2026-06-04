---
name: switch2-hci-daemon
description: Build, deploy, and debug the Switch 2 Pro Controller BLE-to-UHID daemon (sw2d_final) on Raspberry Pi 5 / LibreELEC. Covers two-socket HCI architecture, USB prep, event filters, cross-compilation, and known kernel pitfalls.
version: 1.0.0
platforms: [linux]
metadata:
  hermes:
    tags: [bluetooth, ble, hci, nintendo, switch2, raspberry-pi, libreelec]
    category: devops
    config:
      - key: switch2.target_host
        description: SSH target for crackbery5 (RPi5 + LibreELEC)
        default: "root@10.10.10.140"
        prompt: SSH target for crackbery5
      - key: switch2.repo_path
        description: Local path to switch2-bt repository
        default: "~/switch2-raw"
        prompt: Local repo path
      - key: switch2.deploy_path
        description: Deployment path on target
        default: "/storage/switch2-bt"
        prompt: Deployment path on target
---

# Switch 2 Pro Controller — HCI Daemon Development

## Prerequisites

- **crackbery5**: Raspberry Pi 5, LibreELEC 12/13, internal Cypress CYW43455 BT
- **crackberry**: Raspberry Pi 4, Debian 12, cross-compiler `aarch64-linux-gnu-gcc`
- **Controller**: Nintendo Switch 2 Pro (VID 0x057E, PID 0x2069)
- **Repo**: `github.com/rixhal/switch2-bt`

## Architecture

```
┌──────────────────────────────────────┐
│  cmd_fd (HCI_CHANNEL_RAW)            │
│  → send_hci_cmd(): LE Create Conn,   │
│    ACL data                          │
├──────────────────────────────────────┤
│  evt_fd (HCI_CHANNEL_RAW)            │
│  → read_evt_frame(): CMD_STATUS,     │
│    LE Connection Complete, Disconnect│
└──────────────────────────────────────┘
```

Two sockets are required because:
- Linux HCI sockets are `SOCK_RAW` (datagram). Each `read()` consumes the entire skb.
- The kernel sets `skb->sk` on outgoing commands. While hardware events have
  `skb->sk=NULL` and are delivered to all sockets, using separate sockets avoids
  filter conflicts and mgmt interference edge cases.
- Pattern validated against: `sowifi/hcimin`, `thewierdnut/asha_pipewire_sink`

## Build & Deploy

```bash
# Cross-compile on crackberry
cd ~/switch2-raw
make sw2d_final        # native
make aarch64           # cross-compile for Pi5

# Deploy
scp sw2d_final root@10.10.10.140:/storage/switch2-bt/

# Test with two_socket_test first
make two_socket_test
scp two_socket_test root@10.10.10.140:/storage/
ssh root@10.10.10.140 /storage/two_socket_test E0:EF:BF:3B:C6:76 public
```

## USB Prep Workflow

The controller's Bluetooth module is OFF by default. USB init is required:

```bash
# On crackbery5: controller connected via USB
ssh root@10.10.10.140

# 1. Kill interfering processes
killall -9 bluetoothctl bluetoothd 2>/dev/null

# 2. USB prep
/storage/switch2-bt/sw2d_final --usb-init --host-bdaddr auto --verbose

# 3. WAIT for prompt: "UNPLUG the controller from USB NOW"
# 4. Physically unplug USB
# 5. Controller begins BLE advertising (~2-3 seconds)
# 6. Verify with hcitool:
hcitool lecc E0:EF:BF:3B:C6:76
# Should show: Connection handle 64

# 7. Start daemon
/storage/switch2-bt/sw2d_final --bdaddr E0:EF:BF:3B:C6:76 --peer-addr-type public --verbose
```

USB subcommand details: see `references/usb-subcommands.md`

## Connection Parameters

```
LE Create Connection (HCI 7.8.12):
  scan_interval:    10ms   (0x0010)
  scan_window:      10ms   (0x0010)
  peer_addr_type:   public (0x00) or random (0x01)
  own_addr_type:    public (0x00)
  conn_interval_min: 50ms (0x0028)
  conn_interval_max: 70ms (0x0038)
  conn_latency:     0
  supervision_timeout: 20s (0x07D0)
```

These match working traces from bleno#225 btmon captures.

## Event Filter

```c
hci_filter_clear(&nf);
hci_filter_set_ptype(HCI_EVENT_PKT, &nf);
memset(nf.event_mask, 0xFF, sizeof(nf.event_mask));  // allow ALL events
setsockopt(fd, SOL_HCI, HCI_FILTER, &nf, sizeof(nf));
```

**Why 0xFFFFFFFF and not specific events?** The kernel's security filter
(`hci_sec_filter`) ANDs with our filter. Specific bits (EVT_CMD_COMPLETE=0x0e,
EVT_LE_META=0x3e) may be cleared. `0xFFFFFFFF` passes everything.
Verify with `getsockopt(HCI_FILTER)`.

## Known Pitfalls

### Pitfall 1: Datagram read (CRITICAL)
HCI sockets are `SOCK_RAW` = datagram. Each `read()` consumes the ENTIRE skb.
**Never read byte-by-byte** — the remaining bytes are discarded.
Always use a single `read(fd, buf, bufsz)` for the full frame.

### Pitfall 2: hci0 DOWN before HCI_CHANNEL_USER bind
If hci0 is UP, `bind(HCI_CHANNEL_USER)` returns EBUSY.
Sequence: `hciconfig hci0 down` → bind → `hciconfig hci0 up`
(Pattern from bleno#225 — sandeepmistry)

### Pitfall 3: bluetoothd interference
Even with `bluetooth.service` masked, hanging `bluetoothctl`/`bluetoothd`
processes hold MGMT channels that send `LE Read Remote Used Features`,
killing connections. Always: `killall -9 bluetoothctl bluetoothd` before start.

### Pitfall 4: Controller sleep
Controller stops advertising after ~10 seconds unless buttons are pressed.
After USB prep: immediately run daemon. If controller sleeps: redo USB prep.

### Pitfall 5: `bluetooth.disable_mgmt=1` not in kernel 6.12
This parameter was added later. On LE12 (kernel 6.12.56), it's ignored.
Workaround: HCI_CHANNEL_RAW with proper pre-flight cleanup.

### Pitfall 6: Kernel version matters
- LE12 (6.12.56): `hcitool lescan` fails with "Input/output error" on Cypress
- LE13 (6.18.32): BLE scan works, HCI_CHANNEL_USER bind is EBUSY (hci0 UP at boot)
- `two_socket_test` validates architecture on both

### Pitfall 7: BDADDR overwrite
Never reuse the `peer` bdaddr_t variable for `hci_devba()` — it overwrites
the target address. Always use a separate `bdaddr_t local` variable.

## Event Sequence (expected btmon trace)

```
#1 LE Create Connection (0x08|0x000d) → sent on cmd_fd
#2 CMD_STATUS: Success → read from evt_fd
#3 LE Connection Complete: Status=0x00, Handle=64 → read from evt_fd
#4 (kernel may send LE Read Remote Used Features → controller drops with 0x3E)
```

If `LE Read Remote Used Features` appears in btmon after #3:
- A BlueZ MGMT client is still running → kill it
- Solution: HCI_CHANNEL_USER (requires hci0 DOWN before bind)

## Troubleshooting

| Symptom | Check | Fix |
|---|---|---|
| `bind HCI_CHANNEL_USER: EBUSY` | `hciconfig hci0` shows UP | `hciconfig hci0 down` first |
| `read_evt` always returns 0 (timeout) | Filter wrong or security-policy AND | Set 0xFFFFFFFF, verify with getsockopt |
| `LE Create Connection: timeout 6000ms` | Controller not advertising | USB prep, check with hcitool lecc |
| `Disconn: reason 0x3e` | Kernel sent LE Read Remote Used Features | Kill bluetoothd/bluetoothctl |
| Short frames from read() | Byte-by-byte read on datagram | Single read(fd, buf, big_bufsz) |
| `hci_devba: Network is down` | Called before hci0 UP | Call after hci_user_open() brings hci0 up |
| `@ MGMT Open` in btmon | bluetoothctl running | killall -9 bluetoothctl |
| Controller not in scan | Sleep after 10s idle | USB prep to wake |

## Reference Repositories

- `sowifi/hcimin` — HCI filter helpers, hci_send_req pattern, LE address types
- `thewierdnut/asha_pipewire_sink` — Raw HCI socket bind, event parsing loop
- `IanHarvey/bluepy` — BLE scan/advertisement patterns
- `edrosten/libblepp` — LE scan loop, advertising report parsing
- `aep/mble` — HCI filter helper comparison
- `bluez/bluez` — Canonical HCI command packing

## Git Workflow

```bash
cd ~/switch2-raw
# Small, logical commits
git add -p
git commit -m "feat(daemon): description"
git push origin master
```
