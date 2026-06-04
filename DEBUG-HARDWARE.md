# Hardware Debug Checklist

Use this checklist on the Raspberry Pi / LibreELEC target before changing the
handshake again. The goal is to capture one reproducible run with enough context
to compare HCI/SMP behavior.

## Build

```bash
make clean
make
make test
```

Expected result: `sw2d_final` builds and all unit tests pass. The `test_bdaddr`
test may skip live `hci0` checks when no Bluetooth adapter is available.

## Preflight

Run this before plugging or unplugging the controller:

```bash
sudo ./sw2d_final --preflight --exclusive-hci --hidraw /dev/hidraw0
```

Check the output for:

- `hci0: present`
- host BDADDR visible from sysfs
- `/dev/uhid: present`
- `/dev/hidraw0: present` when the controller is connected by USB
- no running `bluetoothd`, `bluetoothctl`, `btmgmt`, `gatttool`, or `hcitool`
- whether `bluetooth.disable_mgmt=1` is present in `/proc/cmdline`

If the kernel drops connection events or HCI_CHANNEL_USER does not receive
`LE Connection Complete`, test with this kernel argument on LibreELEC:

```text
bluetooth.disable_mgmt=1
```

On LibreELEC this usually means editing `/flash/cmdline.txt` and rebooting.

## Capture A: Default SMP Path

Terminal 1:

```bash
sudo btmon -w sw2d-default.btsnoop
```

Terminal 2:

```bash
sudo systemctl stop bluetooth 2>/dev/null || true
sudo ./sw2d_final --usb-init --auto-scan --peer-addr-type auto --verbose --exclusive-hci
```

Workflow:

1. Connect the controller over USB.
2. Start the daemon command.
3. When prompted, physically unplug USB within the 8 second window.
4. Let it reconnect at least twice if it fails.
5. Stop `btmon` with `Ctrl+C`.

## Capture B: Legacy Security Request Variant

Only run this after Capture A. It keeps the older experimental behavior where the
central sends an SMP Security Request before Pairing Request.

Terminal 1:

```bash
sudo btmon -w sw2d-security-request.btsnoop
```

Terminal 2:

```bash
sudo systemctl stop bluetooth 2>/dev/null || true
sudo ./sw2d_final --usb-init --auto-scan --peer-addr-type auto --verbose --exclusive-hci --send-security-request
```

## What To Report Back

Copy the daemon text log from the first failed run and keep the matching
`.btsnoop` file. The important milestones are:

- `Found: ... type=random|public`
- `LE_CONN_COMPLETE SUCCESS`
- `Pairing Response`
- `Confirm MATCH`
- `LTK Request`
- `Encryption change: status=0x00`
- `ATT MTU req sent`
- `CCCD write OK`
- first `REPORT #...`

If it disconnects, keep the exact reason code and the last SMP/ATT line before
the disconnect.

Common meanings:

- `0x13`: remote user terminated connection
- `0x3e`: connection failed to be established
- no `LE_CONN_COMPLETE` on daemon but visible in `btmon`: likely HCI user/mgmt
  routing problem
- `SMP Pairing Failed reason=...`: pairing parameter mismatch; compare Capture A
  and Capture B

