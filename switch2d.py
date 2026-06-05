#!/usr/bin/env python3
"""
switch2d.py — Nintendo Switch 2 Pro Controller Linux Wireless Daemon (v4.0)

BlueZ/bleak → GATT init → uinput gamepad. Reconnect loop, structured
stage logging, diagnostic mode, systemd-ready.

Usage:
  python3 switch2d.py --diagnose              # verify everything works, then exit
  python3 switch2d.py --daemon                # run forever with reconnect
  python3 switch2d.py --max-reports 0         # infinite reports (no limit)
  python3 switch2d.py --address E0:EF:BF:3B:C6:76 --verbose

Stage exit codes:
  10 — Preflight: missing dependencies or permissions
  11 — Scan: no controller found
  12 — Connect: BLE connection failed
  13 — Discover: required GATT characteristics missing
  14 — Init: ProCon2 init sequence failed
  15 — Subscribe: notification subscription failed
  16 — Running: disconnected unexpectedly
  0  — Diagnose: all stages passed, reports received
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import signal
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional, Dict, List, Any

# ── bleak import guard ────────────────────────────────────────
try:
    from bleak import BleakScanner, BleakClient
    from bleak.backends.device import BLEDevice
    from bleak.backends.scanner import AdvertisementData
except ImportError:
    print("FATAL: bleak not installed. Run: pip install bleak", file=sys.stderr)
    sys.exit(99)

# ── Protocol Constants ─────────────────────────────────────────

NINTENDO_MFR_ID        = 0x0553
NINTENDO_VID           = 0x057E
PRO_CONTROLLER2_PID    = 0x2069

UUID_INPUT_REPORT      = "ab7de9be-89fe-49ad-828f-118f09df7fd2"
UUID_COMMAND_WRITE     = "649d4ac9-8eb7-4e6c-af44-1ea54fe5f005"
UUID_COMMAND_RESPONSE  = "c765a961-d9d8-4d36-a20a-5315b111836a"

# ProCon2 init sequence (joycon2cpp-proven)
PROCON2_INIT_FEATURE_02 = bytes.fromhex("0c 91 01 02 00 04 00 00 ff 00 00 00")
PROCON2_INIT_FEATURE_04 = bytes.fromhex("0c 91 01 04 00 04 00 00 ff 00 00 00")
PROCON2_LED_CMD         = bytes.fromhex("09 91 01 07 00 08 00 00 01 00 00 00 00 00 00 00")

PROCON2_BUTTON_MASKS: Dict[str, int] = {
    "a": 0x000800000000, "b": 0x000400000000, "x": 0x000200000000,
    "y": 0x000100000000, "r": 0x004000000000, "l": 0x000000400000,
    "zr": 0x008000000000, "zl": 0x000000800000, "home": 0x000010000000,
    "back": 0x000001000000, "start": 0x000002000000, "r3": 0x000004000000,
    "l3": 0x000008000000, "dpad_up": 0x000000020000, "dpad_right": 0x000000040000,
    "dpad_down": 0x000000010000, "dpad_left": 0x000000080000,
    "gl": 0x000000000200, "gr": 0x000000000100,
    "screenshot": 0x000020000000, "c": 0x000040000000,
}

# Reconnect backoff parameters
RECONNECT_BASE_DELAY = 1.0
RECONNECT_MAX_DELAY  = 30.0
RECONNECT_BACKOFF    = 2.0

# ── Structured Stage Logger ────────────────────────────────────

class StageLogger:
    """JSON-line structured logging for daemon stages."""

    def __init__(self, json_log: bool = False, verbose: bool = False):
        self.json_log = json_log
        self.verbose = verbose
        self._start_time = time.monotonic()

    def _now(self) -> float:
        return time.monotonic() - self._start_time

    def stage(self, name: str, status: str, **fields) -> None:
        """Log a stage transition with structured data."""
        ts = self._now()
        if self.json_log:
            record = {
                "ts": round(ts, 3),
                "stage": name,
                "status": status,
                **fields,
            }
            print(json.dumps(record), flush=True)
        else:
            marker = {"start": "▶", "ok": "✅", "fail": "❌", "warn": "⚠️", "info": "  "}.get(status, "  ")
            extra = " " + " ".join(f"{k}={v}" for k, v in fields.items()) if fields else ""
            line = f"[{ts:07.3f}] {marker} {name}{extra}"
            if status in ("fail", "warn"):
                print(line, file=sys.stderr, flush=True)
            else:
                print(line, flush=True)

    def info(self, msg: str) -> None:
        if self.verbose or self.json_log:
            self.stage("info", "info", msg=msg)

    def warn(self, msg: str) -> None:
        self.stage("warn", "warn", msg=msg)

    def error(self, msg: str) -> None:
        self.stage("error", "fail", msg=msg)

    def report(self, count: int, length: int, pressed: List[str],
               sticks: Optional[dict] = None) -> None:
        if self.json_log:
            record = {
                "ts": round(self._now(), 3),
                "stage": "report",
                "status": "info",
                "count": count,
                "length": length,
                "pressed": pressed,
            }
            if sticks:
                record["sticks"] = sticks
            print(json.dumps(record), flush=True)
        elif self.verbose:
            pressed_str = ",".join(pressed) if pressed else "none"
            stick_str = ""
            if sticks:
                lx = sticks.get("left", {}).get("x_norm", 0)
                ly = sticks.get("left", {}).get("y_norm", 0)
                rx = sticks.get("right", {}).get("x_norm", 0)
                ry = sticks.get("right", {}).get("y_norm", 0)
                stick_str = f" L=({lx:.2f},{ly:.2f}) R=({rx:.2f},{ry:.2f})"
            print(f"[{self._now():07.3f}]   report #{count}: {pressed_str}{stick_str}", flush=True)


# ── Report Parsing ─────────────────────────────────────────────

def decode_stick12(data: bytes, offset: int) -> Optional[Dict[str, Any]]:
    """Decode 12-bit packed stick at offset..offset+2."""
    if len(data) < offset + 3:
        return None
    raw = data[offset:offset + 3]
    x = ((raw[1] & 0x0F) << 8) | raw[0]
    y = (raw[2] << 4) | ((raw[1] & 0xF0) >> 4)
    return {
        "x": x, "y": y,
        "x_norm": round((x - 2048) / 2047, 3),
        "y_norm": round((y - 2048) / 2047, 3),
    }


def s16(data: bytes, offset: int) -> Optional[int]:
    """Signed 16-bit LE from data."""
    if len(data) < offset + 2:
        return None
    return int.from_bytes(data[offset:offset + 2], "little", signed=True)


def decode_report(data: bytes) -> Optional[Dict[str, Any]]:
    """Decode a Switch 2 Pro Controller input report.

    Report format (≥0x3C bytes):
      bytes 0-2:   packet ID (24-bit LE)
      bytes 3-8:   buttons (48-bit BE bitfield)
      bytes 10-12: left stick (12-bit packed X,Y)
      bytes 13-15: right stick (12-bit packed X,Y)
      bytes 0x30-0x35: accelerometer (3x s16 LE)
      bytes 0x36-0x3B: gyroscope (3x s16 LE)
    """
    if len(data) < 0x3C:
        return None

    # 48-bit button state from bytes 3..8 (big-endian)
    state = 0
    for i in range(3, 9):
        state = (state << 8) | data[i]

    pressed = [name for name, mask in PROCON2_BUTTON_MASKS.items() if state & mask]

    left = decode_stick12(data, 10)
    right = decode_stick12(data, 13)

    return {
        "packet_id": int.from_bytes(data[0:3], "little") if len(data) >= 3 else data[0],
        "button_state": f"0x{state:012x}",
        "pressed": pressed,
        "left_stick": left,
        "right_stick": right,
        "accel": {"x": s16(data, 0x30), "y": s16(data, 0x32), "z": s16(data, 0x34)},
        "gyro":  {"x": s16(data, 0x36), "y": s16(data, 0x38), "z": s16(data, 0x3A)},
    }


# ── Command Building ───────────────────────────────────────────

def build_command(cmd_id: int, sub_id: int, data: bytes = b"") -> bytes:
    """Build a Switch 2 GATT command write payload.

    Wire format: [cmd_id] 0x91 0x01 [sub_id] 0x00 [len(data)] 0x00 0x00 [data]
    """
    return bytes([cmd_id, 0x91, 0x01, sub_id, 0x00, len(data), 0x00, 0x00]) + data


def parse_manufacturer_data(mfg_data: bytes) -> Dict[str, Any]:
    """Parse manufacturer data into vendor_id, product_id, reconnect_mac."""
    result: Dict[str, Any] = {"vendor_id": None, "product_id": None, "reconnect_mac": None}
    if len(mfg_data) >= 16:
        result["vendor_id"] = int.from_bytes(mfg_data[3:5], "little")
        result["product_id"] = int.from_bytes(mfg_data[5:7], "little")
        result["reconnect_mac"] = mfg_data[10:16]
    return result


# ── Daemon Core ────────────────────────────────────────────────

@dataclass
class DaemonState:
    """Mutable state across the daemon lifecycle."""
    args: argparse.Namespace
    log: StageLogger
    client: Optional[BleakClient] = None
    device: Optional[BLEDevice] = None
    cmd_write_handle: Optional[int] = None
    input_report_handle: Optional[int] = None

    # Runtime stats
    report_count: int = 0
    total_reports: int = 0
    reconnect_count: int = 0
    start_time: float = 0.0
    shutdown: bool = False
    disconnect_event: asyncio.Event = field(default_factory=asyncio.Event)
    notify_event: asyncio.Event = field(default_factory=asyncio.Event)

    # uinput
    uinput: Any = None

    async def sleep_breakable(self, delay: float) -> None:
        """Sleep that checks shutdown flag periodically for fast exit."""
        steps = int(delay / 0.25)
        for _ in range(max(steps, 1)):
            if self.shutdown:
                return
            await asyncio.sleep(0.25)

    def reset_session(self) -> None:
        """Reset per-connection state for reconnect."""
        self.client = None
        self.report_count = 0
        self.cmd_write_handle = None
        self.input_report_handle = None
        self.disconnect_event.clear()
        self.notify_event.clear()

    def exit_code_for_stage(self, stage: str) -> int:
        """Map stage name to exit code."""
        codes = {
            "preflight": 10, "scan": 11, "connect": 12,
            "discover": 13, "init": 14, "subscribe": 15, "running": 16,
        }
        return codes.get(stage, 99)


# ── Stage: Preflight ───────────────────────────────────────────

async def stage_preflight(state: DaemonState) -> bool:
    """Check dependencies and permissions."""
    state.log.stage("preflight", "start")

    # Check bleak
    try:
        import bleak
        state.log.info(f"bleak {bleak.__version__}")
    except Exception:
        state.log.error("bleak not available")
        return False

    # Check evdev if uinput requested
    if state.args.uinput:
        try:
            import evdev
            state.log.info("evdev available")
        except ImportError:
            state.log.error("python-evdev not installed; run: pip install evdev")
            return False

        if not os.path.exists("/dev/uinput"):
            state.log.warn("/dev/uinput not found; modprobe uinput?")
            # Non-fatal — uinput will fail later gracefully

    # Try to access Bluetooth adapter
    try:
        devices = await BleakScanner.discover(timeout=2.0, return_adv=False)
        state.log.info("Bluetooth adapter accessible")
    except Exception as e:
        state.log.error(f"Bluetooth adapter not accessible: {e}")
        return False

    state.log.stage("preflight", "ok")
    return True


# ── Stage: Scan ────────────────────────────────────────────────

async def stage_scan(state: DaemonState) -> bool:
    """BLE scan for Nintendo Switch 2 controller."""
    state.log.stage("scan", "start", timeout=state.args.scan_timeout)

    address_filter = state.args.address
    found_device: Optional[BLEDevice] = None
    found_info: Dict[str, Any] = {}
    found_event = asyncio.Event()

    def callback(device: BLEDevice, adv: AdvertisementData):
        nonlocal found_device, found_info

        if address_filter and device.address.upper() != address_filter.upper():
            return

        mfg_data = adv.manufacturer_data.get(NINTENDO_MFR_ID)
        if not mfg_data:
            return

        info = parse_manufacturer_data(mfg_data)
        vid = info.get("vendor_id")
        pid = info.get("product_id")

        if not found_device:
            found_device = device
            found_info = info
            found_event.set()

        state.log.info(
            f"found {device.address} rssi={adv.rssi} "
            f"vid=0x{vid:04x} pid=0x{pid:04x} "
            f"reconnect_mac={info.get('reconnect_mac', b'').hex(':') if info.get('reconnect_mac') else '?'}"
        )

    async with BleakScanner(callback):
        try:
            await asyncio.wait_for(found_event.wait(), timeout=state.args.scan_timeout)
        except asyncio.TimeoutError:
            pass

    if not found_device:
        state.log.stage("scan", "fail", reason="no Nintendo controller found")
        return False

    state.device = found_device
    state.log.stage("scan", "ok", address=found_device.address,
                    vid=f"0x{found_info.get('vendor_id', 0):04x}",
                    pid=f"0x{found_info.get('product_id', 0):04x}")
    return True


# ── Stage: Connect ─────────────────────────────────────────────

async def stage_connect(state: DaemonState) -> bool:
    """Establish BLE connection to the discovered device."""
    state.log.stage("connect", "start", address=state.device.address)

    def on_disconnect(client: BleakClient):  # noqa: ARG001
        state.log.warn(f"disconnected from {state.device.address}")
        state.disconnect_event.set()

    client = BleakClient(state.device, disconnected_callback=on_disconnect)

    for attempt in range(1, state.args.connect_retries + 1):
        if attempt > 1:
            delay = min(1.0 * attempt, 5.0)
            state.log.info(f"connect retry {attempt}/{state.args.connect_retries} in {delay:.1f}s")
            await asyncio.sleep(delay)

        try:
            await asyncio.wait_for(client.connect(), timeout=20.0)
            state.client = client
            state.log.stage("connect", "ok", mtu=client.mtu_size,
                           address=state.device.address)
            return True
        except asyncio.TimeoutError:
            state.log.warn(f"connect timeout (attempt {attempt})")
        except Exception as e:
            state.log.warn(f"connect failed: {e} (attempt {attempt})")

    state.log.stage("connect", "fail", reason="all retries exhausted")
    return False


# ── Stage: Discover ────────────────────────────────────────────

async def stage_discover(state: DaemonState) -> bool:
    """GATT service + characteristic discovery."""
    state.log.stage("discover", "start")

    services = None
    max_attempts = state.args.gatt_retries
    for attempt in range(1, max_attempts + 1):
        try:
            services = state.client.services
            if services:
                break
        except Exception as e:
            state.log.warn(f"GATT service access failed ({attempt}/{max_attempts}): {e}")
        if attempt < max_attempts:
            await asyncio.sleep(0.5)

    if not services:
        state.log.stage("discover", "fail", reason="no services discovered")
        return False

    state.cmd_write_handle = None
    state.input_report_handle = None
    service_count = 0
    char_count = 0

    for svc in services:
        service_count += 1
        for char in svc.characteristics:
            char_count += 1
            uuid_str = str(char.uuid).lower()
            props = ",".join(sorted(char.properties))

            if state.args.verbose:
                state.log.info(f"  char {uuid_str[:36]} [{props}] handle={char.handle}")

            if uuid_str == UUID_INPUT_REPORT.lower():
                state.input_report_handle = char.handle
                state.log.info(f"  → input report: handle={char.handle} props={props}")
            elif uuid_str == UUID_COMMAND_WRITE.lower():
                state.cmd_write_handle = char.handle
                state.log.info(f"  → command write: handle={char.handle} props={props}")

    if not state.input_report_handle:
        state.log.stage("discover", "fail",
                        reason=f"input report UUID {UUID_INPUT_REPORT} not found",
                        services=service_count, chars=char_count)
        return False

    if not state.cmd_write_handle and state.args.mode != "none":
        state.log.stage("discover", "fail",
                        reason=f"command write UUID {UUID_COMMAND_WRITE} not found",
                        services=service_count, chars=char_count)
        return False

    state.log.stage("discover", "ok",
                    services=service_count, chars=char_count,
                    input_handle=state.input_report_handle,
                    cmd_handle=state.cmd_write_handle)
    return True


# ── Stage: Init ────────────────────────────────────────────────

async def stage_init(state: DaemonState) -> bool:
    """Send ProCon2 init sequence (joycon2cpp-proven)."""
    if state.args.mode == "none":
        state.log.stage("init", "ok", skipped="mode=none")
        return True

    state.log.stage("init", "start", mode=state.args.mode)

    commands = [
        ("feature-select 0x02", PROCON2_INIT_FEATURE_02, 0.5),
        ("feature-select 0x04", PROCON2_INIT_FEATURE_04, 0.2),
    ]
    if not state.args.no_led:
        commands.append(("set LED", PROCON2_LED_CMD, 0.05))

    for label, payload, delay_s in commands:
        state.log.info(f"  → {label}: {payload.hex(' ')}")
        try:
            await state.client.write_gatt_char(
                UUID_COMMAND_WRITE, payload, response=False)
            state.log.info(f"  ← {label}: OK")
        except Exception as e:
            state.log.error(f"  ← {label}: FAILED: {e}")
            return False
        if delay_s:
            await asyncio.sleep(delay_s)

    state.log.stage("init", "ok")
    return True


# ── Stage: Subscribe ───────────────────────────────────────────

async def stage_subscribe(state: DaemonState) -> bool:
    """Subscribe to input report notifications."""
    state.log.stage("subscribe", "start")

    def report_handler(sender, data: bytearray):
        handle_report(state, bytes(data), getattr(sender, "handle", None))

    max_attempts = state.args.gatt_retries
    for attempt in range(1, max_attempts + 1):
        try:
            await state.client.start_notify(UUID_INPUT_REPORT, report_handler)
            state.log.stage("subscribe", "ok",
                           handle=state.input_report_handle,
                           uuid=UUID_INPUT_REPORT)
            return True
        except Exception as e:
            state.log.warn(f"subscribe failed ({attempt}/{max_attempts}): {e}")
        if attempt < max_attempts:
            await asyncio.sleep(0.5)

    state.log.stage("subscribe", "fail", reason="all subscribe attempts failed")
    return False


# ── Report Handling ────────────────────────────────────────────

def handle_report(state: DaemonState, data: bytes, handle: Optional[int] = None) -> None:
    """Process an incoming input report."""
    state.report_count += 1
    state.total_reports += 1
    state.notify_event.set()

    decoded = decode_report(data)
    if decoded:
        # Log
        sticks = {}
        if decoded.get("left_stick"):
            sticks["left"] = decoded["left_stick"]
        if decoded.get("right_stick"):
            sticks["right"] = decoded["right_stick"]
        state.log.report(state.report_count, len(data), decoded.get("pressed", []), sticks)

        # Update uinput
        if state.uinput:
            update_uinput(state, decoded)


def update_uinput(state: DaemonState, decoded: Dict[str, Any]) -> None:
    """Map decoded ProCon2 report to Linux uinput events."""
    try:
        from evdev import ecodes as e

        pressed = set(decoded.get("pressed", []))
        btn_map = {
            "b": e.BTN_SOUTH, "a": e.BTN_EAST, "y": e.BTN_NORTH, "x": e.BTN_WEST,
            "l": e.BTN_TL, "r": e.BTN_TR, "zl": e.BTN_TL2, "zr": e.BTN_TR2,
            "back": e.BTN_SELECT, "start": e.BTN_START, "home": e.BTN_MODE,
            "l3": e.BTN_THUMBL, "r3": e.BTN_THUMBR,
            "dpad_up": e.BTN_DPAD_UP, "dpad_down": e.BTN_DPAD_DOWN,
            "dpad_left": e.BTN_DPAD_LEFT, "dpad_right": e.BTN_DPAD_RIGHT,
        }
        for btn_name, code in btn_map.items():
            state.uinput.write(e.EV_KEY, code, 1 if btn_name in pressed else 0)

        def axis_val(stick: Optional[dict], key: str, invert: bool = False) -> int:
            if not stick:
                return 0
            v = stick.get(key, 0.0)
            if invert:
                v = -v
            return max(-32768, min(32767, int(v * 32767)))

        state.uinput.write(e.EV_ABS, e.ABS_X, axis_val(decoded.get("left_stick"), "x_norm"))
        state.uinput.write(e.EV_ABS, e.ABS_Y, axis_val(decoded.get("left_stick"), "y_norm", invert=True))
        state.uinput.write(e.EV_ABS, e.ABS_RX, axis_val(decoded.get("right_stick"), "x_norm"))
        state.uinput.write(e.EV_ABS, e.ABS_RY, axis_val(decoded.get("right_stick"), "y_norm", invert=True))
        state.uinput.write(e.EV_ABS, e.ABS_Z, 255 if "zl" in pressed else 0)
        state.uinput.write(e.EV_ABS, e.ABS_RZ, 255 if "zr" in pressed else 0)
        state.uinput.syn()
    except Exception as e:
        state.log.warn(f"uinput update failed: {e}")


# ── Uinput Setup ───────────────────────────────────────────────

def setup_uinput(state: DaemonState) -> bool:
    """Create a Linux uinput gamepad device."""
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

        state.uinput = UInput(
            {
                e.EV_KEY: buttons,
                e.EV_ABS: [
                    (e.ABS_X, abs_axis), (e.ABS_Y, abs_axis),
                    (e.ABS_RX, abs_axis), (e.ABS_RY, abs_axis),
                    (e.ABS_Z, trigger_axis), (e.ABS_RZ, trigger_axis),
                ],
            },
            name="Nintendo Switch 2 Pro Controller",
            vendor=NINTENDO_VID,
            product=PRO_CONTROLLER2_PID,
            version=1,
        )
        state.log.stage("uinput", "ok", name="Nintendo Switch 2 Pro Controller")
        return True
    except Exception as e:
        state.log.error(f"uinput setup failed: {e}")
        return False


# ── Connection Session ─────────────────────────────────────────

async def run_session(state: DaemonState) -> bool:
    """Run a single full connection session. Returns True on clean shutdown."""
    state.reset_session()
    state.log.stage("session", "start", attempt=state.reconnect_count + 1)

    # Stage pipeline
    stages = [
        ("scan", stage_scan),
        ("connect", stage_connect),
        ("discover", stage_discover),
        ("init", stage_init),
        ("subscribe", stage_subscribe),
    ]

    for name, fn in stages:
        if state.shutdown:
            return True
        if not await fn(state):
            await cleanup_client(state)
            return False

    # Running: wait for reports or disconnect
    state.log.stage("running", "start", max_reports=state.args.max_reports)

    timeout = state.args.notify_timeout
    try:
        while not state.shutdown:
            state.notify_event.clear()

            try:
                await asyncio.wait_for(state.notify_event.wait(), timeout=timeout)
            except asyncio.TimeoutError:
                state.log.warn(f"no reports for {timeout}s")
                break

            if state.args.max_reports > 0 and state.report_count >= state.args.max_reports:
                state.log.stage("running", "ok",
                               reports=state.report_count,
                               reason="max_reports reached")
                break

            timeout = state.args.notify_timeout  # reset after each report

    except Exception as e:
        state.log.error(f"running error: {e}")

    state.log.stage("session", "ok", reports=state.report_count)
    await cleanup_client(state)
    return True


async def cleanup_client(state: DaemonState) -> None:
    """Disconnect and clean up client state."""
    if state.client and state.client.is_connected:
        try:
            await state.client.disconnect()
        except Exception:
            pass
    state.client = None


# ── Reconnect Loop ─────────────────────────────────────────────

async def reconnect_loop(state: DaemonState) -> int:
    """Infinite reconnect loop for daemon mode. Returns only on shutdown."""
    state.log.info("daemon mode: reconnect loop active")

    while not state.shutdown:
        session_ok = await run_session(state)

        if state.shutdown:
            break

        if session_ok:
            # Clean session end — wait for disconnect event before retrying
            state.reconnect_count = 0  # reset backoff on clean sessions
            state.log.info("session ended; waiting for controller...")
            try:
                await asyncio.wait_for(state.disconnect_event.wait(), timeout=5.0)
            except asyncio.TimeoutError:
                pass
        else:
            # Failed session — exponential backoff
            state.reconnect_count += 1
            delay = min(
                RECONNECT_BASE_DELAY * (RECONNECT_BACKOFF ** (state.reconnect_count - 1)),
                RECONNECT_MAX_DELAY,
            )
            state.log.warn(f"reconnect #{state.reconnect_count} in {delay:.1f}s")
            try:
                await asyncio.wait_for(state.sleep_breakable(delay), timeout=delay + 1)
            except asyncio.TimeoutError:
                pass

    return 0


# ── Diagnose Mode ──────────────────────────────────────────────

async def diagnose_mode(state: DaemonState) -> int:
    """Run one full session, print summary, then exit."""
    state.log.stage("diagnose", "start")

    session_ok = await run_session(state)

    state.log.stage("diagnose", "ok" if session_ok and state.report_count > 0 else "fail",
                    session_ok=session_ok,
                    reports=state.report_count,
                    total_reports=state.total_reports)

    if session_ok and state.report_count > 0:
        state.log.info("DIAGNOSE PASSED: connection + reports working")
        return 0
    elif session_ok:
        state.log.error("DIAGNOSE: connected but no reports received")
        return 15
    else:
        state.log.error("DIAGNOSE FAILED: see stage errors above")
        return 16


# ── Signal Handling ────────────────────────────────────────────

def setup_signal_handlers(state: DaemonState, loop: asyncio.AbstractEventLoop) -> None:
    """Register signal handlers for graceful shutdown."""
    def handler():
        state.log.info("shutdown signal received")
        state.shutdown = True

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, handler)
        except NotImplementedError:
            pass  # Windows


# ── CLI ────────────────────────────────────────────────────────

def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="switch2d — Switch 2 Pro Controller BLE daemon (BlueZ/bleak/uinput)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Modes:
  --diagnose     Run full connect→discover→init→subscribe cycle, report results, exit.
  --daemon       Run forever with automatic reconnect on disconnect.
  (default)      Run one session and exit (like diagnose, but no summary).

Examples:
  %(prog)s --diagnose --verbose
  %(prog)s --daemon --uinput --max-reports 0
  %(prog)s --address E0:EF:BF:3B:C6:76 --mode none --verbose
""",
    )

    # Mode
    p.add_argument("--diagnose", action="store_true", default=False,
                   help="Diagnostic mode: verify everything works, then exit")
    p.add_argument("--daemon", action="store_true", default=False,
                   help="Daemon mode: run forever with reconnect loop")

    # Scan
    p.add_argument("--scan-timeout", type=float, default=10.0,
                   help="BLE scan duration in seconds (default: 10)")
    p.add_argument("--address", type=str, default=None,
                   help="Filter scan to specific BDADDR")

    # Connect
    p.add_argument("--connect-retries", type=int, default=3,
                   help="BLE connection retries (default: 3)")

    # GATT
    p.add_argument("--gatt-retries", type=int, default=10,
                   help="GATT service discovery retries (default: 10)")
    p.add_argument("--mode", choices=("procon2", "none"), default="procon2",
                   help="Init mode: procon2=joycon2cpp init, none=skip init (default: procon2)")
    p.add_argument("--no-led", action="store_true", default=False,
                   help="Skip LED command in procon2 init")
    p.add_argument("--notify-timeout", type=float, default=30.0,
                   help="Max seconds without reports before disconnect (default: 30)")

    # Reports
    p.add_argument("--max-reports", type=int, default=100,
                   help="Max reports before exit (0=unlimited, default: 100)")

    # uinput
    p.add_argument("--uinput", action="store_true", default=False,
                   help="Create Linux uinput gamepad device")

    # Logging
    p.add_argument("--verbose", action="store_true", default=False,
                   help="Verbose output")
    p.add_argument("--json", action="store_true", default=False,
                   help="JSON-line structured log output")
    p.add_argument("--quiet", action="store_true", default=False,
                   help="Minimal output (errors only)")

    return p.parse_args()


