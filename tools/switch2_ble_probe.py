#!/usr/bin/env python3
"""
switch2_ble_probe.py — Phase A: Protocol Verification Tool

Scans for Nintendo Switch 2 controllers over BLE, connects, writes the
CareyScott custom-GATT pair command sequence, subscribes to input report
notifications, and logs received reports as hexdump / JSONL.

Target: Raspberry Pi 5 / LibreELEC (Linux, bleak + BlueZ backend)

Usage:
  python3 tools/switch2_ble_probe.py --verbose
  python3 tools/switch2_ble_probe.py --address E0:EF:BF:3B:C6:76 --dump-jsonl reports.jsonl
  python3 tools/switch2_ble_probe.py --no-pair         # only discover services
  python3 tools/switch2_ble_probe.py --pair-only       # pair + exit, no notify

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

PAIR_CMD = 0x15
PAIR_SUB_SET_MAC = 0x01
PAIR_SUB_KEY_1 = 0x04
PAIR_SUB_KEY_2 = 0x02
PAIR_SUB_FINISH = 0x03

PAIR_KEY_1 = bytes([0x00, 0xea, 0xbd, 0x47, 0x13, 0x89, 0x35, 0x42,
                    0xc6, 0x79, 0xee, 0x07, 0xf2, 0x53, 0x2c, 0x6c, 0x31])
PAIR_KEY_2 = bytes([0x00, 0x40, 0xb0, 0x8a, 0x5f, 0xcd, 0x1f, 0x9b,
                    0x41, 0x12, 0x5c, 0xac, 0xc6, 0x3f, 0x38, 0xa0, 0x73])

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
        result["reconnect_mac"] = decodeu(data[10:16])
    return result

def get_pid_name(pid: int) -> str:
    """Human-readable name for Switch 2 product IDs."""
    names = {
        0x2066: "Joy-Con 2 (Right)",
        0x2067: "Joy-Con 2 (Left)",
        0x2069: "Pro Controller 2",
        0x2073: "NSO GameCube Controller",
    }
    return names.get(pid, f"Unknown (0x{pid:04x})")

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
        # Track UUID discovery
        self.cmd_write_handle: Optional[int] = None
        self.input_report_handle: Optional[int] = None
        self.cmd_response_handle: Optional[int] = None

    async def run(self) -> int:
        """Main orchestration: scan → connect → pair → notify."""
        self.start_time = time.monotonic()

        # 1. Scan
        self.device = await self._scan()
        if not self.device:
            return 1  # No controller found

        # 2. Connect
        ok = await self._connect()
        if not ok:
            return 2  # Connect failed

        # 3. Service discovery
        ok = await self._discover_characteristics()
        if not ok:
            return 3  # Characteristics missing

        # 4. Pair
        if not self.args.no_pair:
            ok = await self._pair()
            if not ok:
                return 4  # Pair failed

        if self.args.pair_only:
            self._log("Pair sequence complete. Exiting (--pair-only).")
            return 0

        # 5. Subscribe to notifications
        ok = await self._subscribe()
        if not ok:
            return 5  # Notify subscribe failed

        # 6. Wait for reports
        self._log("Waiting for input reports... press buttons on the controller.")
        self._log(f"Will run for up to {self.args.notify_timeout}s or {self.args.max_reports} reports.")
        await self._wait_for_reports()

        return 0 if self.report_count > 0 else 5

    # ── Scan ────────────────────────────────────────────────

    async def _scan(self) -> Optional[BLEDevice]:
        """BLE scan for Nintendo Switch 2 controllers."""
        timeout = self.args.scan_timeout
        address_filter = self.args.address
        name_filter = self.args.name_filter

        self._log(f"Scanning for {timeout}s (manufacturer 0x{NINTENDO_MANUFACTURER_ID:04x})...")
        found_device: Optional[BLEDevice] = None
        found_info: dict = {}

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
            pid = info.get("product_id")
            pid_str = get_pid_name(pid) if pid else "unknown"

            self._log(f"  FOUND: {device.address} ({pid_str}) "
                      f"rssi={adv.rssi} name={device.name} "
                      f"mfg_data={hexdump(mgf_data)}")

            if not found_device:
                found_device = device
                found_info = info

        async with BleakScanner(scan_callback):
            await asyncio.sleep(timeout)

        if found_device:
            self._log(f"Selected: {found_device.address} ({get_pid_name(found_info.get('product_id'))})")
            return found_device

        self._log("ERROR: No Nintendo Switch 2 controller found.", level="error")
        if address_filter:
            self._log(f"  Address filter: {address_filter} — is controller advertising?")
        self._log("  Make sure controller is in Sync/Pairing mode (hold Sync button).")
        return None

    # ── Connect ──────────────────────────────────────────────

    async def _connect(self) -> bool:
        """Connect to the discovered device."""
        addr = self.device.address
        self._log(f"Connecting to {addr}...")

        disconnect_event = asyncio.Event()

        def on_disconnect(client: BleakClient):
            self._log(f"Disconnected from {addr}", level="warn")
            disconnect_event.set()

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

    # ── Service Discovery ────────────────────────────────────

    async def _discover_characteristics(self) -> bool:
        """Discover all services. Find command write + input report characteristics."""
        self._log("Discovering GATT services...")

        services = self.client.services
        if not services:
            self._log("ERROR: No services discovered.", level="error")
            return False

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
        if not self.cmd_write_handle:
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
        self._log(f"Subscribing to input report notifications ({INPUT_REPORT_UUID})...")

        def report_callback(sender, data: bytearray):
            self._on_report(bytes(data))

        try:
            await self.client.start_notify(INPUT_REPORT_UUID, report_callback)
            self._log("Notification subscription successful.")
            return True
        except Exception as e:
            self._log(f"ERROR: Failed to subscribe: {e}", level="error")
            return False

    def _on_report(self, data: bytes):
        """Handle an incoming input report."""
        self.report_count += 1
        elapsed = time.monotonic() - self.start_time

        # Build changed_bytes info
        changed = []
        if self.last_report and len(data) == len(self.last_report):
            for i, (a, b) in enumerate(zip(data, self.last_report)):
                if a != b:
                    changed.append(i)
        self.last_report = data

        # Log
        self._log(f"--- Report #{self.report_count} ({len(data)} bytes, t={elapsed:.2f}s) ---")
        self._log(f"  hex: {hexdump(data)}")
        if changed:
            byte_changes = ", ".join(f"[{i}]=0x{data[i]:02x}" for i in changed)
            self._log(f"  changed bytes: {byte_changes}")
        else:
            self._log(f"  changed bytes: (first report or no change)")

        # Optional JSONL dump
        if self.jsonl_file:
            record = {
                "timestamp": elapsed,
                "report_number": self.report_count,
                "length": len(data),
                "hex": data.hex(),
                "changed_bytes": changed,
            }
            self.jsonl_file.write(json.dumps(record) + "\n")
            self.jsonl_file.flush()

        # Signal that we got a report
        self.notify_event.set()

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

# ── CLI ─────────────────────────────────────────────────────────

def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Switch 2 BLE Protocol Probe (Phase A) — verify custom GATT pairing"
    )
    p.add_argument("--scan-timeout", type=float, default=10.0,
                   help="BLE scan duration in seconds (default: 10)")
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
    p.add_argument("--dump-jsonl", type=str, default=None,
                   help="Save received reports to JSONL file")
    p.add_argument("--verbose", action="store_true", default=False,
                   help="Verbose output")
    p.add_argument("--no-pair", action="store_true", default=False,
                   help="Skip pair commands (only service/notify test)")
    p.add_argument("--pair-only", action="store_true", default=False,
                   help="Only write pair commands and exit (no notify)")
    p.add_argument("--notify-timeout", type=float, default=30.0,
                   help="Max seconds to wait for reports after subscribe (default: 30)")
    p.add_argument("--max-reports", type=int, default=100,
                   help="Max reports before exit (default: 100)")
    return p.parse_args()

async def main() -> int:
    args = parse_args()

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
        print("Next: parse reports, build BTstack/C bridge.")
    elif exit_code == 5 and session.report_count == 0:
        print("NO NOTIFICATIONS: Pairing wrote OK but no input reports received.")
        print("Check: controller must be awake (press buttons after pairing).")
        print("Check: verify pair command payloads against CareyScott byte-for-byte.")
        print("Check: try Write-Without-Response vs Write-With-Response.")
    return exit_code

if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
