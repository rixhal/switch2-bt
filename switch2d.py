#!/usr/bin/env python3
"""
switch2d.py — Nintendo Switch 2 Pro Controller Linux Wireless Daemon

Primary daemon: scans, connects via BlueZ/bleak, runs CareyScott GATT init,
subscribes to input reports, exposes via /dev/uinput as standard gamepad.

Reference: TheFrano/joycon2cpp (Windows) → CareyScott GATT protocol
Target: Raspberry Pi 5 / LibreELEC (Linux, bleak + BlueZ backend)

Usage:
  python3 switch2d.py --verbose
  python3 switch2d.py --address E0:EF:BF:3B:C6:76 --dump-jsonl reports.jsonl
  python3 switch2d.py --mode none       # only discover services + notify
  python3 switch2d.py --mode pair       # legacy CareyScott pair writes

Exit codes:
  0 — Success: notifications received
  1 — Scan: no Nintendo controller found
  2 — Connect failed
  3 — Required GATT characteristics missing
  4 — Pair command write failed
  5 — Notification subscription failed
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import sys
import time
from pathlib import Path
from typing import Optional

# ── bleak imports ──────────────────────────────────────────────
try:
    from bleak import BleakScanner, BleakClient
    from bleak.backends.device import BLEDevice
    from bleak.backends.scanner import AdvertisementData
except ImportError:
    print("ERROR: bleak not installed. Run: pip install bleak", file=sys.stderr)
    sys.exit(99)

# ── Constants (canonical, from CareyScott reverse engineering) ──

NINTENDO_MANUFACTURER_ID = 0x0553
NINTENDO_VENDOR_ID = 0x057E
PRO_CONTROLLER2_PID = 0x2069

INPUT_REPORT_UUID = "ab7de9be-89fe-49ad-828f-118f09df7fd2"
COMMAND_WRITE_UUID = "649d4ac9-8eb7-4e6c-af44-1ea54fe5f005"
COMMAND_RESPONSE_UUID = "c765a961-d9d8-4d36-a20a-5315b111836a"
SPRO2WIN_INPUT_REPORT_UUID = "7492866c-ec3e-4619-8258-32755ffcc0f9"
SPRO2WIN_INPUT_REPORT_HANDLE = 45

PAIR_CMD = 0x15
PAIR_SUB_SET_MAC = 0x01
PAIR_SUB_KEY_1 = 0x04
PAIR_SUB_KEY_2 = 0x02
PAIR_SUB_FINISH = 0x03

PAIR_KEY_1 = bytes([0x00, 0xea, 0xbd, 0x47, 0x13, 0x89, 0x35, 0x42,
                    0xc6, 0x79, 0xee, 0x07, 0xf2, 0x53, 0x2c, 0x6c, 0x31])
PAIR_KEY_2 = bytes([0x00, 0x40, 0xb0, 0x8a, 0x5f, 0xcd, 0x1f, 0x9b,
                    0x41, 0x12, 0x5c, 0xac, 0xc6, 0x3f, 0x38, 0xa0, 0x73])

# Minimal Switch 2 Pro Controller init sequence proven by TheFrano/joycon2cpp.
# Written to COMMAND_WRITE_UUID with Write Without Response before enabling
# notifications on INPUT_REPORT_UUID.
PROCON2_INIT_COMMANDS = [
    ("IMU/Input mode 0x02", bytes.fromhex("0c 91 01 02 00 04 00 00 ff 00 00 00"), 0.5),
    ("IMU/Input mode 0x04", bytes.fromhex("0c 91 01 04 00 04 00 00 ff 00 00 00"), 0.5),
]
PROCON2_LED_COMMAND = bytes.fromhex("09 91 01 07 00 08 00 00 01 00 00 00 00 00 00 00")
PROCON2_SOUND_COMMAND = bytes.fromhex("0a 91 01 02 00 08 00 00 04 00 00 00 00 00 00 00")

PROCON2_BUTTON_MASKS = {
    "a": 0x000800000000,
    "b": 0x000400000000,
    "x": 0x000200000000,
    "y": 0x000100000000,
    "r": 0x004000000000,
    "l": 0x000000400000,
    "zr": 0x008000000000,
    "zl": 0x000000800000,
    "home": 0x000010000000,
    "back": 0x000001000000,
    "start": 0x000002000000,
    "r3": 0x000004000000,
    "l3": 0x000008000000,
    "dpad_up": 0x000000020000,
    "dpad_right": 0x000000040000,
    "dpad_down": 0x000000010000,
    "dpad_left": 0x000000080000,
    "gl": 0x000000000200,
    "gr": 0x000000000100,
    "screenshot": 0x000020000000,
    "c": 0x000040000000,
}

# ── Helpers ────────────────────────────────────────────────────

def hexdump(data: bytes, label: str = "") -> str:
    """Format bytes as hex string with optional label."""
    hx = data.hex(" ")
    return f"{label + ': ' if label else ''}{hx}"

def decodeu(data: bytes) -> int:
    """Little-endian unsigned integer from bytes."""
    return int.from_bytes(data, "little")

def build_command(cmd_id: int, sub_id: int, data: bytes = b"") -> bytes:
    """
    Build a Switch 2 GATT command write payload.

    Wire format (CareyScott):
        [0]     cmd_id
        [1]     0x91
        [2]     0x01
        [3]     sub_id
        [4]     0x00
        [5]     len(data)
        [6]     0x00
        [7]     0x00
        [8..]   data
    """
    header = bytes([cmd_id, 0x91, 0x01, sub_id, 0x00, len(data), 0x00, 0x00])
    return header + data

def build_pair_setmac(host_mac: bytes) -> bytes:
    """Build SetMAC pair subcommand payload (14 bytes)."""
    # CareyScott: b"\x00\x02" + mac_value (6) + mac_value (6)
    assert len(host_mac) == 6
    return b"\x00\x02" + host_mac + host_mac

def extract_manufacturer_data(adv: AdvertisementData) -> Optional[bytes]:
    """Extract Nintendo manufacturer data from advertisement."""
    return adv.manufacturer_data.get(NINTENDO_MANUFACTURER_ID)

def parse_manufacturer_raw(data: bytes) -> dict:
    """Parse raw manufacturer data into vendor_id, product_id, reconnect_mac."""
    result = {"vendor_id": None, "product_id": None, "reconnect_mac": None}
    if len(data) >= 16:
        result["vendor_id"] = decodeu(data[3:5])
        result["product_id"] = decodeu(data[5:7])
        result["reconnect_mac"] = data[10:16]
    return result

def reconnect_mac_to_str(value: Optional[bytes]) -> str:
    """Format reconnect MAC bytes from the manufacturer payload."""
    if not value:
        return "unknown"
    if value == bytes(6):
        return "00:00:00:00:00:00"
    return value.hex(":")

def get_pid_name(pid: int) -> str:
    """Human-readable name for Switch 2 product IDs."""
    names = {
        0x2066: "Joy-Con 2 (Right)",
        0x2067: "Joy-Con 2 (Left)",
        0x2069: "Pro Controller 2",
        0x2073: "NSO GameCube Controller",
    }
    return names.get(pid, f"Unknown (0x{pid:04x})")

def decode_stick12(data: bytes, offset: int) -> Optional[dict]:
    """Decode a 12-bit packed Switch stick at offset..offset+2."""
    if len(data) < offset + 3:
        return None
    raw = data[offset:offset + 3]
    x = ((raw[1] & 0x0F) << 8) | raw[0]
    y = (raw[2] << 4) | ((raw[1] & 0xF0) >> 4)
    return {
        "x": x,
        "y": y,
        "x_norm": round((x - 2048) / 2047, 3),
        "y_norm": round((y - 2048) / 2047, 3),
    }

def s16(data: bytes, offset: int) -> Optional[int]:
    """Little-endian signed 16-bit from data."""
    if len(data) < offset + 2:
        return None
    return int.from_bytes(data[offset:offset + 2], "little", signed=True)

def decode_procon2_report(data: bytes) -> Optional[dict]:
    """
    Decode the long Switch 2 Pro Controller report used by joycon2cpp.

    Report shape:
      buttons: bytes 3..8 as a 48-bit big-endian bitfield
      sticks:  left at 10..12, right at 13..15
      motion:  accel 0x30..0x35, gyro 0x36..0x3b
    """
    if len(data) < 0x3C:
        return None

    state = 0
    for i in range(3, 9):
        state = (state << 8) | data[i]

    pressed = [name for name, mask in PROCON2_BUTTON_MASKS.items() if state & mask]
    return {
        "packet_id": int.from_bytes(data[0:3], "little") if len(data) >= 3 else data[0],
        "button_state": f"0x{state:012x}",
        "pressed": pressed,
        "left_stick": decode_stick12(data, 10),
        "right_stick": decode_stick12(data, 13),
        "accel": {
            "x": s16(data, 0x30),
            "y": s16(data, 0x32),
            "z": s16(data, 0x34),
        },
        "gyro": {
            "x": s16(data, 0x36),
            "z": s16(data, 0x38),
            "y": s16(data, 0x3A),
        },
    }

# ── Async Core ─────────────────────────────────────────────────

class ProbeSession:
    """Holds state across the probe run."""

    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.client: Optional[BleakClient] = None
        self.device: Optional[BLEDevice] = None
        self.report_count: int = 0
        self.last_report: Optional[bytes] = None
        self.jsonl_file = None
        self.start_time: float = 0.0
        self.notify_event: asyncio.Event = asyncio.Event()
        self.disconnect_event: asyncio.Event = asyncio.Event()
        # Track UUID discovery
        self.cmd_write_handle: Optional[int] = None
        self.input_report_handle: Optional[int] = None
        self.cmd_response_handle: Optional[int] = None
        self.gamepad = None
        self.uinput = None

    async def run(self) -> int:
        """Main orchestration: scan → connect → pair → notify."""
        self.start_time = time.monotonic()

        # 1. Scan
        self.device = await self._scan()
        if not self.device:
            return 1  # No controller found

        # 2. Connect
        ok = await self._connect_with_retries()
        if not ok:
            return 2  # Connect failed

        # 3. Service discovery
        ok = await self._discover_characteristics()
        if not ok:
            return 3  # Characteristics missing

        # 4. Controller init / legacy pair sequence
        if self.args.notify_before_init:
            ok = await self._subscribe()
            if not ok:
                return 5

        if self.args.mode == "procon2":
            ok = await self._procon2_init()
            if not ok:
                return 4
        elif self.args.mode == "pair":
            ok = await self._pair()
            if not ok:
                return 4  # Pair failed

        if self.args.pair_only:
            self._log("Pair sequence complete. Exiting (--pair-only).")
            return 0

        # 5. Subscribe to notifications
        if self.args.xinput:
            ok = self._start_xinput()
            if not ok:
                return 6
        if self.args.uinput:
            ok = self._start_uinput()
            if not ok:
                return 6

        if not self.args.notify_before_init:
            ok = await self._subscribe()
            if not ok:
                return 5  # Notify subscribe failed

        # 6. Wait for reports
        self._log("Waiting for input reports... press buttons on the controller.")
        self._log(f"Will run for up to {self.args.notify_timeout}s or {self.args.max_reports} reports.")
        await self._wait_for_reports()

        return 0 if self.report_count > 0 else 5

    def _start_xinput(self) -> bool:
        """Create a virtual Xbox 360 controller via ViGEmBus."""
        try:
            import vgamepad as vg
            self.gamepad = vg.VX360Gamepad()
            self._log("Virtual Xbox 360 gamepad created.")
            return True
        except Exception as e:
            self._log(f"ERROR: Could not create virtual gamepad: {e}", level="error")
            self._log("Install ViGEmBus and vgamepad, then retry.", level="error")
            return False

    def _start_uinput(self) -> bool:
        """Create a Linux uinput gamepad for decoded ProCon2 reports."""
        try:
            from evdev import UInput, AbsInfo, ecodes as e

            buttons = [
                e.BTN_SOUTH, e.BTN_EAST, e.BTN_NORTH, e.BTN_WEST,
                e.BTN_TL, e.BTN_TR, e.BTN_TL2, e.BTN_TR2,
                e.BTN_SELECT, e.BTN_START, e.BTN_MODE,
                e.BTN_THUMBL, e.BTN_THUMBR,
                e.BTN_DPAD_UP, e.BTN_DPAD_DOWN, e.BTN_DPAD_LEFT, e.BTN_DPAD_RIGHT,
            ]
            abs_axis = AbsInfo(value=0, min=-32768, max=32767, fuzz=16, flat=128, resolution=0)
            trigger_axis = AbsInfo(value=0, min=0, max=255, fuzz=0, flat=0, resolution=0)
            capabilities = {
                e.EV_KEY: buttons,
                e.EV_ABS: [
                    (e.ABS_X, abs_axis),
                    (e.ABS_Y, abs_axis),
                    (e.ABS_RX, abs_axis),
                    (e.ABS_RY, abs_axis),
                    (e.ABS_Z, trigger_axis),
                    (e.ABS_RZ, trigger_axis),
                ],
            }
            self.uinput = UInput(
                capabilities,
                name="Nintendo Switch 2 Pro Controller",
                vendor=NINTENDO_VENDOR_ID,
                product=PRO_CONTROLLER2_PID,
                version=1,
            )
            self._log("Linux uinput gamepad created.")
            return True
        except Exception as e:
            self._log(f"ERROR: Could not create uinput gamepad: {e}", level="error")
            self._log("Install python-evdev and run with access to /dev/uinput.", level="error")
            return False

    # ── Scan ────────────────────────────────────────────────

    async def _scan(self) -> Optional[BLEDevice]:
        """BLE scan for Nintendo Switch 2 controllers."""
        timeout = self.args.scan_timeout
        address_filter = self.args.address
        name_filter = self.args.name_filter

        self._log(f"Scanning for {timeout}s (manufacturer 0x{NINTENDO_MANUFACTURER_ID:04x})...")
        found_device: Optional[BLEDevice] = None
        found_info: dict = {}
        found_event = asyncio.Event()

        def scan_callback(device: BLEDevice, adv: AdvertisementData):
            nonlocal found_device, found_info

            # Address filter
            if address_filter and device.address.upper() != address_filter.upper():
                return

            # Name filter
            if name_filter and name_filter.lower() not in (device.name or "").lower():
                return

            mfg_data = extract_manufacturer_data(adv)
            if mfg_data is None:
                return

            info = parse_manufacturer_raw(mfg_data)
            if (info.get("vendor_id") != NINTENDO_VENDOR_ID or
                    info.get("product_id") != PRO_CONTROLLER2_PID):
                if self.args.verbose:
                    self._log(f"  skip Nintendo-like adv: {device.address} "
                              f"vendor={info.get('vendor_id')} pid={info.get('product_id')} "
                              f"rssi={adv.rssi} mfg_data={hexdump(mfg_data)}")
                return

            pid = info.get("product_id")
            pid_str = get_pid_name(pid) if pid else "unknown"

            self._log(f"  FOUND: {device.address} ({pid_str}) "
                      f"rssi={adv.rssi} name={device.name} "
                      f"reconnect_mac={reconnect_mac_to_str(info.get('reconnect_mac'))} "
                      f"mfg_data={hexdump(mfg_data)}")

            if not found_device:
                found_device = device
                found_info = info
                found_event.set()

        async with BleakScanner(scan_callback):
            try:
                await asyncio.wait_for(found_event.wait(), timeout=timeout)
            except asyncio.TimeoutError:
                pass

        if found_device:
            self._log(f"Selected: {found_device.address} ({get_pid_name(found_info.get('product_id'))})")
            return found_device

        self._log("ERROR: No Nintendo Switch 2 controller found.", level="error")
        if address_filter:
            self._log(f"  Address filter: {address_filter} — is controller advertising?")
        self._log("  Make sure controller is in Sync/Pairing mode (hold Sync button).")
        return None

    # ── Connect ──────────────────────────────────────────────

    async def _connect_with_retries(self) -> bool:
        """Connect with retries because BLE advertising windows are short."""
        for attempt in range(1, self.args.connect_retries + 1):
            if attempt > 1:
                delay = min(2.0 * attempt, 8.0)
                self._log(f"Retrying connect in {delay:.1f}s ({attempt}/{self.args.connect_retries})...")
                await asyncio.sleep(delay)
            if await self._connect():
                return True
            await self._disconnect_client()
        return False

    async def _connect(self) -> bool:
        """Connect to the discovered device."""
        addr = self.device.address
        self._log(f"Connecting to {addr}...")

        def on_disconnect(client: BleakClient):
            self._log(f"Disconnected from {addr}", level="warn")
            self.disconnect_event.set()

        self.disconnect_event.clear()
        self.client = BleakClient(self.device, disconnected_callback=on_disconnect)

        try:
            await asyncio.wait_for(self.client.connect(), timeout=20.0)
        except asyncio.TimeoutError:
            self._log(f"ERROR: Connect timeout to {addr}", level="error")
            return False
        except Exception as e:
            self._log(f"ERROR: Connect failed: {e}", level="error")
            return False

        self._log(f"Connected to {addr} (mtu={self.client.mtu_size})")
        return True

    async def _disconnect_client(self) -> None:
        """Disconnect and clear current client if present."""
        if self.client and self.client.is_connected:
            try:
                await self.client.disconnect()
            except Exception:
                pass
        self.client = None

    # ── Service Discovery ────────────────────────────────────

    async def _discover_characteristics(self) -> bool:
        """Discover all services. Find command write + input report characteristics."""
        self._log("Discovering GATT services...")

        services = None
        for attempt in range(1, self.args.gatt_retries + 1):
            try:
                services = self.client.services
                if services:
                    break
            except Exception as e:
                self._log(f"GATT service access failed ({attempt}/{self.args.gatt_retries}): {e}", level="warn")
            await asyncio.sleep(0.5)

        if not services:
            self._log("ERROR: No services discovered.", level="error")
            self._log("  Try: bluetoothctl remove <addr>, restart bluetooth, then hold Sync again.", level="error")
            return False

        self.cmd_write_handle = None
        self.input_report_handle = None
        self.cmd_response_handle = None
        for service in services:
            self._log(f"  Service: {service.uuid}")
            for char in service.characteristics:
                uuid_str = str(char.uuid)
                props = char.properties
                flags = []
                if "read" in props: flags.append("R")
                if "write" in props: flags.append("W")
                if "write-without-response" in props: flags.append("WW")
                if "notify" in props: flags.append("N")
                if "indicate" in props: flags.append("I")

                self._log(f"    Char: {uuid_str} [{','.join(flags)}] handle={char.handle}")

                if uuid_str.lower() == INPUT_REPORT_UUID.lower():
                    self.input_report_handle = char.handle
                    self._log(f"    ^^ INPUT REPORT CHARACTERISTIC FOUND (handle {char.handle})")
                elif uuid_str.lower() == COMMAND_WRITE_UUID.lower():
                    self.cmd_write_handle = char.handle
                    self._log(f"    ^^ COMMAND WRITE CHARACTERISTIC FOUND (handle {char.handle})")
                elif uuid_str.lower() == COMMAND_RESPONSE_UUID.lower():
                    self.cmd_response_handle = char.handle

        # Check required characteristics
        missing = []
        if self.args.mode in ("procon2", "pair") and not self.cmd_write_handle:
            missing.append(COMMAND_WRITE_UUID)
        if not self.input_report_handle:
            missing.append(INPUT_REPORT_UUID)

        if missing:
            self._log(f"ERROR: Missing required characteristics: {missing}", level="error")
            return False

        self._log("All required characteristics found.")
        return True

    # ── Pair ─────────────────────────────────────────────────

    async def _pair(self) -> bool:
        """Write the custom GATT pair command sequence."""
        self._log("=== Pair Command Sequence ===")

        # Get host MAC from Bluetooth adapter
        host_mac = await self._get_host_mac()
        self._log(f"Host MAC: {host_mac.hex(':')}")

        # 1. SetMAC
        await self._write_pair_command(
            PAIR_SUB_SET_MAC,
            build_pair_setmac(host_mac),
            "SetMAC"
        )

        # 2. Pair Key 1
        await self._write_pair_command(
            PAIR_SUB_KEY_1,
            PAIR_KEY_1,
            "Pair Key 1"
        )

        # 3. Pair Key 2
        await self._write_pair_command(
            PAIR_SUB_KEY_2,
            PAIR_KEY_2,
            "Pair Key 2"
        )

        # 4. Finish
        await self._write_pair_command(
            PAIR_SUB_FINISH,
            b"\x00",
            "Finish"
        )

        self._log("Pair command sequence complete.")
        return True

    async def _procon2_init(self) -> bool:
        """Write the joycon2cpp-proven Switch 2 Pro init sequence."""
        self._log("=== ProCon2 Init Sequence (joycon2cpp) ===")
        if not self.cmd_write_handle:
            self._log("ERROR: command write characteristic missing.", level="error")
            return False

        commands = list(PROCON2_INIT_COMMANDS)
        # 200ms barrier between feat-select init and LED/Sound (joycon2cpp-proven gap)
        has_output = (not self.args.no_led) or (not self.args.no_sound)
        if has_output:
            commands.append(("--- 200ms barrier ---", None, 0.2))
        if not self.args.no_led:
            commands.append(("Set LED 1", PROCON2_LED_COMMAND, 0.05))
        if not self.args.no_sound:
            commands.append(("Emit sound", PROCON2_SOUND_COMMAND, 0.05))

        for label, payload, delay_s in commands:
            self._log(f"  -> {label}: {hexdump(payload) if payload else '(barrier only)'}")
            if payload:  # barrier entries have no payload
                try:
                    await self.client.write_gatt_char(
                        COMMAND_WRITE_UUID,
                        payload,
                        response=False,
                    )
                except Exception as e:
                    self._log(f"  <- {label}: FAILED: {e}", level="error")
                    return False
                self._log(f"  <- {label}: write OK (without response)")
            if delay_s:
                await asyncio.sleep(delay_s)

        self._log("ProCon2 init sequence complete.")
        return True

    async def _write_pair_command(self, sub_id: int, data: bytes, label: str) -> None:
        """Write a single pair subcommand and log it."""
        payload = build_command(PAIR_CMD, sub_id, data)
        self._log(f"  -> {label}: {hexdump(payload)}")

        try:
            await self.client.write_gatt_char(
                COMMAND_WRITE_UUID,
                payload,
                response=True
            )
            self._log(f"  <- {label}: write OK (response mode)")
        except Exception as e:
            self._log(f"  <- {label}: Write-With-Response failed: {e}", level="warn")
            # Fallback: try without response
            try:
                await self.client.write_gatt_char(
                    COMMAND_WRITE_UUID,
                    payload,
                    response=False
                )
                self._log(f"  <- {label}: write OK (without-response fallback)")
            except Exception as e2:
                self._log(f"  <- {label}: FAILED: {e2}", level="error")
                raise

    async def _get_host_mac(self) -> bytes:
        """Get the host Bluetooth adapter MAC address."""
        try:
            devices = await BleakScanner.discover(timeout=0.5, return_adv=False)
            # bleak doesn't expose adapter MAC directly; try to get it from the system
            import subprocess
            result = subprocess.run(
                ["hcitool", "dev"],
                capture_output=True, text=True, timeout=5
            )
            if result.returncode == 0:
                lines = result.stdout.strip().split("\n")
                for line in lines[1:]:
                    parts = line.strip().split()
                    if len(parts) >= 2:
                        mac = parts[1]
                        self._log(f"  Host MAC from hcitool: {mac}")
                        return bytes.fromhex(mac.replace(":", ""))
        except Exception:
            pass

        # Fallback: use the device's own address from the client if available
        try:
            if self.client and hasattr(self.client, '_backend'):
                backend = self.client._backend
                if hasattr(backend, 'address'):
                    mac = backend.address
                    return bytes.fromhex(mac.replace(":", "").replace("-", ""))
        except Exception:
            pass

        self._log("WARNING: Could not determine host MAC. Using sentinel 00:00:00:00:00:00", level="warn")
        return bytes(6)

    # ── Subscribe ────────────────────────────────────────────

    async def _subscribe(self) -> bool:
        """Subscribe to input report notifications."""
        if self.args.subscribe_all_notify:
            self._log("Subscribing to all notify characteristics (SPro2Win style)...")

            def report_callback(sender, data: bytearray):
                handle = getattr(sender, "handle", None)
                if self.args.input_handle and handle != self.args.input_handle:
                    return
                self._on_report(bytes(data), handle=handle)

            try:
                count = 0
                for service in self.client.services:
                    for char in service.characteristics:
                        if "notify" not in char.properties:
                            continue
                        await self.client.start_notify(char, report_callback)
                        count += 1
                        self._log(f"  subscribed: {char.uuid} handle={char.handle}")
                if count == 0:
                    self._log("ERROR: No notify characteristics found.", level="error")
                    return False
                self._log(f"Notification subscription successful ({count} characteristics).")
                return True
            except Exception as e:
                self._log(f"ERROR: Failed to subscribe: {e}", level="error")
                return False

        self._log(f"Subscribing to input report notifications ({INPUT_REPORT_UUID})...")

        def report_callback(sender, data: bytearray):
            self._on_report(bytes(data), handle=getattr(sender, "handle", None))

        try:
            await self.client.start_notify(INPUT_REPORT_UUID, report_callback)
            self._log("Notification subscription successful.")
            return True
        except Exception as e:
            self._log(f"ERROR: Failed to subscribe: {e}", level="error")
            return False

    def _on_report(self, data: bytes, handle: Optional[int] = None):
        """Handle an incoming input report."""
        self.report_count += 1
        elapsed = time.monotonic() - self.start_time
        if self.gamepad:
            self._update_xinput(data, handle=handle)
        decoded_for_uinput = decode_procon2_report(data) if self.uinput else None
        if decoded_for_uinput:
            self._update_uinput(decoded_for_uinput)

        # Build changed_bytes info
        changed = []
        if self.last_report and len(data) == len(self.last_report):
            for i, (a, b) in enumerate(zip(data, self.last_report)):
                if a != b:
                    changed.append(i)
        self.last_report = data

        if not self.args.quiet_reports:
            handle_text = f", handle={handle}" if handle is not None else ""
            self._log(f"--- Report #{self.report_count} ({len(data)} bytes{handle_text}, t={elapsed:.2f}s) ---")
            self._log(f"  hex: {hexdump(data)}")
            if changed:
                byte_changes = ", ".join(f"[{i}]=0x{data[i]:02x}" for i in changed)
                self._log(f"  changed bytes: {byte_changes}")
            else:
                self._log(f"  changed bytes: (first report or no change)")
            decoded = decode_procon2_report(data) if self.args.decode_procon2 else None
            if decoded:
                sticks = decoded["left_stick"], decoded["right_stick"]
                self._log(
                    "  procon2: "
                    f"buttons={decoded['pressed'] or []} "
                    f"L=({sticks[0]['x']},{sticks[0]['y']}) "
                    f"R=({sticks[1]['x']},{sticks[1]['y']}) "
                    f"accel={decoded['accel']} gyro={decoded['gyro']}"
                )

        # Optional JSONL dump
        if self.jsonl_file:
            record = {
                "timestamp": elapsed,
                "report_number": self.report_count,
                "handle": handle,
                "length": len(data),
                "hex": data.hex(),
                "changed_bytes": changed,
            }
            if self.args.decode_procon2:
                decoded = decode_procon2_report(data)
                if decoded:
                    record["procon2"] = decoded
            self.jsonl_file.write(json.dumps(record) + "\n")
            self.jsonl_file.flush()

        # Signal that we got a report
        self.notify_event.set()

    def _update_xinput(self, data: bytes, handle: Optional[int] = None) -> None:
        """Map SPro2Win gameplay reports to the virtual Xbox 360 controller."""
        if self.args.input_handle and handle is not None and handle != self.args.input_handle:
            return
        if len(data) < 11:
            return
        try:
            import vgamepad as vg

            right = data[2]
            left = data[3]
            system = data[4]

            mapping = [
                (right & 0x01, vg.XUSB_BUTTON.XUSB_GAMEPAD_A),
                (right & 0x02, vg.XUSB_BUTTON.XUSB_GAMEPAD_B),
                (right & 0x04, vg.XUSB_BUTTON.XUSB_GAMEPAD_X),
                (right & 0x08, vg.XUSB_BUTTON.XUSB_GAMEPAD_Y),
                (right & 0x10, vg.XUSB_BUTTON.XUSB_GAMEPAD_RIGHT_SHOULDER),
                (right & 0x40, vg.XUSB_BUTTON.XUSB_GAMEPAD_START),
                (right & 0x80, vg.XUSB_BUTTON.XUSB_GAMEPAD_RIGHT_THUMB),
                (left & 0x01, vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_DOWN),
                (left & 0x02, vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_RIGHT),
                (left & 0x04, vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_LEFT),
                (left & 0x08, vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_UP),
                (left & 0x10, vg.XUSB_BUTTON.XUSB_GAMEPAD_LEFT_SHOULDER),
                (left & 0x40, vg.XUSB_BUTTON.XUSB_GAMEPAD_BACK),
                (left & 0x80, vg.XUSB_BUTTON.XUSB_GAMEPAD_LEFT_THUMB),
                (system & 0x01, vg.XUSB_BUTTON.XUSB_GAMEPAD_GUIDE),
            ]
            for pressed, button in mapping:
                if pressed:
                    self.gamepad.press_button(button=button)
                else:
                    self.gamepad.release_button(button=button)

            self.gamepad.right_trigger(value=255 if (right & 0x20) else 0)
            self.gamepad.left_trigger(value=255 if (left & 0x20) else 0)

            left_x_raw = data[5] + ((data[6] & 0x0F) << 8)
            left_y_raw = (data[6] >> 4) + (data[7] << 4)
            right_x_raw = data[8] + ((data[9] & 0x0F) << 8)
            right_y_raw = (data[9] >> 4) + (data[10] << 4)

            def scale(value: int) -> int:
                return max(-32767, min(32767, int((value - 2048) * 18.5)))

            self.gamepad.left_joystick(x_value=scale(left_x_raw), y_value=scale(left_y_raw))
            self.gamepad.right_joystick(x_value=scale(right_x_raw), y_value=scale(right_y_raw))
            self.gamepad.update()
        except Exception as e:
            self._log(f"XInput update failed: {e}", level="warn")

    def _update_uinput(self, decoded: dict) -> None:
        """Map decoded long ProCon2 reports to Linux uinput."""
        try:
            from evdev import ecodes as e

            pressed = set(decoded.get("pressed", []))
            key_map = {
                "b": e.BTN_SOUTH,
                "a": e.BTN_EAST,
                "y": e.BTN_NORTH,
                "x": e.BTN_WEST,
                "l": e.BTN_TL,
                "r": e.BTN_TR,
                "zl": e.BTN_TL2,
                "zr": e.BTN_TR2,
                "back": e.BTN_SELECT,
                "start": e.BTN_START,
                "home": e.BTN_MODE,
                "l3": e.BTN_THUMBL,
                "r3": e.BTN_THUMBR,
                "dpad_up": e.BTN_DPAD_UP,
                "dpad_down": e.BTN_DPAD_DOWN,
                "dpad_left": e.BTN_DPAD_LEFT,
                "dpad_right": e.BTN_DPAD_RIGHT,
            }
            for name, code in key_map.items():
                self.uinput.write(e.EV_KEY, code, 1 if name in pressed else 0)

            def axis(stick: Optional[dict], key: str, invert: bool = False) -> int:
                if not stick:
                    return 0
                value = stick.get(key, 0.0)
                if invert:
                    value = -value
                return max(-32768, min(32767, int(value * 32767)))

            self.uinput.write(e.EV_ABS, e.ABS_X, axis(decoded.get("left_stick"), "x_norm"))
            self.uinput.write(e.EV_ABS, e.ABS_Y, axis(decoded.get("left_stick"), "y_norm", invert=True))
            self.uinput.write(e.EV_ABS, e.ABS_RX, axis(decoded.get("right_stick"), "x_norm"))
            self.uinput.write(e.EV_ABS, e.ABS_RY, axis(decoded.get("right_stick"), "y_norm", invert=True))
            self.uinput.write(e.EV_ABS, e.ABS_Z, 255 if "zl" in pressed else 0)
            self.uinput.write(e.EV_ABS, e.ABS_RZ, 255 if "zr" in pressed else 0)
            self.uinput.syn()
        except Exception as e:
            self._log(f"uinput update failed: {e}", level="warn")

    # ── Wait Loop ────────────────────────────────────────────

    async def _wait_for_reports(self) -> None:
        """Wait for reports up to notify_timeout or max_reports."""
        timeout = self.args.notify_timeout
        max_count = self.args.max_reports

        try:
            while True:
                self.notify_event.clear()

                try:
                    await asyncio.wait_for(self.notify_event.wait(), timeout=timeout)
                except asyncio.TimeoutError:
                    self._log(f"Timeout: no reports for {timeout}s.", level="warn")
                    break

                if self.report_count >= max_count:
                    self._log(f"Reached max reports ({max_count}).")
                    break

                # Reduce timeout after first report — wait concurrently
                timeout = self.args.notify_timeout  # reset full timeout per report

        except KeyboardInterrupt:
            self._log("Interrupted by user.")

    # ── Logging ──────────────────────────────────────────────

    def _log(self, msg: str, level: str = "info") -> None:
        prefix = {"error": "  ERROR", "warn": "  WARN "}.get(level, "  ")
        ts = f"[{time.monotonic() - self.start_time:07.3f}]"
        print(f"{ts} {prefix} {msg}", file=sys.stderr if level in ("error", "warn") else sys.stdout)

    # ── Cleanup ──────────────────────────────────────────────

    async def close(self) -> None:
        if self.jsonl_file:
            self.jsonl_file.close()
            self.jsonl_file = None
        if self.client and self.client.is_connected:
            try:
                await self.client.disconnect()
            except Exception:
                pass
        if self.gamepad:
            try:
                del self.gamepad
            except Exception:
                pass
            self.gamepad = None
        if self.uinput:
            try:
                self.uinput.close()
            except Exception:
                pass
            self.uinput = None

# ── CLI ─────────────────────────────────────────────────────────

def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Switch 2 BLE Protocol Probe — BlueZ GATT path for Switch 2 Pro"
    )
    p.add_argument("--scan-timeout", type=float, default=10.0,
                   help="BLE scan duration in seconds (default: 10)")
    p.add_argument("--connect-retries", type=int, default=3,
                   help="BLE connection attempts after a matching advertisement is found (default: 3)")
    p.add_argument("--gatt-retries", type=int, default=10,
                   help="GATT service discovery/access attempts after connect (default: 10)")
    p.add_argument("--address", type=str, default=None,
                   help="Filter scan to specific BDADDR (e.g. E0:EF:BF:3B:C6:76)")
    p.add_argument("--name-filter", type=str, default=None,
                   help="Filter scan to device name substring")
    p.add_argument("--manufacturer", type=str, default="0553",
                   help="BLE manufacturer ID to scan for (hex, default: 0553)")
    p.add_argument("--cmd-uuid", type=str, default=COMMAND_WRITE_UUID,
                   help="GATT command write characteristic UUID")
    p.add_argument("--input-uuid", type=str, default=INPUT_REPORT_UUID,
                   help="GATT input report characteristic UUID")
    p.add_argument("--input-handle", type=int, default=None,
                   help="Only record notifications from this ATT handle")
    p.add_argument("--subscribe-all-notify", action="store_true", default=False,
                   help="Subscribe to every notify characteristic and filter by --input-handle")
    p.add_argument("--spro2win", action="store_true", default=False,
                   help="Use SquareDonut1/SPro2Win behavior: no pair writes, subscribe all notify, input handle 45")
    p.add_argument("--mode", choices=("procon2", "pair", "none"), default="procon2",
                   help="Init mode: joycon2cpp ProCon2 init, legacy pair sequence, or no writes (default: procon2)")
    p.add_argument("--decode-procon2", action="store_true", default=True,
                   help="Decode long Pro Controller 2 reports (default: enabled)")
    p.add_argument("--no-decode-procon2", action="store_false", dest="decode_procon2",
                   help="Disable Pro Controller 2 report decoding")
    p.add_argument("--no-led", action="store_true", default=False,
                   help="Do not send the optional LED command in --mode procon2")
    p.add_argument("--no-sound", action="store_true", default=False,
                   help="Do not send the optional sound command in --mode procon2")
    p.add_argument("--notify-before-init", action="store_true", default=False,
                   help="Subscribe to input notifications before sending init commands")
    p.add_argument("--xinput", action="store_true", default=False,
                   help="Create a virtual Xbox 360 controller and map SPro2Win reports to XInput")
    p.add_argument("--uinput", action="store_true", default=False,
                   help="Create a Linux uinput gamepad and map decoded ProCon2 reports")
    p.add_argument("--quiet-reports", action="store_true", default=False,
                   help="Do not print every input report")
    p.add_argument("--dump-jsonl", type=str, default=None,
                   help="Save received reports to JSONL file")
    p.add_argument("--verbose", action="store_true", default=False,
                   help="Verbose output")
    p.add_argument("--no-pair", action="store_true", default=False,
                   help="Compatibility alias for --mode none")
    p.add_argument("--pair-only", action="store_true", default=False,
                   help="Only write selected --mode commands and exit (no notify)")
    p.add_argument("--notify-timeout", type=float, default=30.0,
                   help="Max seconds to wait for reports after subscribe (default: 30)")
    p.add_argument("--max-reports", type=int, default=100,
                   help="Max reports before exit (default: 100)")
    return p.parse_args()

async def main() -> int:
    args = parse_args()
    if args.no_pair:
        args.mode = "none"
    if args.spro2win:
        args.mode = "none"
        args.input_uuid = SPRO2WIN_INPUT_REPORT_UUID
        args.input_handle = SPRO2WIN_INPUT_REPORT_HANDLE
        args.subscribe_all_notify = True
        args.decode_procon2 = False

    # Override global constants from CLI
    global NINTENDO_MANUFACTURER_ID, COMMAND_WRITE_UUID, INPUT_REPORT_UUID
    NINTENDO_MANUFACTURER_ID = int(args.manufacturer, 16)
    COMMAND_WRITE_UUID = args.cmd_uuid
    INPUT_REPORT_UUID = args.input_uuid

    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)

    session = ProbeSession(args)

    # Open JSONL file if requested
    if args.dump_jsonl:
        jsonl_path = Path(args.dump_jsonl)
        session.jsonl_file = jsonl_path.open("w")
        print(f"Writing reports to {jsonl_path}")

    exit_code = 99
    try:
        exit_code = await session.run()
    except KeyboardInterrupt:
        print("\nInterrupted.")
        exit_code = 0 if session.report_count > 0 else 5
    except Exception as e:
        print(f"\nFATAL: {e}", file=sys.stderr)
        if args.verbose:
            import traceback
            traceback.print_exc()
        exit_code = 99
    finally:
        await session.close()

    # Summary
    print(f"\n{'='*60}")
    print(f"Probe complete. Reports received: {session.report_count}")
    print(f"Exit code: {exit_code}")
    if exit_code == 0:
        print("SUCCESS: Notifications are flowing.")
        print("Next: map decoded ProCon2 reports to Linux uinput/UHID.")
    elif exit_code == 5 and session.report_count == 0:
        print("NO NOTIFICATIONS: GATT connected but no input reports received.")
        print("Check: controller must be awake (press buttons after pairing).")
        print("Check: try --mode none to see whether init writes are causing disconnect.")
        print("Check: try --subscribe-all-notify to discover alternate notify handles.")
    return exit_code

if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
