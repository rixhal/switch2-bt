#!/usr/bin/env python3
"""
ns2-ble-uinput.py — Switch 2 Pro Controller BLE → uinput Bridge (Pure Python)
No compiled dependencies. Uses only stdlib + bleak (pure Python wheel).
Tested on LibreELEC 12 (RPi5, kernel 6.12.56, no pip/evdev/dbus).
"""
import asyncio, fcntl, os, struct, sys, time
from bleak import BleakScanner, BleakClient
from bleak.backends.device import BLEDevice

# ── uinput pure-Python (no evdev dependency) ──
UI_DEV_CREATE = 0x5501
UI_DEV_DESTROY = 0x5502
UI_SET_EVBIT = 0x40045564
UI_SET_KEYBIT = 0x40045565
UI_SET_ABSBIT = 0x40045567
EV_KEY = 0x01
EV_ABS = 0x03
EV_SYN = 0x00
SYN_REPORT = 0

class UInputDevice:
    def __init__(self, name=b"NS2 ProCon BLE", vendor=0x057E, product=0x2069):
        self.fd = os.open("/dev/uinput", os.O_WRONLY)
        uidev = struct.pack("80sHHHH", name, vendor, product, 1, 0, 0)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_ABS)
        for code in BTN_CODES:
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, code)
        for code in ABS_CODES:
            fcntl.ioctl(self.fd, UI_SET_ABSBIT, code)
        fcntl.ioctl(self.fd, UI_DEV_CREATE)
    def emit(self, etype, code, value):
        os.write(self.fd, struct.pack("LLHHi", 0, 0, etype, code, value))
    def syn(self):
        self.emit(EV_SYN, SYN_REPORT, 0)
    def close(self):
        fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        os.close(self.fd)

# evdev button codes (from linux/input-event-codes.h)
BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST = 0x130, 0x131, 0x133, 0x134
BTN_TL, BTN_TR, BTN_TL2, BTN_TR2 = 0x136, 0x137, 0x138, 0x139
BTN_SELECT, BTN_START, BTN_MODE = 0x13a, 0x13b, 0x13c
BTN_THUMBL, BTN_THUMBR = 0x13d, 0x13e
BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT = 0x220, 0x221, 0x222, 0x223
BTN_TRIGGER_HAPPY1, BTN_TRIGGER_HAPPY2 = 0x2c0, 0x2c1
BTN_TRIGGER_HAPPY3, BTN_TRIGGER_HAPPY4 = 0x2c2, 0x2c3
ABS_X, ABS_Y, ABS_RX, ABS_RY = 0x00, 0x01, 0x03, 0x04
ABS_Z, ABS_RZ = 0x02, 0x05

BTN_CODES = [
    BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR, BTN_TL2, BTN_TR2,
    BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR,
    BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT,
    BTN_TRIGGER_HAPPY1, BTN_TRIGGER_HAPPY2, BTN_TRIGGER_HAPPY3, BTN_TRIGGER_HAPPY4,
]
ABS_CODES = [ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ]

# ── Button map (SPro2Win/macOS verified) ──
BTN_MAP = {
    # Byte 2 — right cluster
    "b": BTN_EAST, "a": BTN_SOUTH, "y": BTN_WEST, "x": BTN_NORTH,
    "r": BTN_TR, "zr": BTN_TR2, "start": BTN_START, "r3": BTN_THUMBR,
    # Byte 3 — left cluster
    "dpad_down": BTN_DPAD_DOWN, "dpad_right": BTN_DPAD_RIGHT,
    "dpad_left": BTN_DPAD_LEFT, "dpad_up": BTN_DPAD_UP,
    "l": BTN_TL, "zl": BTN_TL2, "back": BTN_SELECT, "l3": BTN_THUMBL,
    # Byte 4 — system + grip
    "home": BTN_MODE, "c": BTN_TRIGGER_HAPPY1,
    "gr": BTN_TRIGGER_HAPPY2, "gl": BTN_TRIGGER_HAPPY3,
    "screenshot": BTN_TRIGGER_HAPPY4,
}

