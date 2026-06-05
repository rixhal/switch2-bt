"""Tests for low-level parsing functions: decode_stick12, s16, parse_manufacturer_data."""

import pytest
from switch2d import decode_stick12, s16, parse_manufacturer_data


# ── decode_stick12 ──────────────────────────────────────────────

class TestDecodeStick12:
    """12-bit packed stick decoder tests."""

    # --- Helper: raw bytes for given (x, y) ---
    def _raw(self, x: int, y: int) -> bytes:
        """Pack x,y into the 3-byte 12-bit stick format."""
        x_low = x & 0xFF
        x_high = (x >> 8) & 0x0F
        y_low = y & 0x0F
        y_high = (y >> 4) & 0xFF
        return bytes([x_low, (y_low << 4) | x_high, y_high])

    def test_center(self):
        """Stick at center (2048, 2048)."""
        data = self._raw(2048, 2048)
        result = decode_stick12(data, 0)
        assert result is not None
        assert result["x"] == 2048
        assert result["y"] == 2048
        assert result["x_norm"] == 0.0
        assert result["y_norm"] == 0.0

    def test_min(self):
        """Stick at minimum (0, 0)."""
        data = self._raw(0, 0)
        result = decode_stick12(data, 0)
        assert result is not None
        assert result["x"] == 0
        assert result["y"] == 0
        assert result["x_norm"] == pytest.approx(-2048 / 2047, abs=1e-3)
        assert result["y_norm"] == pytest.approx(-2048 / 2047, abs=1e-3)

    def test_max(self):
        """Stick at maximum (4095, 4095)."""
        data = self._raw(4095, 4095)
        result = decode_stick12(data, 0)
        assert result is not None
        assert result["x"] == 4095
        assert result["y"] == 4095
        assert result["x_norm"] == pytest.approx((4095 - 2048) / 2047, abs=1e-3)
        assert result["y_norm"] == pytest.approx((4095 - 2048) / 2047, abs=1e-3)

    def test_partial_data_too_short(self):
        """Data shorter than offset+3 returns None."""
        assert decode_stick12(b"\x00\x00", 0) is None
        assert decode_stick12(b"\x00", 0) is None
        assert decode_stick12(b"", 0) is None

    def test_offset(self):
        """Non-zero offset decodes from the correct position."""
        prefix = b"\xFF" * 5  # 5 garbage bytes
        center = self._raw(2048, 2048)
        data = prefix + center
        result = decode_stick12(data, 5)
        assert result is not None
        assert result["x"] == 2048
        assert result["y"] == 2048

    def test_offset_too_short(self):
        """Data too short after offset returns None."""
        result = decode_stick12(b"\x00\x00\x00\x00", 2)  # only 2 bytes from offset 2
        assert result is None

    def test_known_values(self):
        """Pre-computed known values from manual calculation."""
        # x=512, y=1024
        # x_low=0x00, x_high=0x02, y_low=0x0, y_high=0x40
        # raw[0]=0x00, raw[1]=(0<<4)|0x02=0x02, raw[2]=0x40
        data = bytes([0x00, 0x02, 0x40])
        result = decode_stick12(data, 0)
        assert result is not None
        assert result["x"] == 512
        assert result["y"] == 1024

    def test_quarter_values(self):
        """x=3072 (center+1024), y=1024 (center-1024)."""
        data = self._raw(3072, 1024)
        result = decode_stick12(data, 0)
        assert result is not None
        assert result["x"] == 3072
        assert result["y"] == 1024


# ── s16 ─────────────────────────────────────────────────────────

