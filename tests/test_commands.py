"""Tests for command building: build_command."""

import pytest
from switch2d import build_command


class TestBuildCommand:
    """Command builder: build_command(cmd_id, sub_id, data)."""

    def test_empty_data(self):
        """Command with no payload — just the 8-byte header."""
        result = build_command(0x02, 0x01, b"")
        expected = bytes([0x02, 0x91, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00])
        assert result == expected
        assert len(result) == 8

    def test_set_mac_command(self):
        """SetMAC command: cmd=0x15, sub=0x01, 14 bytes of data."""
        mac_data = bytes([0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00])
        result = build_command(0x15, 0x01, mac_data)
        # header: [0x15, 0x91, 0x01, 0x01, 0x00, 0x0E, 0x00, 0x00]
        expected = bytes([0x15, 0x91, 0x01, 0x01, 0x00, 0x0E, 0x00, 0x00]) + mac_data
        assert result == expected
        assert len(result) == 8 + 14

    def test_key1_command(self):
        """Key1 command: cmd=0x15, sub=0x04, 17 bytes."""
        key = bytes(range(17))  # 0x00..0x10
        result = build_command(0x15, 0x04, key)
        expected = bytes([0x15, 0x91, 0x01, 0x04, 0x00, 0x11, 0x00, 0x00]) + key
        assert result == expected
        assert len(result) == 8 + 17

    def test_key2_command(self):
        """Key2 command: cmd=0x15, sub=0x02, 17 bytes."""
        key = bytes([0xFF] * 17)
        result = build_command(0x15, 0x02, key)
        expected = bytes([0x15, 0x91, 0x01, 0x02, 0x00, 0x11, 0x00, 0x00]) + key
        assert result == expected

    def test_finish_command(self):
        """Finish command: cmd=0x15, sub=0x03, data=b\"\\x00\"."""
        result = build_command(0x15, 0x03, b"\x00")
        expected = bytes([0x15, 0x91, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00])
        assert result == expected

    def test_length_field(self):
        """The 6th byte is the data length."""
        # 5 bytes of data
        result = build_command(0x10, 0x20, b"\x01\x02\x03\x04\x05")
        assert result[5] == 5  # length byte

        # 255 bytes of data
        large = bytes(255)
        result = build_command(0x10, 0x20, large)
        assert result[5] == 255
        assert len(result) == 8 + 255

    def test_zero_data_truncates_length(self):
        """Length=0 data is still valid."""
        result = build_command(0x00, 0x00, b"")
        assert len(result) == 8
        assert result[5] == 0

    def test_different_cmd_and_sub_ids(self):
        """Arbitrary cmd_id and sub_id values are placed correctly."""
        result = build_command(0xAB, 0xCD, b"\x42")
        assert result[0] == 0xAB  # cmd_id
        assert result[1] == 0x91
        assert result[2] == 0x01
        assert result[3] == 0xCD  # sub_id
        assert result[4] == 0x00
        assert result[5] == 0x01  # len = 1
        assert result[6] == 0x00
        assert result[7] == 0x00
        assert result[8] == 0x42  # data byte

    def test_data_preserved(self):
        """All data bytes are preserved exactly."""
        data = bytes([0xDE, 0xAD, 0xBE, 0xEF])
        result = build_command(0x01, 0x02, data)
        assert result[8:] == data

    def test_len_overflow_raises(self):
        """Data >255 bytes raises ValueError (length byte only holds 0-255)."""
        with pytest.raises(ValueError, match="range"):
            build_command(0x01, 0x02, bytes(256))
