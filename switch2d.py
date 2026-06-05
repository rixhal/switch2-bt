#!/usr/bin/env python3
"""
switch2d.py — Nintendo Switch 2 Pro Controller Linux Wireless Daemon (v5.0)

Protocol profile system based on three documented working implementations:
  - macOS:  switch2bridge-macos (UUID 7492866c, selected notify)
  - SPro2Win: SquareDonut1/SPro2Win (subscribe-all, handle 45)
  - joycon2cpp: TheFrano/joycon2cpp (init writes, UUID ab7de9be)

BlueZ/bleak → profile-driven GATT strategy → uinput gamepad.
Reconnect loop, structured stage logging, diagnostic JSON summary, systemd-ready.

Usage:
  python3 switch2d.py --diagnose --verbose          # auto-detect best profile
  python3 switch2d.py --daemon --profile macos       # run forever with explicit profile
  python3 switch2d.py --max-reports 0               # infinite reports
  python3 switch2d.py --address E0:EF:BF:3B:C6:76 --verbose

Stage exit codes:
  10 — Preflight: missing dependencies or permissions
  11 — Scan: no controller found
  12 — Connect: BLE connection failed
  13 — Discover: required GATT characteristics missing
  14 — Init: init sequence failed (joycon2cpp profile only)
  15 — Subscribe: notification subscription failed or zero reports
  16 — Running: disconnected unexpectedly
  0  — Diagnose: all stages passed, reports received

⚠️ STATUS: Awaiting hardware validation. The daemon compiles and passes unit
   tests but has NOT been tested with the actual Switch 2 Pro Controller on a
   Raspberry Pi. Do not claim wireless working until a real hardware run is
   documented.
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

# ═══════════════════════════════════════════════════════════════
# PROTOCOL PROFILES
# ═══════════════════════════════════════════════════════════════

@dataclass
class ProtocolProfile:
    """Encapsulates one documented connection strategy."""
    name: str                                    # "macos" | "spro2win" | "joycon2cpp"
    source: str                                  # citation
    input_uuids: List[str]                       # UUIDs to probe (first match wins)
    command_uuid: Optional[str]                  # for init writes (None = no init)
    init_commands: List[tuple[str, bytes, float]]  # (label, payload, delay_s)
    subscribe_strategy: str                      # "selected" | "all"
    spro2win_handle: Optional[int]              # handle filter for "all" strategy
    description: str

# ── Profile definitions ──

PROFILE_MACOS = ProtocolProfile(
    name="macos",
    source="mlstr0m/switch2bridge-macos (macOS, WORKING)",
    input_uuids=["7492866c-ec3e-4619-8258-32755ffcc0f9"],
    command_uuid=None,
    init_commands=[],
    subscribe_strategy="selected",
    spro2win_handle=None,
    description="Selected notify on 7492866c. No init. Matches macOS CoreBluetooth path.",
)

PROFILE_SPRO2WIN = ProtocolProfile(
    name="spro2win",
    source="SquareDonut1/SPro2Win (Windows, WORKING)",
    input_uuids=[],  # no fixed UUID — subscribe to ALL notifies
    command_uuid=None,
    init_commands=[],
    subscribe_strategy="all",
    spro2win_handle=45,
    description="Subscribe-all notify, filter handle 45. No init. Matches Windows BLE path.",
)

PROFILE_JOYCON2CPP = ProtocolProfile(
    name="joycon2cpp",
    source="TheFrano/joycon2cpp (Windows, WORKING)",
    input_uuids=["ab7de9be-89fe-49ad-828f-118f09df7fd2"],
    command_uuid="649d4ac9-8eb7-4e6c-af44-1ea54fe5f005",
    init_commands=[
        ("feature-select 0x02", bytes.fromhex("0c 91 01 02 00 04 00 00 ff 00 00 00"), 0.5),
        ("feature-select 0x04", bytes.fromhex("0c 91 01 04 00 04 00 00 ff 00 00 00"), 0.2),
        ("set LED", bytes.fromhex("09 91 01 07 00 08 00 00 01 00 00 00 00 00 00 00"), 0.05),
    ],
    subscribe_strategy="selected",
    spro2win_handle=None,
    description="Init writes + selected notify on ab7de9be. Full ProCon2 init sequence.",
)

PROTOCOL_PROFILES: Dict[str, ProtocolProfile] = {
    "macos": PROFILE_MACOS,
    "spro2win": PROFILE_SPRO2WIN,
    "joycon2cpp": PROFILE_JOYCON2CPP,
}

# Auto-detect order (for --profile auto)
AUTO_PROFILE_ORDER = ["macos", "spro2win", "joycon2cpp"]

# ═══════════════════════════════════════════════════════════════
# PROTOCOL CONSTANTS
# ═══════════════════════════════════════════════════════════════

NINTENDO_MFR_ID        = 0x0553
NINTENDO_VID           = 0x057E
PRO_CONTROLLER2_PID    = 0x2069
PRO_CONTROLLER2_PID_LE = b'\x69\x20'  # 0x2069 little-endian, for scan matching

# Button map: byte-level bitmasks from working implementations (SPro2Win + macOS bridge).
# Byte 2 (right cluster): bits 0-7
# Byte 3 (left cluster):  bits 0-7
# Byte 4 (system/grip):  bits 0-7
BUTTON_MAP: Dict[str, tuple] = {
    # Byte 2 — right cluster (verified by SPro2Win + macOS bridge)
    "b":     (2, 0x01), "a":  (2, 0x02), "y":  (2, 0x04), "x":  (2, 0x08),
    "r":     (2, 0x10), "zr": (2, 0x20), "start": (2, 0x40), "r3": (2, 0x80),
    # Byte 3 — left cluster
    "dpad_down":  (3, 0x01), "dpad_right": (3, 0x02),
    "dpad_left":  (3, 0x04), "dpad_up":    (3, 0x08),
    "l":     (3, 0x10), "zl": (3, 0x20), "back": (3, 0x40), "l3": (3, 0x80),
    # Byte 4 — system + grip
    "home":       (4, 0x01),
    "c":          (4, 0x02),  # SPro2Win: bit 1 = C button
    "gr":         (4, 0x04),
    "gl":         (4, 0x08),
    "screenshot": (4, 0x10),
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
    """Decode a Switch 2 Pro Controller input report using byte-level bitmasks.

    Report format (verified by SPro2Win + macOS bridge):
      byte 0-1:   timer/counter (varies)
      byte 2:     right cluster buttons (8 bits)
      byte 3:     left cluster buttons (8 bits)
      byte 4:     system + grip buttons (8 bits)
      bytes 5-7:  left stick (12-bit packed X,Y)
      bytes 8-10: right stick (12-bit packed X,Y)
      bytes 11+:  accelerometer, gyro (optional, varies by report length)

    Minimum valid report: 11 bytes (button data + sticks).
    Longer reports (0x3C bytes) include accel/gyro at 0x30+.
    """
    if len(data) < 11:
        return None

    pressed = [
        name for name, (byte_idx, mask) in BUTTON_MAP.items()
        if len(data) > byte_idx and (data[byte_idx] & mask)
    ]

    left = decode_stick12(data, 5)
    right = decode_stick12(data, 8)

    result: Dict[str, Any] = {
        "packet_id": int.from_bytes(data[0:2], "little") if len(data) >= 2 else data[0],
        "pressed": pressed,
        "left_stick": left,
        "right_stick": right,
    }

    if len(data) >= 0x3C:
        result["accel"] = {"x": s16(data, 0x30), "y": s16(data, 0x32), "z": s16(data, 0x34)}
        result["gyro"] = {"x": s16(data, 0x36), "y": s16(data, 0x38), "z": s16(data, 0x3A)}

    return result


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
    input_report_uuid: Optional[str] = None

    # Active profile — always set before any stage function runs
    active_profile: ProtocolProfile = field(default_factory=lambda: PROFILE_MACOS)

    # Runtime stats
    report_count: int = 0
    total_reports: int = 0
    reconnect_count: int = 0
    start_time: float = 0.0
    shutdown: bool = False
    disconnect_event: asyncio.Event = field(default_factory=asyncio.Event)
    notify_event: asyncio.Event = field(default_factory=asyncio.Event)

    # Diagnostic tracking
    first_report_hex: Optional[str] = None
    last_report_hex: Optional[str] = None
    jsonl_file: Any = None
    report_lengths: List[int] = field(default_factory=list)
    report_timestamps: List[float] = field(default_factory=list)

    # Stage result flags — survive cleanup_client()
    stage_scan_ok: bool = False
    stage_connect_ok: bool = False
    stage_discover_ok: bool = False
    stage_subscribe_ok: bool = False
    stage_reports_ok: bool = False

    # Profile tracking (auto mode)
    attempted_profiles: List[Dict[str, Any]] = field(default_factory=list)
    winning_profile: Optional[str] = None

    # uinput
    uinput: Any = None

    async def sleep_breakable(self, delay: float) -> None:
        steps = int(delay / 0.25)
        for _ in range(max(steps, 1)):
            if self.shutdown:
                return
            await asyncio.sleep(0.25)

    def reset_session(self) -> None:
        self.client = None
        self.report_count = 0
        self.cmd_write_handle = None
        self.input_report_handle = None
        self.input_report_uuid = None
        self.disconnect_event.clear()
        self.notify_event.clear()

    def exit_code_for_stage(self, stage: str) -> int:
        codes = {
            "preflight": 10, "scan": 11, "connect": 12,
            "discover": 13, "init": 14, "subscribe": 15, "running": 16,
        }
        return codes.get(stage, 99)


# ── Stage: Preflight ───────────────────────────────────────────

async def stage_preflight(state: DaemonState) -> bool:
    state.log.stage("preflight", "start")
    try:
        import bleak
        state.log.info(f"bleak {bleak.__version__}")
    except Exception:
        state.log.error("bleak not available")
        return False
    if state.args.uinput:
        try:
            import evdev
            state.log.info("evdev available")
        except ImportError:
            state.log.error("python-evdev not installed; run: pip install evdev")
            return False
        if not os.path.exists("/dev/uinput"):
            state.log.warn("/dev/uinput not found; modprobe uinput?")
    try:
        await BleakScanner.discover(timeout=2.0, return_adv=False)
        state.log.info("Bluetooth adapter accessible")
    except Exception as e:
        state.log.error(f"Bluetooth adapter not accessible: {e}")
        return False
    state.log.stage("preflight", "ok")
    return True


# ── Stage: Scan ────────────────────────────────────────────────

async def stage_scan(state: DaemonState) -> bool:
    strict_pid = not state.args.loose_scan
    state.log.stage("scan", "start", timeout=state.args.scan_timeout, strict_pid=strict_pid)

    address_filter = state.args.address
    found_device: Optional[BLEDevice] = None
    found_info: Dict[str, Any] = {}
    found_event = asyncio.Event()

    def callback(device: BLEDevice, adv: AdvertisementData):
        nonlocal found_device, found_info
        if address_filter and device.address.upper() != address_filter.upper():
            return
        mfg_data = adv.manufacturer_data.get(NINTENDO_MFR_ID)
        nintendo_vid_data = adv.manufacturer_data.get(NINTENDO_VID)
        if not mfg_data and not nintendo_vid_data:
            return
        if nintendo_vid_data and PRO_CONTROLLER2_PID_LE not in nintendo_vid_data:
            if not mfg_data:
                return
        info = parse_manufacturer_data(mfg_data if mfg_data else nintendo_vid_data)
        pid = info.get("product_id")
        if strict_pid and pid != PRO_CONTROLLER2_PID:
            if state.args.verbose:
                state.log.info(f"  skip {device.address} pid=0x{pid:04x} (not Pro Controller 2)")
            return
        if not found_device:
            found_device = device
            found_info = info
            found_event.set()
        state.log.info(
            f"found {device.address} rssi={adv.rssi} "
            f"vid=0x{info.get('vendor_id', 0):04x} pid=0x{pid:04x} "
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
    state.stage_scan_ok = True
    state.log.stage("scan", "ok", address=found_device.address,
                    vid=f"0x{found_info.get('vendor_id', 0):04x}",
                    pid=f"0x{found_info.get('product_id', 0):04x}")
    return True


# ── Stage: Connect ─────────────────────────────────────────────

async def stage_connect(state: DaemonState) -> bool:
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
            state.stage_connect_ok = True
            state.log.stage("connect", "ok", mtu=client.mtu_size, address=state.device.address)
            return True
        except asyncio.TimeoutError:
            state.log.warn(f"connect timeout (attempt {attempt})")
        except Exception as e:
            state.log.warn(f"connect failed: {e} (attempt {attempt})")

    state.log.stage("connect", "fail", reason="all retries exhausted")
    return False


# ── Stage: Discover ────────────────────────────────────────────

async def stage_discover(state: DaemonState) -> bool:
    """GATT service + characteristic discovery, profile-driven."""
    profile = state.active_profile
    state.log.stage("discover", "start", profile=profile.name)

    services = None
    max_attempts = state.args.gatt_retries
    for attempt in range(1, max_attempts + 1):
        try:
            services = state.client.services
            if services:
                break
        except Exception as e:
            state.log.warn(f"GATT .services failed ({attempt}/{max_attempts}): {e}")
        try:
            services = await state.client.get_services()
            if services:
                state.log.info("GATT services retrieved via get_services()")
                break
        except Exception:
            pass
        if attempt < max_attempts:
            await asyncio.sleep(0.5)

    if not services:
        state.log.stage("discover", "fail", reason="no services discovered")
        return False

    state.cmd_write_handle = None
    state.input_report_handle = None
    state.input_report_uuid = None
    service_count = 0
    char_count = 0

    input_candidates = [] if profile.subscribe_strategy == "all" else profile.input_uuids

    for svc in services:
        service_count += 1
        for char in svc.characteristics:
            char_count += 1
            uuid_str = str(char.uuid).lower()
            props = ",".join(sorted(char.properties))

            if state.args.verbose:
                state.log.info(f"  char {uuid_str[:36]} [{props}] handle={char.handle}")

            # Match input UUIDs (only for "selected" strategy)
            if profile.subscribe_strategy == "selected" and not state.input_report_handle:
                for candidate_uuid in input_candidates:
                    if uuid_str == candidate_uuid.lower():
                        state.input_report_handle = char.handle
                        state.input_report_uuid = candidate_uuid
                        state.log.info(f"  → input report: handle={char.handle} uuid={candidate_uuid[:36]} props={props}")
                        break

            # Match command UUID (only if profile requires init)
            if profile.command_uuid and uuid_str == profile.command_uuid.lower():
                state.cmd_write_handle = char.handle
                state.log.info(f"  → command write: handle={char.handle} props={props}")

    if profile.subscribe_strategy == "selected" and not state.input_report_handle:
        state.log.stage("discover", "fail",
                        reason=f"no input UUID found (profile={profile.name}, tried={input_candidates})",
                        services=service_count, chars=char_count)
        return False

    if profile.command_uuid and not state.cmd_write_handle:
        state.log.stage("discover", "fail",
                        reason=f"command write UUID {profile.command_uuid} not found",
                        services=service_count, chars=char_count)
        return False

    state.stage_discover_ok = True
    state.log.stage("discover", "ok",
                    services=service_count, chars=char_count,
                    input_handle=state.input_report_handle,
                    cmd_handle=state.cmd_write_handle)
    return True


# ── Stage: Init ────────────────────────────────────────────────

async def stage_init(state: DaemonState) -> bool:
    """Send profile-specific init commands."""
    profile = state.active_profile
    if not profile.init_commands:
        state.log.stage("init", "ok", skipped=f"profile={profile.name} has no init")
        return True

    state.log.stage("init", "start", profile=profile.name)
    for label, payload, delay_s in profile.init_commands:
        state.log.info(f"  → {label}: {payload.hex(' ')}")
        try:
            await state.client.write_gatt_char(profile.command_uuid, payload, response=False)
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
    """Subscribe to input reports — profile-driven strategy."""
    profile = state.active_profile
    state.log.stage("subscribe", "start", profile=profile.name, strategy=profile.subscribe_strategy)

    handler = _make_report_handler(state)

    if profile.subscribe_strategy == "all":
        return await _subscribe_all_notify(state, handler)

    # "selected" strategy: subscribe to matched UUID
    matched_uuid = state.input_report_uuid
    if not matched_uuid:
        state.log.stage("subscribe", "fail", reason="no input UUID matched during discover")
        return False

    max_attempts = state.args.gatt_retries
    for attempt in range(1, max_attempts + 1):
        try:
            await state.client.start_notify(matched_uuid, handler)
            state.stage_subscribe_ok = True
            state.log.stage("subscribe", "ok", handle=state.input_report_handle, uuid=matched_uuid)
            return True
        except Exception as e:
            state.log.warn(f"subscribe failed ({attempt}/{max_attempts}): {e}")
        if attempt < max_attempts:
            await asyncio.sleep(0.5)

    state.log.stage("subscribe", "fail", reason="all subscribe attempts failed")
    return False


async def _subscribe_all_notify(state: DaemonState, handler) -> bool:
    """Subscribe to every notify characteristic, filter by profile handle."""
    profile = state.active_profile
    state.log.info("subscribe-all mode: subscribing to all notify characteristics")
    count = 0
    for svc in state.client.services:
        for char in svc.characteristics:
            if "notify" not in char.properties:
                continue
            try:
                await state.client.start_notify(char, handler)
                count += 1
                state.log.info(f"  subscribed: {char.uuid} handle={char.handle}")
                # Record first notify-capable handle as input source
                if state.input_report_handle is None:
                    state.input_report_handle = char.handle
            except Exception as e:
                state.log.warn(f"  subscribe failed handle={char.handle}: {e}")
    if count == 0:
        state.log.stage("subscribe", "fail", reason="no notify characteristics found")
        return False
    state.stage_subscribe_ok = True
    state.log.stage("subscribe", "ok", mode="subscribe-all", count=count)
    return count > 0


# ── Report Handling ────────────────────────────────────────────

def handle_report(state: DaemonState, data: bytes, handle: Optional[int] = None) -> None:
    """Process an incoming input report."""
    # SPro2Win-style handle filter
    profile = state.active_profile
    if profile and profile.spro2win_handle is not None:
        if handle is not None and handle != profile.spro2win_handle:
            return

    state.report_count += 1
    state.total_reports += 1
    state.notify_event.set()
    state.stage_reports_ok = True

    now = time.monotonic()
    state.report_lengths.append(len(data))
    state.report_timestamps.append(now)

    hex_str = data.hex()
    if state.first_report_hex is None:
        state.first_report_hex = hex_str
    state.last_report_hex = hex_str

    if state.jsonl_file:
        record = {
            "ts": round(time.monotonic() - state.start_time, 3),
            "report": state.report_count,
            "handle": handle,
            "length": len(data),
            "hex": hex_str,
        }
        state.jsonl_file.write(json.dumps(record) + "\n")
        state.jsonl_file.flush()

    decoded = decode_report(data)
    if decoded:
        sticks = {}
        if decoded.get("left_stick"):
            sticks["left"] = decoded["left_stick"]
        if decoded.get("right_stick"):
            sticks["right"] = decoded["right_stick"]
        state.log.report(state.report_count, len(data), decoded.get("pressed", []), sticks)
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
            {e.EV_KEY: buttons, e.EV_ABS: [
                (e.ABS_X, abs_axis), (e.ABS_Y, abs_axis),
                (e.ABS_RX, abs_axis), (e.ABS_RY, abs_axis),
                (e.ABS_Z, trigger_axis), (e.ABS_RZ, trigger_axis),
            ]},
            name="Nintendo Switch 2 Pro Controller",
            vendor=NINTENDO_VID, product=PRO_CONTROLLER2_PID, version=1,
        )
        state.log.stage("uinput", "ok", name="Nintendo Switch 2 Pro Controller")
        return True
    except Exception as e:
        state.log.error(f"uinput setup failed: {e}")
        return False


# ── Session (profile-driven) ───────────────────────────────────

async def run_session(state: DaemonState, profile: Optional[ProtocolProfile] = None) -> bool:
    """Run a single connection session with the given profile."""
    if profile:
        state.active_profile = profile
    else:
        # No profile set yet — use macos as fallback
        state.active_profile = PROFILE_MACOS

    state.reset_session()
    state.log.stage("session", "start", profile=state.active_profile.name, attempt=state.reconnect_count + 1)

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
                state.log.stage("running", "ok", reports=state.report_count, reason="max_reports reached")
                break
            timeout = state.args.notify_timeout
    except Exception as e:
        state.log.error(f"running error: {e}")

    state.log.stage("session", "ok", reports=state.report_count)
    await cleanup_client(state)
    return state.report_count > 0


async def cleanup_client(state: DaemonState) -> None:
    if state.client and state.client.is_connected:
        try:
            await state.client.disconnect()
        except Exception:
            pass
    state.client = None


# ── Reconnect Loop ─────────────────────────────────────────────

async def reconnect_loop(state: DaemonState) -> int:
    state.log.info("daemon mode: reconnect loop active")
    while not state.shutdown:
        session_ok = await run_session(state)
        if state.shutdown:
            break
        if session_ok:
            state.reconnect_count = 0
            state.log.info("session ended; waiting for controller...")
            try:
                await asyncio.wait_for(state.disconnect_event.wait(), timeout=5.0)
            except asyncio.TimeoutError:
                pass
        else:
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


# ── Auto-Detect Mode ───────────────────────────────────────────

async def _try_profile(state: DaemonState, profile: ProtocolProfile) -> Dict[str, Any]:
    """Try one profile: connect, run session, record result."""
    result: Dict[str, Any] = {
        "profile": profile.name,
        "source": profile.source,
        "success": False,
        "failure_reason": None,
        "report_count": 0,
        "first_report_hex": None,
    }
    state.log.stage("auto", "start", profile=profile.name)

    session_ok = await run_session(state, profile)
    result["report_count"] = state.report_count
    result["first_report_hex"] = state.first_report_hex

    if session_ok and state.report_count > 0:
        result["success"] = True
        state.winning_profile = profile.name
        state.log.stage("auto", "ok", profile=profile.name, reports=state.report_count)
    elif session_ok:
        result["failure_reason"] = "subscribed but zero reports"
        state.log.stage("auto", "fail", profile=profile.name, reason=result["failure_reason"])
    elif state.stage_subscribe_ok:
        result["failure_reason"] = "subscribed but zero reports"
        state.log.stage("auto", "fail", profile=profile.name, reason=result["failure_reason"])
    elif state.stage_connect_ok:
        result["failure_reason"] = "subscribe failed"
        state.log.stage("auto", "fail", profile=profile.name, reason=result["failure_reason"])
    elif state.stage_scan_ok:
        result["failure_reason"] = "connect failed"
        state.log.stage("auto", "fail", profile=profile.name, reason=result["failure_reason"])
    else:
        result["failure_reason"] = "scan failed"
        state.log.stage("auto", "fail", profile=profile.name, reason=result["failure_reason"])

    return result


async def auto_detect_mode(state: DaemonState) -> int:
    """Try profiles in order: macos → spro2win → joycon2cpp."""
    state.log.stage("diagnose", "start", mode="auto")

    for profile_name in AUTO_PROFILE_ORDER:
        if state.shutdown:
            break
        profile = PROTOCOL_PROFILES[profile_name]

        # Collect telemetry from this attempt
        state.report_lengths.clear()
        state.report_timestamps.clear()
        state.first_report_hex = None

        result = await _try_profile(state, profile)
        state.attempted_profiles.append(result)

        if result["success"]:
            break

        # Need to re-scan/re-connect for next profile attempt
        if profile_name != AUTO_PROFILE_ORDER[-1]:
            state.log.info(f"auto: trying next profile...")
            state.reset_session()

    # ── Diagnostic summary ──
    intervals = []
    for i in range(1, len(state.report_timestamps)):
        intervals.append((state.report_timestamps[i] - state.report_timestamps[i-1]) * 1000.0)

    telemetry: Dict[str, Any] = {}
    if state.report_lengths:
        telemetry["length_min"] = min(state.report_lengths)
        telemetry["length_max"] = max(state.report_lengths)
        telemetry["length_avg"] = round(sum(state.report_lengths) / len(state.report_lengths), 1)
    if intervals:
        telemetry["interval_min_ms"] = round(min(intervals), 1)
        telemetry["interval_max_ms"] = round(max(intervals), 1)
        telemetry["interval_avg_ms"] = round(sum(intervals) / len(intervals), 1)

    winner = state.winning_profile
    summary: Dict[str, Any] = {
        "exit_code": 0 if winner else 16,
        "mode": "auto",
        "attempted_profiles": state.attempted_profiles,
        "winning_profile": winner,
        "report_count": state.report_count if winner else 0,
        "first_report_hex": state.first_report_hex,
        "telemetry": telemetry,
    }

    if state.uinput is not None:
        summary["uinput"] = True

    print(json.dumps(summary, indent=2), flush=True)

    if winner:
        state.log.info(f"AUTO-DIAGNOSE PASSED: winning profile={winner}, reports={state.report_count}")
        return 0
    else:
        state.log.error("AUTO-DIAGNOSE FAILED: no profile succeeded")
        return 16


# ── Diagnose Mode ──────────────────────────────────────────────

async def diagnose_mode(state: DaemonState) -> int:
    """Run one session with explicit profile, print diagnostic JSON summary."""
    if state.args.profile == "auto":
        return await auto_detect_mode(state)

    state.log.stage("diagnose", "start", profile=state.args.profile)
    profile = PROTOCOL_PROFILES[state.args.profile]
    session_ok = await run_session(state, profile)

    intervals = []
    for i in range(1, len(state.report_timestamps)):
        intervals.append((state.report_timestamps[i] - state.report_timestamps[i-1]) * 1000.0)

    telemetry: Dict[str, Any] = {}
    if state.report_lengths:
        telemetry["length_min"] = min(state.report_lengths)
        telemetry["length_max"] = max(state.report_lengths)
        telemetry["length_avg"] = round(sum(state.report_lengths) / len(state.report_lengths), 1)
    if intervals:
        telemetry["interval_min_ms"] = round(min(intervals), 1)
        telemetry["interval_max_ms"] = round(max(intervals), 1)
        telemetry["interval_avg_ms"] = round(sum(intervals) / len(intervals), 1)

    stages = {
        "scan": state.stage_scan_ok,
        "connect": state.stage_connect_ok,
        "discover": state.stage_discover_ok,
        "subscribe": state.stage_subscribe_ok,
        "reports": state.stage_reports_ok,
        "uinput": state.uinput is not None,
    }
    summary: Dict[str, Any] = {
        "exit_code": 0 if (session_ok and state.report_count > 0) else (15 if session_ok else 16),
        "profile": profile.name,
        "stages": stages,
        "device": state.device.address if state.device else None,
        "input_uuid": state.input_report_uuid,
        "input_handle": state.input_report_handle,
        "report_count": state.report_count,
        "first_report_hex": state.first_report_hex,
        "last_report_hex": state.last_report_hex,
        "telemetry": telemetry,
    }

    print(json.dumps(summary, indent=2), flush=True)

    if session_ok and state.report_count > 0:
        state.log.info("DIAGNOSE PASSED: connection + reports working")
        return 0
    elif session_ok:
        state.log.error("DIAGNOSE: connected but no reports received")
        return 15
    else:
        state.log.error("DIAGNOSE FAILED: see stage errors above")
        return 16


def _make_report_handler(state: DaemonState):
    def handler(sender, data: bytearray):
        handle_report(state, bytes(data), getattr(sender, "handle", None))
    return handler


# ── Signal Handling ────────────────────────────────────────────

def setup_signal_handlers(state: DaemonState, loop: asyncio.AbstractEventLoop) -> None:
    def handler():
        state.log.info("shutdown signal received")
        state.shutdown = True
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, handler)
        except NotImplementedError:
            pass


# ── CLI ────────────────────────────────────────────────────────

def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="switch2d — Switch 2 Pro Controller BLE daemon (BlueZ/bleak/uinput) v5.0",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Modes:
  --diagnose     Auto-detect best profile, verify, print JSON summary, exit.
  --daemon       Run forever with automatic reconnect on disconnect.
  (default)      Run one session and exit.

Profiles:
  auto           Try macos → spro2win → joycon2cpp sequentially.
  macos          Selected notify on UUID 7492866c (switch2bridge-macos).
  spro2win       Subscribe-all notify, filter handle 45 (SPro2Win).
  joycon2cpp     Init writes + selected notify on ab7de9be (joycon2cpp).

Examples:
  %(prog)s --diagnose --verbose                     # auto-detect
  %(prog)s --diagnose --profile spro2win --verbose  # explicit profile
  %(prog)s --daemon --profile macos --uinput --max-reports 0
  %(prog)s --address E0:EF:BF:3B:C6:76 --verbose
""",
    )

    # Mode
    p.add_argument("--diagnose", action="store_true", default=False,
                   help="Diagnostic mode: verify everything, print JSON summary, exit")
    p.add_argument("--daemon", action="store_true", default=False,
                   help="Daemon mode: run forever with reconnect loop")

    # Profile
    p.add_argument("--profile", type=str, default="auto",
                   choices=("auto", "macos", "spro2win", "joycon2cpp"),
                   help="Protocol profile. auto=sequential fallback, macos/spro2win/joycon2cpp=explicit. "
                        "Default: auto for --diagnose, macos for --daemon.")

    # Scan
    p.add_argument("--scan-timeout", type=float, default=10.0)
    p.add_argument("--address", type=str, default=None)
    p.add_argument("--loose-scan", action="store_true", default=False,
                   help="Accept any Nintendo device (default: PID 0x2069 only)")

    # Connect
    p.add_argument("--connect-retries", type=int, default=3)

    # GATT
    p.add_argument("--gatt-retries", type=int, default=10)
    p.add_argument("--notify-timeout", type=float, default=30.0,
                   help="Max seconds without reports before disconnect (default: 30)")

    # Reports
    p.add_argument("--max-reports", type=int, default=100,
                   help="Max reports before exit (0=unlimited, default: 100)")
    p.add_argument("--dump-jsonl", type=str, default=None,
                   help="Save all received reports to JSONL file")

    # uinput
    p.add_argument("--uinput", action="store_true", default=False,
                   help="Create Linux uinput gamepad device")

    # Logging
    p.add_argument("--verbose", action="store_true", default=False)
    p.add_argument("--json", action="store_true", default=False,
                   help="JSON-line structured log output")
    p.add_argument("--quiet", action="store_true", default=False,
                   help="Minimal output (errors only)")

    return p.parse_args()


