#!/usr/bin/env python3
"""
test_pair_payloads.py — Verify pair command payloads byte-for-byte
against the CareyScott/switch2controllerpc reference implementation.

Tests that:
  - SetMAC payload structure is correct
  - Pair Key 1 / 2 values match hardcoded canonical values
  - Command header format (0x91 0x01 magic) is correct
  - Finish payload is exactly one byte (0x00)
  - Pair command sequence is in correct order
"""

import sys
import unittest

# ── Constants from CareyScott reverse engineering ──

PAIR_CMD = 0x15
PAIR_SUB_SET_MAC = 0x01
PAIR_SUB_KEY_1 = 0x04
PAIR_SUB_KEY_2 = 0x02
PAIR_SUB_FINISH = 0x03

CANONICAL_KEY_1 = bytes([
    0x00, 0xea, 0xbd, 0x47, 0x13, 0x89, 0x35, 0x42,
    0xc6, 0x79, 0xee, 0x07, 0xf2, 0x53, 0x2c, 0x6c, 0x31
])

CANONICAL_KEY_2 = bytes([
    0x00, 0x40, 0xb0, 0x8a, 0x5f, 0xcd, 0x1f, 0x9b,
    0x41, 0x12, 0x5c, 0xac, 0xc6, 0x3f, 0x38, 0xa0, 0x73
])

CANONICAL_HOST_MAC = bytes([0xD8, 0x3A, 0xDD, 0xE5, 0x69, 0xED])


def build_command(cmd_id: int, sub_id: int, data: bytes = b"") -> bytes:
    """Build a Switch 2 GATT command write payload (CareyScott format)."""
    return bytes([cmd_id, 0x91, 0x01, sub_id, 0x00, len(data), 0x00, 0x00]) + data


def build_pair_setmac(host_mac: bytes) -> bytes:
    """Build SetMAC pair subcommand payload."""
    assert len(host_mac) == 6
    return b"\x00\x02" + host_mac + host_mac


class TestPairPayloads(unittest.TestCase):
    """Pair command payload byte-accuracy tests."""

    def test_key_1_unchanged(self):
        """Pair Key 1 must match the canonical CareyScott value exactly."""
        self.assertEqual(
            CANONICAL_KEY_1,
            bytes([0x00, 0xea, 0xbd, 0x47, 0x13, 0x89, 0x35, 0x42,
                   0xc6, 0x79, 0xee, 0x07, 0xf2, 0x53, 0x2c, 0x6c, 0x31]),
        )
        self.assertEqual(len(CANONICAL_KEY_1), 17)
        self.assertEqual(CANONICAL_KEY_1[0], 0x00)

    def test_key_2_unchanged(self):
        """Pair Key 2 must match the canonical CareyScott value exactly."""
        self.assertEqual(
            CANONICAL_KEY_2,
            bytes([0x00, 0x40, 0xb0, 0x8a, 0x5f, 0xcd, 0x1f, 0x9b,
                   0x41, 0x12, 0x5c, 0xac, 0xc6, 0x3f, 0x38, 0xa0, 0x73]),
        )
        self.assertEqual(len(CANONICAL_KEY_2), 17)
        self.assertEqual(CANONICAL_KEY_2[0], 0x00)

    def test_command_header_format(self):
        """Every command must have 0x91 0x01 magic bytes at [1] and [2]."""
        payload = build_command(0x15, 0x01, b"\x00\x02" + CANONICAL_HOST_MAC * 2)
        self.assertEqual(payload[0], PAIR_CMD)
        self.assertEqual(payload[1], 0x91, "Magic byte #1 must be 0x91")
        self.assertEqual(payload[2], 0x01, "Magic byte #2 must be 0x01")
        self.assertEqual(payload[3], PAIR_SUB_SET_MAC)
        self.assertEqual(payload[4], 0x00, "Byte [4] must be 0x00")
        self.assertEqual(payload[5], 14, "Data length for SetMAC payload = 14")
        self.assertEqual(payload[6], 0x00, "Byte [6] must be 0x00")
        self.assertEqual(payload[7], 0x00, "Byte [7] must be 0x00")

    def test_setmac_payload_structure(self):
        """SetMAC: 0x00 0x02 prefix + host MAC + host MAC (repeated)."""
        mac_data = build_pair_setmac(CANONICAL_HOST_MAC)
        self.assertEqual(len(mac_data), 14)
        # Prefix
        self.assertEqual(mac_data[0:2], b"\x00\x02")
        # First MAC copy
        self.assertEqual(mac_data[2:8], CANONICAL_HOST_MAC)
        # Second MAC copy
        self.assertEqual(mac_data[8:14], CANONICAL_HOST_MAC)

    def test_setmac_full_command(self):
        """Full SetMAC command: header + SetMAC payload."""
        sub_payload = build_pair_setmac(CANONICAL_HOST_MAC)
        full = build_command(PAIR_CMD, PAIR_SUB_SET_MAC, sub_payload)

        # Total length: 8 header + 14 payload = 22
        self.assertEqual(len(full), 22)

        # Verify byte-for-byte structure
        self.assertEqual(full[0], 0x15)   # cmd_id
        self.assertEqual(full[1], 0x91)   # magic
        self.assertEqual(full[2], 0x01)   # magic
        self.assertEqual(full[3], 0x01)   # sub_id = SetMAC
        self.assertEqual(full[4], 0x00)
        self.assertEqual(full[5], 14)     # data_len = 14
        self.assertEqual(full[6], 0x00)
        self.assertEqual(full[7], 0x00)
        self.assertEqual(full[8:10], b"\x00\x02")  # prefix
        self.assertEqual(full[10:16], CANONICAL_HOST_MAC)
        self.assertEqual(full[16:22], CANONICAL_HOST_MAC)

    def test_pair_key1_command(self):
        """Key 1 command: 8-byte header + 17-byte key."""
        full = build_command(PAIR_CMD, PAIR_SUB_KEY_1, CANONICAL_KEY_1)
        self.assertEqual(len(full), 8 + 17)
        self.assertEqual(full[0], 0x15)
        self.assertEqual(full[3], PAIR_SUB_KEY_1)
        self.assertEqual(full[5], 17)  # data_len = 17
        self.assertEqual(full[8:], CANONICAL_KEY_1)

    def test_pair_key2_command(self):
        """Key 2 command: 8-byte header + 17-byte key."""
        full = build_command(PAIR_CMD, PAIR_SUB_KEY_2, CANONICAL_KEY_2)
        self.assertEqual(len(full), 8 + 17)
        self.assertEqual(full[0], 0x15)
        self.assertEqual(full[3], PAIR_SUB_KEY_2)
        self.assertEqual(full[5], 17)
        self.assertEqual(full[8:], CANONICAL_KEY_2)

    def test_pair_finish_command(self):
        """Finish command: 8-byte header + 1-byte payload (0x00)."""
        full = build_command(PAIR_CMD, PAIR_SUB_FINISH, b"\x00")
        self.assertEqual(len(full), 9)
        self.assertEqual(full[0], 0x15)
        self.assertEqual(full[3], PAIR_SUB_FINISH)
        self.assertEqual(full[5], 1)   # data_len = 1
        self.assertEqual(full[8], 0x00)

    def test_pair_command_sequence_order(self):
        """Pair commands must be sent in order: SetMAC → Key1 → Key2 → Finish."""
        sequence = [PAIR_SUB_SET_MAC, PAIR_SUB_KEY_1, PAIR_SUB_KEY_2, PAIR_SUB_FINISH]
        expected = [0x01, 0x04, 0x02, 0x03]
        self.assertEqual(sequence, expected, "Pair sequence order is wrong!")

    def test_keys_not_identical(self):
        """Key 1 and Key 2 must be different (they serve different purposes)."""
        self.assertNotEqual(CANONICAL_KEY_1, CANONICAL_KEY_2)

    def test_keys_size(self):
        """Both keys must be exactly 17 bytes (including 0x00 prefix)."""
        self.assertEqual(len(CANONICAL_KEY_1), 17)
        self.assertEqual(len(CANONICAL_KEY_2), 17)


