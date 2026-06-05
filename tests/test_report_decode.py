"""Tests for decode_report with byte-level button parsing.

Report format (new byte-level model):
  bytes 0-1:   packet_id (16-bit LE)
  byte 2:      right cluster buttons (8 bits)
  byte 3:      left cluster buttons (8 bits)
  byte 4:      system + grip buttons (8 bits)
  bytes 5-7:   left stick (12-bit packed X,Y)
  bytes 8-10:  right stick (12-bit packed X,Y)
  bytes 11+:   optional; accel/gyro only in >= 0x3C byte reports
"""

import pytest
from switch2d import decode_report, BUTTON_MAP


# ── Helpers ─────────────────────────────────────────────────────

def _pack_stick(x: int, y: int) -> bytes:
    """Pack 12-bit stick values into 3 bytes (same format as decode_stick12)."""
    return bytes([
        x & 0xFF,
        ((y & 0x0F) << 4) | ((x >> 8) & 0x0F),
        (y >> 4) & 0xFF,
    ])


def build_report(byte2: int = 0, byte3: int = 0, byte4: int = 0,
                 left_stick: tuple = (2048, 2048),
                 right_stick: tuple = (2048, 2048),
                 packet_id: int = 0,
                 include_imu: bool = False,
                 accel: tuple = (0, 0, 0),
                 gyro: tuple = (0, 0, 0)) -> bytes:
    """Build a Switch 2 Pro Controller input report.

    Minimum 11 bytes. When include_imu=True, builds a full 0x3C-byte report
    with accelerometer and gyroscope data at the standard offsets.
    """
    length = 0x3C if include_imu else 11
    buf = bytearray(length)

    # Packet ID (2 bytes LE)
    buf[0:2] = packet_id.to_bytes(2, "little")

    # Buttons (bytes 2, 3, 4)
    buf[2] = byte2
    buf[3] = byte3
    buf[4] = byte4

    # Left stick (bytes 5-7)
    buf[5:8] = _pack_stick(*left_stick)

    # Right stick (bytes 8-10)
    buf[8:11] = _pack_stick(*right_stick)

    # Accel / Gyro (only in full reports)
    if include_imu:
        for i, val in enumerate(accel):
            off = 0x30 + i * 2
            buf[off:off + 2] = val.to_bytes(2, "little", signed=True)
        for i, val in enumerate(gyro):
            off = 0x36 + i * 2
            buf[off:off + 2] = val.to_bytes(2, "little", signed=True)

    return bytes(buf)


def _buttons_for(*names: str) -> tuple:
    """Return (byte2, byte3, byte4) from button names using BUTTON_MAP."""
    b2, b3, b4 = 0, 0, 0
    for name in names:
        byte_idx, mask = BUTTON_MAP[name]
        if byte_idx == 2:
            b2 |= mask
        elif byte_idx == 3:
            b3 |= mask
        elif byte_idx == 4:
            b4 |= mask
    return b2, b3, b4


# ── Tests ───────────────────────────────────────────────────────

