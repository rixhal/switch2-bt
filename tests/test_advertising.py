"""Unit tests for reconnect_mac parsing (from advertising manufacturer data)."""

import sys
from pathlib import Path

# Allow importing switch2d modules
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from switch2d import parse_reconnect_mac, parse_manufacturer_data


class TestParseReconnectMac:
    """Tests for parse_reconnect_mac() — pairing status derivation."""

    def test_zero_reconnect_mac_pairing_mode(self):
        """Zero reconnect_mac → pairing_mode True."""
        zero_mac = b'\x00\x00\x00\x00\x00\x00'
        result = parse_reconnect_mac(zero_mac)
        assert result["pairing_mode"] is True
        assert result["paired_to_host"] is None  # no host_mac provided
        assert result["paired_to_other_host"] is None

    def test_host_reconnect_mac_paired_to_host(self):
        """Host reconnect_mac → paired_to_host True."""
        host_mac_bytes = b'\xaa\xbb\xcc\xdd\xee\xff'
        host_mac_str = "aa:bb:cc:dd:ee:ff"
        result = parse_reconnect_mac(host_mac_bytes, host_mac=host_mac_str)
        assert result["pairing_mode"] is False
        assert result["paired_to_host"] is True
        assert result["paired_to_other_host"] is False

    def test_other_reconnect_mac_paired_to_other(self):
        """Other reconnect_mac → paired_to_other_host True."""
        other_mac = b'\x11\x22\x33\x44\x55\x66'
        host_mac_str = "aa:bb:cc:dd:ee:ff"
        result = parse_reconnect_mac(other_mac, host_mac=host_mac_str)
        assert result["pairing_mode"] is False
        assert result["paired_to_host"] is False
        assert result["paired_to_other_host"] is True

    def test_host_mac_none_preserves_none(self):
        """Without host_mac, paired_to_host/paired_to_other_host are None (unknown)."""
        nonzero_mac = b'\xaa\xbb\xcc\xdd\xee\xff'
        result = parse_reconnect_mac(nonzero_mac, host_mac=None)
        assert result["pairing_mode"] is False
        assert result["paired_to_host"] is None
        assert result["paired_to_other_host"] is None

    def test_host_mac_format_variants(self):
        """Host MAC with different separators or no separators."""
        host_mac_bytes = b'\xaa\xbb\xcc\xdd\xee\xff'
        # Colon-separated
        r1 = parse_reconnect_mac(host_mac_bytes, host_mac="aa:bb:cc:dd:ee:ff")
        assert r1["paired_to_host"] is True
        # No separators
        r2 = parse_reconnect_mac(host_mac_bytes, host_mac="aabbccddeeff")
        assert r2["paired_to_host"] is True
        # Dash-separated (stripped by replace)
        r3 = parse_reconnect_mac(host_mac_bytes, host_mac="aa-bb-cc-dd-ee-ff")
        assert r3["paired_to_host"] is True

    def test_invalid_host_mac_graceful(self):
        """Invalid host_mac string → no match, paired fields return None (unknown)."""
        nonzero_mac = b'\xaa\xbb\xcc\xdd\xee\xff'
        result = parse_reconnect_mac(nonzero_mac, host_mac="invalid")
        assert result["pairing_mode"] is False
        assert result["paired_to_host"] is None
        assert result["paired_to_other_host"] is None


class TestParseManufacturerData:
    """Tests for parse_manufacturer_data() — full ad data parsing."""

    def test_complete_switch2_ad_data(self):
        """Full 16-byte Switch 2 manufacturer data yields all fields."""
        # Simulated: [0:1]=?, [1:3]=?, [3:5]=0x057E, [5:7]=0x2069, [7:10]=?,
        #             [10:16]=reconnect_mac=00:00:00:00:00:00
        data = bytes([
            0x01, 0x00, 0x03,  # unknown
            0x7E, 0x05,        # vendor_id = 0x057E LE
            0x69, 0x20,        # product_id = 0x2069 LE
            0x00, 0x00, 0x00,  # unknown
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  # reconnect_mac = zeros
        ])
        result = parse_manufacturer_data(data)
        assert result["vendor_id"] == 0x057E
        assert result["product_id"] == 0x2069
        assert result["reconnect_mac"] == b'\x00\x00\x00\x00\x00\x00'
        assert result["pairing_mode"] is True
        assert result["paired_to_host"] is None  # no host_mac
        assert result["paired_to_other_host"] is None

    def test_short_data_returns_none_values(self):
        """Data shorter than 16 bytes returns None for all fields."""
        result = parse_manufacturer_data(b'\x01\x02\x03')
        assert result["vendor_id"] is None
        assert result["product_id"] is None
        assert result["reconnect_mac"] is None
        assert result["pairing_mode"] is None
        assert result["paired_to_host"] is None
        assert result["paired_to_other_host"] is None

    def test_nonzero_reconnect_mac(self):
        """Nonzero reconnect_mac → not pairing_mode."""
        data = bytes([
            0x01, 0x00, 0x03,
            0x7E, 0x05, 0x69, 0x20,
            0x00, 0x00, 0x00,
            0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
        ])
        result = parse_manufacturer_data(data)
        assert result["pairing_mode"] is False
        assert result["reconnect_mac"] == b'\xaa\xbb\xcc\xdd\xee\xff'