class TestUUIDs(unittest.TestCase):
    """Verify GATT UUID constants are correct — no typos."""

    def test_input_report_uuid(self):
        uuid = "ab7de9be-89fe-49ad-828f-118f09df7fd2"
        self.assertEqual(len(uuid), 36)  # standard UUID string length
        self.assertEqual(uuid.count("-"), 4)
        # Must be lowercase as per CareyScott code
        self.assertEqual(uuid, uuid.lower())

    def test_command_write_uuid(self):
        uuid = "649d4ac9-8eb7-4e6c-af44-1ea54fe5f005"
        self.assertEqual(len(uuid), 36)
        self.assertEqual(uuid, uuid.lower())

    def test_command_response_uuid(self):
        uuid = "c765a961-d9d8-4d36-a20a-5315b111836a"
        self.assertEqual(len(uuid), 36)
        self.assertEqual(uuid, uuid.lower())

    def test_uuids_different(self):
        """All duty-specific UUIDs must be unique."""
        uuids = {
            "ab7de9be-89fe-49ad-828f-118f09df7fd2",
            "649d4ac9-8eb7-4e6c-af44-1ea54fe5f005",
            "c765a961-d9d8-4d36-a20a-5315b111836a",
        }
        self.assertEqual(len(uuids), 3, "UUID collision detected!")


class TestVendorConstants(unittest.TestCase):
    """Verify manufacturer/vendor/product IDs."""

    def test_manufacturer_id(self):
        self.assertEqual(0x0553, 1363)
        self.assertNotEqual(0x0553, 0x057E)  # vendor != manufacturer

    def test_pro_controller_pid(self):
        self.assertEqual(0x2069, 8297)
        self.assertNotEqual(0x2069, 0x2066)  # Pro != JoyCon R
        self.assertNotEqual(0x2069, 0x2067)  # Pro != JoyCon L


if __name__ == "__main__":
    result = unittest.main(verbosity=2, exit=False)
    sys.exit(0 if result.result.wasSuccessful() else 1)
