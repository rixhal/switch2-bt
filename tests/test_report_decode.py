"""Tests for full report decoding: decode_report.

Report format (≥0x3C bytes):
  bytes 0-2:   packet ID (24-bit LE)
  bytes 3-8:   buttons (48-bit BE bitfield)
  bytes 10-12: left stick (12-bit packed X,Y)
  bytes 13-15: right stick (12-bit packed X,Y)
  bytes 0x30-0x35: accelerometer (3x s16 LE)
  bytes 0x36-0x3B: gyroscope (3x s16 LE)
"""

import pytest
from switch2d import decode_report, PROCON2_BUTTON_MASKS


# Helpers ---------------------------------------------------------

def make_report(button_bytes=(0, 0, 0, 0, 0, 0),
                left_stick=(2048, 2048),
                right_stick=(2048, 2048),
                packet_id=0,
                accel=(0, 0, 0),
                gyro=(0, 0, 0)) -> bytes:
    """Build a 60-byte (0x3C) report with known field values.

    button_bytes: tuple of 6 bytes for bytes 3-8 (48-bit BE button bitfield).
    left_stick: (x, y) 12-bit values packed into bytes 10-12.
    right_stick: (x, y) 12-bit values packed into bytes 13-15.
    """
    buf = bytearray(60)  # 0x3C zero-filled

    # Packet ID (24-bit LE, bytes 0-2)
    buf[0:3] = packet_id.to_bytes(3, "little")

    # Buttons (48-bit BE, bytes 3-8)
    buf[3:9] = button_bytes

    # Left stick (bytes 10-12)
    lx, ly = left_stick
    buf[10] = lx & 0xFF
    buf[11] = ((ly & 0x0F) << 4) | ((lx >> 8) & 0x0F)
    buf[12] = (ly >> 4) & 0xFF

    # Right stick (bytes 13-15)
    rx, ry = right_stick
    buf[13] = rx & 0xFF
    buf[14] = ((ry & 0x0F) << 4) | ((rx >> 8) & 0x0F)
    buf[15] = (ry >> 4) & 0xFF

    # Accel (3x s16 LE, bytes 0x30-0x35)
    for i, val in enumerate(accel):
        off = 0x30 + i * 2
        buf[off:off + 2] = val.to_bytes(2, "little", signed=True)

    # Gyro (3x s16 LE, bytes 0x36-0x3B)
    for i, val in enumerate(gyro):
        off = 0x36 + i * 2
        buf[off:off + 2] = val.to_bytes(2, "little", signed=True)

    return bytes(buf)


def button_bytes_from_mask(mask: int) -> tuple:
    """Convert a 48-bit mask to 6 big-endian bytes (bytes 3-8)."""
    return tuple((mask >> (40 - i * 8)) & 0xFF for i in range(6))


# Tests -----------------------------------------------------------