# ── Main ───────────────────────────────────────────────────────

async def async_main() -> int:
    args = parse_args()

    # Logging setup
    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)
    elif args.quiet:
        logging.basicConfig(level=logging.WARNING)

    log = StageLogger(json_log=args.json, verbose=args.verbose)
    state = DaemonState(args=args, log=log)
    state.start_time = time.monotonic()

    # Signal handlers
    loop = asyncio.get_running_loop()
    setup_signal_handlers(state, loop)

    # Preflight
    if not await stage_preflight(state):
        return 10

    # Setup uinput if requested
    if args.uinput and not setup_uinput(state):
        if args.diagnose:
            return 10

    try:
        if args.diagnose:
            return await diagnose_mode(state)
        elif args.daemon:
            return await reconnect_loop(state)
        else:
            # Default: single session
            session_ok = await run_session(state)
            return 0 if (session_ok and state.report_count > 0) else 16
    except KeyboardInterrupt:
        log.info("interrupted")
        return 0
    except Exception as e:
        log.error(f"fatal: {e}")
        if args.verbose:
            import traceback
            traceback.print_exc()
        return 99
    finally:
        # Cleanup uinput
        if state.uinput:
            try:
                state.uinput.close()
            except Exception:
                pass
        await cleanup_client(state)


def main() -> None:
    sys.exit(asyncio.run(async_main()))


if __name__ == "__main__":
    main()