BUTTON_BITS = {
    "b": (2, 0x01), "a": (2, 0x02), "y": (2, 0x04), "x": (2, 0x08),
    "r": (2, 0x10), "zr": (2, 0x20), "start": (2, 0x40), "r3": (2, 0x80),
    "dpad_down": (3, 0x01), "dpad_right": (3, 0x02), "dpad_left": (3, 0x04), "dpad_up": (3, 0x08),
    "l": (3, 0x10), "zl": (3, 0x20), "back": (3, 0x40), "l3": (3, 0x80),
    "home": (4, 0x01), "c": (4, 0x02), "gr": (4, 0x04), "gl": (4, 0x08), "screenshot": (4, 0x10),
}

# Profile GATT UUIDs
PROFILES = {
    "macos":   {"input": ["7492866c-ec3e-4619-8258-32755ffcc0f9"], "subscribe": "selected"},
    "spro2win":{"input": [], "subscribe": "all", "handle": 45},
    "joycon2cpp": {"input": ["ab7de9be-89fe-49ad-828f-118f09df7fd2"], "subscribe": "selected",
                   "init_cmd_uuid": "649d4ac9-8eb7-4e6c-af44-1ea54fe5f005",
                   "init": [
                       ("fs02", bytes.fromhex("0c91010200040000ff00000000"), 0.5),
                       ("fs04", bytes.fromhex("0c91010400040000ff00000000"), 0.7),
                       ("led",  bytes.fromhex("0991010700080000010000000000000000"), 0.05),
                       ("sound",bytes.fromhex("0a91010200080000040000000000000000"), 0.05),
                   ]},
}

NINTENDO_MFR_ID = 0x0553  # 1363
CONTROLLER_PID = 0x2069
CONTROLLER_PID_BLE = 0x0569  # BLE advertising PID (Nintendo internal format)