# ── Main ───────────────────────────────────────────────────────

async def async_main() -> int:
    args = parse_args()

    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)
    elif args.quiet:
        logging.basicConfig(level=logging.WARNING)

    log = StageLogger(json_log=args.json, verbose=args.verbose)
    state = DaemonState(args=args, log=log)
    state.start_time = time.monotonic()

    # Resolve profile
    if args.daemon and args.profile == "auto":
        args.profile = "macos"  # daemon default
        state.log.info("--daemon with profile=auto → defaulting to macos")

    loop = asyncio.get_running_loop()
    setup_signal_handlers(state, loop)

    if not await stage_preflight(state):
        return 10

    if args.uinput and not setup_uinput(state):
        if args.diagnose:
            return 10

    if args.dump_jsonl:
        jsonl_path = Path(args.dump_jsonl)
        state.jsonl_file = jsonl_path.open("w")
        log.info(f"Writing reports to {jsonl_path}")

    try:
        if args.diagnose:
            return await diagnose_mode(state)
        elif args.daemon:
            return await reconnect_loop(state)
        else:
            profile = PROTOCOL_PROFILES.get(args.profile, PROFILE_MACOS)
            session_ok = await run_session(state, profile)
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
        if state.uinput:
            try:
                state.uinput.close()
            except Exception:
                pass
        if state.jsonl_file:
            try:
                state.jsonl_file.close()
            except Exception:
                pass
        await cleanup_client(state)


def main() -> None:
    sys.exit(asyncio.run(async_main()))


if __name__ == "__main__":
    main()