class TestDecodeReport:
    """Full report decoder tests with byte-level button parsing."""

    # ── Short / invalid reports ─────────────────────────────

    def test_empty_data_returns_none(self):
        """Empty bytes returns None."""
        assert decode_report(b"") is None

    def test_too_short_10_bytes(self):
        """10 bytes (below 11-byte minimum) returns None."""
        assert decode_report(bytes(10)) is None

    def test_too_short_5_bytes(self):
        """5 bytes returns None."""
        assert decode_report(bytes(5)) is None

    def test_too_short_1_byte(self):
        """1 byte returns None."""
        assert decode_report(bytes(1)) is None

    def test_exactly_11_bytes_valid(self):
        """Exactly 11 bytes decodes without error."""
        result = decode_report(build_report())
        assert result is not None
        assert result["packet_id"] == 0
        assert result["pressed"] == []
        assert result["left_stick"] is not None
        assert result["right_stick"] is not None

    def test_12_bytes_valid(self):
        """Report between 11 and 0x3C bytes still decodes."""
        buf = bytearray(12)
        buf[5:8] = _pack_stick(2048, 2048)
        buf[8:11] = _pack_stick(2048, 2048)
        result = decode_report(bytes(buf))
        assert result is not None
        assert result["pressed"] == []

    # ── Packet ID ───────────────────────────────────────────

    def test_packet_id_zero(self):
        result = decode_report(build_report(packet_id=0))
        assert result["packet_id"] == 0

    def test_packet_id_nonzero(self):
        result = decode_report(build_report(packet_id=0x1234))
        assert result["packet_id"] == 0x1234

    def test_packet_id_max_16bit(self):
        result = decode_report(build_report(packet_id=0xFFFF))
        assert result["packet_id"] == 0xFFFF

    # ── No buttons pressed ──────────────────────────────────

    def test_no_buttons_pressed(self):
        """All-zero button bytes → empty pressed list."""
        result = decode_report(build_report())
        assert result is not None
        assert result["pressed"] == []

    # ── Byte 2 buttons (right cluster) ──────────────────────
    #   b=0x01, a=0x02, y=0x04, x=0x08,
    #   r=0x10, zr=0x20, start=0x40, r3=0x80

    @pytest.mark.parametrize("name,mask", [
        ("b", 0x01),
        ("a", 0x02),
        ("y", 0x04),
        ("x", 0x08),
        ("r", 0x10),
        ("zr", 0x20),
        ("start", 0x40),
        ("r3", 0x80),
    ])
    def test_byte2_button_individual(self, name, mask):
        """Each byte-2 button is detected when its bit is set."""
        b2, b3, b4 = _buttons_for(name)
        result = decode_report(build_report(byte2=b2, byte3=b3, byte4=b4))
        assert result["pressed"] == [name]

    # ── Byte 3 buttons (left cluster) ───────────────────────
    #   dpad_down=0x01, dpad_right=0x02, dpad_left=0x04, dpad_up=0x08,
    #   l=0x10, zl=0x20, back=0x40, l3=0x80

    @pytest.mark.parametrize("name,mask", [
        ("dpad_down", 0x01),
        ("dpad_right", 0x02),
        ("dpad_left", 0x04),
        ("dpad_up", 0x08),
        ("l", 0x10),
        ("zl", 0x20),
        ("back", 0x40),
        ("l3", 0x80),
    ])
    def test_byte3_button_individual(self, name, mask):
        """Each byte-3 button is detected when its bit is set."""
        b2, b3, b4 = _buttons_for(name)
        result = decode_report(build_report(byte2=b2, byte3=b3, byte4=b4))
        assert result["pressed"] == [name]

    # ── Byte 4 buttons (system + grip) ──────────────────────
    #   home=0x01, c=0x02, gr=0x04, gl=0x08, screenshot=0x10

    @pytest.mark.parametrize("name,mask", [
        ("home", 0x01),
        ("c", 0x02),
        ("gr", 0x04),
        ("gl", 0x08),
        ("screenshot", 0x10),
    ])
    def test_byte4_button_individual(self, name, mask):
        """Each byte-4 button is detected when its bit is set."""
        b2, b3, b4 = _buttons_for(name)
        result = decode_report(build_report(byte2=b2, byte3=b3, byte4=b4))
        assert result["pressed"] == [name]

    # ── Multiple buttons across bytes ───────────────────────

    def test_multiple_buttons_byte2(self):
        """Multiple byte-2 buttons simultaneously (a + b + x + y)."""
        b2, b3, b4 = _buttons_for("a", "b", "x", "y")
        result = decode_report(build_report(byte2=b2, byte3=b3, byte4=b4))
        assert set(result["pressed"]) == {"a", "b", "x", "y"}

    def test_multiple_buttons_byte3(self):
        """Multiple byte-3 buttons simultaneously (all dpad directions)."""
        b2, b3, b4 = _buttons_for("dpad_up", "dpad_down", "dpad_left", "dpad_right")
        result = decode_report(build_report(byte2=b2, byte3=b3, byte4=b4))
        assert set(result["pressed"]) == {"dpad_up", "dpad_down", "dpad_left", "dpad_right"}

    def test_shoulder_buttons(self):
        """L, R, ZL, ZR across bytes 2 and 3."""
        b2, b3, b4 = _buttons_for("l", "r", "zl", "zr")
        result = decode_report(build_report(byte2=b2, byte3=b3, byte4=b4))
        assert set(result["pressed"]) == {"l", "r", "zl", "zr"}

    def test_buttons_across_all_bytes(self):
        """Buttons across bytes 2, 3, and 4 simultaneously."""
        names = ["a", "dpad_up", "home"]
        b2, b3, b4 = _buttons_for(*names)
        result = decode_report(build_report(byte2=b2, byte3=b3, byte4=b4))
        assert set(result["pressed"]) == set(names)

    def test_all_buttons(self):
        """All known buttons pressed at once."""
        all_names = list(BUTTON_MAP.keys())
        b2, b3, b4 = _buttons_for(*all_names)
        result = decode_report(build_report(byte2=b2, byte3=b3, byte4=b4))
        assert set(result["pressed"]) == set(all_names)

    # ── Button ordering ─────────────────────────────────────

    def test_button_order_matches_map_order(self):
        """Pressed buttons are returned in BUTTON_MAP insertion order."""
        # Press b (first in map) and screenshot (last in map)
        b2, b3, b4 = _buttons_for("b", "screenshot")
        result = decode_report(build_report(byte2=b2, byte3=b3, byte4=b4))
        assert result["pressed"] == ["b", "screenshot"]

    # ── Sticks ──────────────────────────────────────────────

    def test_sticks_center(self):
        """Both sticks at center (2048, 2048)."""
        result = decode_report(build_report())
        assert result["left_stick"]["x"] == 2048
        assert result["left_stick"]["y"] == 2048
        assert result["left_stick"]["x_norm"] == 0.0
        assert result["left_stick"]["y_norm"] == 0.0
        assert result["right_stick"]["x"] == 2048
        assert result["right_stick"]["y"] == 2048

    def test_left_stick_full_left(self):
        """Left stick x=0, y=center."""
        result = decode_report(build_report(left_stick=(0, 2048)))
        assert result["left_stick"]["x"] == 0
        assert result["left_stick"]["y"] == 2048
        assert result["left_stick"]["x_norm"] == pytest.approx(-2048 / 2047, abs=1e-3)
        assert result["left_stick"]["y_norm"] == 0.0

    def test_left_stick_full_right(self):
        """Left stick x=4095, y=center."""
        result = decode_report(build_report(left_stick=(4095, 2048)))
        assert result["left_stick"]["x"] == 4095
        assert result["left_stick"]["x_norm"] == pytest.approx(2047 / 2047, abs=1e-3)

    def test_right_stick_full_up(self):
        """Right stick x=center, y=4095."""
        result = decode_report(build_report(right_stick=(2048, 4095)))
        assert result["right_stick"]["x"] == 2048
        assert result["right_stick"]["y"] == 4095
        assert result["right_stick"]["y_norm"] == pytest.approx(2047 / 2047, abs=1e-3)

    def test_right_stick_full_down(self):
        """Right stick x=center, y=0."""
        result = decode_report(build_report(right_stick=(2048, 0)))
        assert result["right_stick"]["y"] == 0
        assert result["right_stick"]["y_norm"] == pytest.approx(-2048 / 2047, abs=1e-3)

    # ── 11-byte vs 0x3C-byte (IMU) ──────────────────────────

    def test_11_byte_report_no_imu(self):
        """11-byte report has no accel or gyro keys."""
        result = decode_report(build_report())
        assert result is not None
        assert "accel" not in result
        assert "gyro" not in result

    def test_0x3C_report_has_imu(self):
        """0x3C-byte report includes accel and gyro keys."""
        result = decode_report(build_report(include_imu=True))
        assert result is not None
        assert "accel" in result
        assert "gyro" in result

    def test_0x3C_report_accel_zero(self):
        """Default accel is (0, 0, 0)."""
        result = decode_report(build_report(include_imu=True))
        assert result["accel"] == {"x": 0, "y": 0, "z": 0}

    def test_0x3C_report_accel_values(self):
        """Known accelerometer values round-trip."""
        result = decode_report(build_report(
            include_imu=True, accel=(100, -200, 300)))
        assert result["accel"] == {"x": 100, "y": -200, "z": 300}

    def test_0x3C_report_gyro_zero(self):
        """Default gyro is (0, 0, 0)."""
        result = decode_report(build_report(include_imu=True))
        assert result["gyro"] == {"x": 0, "y": 0, "z": 0}

    def test_0x3C_report_gyro_values(self):
        """Known gyroscope values round-trip."""
        result = decode_report(build_report(
            include_imu=True, gyro=(-32768, 0, 32767)))
        assert result["gyro"] == {"x": -32768, "y": 0, "z": 32767}

    def test_0x3C_report_accel_gyro_combined(self):
        """Both accel and gyro with non-zero values."""
        result = decode_report(build_report(
            include_imu=True,
            accel=(1234, -5678, 9012),
            gyro=(-1234, 5678, -9012),
        ))
        assert result["accel"]["x"] == 1234
        assert result["accel"]["y"] == -5678
        assert result["accel"]["z"] == 9012
        assert result["gyro"]["x"] == -1234
        assert result["gyro"]["y"] == 5678
        assert result["gyro"]["z"] == -9012

    def test_30_byte_report_no_imu(self):
        """Report >= 11 but < 0x3C has no accel/gyro."""
        buf = bytearray(30)
        buf[5:8] = _pack_stick(2048, 2048)
        buf[8:11] = _pack_stick(2048, 2048)
        result = decode_report(bytes(buf))
        assert result is not None
        assert "accel" not in result
        assert "gyro" not in result

    # ── Larger than 0x3C ────────────────────────────────────

    def test_larger_than_0x3C(self):
        """Report > 0x3C bytes still decodes correctly."""
        result = decode_report(build_report(include_imu=True))
        assert result is not None
        assert "accel" in result
        assert "gyro" in result

    # ── No button_state field ───────────────────────────────

    def test_no_button_state_field(self):
        """New decoder does not include 'button_state' hex field."""
        result = decode_report(build_report())
        assert "button_state" not in result

    # ── Sticks at correct new byte offsets ──────────────────

    def test_left_stick_at_bytes_5_to_7(self):
        """Left stick is at bytes 5-7, not the old 10-12 offset."""
        result = decode_report(build_report(left_stick=(0, 4095)))
        assert result["left_stick"]["x"] == 0
        assert result["left_stick"]["y"] == 4095

    def test_right_stick_at_bytes_8_to_10(self):
        """Right stick is at bytes 8-10, not the old 13-15 offset."""
        result = decode_report(build_report(right_stick=(4095, 0)))
        assert result["right_stick"]["x"] == 4095
        assert result["right_stick"]["y"] == 0