class TestS16:
    """Signed 16-bit LE decoder tests."""

    def test_zero(self):
        assert s16(b"\x00\x00", 0) == 0

    def test_positive_one(self):
        assert s16(b"\x01\x00", 0) == 1

    def test_negative_one(self):
        assert s16(b"\xFF\xFF", 0) == -1

    def test_max_positive(self):
        """INT16_MAX = 32767."""
        assert s16(b"\xFF\x7F", 0) == 32767

    def test_min_negative(self):
        """INT16_MIN = -32768."""
        assert s16(b"\x00\x80", 0) == -32768

    def test_too_short(self):
        """Data shorter than offset+2 returns None."""
        assert s16(b"\x00", 0) is None
        assert s16(b"", 0) is None

    def test_offset(self):
        """Non-zero offset decodes from the correct position."""
        data = b"\xFF" * 3 + b"\x34\x12"  # 0x1234 = 4660
        assert s16(data, 3) == 4660

    def test_offset_too_short(self):
        """Data too short after offset returns None."""
        assert s16(b"\x00\x00\x00", 2) is None  # only 1 byte from offset 2

    def test_large_positive(self):
        """10000."""
        assert s16((10000).to_bytes(2, "little", signed=True), 0) == 10000

    def test_large_negative(self):
        """-10000."""
        assert s16((-10000).to_bytes(2, "little", signed=True), 0) == -10000


# ── parse_manufacturer_data ─────────────────────────────────────

class TestParseManufacturerData:
    """Manufacturer data parser tests."""

    def test_valid_nintendo_data(self):
        """Full 26-byte manufacturer data with Nintendo IDs."""
        # vendor_id = 0x057E (NINTENDO_VID) at bytes 3-5 (LE)
        # product_id = 0x2069 (PRO_CONTROLLER2_PID) at bytes 5-7 (LE)
        # reconnect_mac at bytes 10-16
        mfg = bytes([0x00, 0x00, 0x00,
                     0x7E, 0x05,          # vendor_id LE = 0x057E
                     0x69, 0x20,          # product_id LE = 0x2069
                     0x00, 0x00, 0x00,
                     0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF])  # MAC
        result = parse_manufacturer_data(mfg)
        assert result["vendor_id"] == 0x057E
        assert result["product_id"] == 0x2069
        assert result["reconnect_mac"] == bytes([0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF])

    def test_short_data(self):
        """Data shorter than 16 bytes returns all None."""
        result = parse_manufacturer_data(b"\x00" * 10)
        assert result["vendor_id"] is None
        assert result["product_id"] is None
        assert result["reconnect_mac"] is None

    def test_exactly_15_bytes(self):
        """Exactly 15 bytes — still too short."""
        result = parse_manufacturer_data(b"\x00" * 15)
        assert result["vendor_id"] is None
        assert result["product_id"] is None
        assert result["reconnect_mac"] is None

    def test_exactly_16_bytes(self):
        """Bare minimum — exactly 16 bytes."""
        mfg = bytes([0x00] * 3 + [0x7E, 0x05, 0x69, 0x20] + [0x00] * 3 + [0x11] * 6)
        result = parse_manufacturer_data(mfg)
        assert result["vendor_id"] == 0x057E
        assert result["product_id"] == 0x2069
        assert result["reconnect_mac"] == bytes([0x11] * 6)

    def test_all_zeros(self):
        """16 bytes of zeros."""
        result = parse_manufacturer_data(b"\x00" * 16)
        assert result["vendor_id"] == 0
        assert result["product_id"] == 0
        assert result["reconnect_mac"] == bytes(6)  # 6 zero bytes

    def test_empty_data(self):
        """Empty bytes returns all None."""
        result = parse_manufacturer_data(b"")
        assert result["vendor_id"] is None
        assert result["product_id"] is None
        assert result["reconnect_mac"] is None

    def test_nonzero_vendor(self):
        """Custom vendor/product IDs at known offsets."""
        mfg = bytes([0xFF] * 3 + [0x34, 0x12, 0x78, 0x56] + [0x00] * 3 + [0xDE] * 6)
        result = parse_manufacturer_data(mfg)
        assert result["vendor_id"] == 0x1234
        assert result["product_id"] == 0x5678
        assert result["reconnect_mac"] == bytes([0xDE] * 6)

    def test_large_data(self):
        """Data larger than 26 bytes — only first 16 matter."""
        mfg = bytes([0x01] * 50)
        result = parse_manufacturer_data(mfg)
        assert result["vendor_id"] == 0x0101
        assert result["product_id"] == 0x0101
        assert result["reconnect_mac"] == bytes([0x01] * 6)