class NS2Bridge:
    def __init__(self, profile="auto", address=None, verbose=False):
        self.profile_name = profile
        self.target_address = address
        self.verbose = verbose
        self.ui = None
        self.client = None
        self.input_handle = None
        self.prev_buttons = set()

    def log(self, msg):
        if self.verbose:
            print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)

    async def scan(self, timeout=10):
        self.log(f"Scanning {timeout}s for Switch 2 Pro Controller...")
        found = None
        def cb(device, adv):
            nonlocal found
            if self.target_address and device.address.lower() == self.target_address.lower():
                found = device
                return
            if NINTENDO_MFR_ID in adv.manufacturer_data:
                mfr = adv.manufacturer_data[NINTENDO_MFR_ID]
                if len(mfr) >= 6:
                    pid = int.from_bytes(mfr[4:6], 'little')
                    if pid in (CONTROLLER_PID_BLE, CONTROLLER_PID) and not found:
                        found = device
        if self.target_address:
            self.log(f"Looking for {self.target_address}")
        scanner = BleakScanner(detection_callback=cb)
        await scanner.start()
        await asyncio.sleep(timeout)
        await scanner.stop()
        return found

    async def connect_run(self, device):
        if not device:
            self.log("No device found")
            return False
        addr = device.address
        self.log(f"Connecting to {addr} ({device.name or 'unknown'})...")
        try:
            self.client = BleakClient(device, timeout=15.0)
            await self.client.connect()
            self.log("Connected")
        except Exception as e:
            self.log(f"Connect failed: {e}")
            return False

        # GATT discover
        profile = PROFILES.get(self.profile_name, PROFILES["joycon2cpp"])
        if self.profile_name == "auto":
            for p in ["macos", "spro2win", "joycon2cpp"]:
                profile = PROFILES[p]
                self.profile_name = p
                break

        # Discover characteristics
        svc = await self.client.get_services()
        found_input = None
        found_cmd = None
        for service in svc:
            for char in service.characteristics:
                if "notify" in char.properties:
                    if profile["subscribe"] == "all":
                        found_input = char
                    elif any(uuid.lower() in str(char.uuid).lower() for uuid in profile["input"]):
                        found_input = char
                if "write" in char.properties:
                    if "init_cmd_uuid" in profile and profile["init_cmd_uuid"].lower() in str(char.uuid).lower():
                        found_cmd = char
        
        if not found_input and profile["subscribe"] == "all":
            for service in svc:
                for char in service.characteristics:
                    if "notify" in char.properties and char.handle == profile.get("handle"):
                        found_input = char
                        break

        if not found_input:
            self.log("Input characteristic not found")
            return False

        self.input_handle = found_input.handle
        self.log(f"Input UUID: {found_input.uuid}, handle={found_input.handle}")

        # Init sequence (joycon2cpp)
        if "init" in profile and found_cmd:
            self.log("Running init sequence...")
            for label, data, delay in profile["init"]:
                try:
                    await self.client.write_gatt_char(found_cmd, data, response=False)
                    self.log(f"  {label}")
                    await asyncio.sleep(delay)
                except Exception as e:
                    self.log(f"  {label} FAILED: {e}")

        # Subscribe
        await self.client.start_notify(found_input, self._on_notify)
        self.log("Subscribed — waiting for input...")

        # Create uinput
        self.ui = UInputDevice(name=b"NS2 ProCon BLE")
        self.log("uinput device created")

        # Keep running until disconnected
        try:
            while self.client.is_connected:
                await asyncio.sleep(0.5)
        except asyncio.CancelledError:
            pass
        self.log("Disconnected")
        return True

    def _on_notify(self, sender, data):
        if len(data) < 11:
            return
        # Decode buttons
        pressed = set()
        for btn_name, (byte_idx, mask) in BUTTON_BITS.items():
            if data[byte_idx] & mask:
                pressed.add(btn_name)
        
        # Emit button changes
        for btn_name, code in BTN_MAP.items():
            is_pressed = btn_name in pressed
            was_pressed = btn_name in self.prev_buttons
            if is_pressed != was_pressed:
                self.ui.emit(EV_KEY, code, 1 if is_pressed else 0)
        
        # Decode sticks (bytes 5-10, 12-bit packed)
        lx = data[5] | ((data[6] & 0x0F) << 8)
        ly = (data[6] >> 4) | (data[7] << 4)
        rx = data[8] | ((data[9] & 0x0F) << 8)
        ry = (data[9] >> 4) | (data[10] << 4)
        
        # Center 2048, range 0-4095 → signed 16-bit
        def stick_val(raw):
            v = int((raw - 2048) / 2047 * 32767)
            return max(-32768, min(32767, v))
        
        self.ui.emit(EV_ABS, ABS_X, stick_val(lx))
        self.ui.emit(EV_ABS, ABS_Y, -stick_val(ly))
        self.ui.emit(EV_ABS, ABS_RX, stick_val(rx))
        self.ui.emit(EV_ABS, ABS_RY, -stick_val(ry))
        
        # Triggers (if present, bytes 13-14)
        if len(data) >= 15:
            lt = data[13] if data[13] else 0
            rt = data[14] if data[14] else 0
            self.ui.emit(EV_ABS, ABS_Z, lt)
            self.ui.emit(EV_ABS, ABS_RZ, rt)
        
        self.ui.syn()
        self.prev_buttons = pressed

    async def run(self):
        device = await self.scan(timeout=15)
        if not device:
            print("No Switch 2 Pro Controller found. Press SYNC on controller.")
            return 1
        ok = await self.connect_run(device)
        if not ok:
            return 1
        return 0

    def shutdown(self):
        if self.ui:
            try:
                self.ui.close()
            except:
                pass
        if self.client and self.client.is_connected:
            try:
                asyncio.ensure_future(self.client.disconnect())
            except:
                pass

async def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", default="joycon2cpp", choices=["auto","macos","spro2win","joycon2cpp"])
    parser.add_argument("--address", default=None)
    parser.add_argument("--daemon", action="store_true")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    bridge = NS2Bridge(profile=args.profile, address=args.address, verbose=args.verbose)
    
    while True:
        ret = await bridge.run()
        if not args.daemon:
            break
        bridge.shutdown()
        delay = min(1.0 * (2 ** bridge._attempts) if hasattr(bridge,'_attempts') else 1.0, 30.0)
        print(f"Reconnecting in {delay}s...")
        await asyncio.sleep(delay)
        bridge.prev_buttons = set()
    return ret

if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except KeyboardInterrupt:
        print("\nShutdown")