class TestDecodeReport:
    """Full report decoder tests."""

    # --- Short / empty reports ---

    def test_empty_report(self):
        """Empty bytes returns None."""
        assert decode_report(b"") is None

    def test_short_report(self):
        """Report shorter than 0x3C (60) bytes returns None."""
        assert decode_report(bytes(59)) is None
        assert decode_report(bytes(10)) is None
        assert decode_report(bytes(1)) is None

    def test_exactly_minimum_length(self):
        """Exactly 0x3C bytes decodes without error."""
        result = decode_report(bytes(60))
        assert result is not None
        assert result["packet_id"] == 0
        assert result["button_state"] == "0x000000000000"
        assert result["pressed"] == []
        assert result["left_stick"] is not None
        assert result["right_stick"] is not None

    # --- Buttons ---

    def test_no_buttons(self):
        """All-zero report has no pressed buttons."""
        result = decode_report(make_report())
        assert result is not None
        assert result["pressed"] == []
        assert result["button_state"] == "0x000000000000"

    def test_button_a(self):
        """Button 'a' pressed (mask 0x000800000000)."""
        mask = PROCON2_BUTTON_MASKS["a"]
        bb = button_bytes_from_mask(mask)
        result = decode_report(make_report(button_bytes=bb))
        assert result is not None
        assert "a" in result["pressed"]
        assert len(result["pressed"]) == 1

    def test_button_b(self):
        """Button 'b' pressed (mask 0x000400000000)."""
        mask = PROCON2_BUTTON_MASKS["b"]
        result = decode_report(make_report(button_bytes=button_bytes_from_mask(mask)))
        assert result is not None
        assert result["pressed"] == ["b"]

    def test_button_home(self):
        """Button 'home' pressed (mask 0x000010000000)."""
        mask = PROCON2_BUTTON_MASKS["home"]
        result = decode_report(make_report(button_bytes=button_bytes_from_mask(mask)))
        assert result is not None
        assert result["pressed"] == ["home"]

    def test_multiple_buttons(self):
        """Multiple buttons pressed simultaneously (a + b + x)."""
        mask = (PROCON2_BUTTON_MASKS["a"] |
                PROCON2_BUTTON_MASKS["b"] |
                PROCON2_BUTTON_MASKS["x"])
        result = decode_report(make_report(button_bytes=button_bytes_from_mask(mask)))
        assert result is not None
        assert set(result["pressed"]) == {"a", "b", "x"}

    def test_all_dpad_buttons(self):
        """All d-pad directions pressed."""
        mask = (PROCON2_BUTTON_MASKS["dpad_up"] |
                PROCON2_BUTTON_MASKS["dpad_down"] |
                PROCON2_BUTTON_MASKS["dpad_left"] |
                PROCON2_BUTTON_MASKS["dpad_right"])
        result = decode_report(make_report(button_bytes=button_bytes_from_mask(mask)))
        assert result is not None
        assert set(result["pressed"]) == {"dpad_up", "dpad_down", "dpad_left", "dpad_right"}

    def test_shoulder_buttons(self):
        """L, R, ZL, ZR pressed."""
        mask = (PROCON2_BUTTON_MASKS["l"] |
                PROCON2_BUTTON_MASKS["r"] |
                PROCON2_BUTTON_MASKS["zl"] |
                PROCON2_BUTTON_MASKS["zr"])
        result = decode_report(make_report(button_bytes=button_bytes_from_mask(mask)))
        assert result is not None
        assert set(result["pressed"]) == {"l", "r", "zl", "zr"}

    def test_all_buttons(self):
        """All known button masks OR'd together."""
        mask = 0
        for m in PROCON2_BUTTON_MASKS.values():
            mask |= m
        result = decode_report(make_report(button_bytes=button_bytes_from_mask(mask)))
        assert result is not None
        assert set(result["pressed"]) == set(PROCON2_BUTTON_MASKS.keys())

    def test_button_state_hex_format(self):
        """button_state is a 12-hex-digit zero-prefixed string."""
        mask = PROCON2_BUTTON_MASKS["a"]
        result = decode_report(make_report(button_bytes=button_bytes_from_mask(mask)))
        assert result is not None
        assert result["button_state"] == "0x000800000000"
        assert len(result["button_state"]) == 14  # "0x" + 12 hex digits

    # --- Sticks ---

    def test_sticks_center(self):
        """Both sticks at center (2048, 2048)."""
        result = decode_report(make_report())
        assert result is not None
        assert result["left_stick"]["x"] == 2048
        assert result["left_stick"]["y"] == 2048
        assert result["left_stick"]["x_norm"] == 0.0
        assert result["left_stick"]["y_norm"] == 0.0
        assert result["right_stick"]["x"] == 2048
        assert result["right_stick"]["y"] == 2048

    def test_left_stick_full_left(self):
        """Left stick at x=0, y=center."""
        result = decode_report(make_report(left_stick=(0, 2048)))
        assert result is not None
        assert result["left_stick"]["x"] == 0
        assert result["left_stick"]["y"] == 2048
        assert result["left_stick"]["x_norm"] == pytest.approx(-2048 / 2047, abs=1e-3)
        assert result["left_stick"]["y_norm"] == 0.0

    def test_left_stick_full_right(self):
        """Left stick at x=4095, y=center."""
        result = decode_report(make_report(left_stick=(4095, 2048)))
        assert result is not None
        assert result["left_stick"]["x"] == 4095
        assert result["left_stick"]["x_norm"] == pytest.approx(2047 / 2047, abs=1e-3)

    def test_right_stick_full_up(self):
        """Right stick at x=center, y=4095."""
        result = decode_report(make_report(right_stick=(2048, 4095)))
        assert result is not None
        assert result["right_stick"]["x"] == 2048
        assert result["right_stick"]["y"] == 4095
        assert result["right_stick"]["y_norm"] == pytest.approx(2047 / 2047, abs=1e-3)

    def test_right_stick_full_down(self):
        """Right stick at x=center, y=0."""
        result = decode_report(make_report(right_stick=(2048, 0)))
        assert result is not None
        assert result["right_stick"]["y"] == 0
        assert result["right_stick"]["y_norm"] == pytest.approx(-2048 / 2047, abs=1e-3)

    # --- Packet ID ---

    def test_packet_id_zero(self):
        """Default zero packet ID."""
        result = decode_report(make_report(packet_id=0))
        assert result is not None
        assert result["packet_id"] == 0

    def test_packet_id_nonzero(self):
        """Non-zero packet ID (24-bit LE)."""
        result = decode_report(make_report(packet_id=0x123456))
        assert result is not None
        assert result["packet_id"] == 0x123456

    def test_packet_id_max(self):
        """Max 24-bit packet ID."""
        result = decode_report(make_report(packet_id=0xFFFFFF))
        assert result is not None
        assert result["packet_id"] == 0xFFFFFF

    # --- Accel / Gyro ---

    def test_accel_zero(self):
        """Accelerometer defaults to (0, 0, 0)."""
        result = decode_report(make_report())
        assert result is not None
        assert result["accel"] == {"x": 0, "y": 0, "z": 0}

    def test_accel_values(self):
        """Known accelerometer values."""
        result = decode_report(make_report(accel=(100, -200, 300)))
        assert result is not None
        assert result["accel"] == {"x": 100, "y": -200, "z": 300}

    def test_gyro_zero(self):
        """Gyroscope defaults to (0, 0, 0)."""
        result = decode_report(make_report())
        assert result is not None
        assert result["gyro"] == {"x": 0, "y": 0, "z": 0}

    def test_gyro_values(self):
        """Known gyroscope values."""
        result = decode_report(make_report(gyro=(-32768, 0, 32767)))
        assert result is not None
        assert result["gyro"] == {"x": -32768, "y": 0, "z": 32767}

    def test_accel_gyro_combined(self):
        """Both accel and gyro with non-zero values."""
        result = decode_report(make_report(
            accel=(1234, -5678, 9012),
            gyro=(-1234, 5678, -9012),
        ))
        assert result is not None
        assert result["accel"]["x"] == 1234
        assert result["accel"]["y"] == -5678
        assert result["accel"]["z"] == 9012
        assert result["gyro"]["x"] == -1234
        assert result["gyro"]["y"] == 5678
        assert result["gyro"]["z"] == -9012

    # --- Larger than minimum report ---

    def test_larger_report(self):
        """Report larger than 0x3C bytes still decodes correctly."""
        result = decode_report(bytes(100))
        assert result is not None
        assert result["packet_id"] == 0

    # --- Button bitfield byte-level verification ---

    def test_button_bitfield_byte_ordering(self):
        """Verify that the 6 button bytes are interpreted as 48-bit BE."""
        # byte 3 is MSB, byte 8 is LSB
        # Set only bit 0 of the 48-bit field → byte 8 = 0x01
        bb = (0x00, 0x00, 0x00, 0x00, 0x00, 0x01)
        result = decode_report(make_report(button_bytes=bb))
        assert result is not None
        # bit 0 corresponds to... let's check: dpad_down = 0x000000010000
        # 0x000000010000 in 48 bits:
        # byte 3=0x00, byte 4=0x00, byte 5=0x01, byte 6=0x00, byte 7=0x00, byte 8=0x00
        # So bit 0 of 48-bit field → byte8 bit 0, which is NOT dpad_down
        # Let's check: 0x000000010000 → byte 5 = 0x01
        # Our bb puts 0x01 at byte 8, which means bit 0 of the 48-bit field
        # 0x000000000001 → this is just bit 0, no named button
        assert result["pressed"] == []
        assert result["button_state"] == "0x000000000001"
