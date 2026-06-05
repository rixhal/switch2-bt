"""Tests for joycon2cpp-pair experimental profile.

Validates:
  - Profile registration and structure
  - PAIR constants match CareyScott canonical values
  - build_command() produces correct pair payloads
  - Pair step sequence is correct
  - Host MAC helper works (mock-safe)
  - Profile is NOT in auto-detect order
  - Diagnostic fields are present in state
"""

import os
import pytest
from switch2d import (
    PROTOCOL_PROFILES,
    PROFILE_JOYCON2CPP_PAIR,
    PROFILE_JOYCON2CPP,
    AUTO_PROFILE_ORDER,
    PAIR_CMD,
    PAIR_SUB_SET_MAC,
    PAIR_SUB_KEY_1,
    PAIR_SUB_KEY_2,
    PAIR_SUB_FINISH,
    PAIR_KEY_1,
    PAIR_KEY_2,
    build_command,
    get_host_bluetooth_mac,
    _build_setmac_payload,
    DaemonState,
    StageLogger,
    ProtocolProfile,
)


# ═══════════════════════════════════════════════════════════════
# Profile registration
# ═══════════════════════════════════════════════════════════════

class TestJoycon2cppPairProfile:
    """Verify joycon2cpp-pair profile structure."""

    def test_profile_registered(self):
        """joycon2cpp-pair is in PROTOCOL_PROFILES."""
        assert "joycon2cpp-pair" in PROTOCOL_PROFILES
        assert PROTOCOL_PROFILES["joycon2cpp-pair"] is PROFILE_JOYCON2CPP_PAIR

    def test_profile_name(self):
        assert PROFILE_JOYCON2CPP_PAIR.name == "joycon2cpp-pair"

    def test_input_uuid_matches_joycon2cpp(self):
        assert PROFILE_JOYCON2CPP_PAIR.input_uuids == ["ab7de9be-89fe-49ad-828f-118f09df7fd2"]

    def test_command_uuid_matches_joycon2cpp(self):
        assert PROFILE_JOYCON2CPP_PAIR.command_uuid == "649d4ac9-8eb7-4e6c-af44-1ea54fe5f005"

    def test_not_in_auto_detect_order(self):
        """Experimental profiles must NOT be in auto-detect order."""
        assert "joycon2cpp-pair" not in AUTO_PROFILE_ORDER

    def test_has_init_commands(self):
        """Inherits joycon2cpp init commands."""
        assert len(PROFILE_JOYCON2CPP_PAIR.init_commands) == 4
        assert PROFILE_JOYCON2CPP_PAIR.init_commands == PROFILE_JOYCON2CPP.init_commands

    def test_subscribe_strategy_selected(self):
        assert PROFILE_JOYCON2CPP_PAIR.subscribe_strategy == "selected"

    def test_description_mentions_experimental(self):
        assert "EXPERIMENTAL" in PROFILE_JOYCON2CPP_PAIR.description
        assert "COMMAND_PAIR" in PROFILE_JOYCON2CPP_PAIR.description


# ═══════════════════════════════════════════════════════════════
# PAIR constants
# ═══════════════════════════════════════════════════════════════

class TestPairConstants:
    """Verify pair command constants match CareyScott reference."""

    def test_pair_cmd_value(self):
        assert PAIR_CMD == 0x15

    def test_sub_ids(self):
        assert PAIR_SUB_SET_MAC == 0x01
        assert PAIR_SUB_KEY_1 == 0x04
        assert PAIR_SUB_KEY_2 == 0x02
        assert PAIR_SUB_FINISH == 0x03

    def test_pair_command_sequence_order(self):
        """Pair commands must be: SetMAC → Key1 → Key2 → Finish."""
        sequence = [PAIR_SUB_SET_MAC, PAIR_SUB_KEY_1, PAIR_SUB_KEY_2, PAIR_SUB_FINISH]
        assert sequence == [0x01, 0x04, 0x02, 0x03]

    def test_key_1_canonical(self):
        """Key 1 must match CareyScott canonical value exactly."""
        canonical = bytes([
            0x00, 0xea, 0xbd, 0x47, 0x13, 0x89, 0x35, 0x42,
            0xc6, 0x79, 0xee, 0x07, 0xf2, 0x53, 0x2c, 0x6c, 0x31,
        ])
        assert PAIR_KEY_1 == canonical
        assert len(PAIR_KEY_1) == 17
        assert PAIR_KEY_1[0] == 0x00  # prefix byte

    def test_key_2_canonical(self):
        """Key 2 must match CareyScott canonical value exactly."""
        canonical = bytes([
            0x00, 0x40, 0xb0, 0x8a, 0x5f, 0xcd, 0x1f, 0x9b,
            0x41, 0x12, 0x5c, 0xac, 0xc6, 0x3f, 0x38, 0xa0, 0x73,
        ])
        assert PAIR_KEY_2 == canonical
        assert len(PAIR_KEY_2) == 17

    def test_keys_not_identical(self):
        assert PAIR_KEY_1 != PAIR_KEY_2


# ═══════════════════════════════════════════════════════════════
# Pair payload building
# ═══════════════════════════════════════════════════════════════

class TestPairPayloadBuilding:
    """Verify build_command() produces correct pair command payloads."""

    def test_setmac_full_payload(self):
        """SetMAC: [0x15 0x91 0x01 0x01 0x00 0x0E 0x00 0x00] + [0x00 0x02] + MACx2."""
        host_mac = "aa:bb:cc:dd:ee:ff"
        sub_data = _build_setmac_payload(host_mac)
        assert sub_data == b'\x00\x02' + bytes.fromhex('aabbccddeeff') * 2

        payload = build_command(PAIR_CMD, PAIR_SUB_SET_MAC, sub_data)
        assert len(payload) == 8 + 14  # header + 2 + 6 + 6
        assert payload[0] == 0x15
        assert payload[1] == 0x91
        assert payload[2] == 0x01
        assert payload[3] == 0x01  # sub = SetMAC
        assert payload[5] == 14    # data_len

    def test_key1_payload(self):
        payload = build_command(PAIR_CMD, PAIR_SUB_KEY_1, PAIR_KEY_1)
        assert len(payload) == 8 + 17
        assert payload[3] == 0x04
        assert payload[5] == 17
        assert payload[8:] == PAIR_KEY_1

    def test_key2_payload(self):
        payload = build_command(PAIR_CMD, PAIR_SUB_KEY_2, PAIR_KEY_2)
        assert len(payload) == 8 + 17
        assert payload[3] == 0x02
        assert payload[5] == 17

    def test_finish_payload(self):
        payload = build_command(PAIR_CMD, PAIR_SUB_FINISH, b'\x00')
        assert len(payload) == 9
        assert payload[3] == 0x03
        assert payload[5] == 1
        assert payload[8] == 0x00

    def test_all_pair_steps_valid_magic(self):
        """Every pair step has mandatory 0x91 0x01 magic bytes."""
        host_mac = "aa:bb:cc:dd:ee:ff"
        steps = [
            build_command(PAIR_CMD, PAIR_SUB_SET_MAC, _build_setmac_payload(host_mac)),
            build_command(PAIR_CMD, PAIR_SUB_KEY_1, PAIR_KEY_1),
            build_command(PAIR_CMD, PAIR_SUB_KEY_2, PAIR_KEY_2),
            build_command(PAIR_CMD, PAIR_SUB_FINISH, b'\x00'),
        ]
        for i, payload in enumerate(steps):
            assert payload[0] == 0x15, f"Step {i}: bad cmd_id"
            assert payload[1] == 0x91, f"Step {i}: bad magic byte 1"
            assert payload[2] == 0x01, f"Step {i}: bad magic byte 2"


# ═══════════════════════════════════════════════════════════════
# Host MAC helper
# ═══════════════════════════════════════════════════════════════

class TestHostBluetoothMac:
    """Verify get_host_bluetooth_mac() returns plausible results or None."""

    def test_returns_string_or_none(self):
        result = get_host_bluetooth_mac()
        assert result is None or isinstance(result, str)

    def test_colon_format_if_present(self):
        result = get_host_bluetooth_mac()
        if result:
            assert len(result) == 17  # "aa:bb:cc:dd:ee:ff"
            assert result.count(':') == 5
            assert result == result.lower()


# ═══════════════════════════════════════════════════════════════
# DaemonState pair diagnostics
# ═══════════════════════════════════════════════════════════════

class TestDaemonStatePairFields:
    """Verify DaemonState has pair diagnostic fields."""

    def test_pair_fields_exist(self):
        import argparse
        state = DaemonState(args=argparse.Namespace(), log=StageLogger())
        assert hasattr(state, 'host_mac')
        assert hasattr(state, 'pair_sequence_ok')
        assert hasattr(state, 'pair_failure_step')
        assert hasattr(state, 'pair_responses')
        assert hasattr(state, 'command_response_subscribed')

    def test_pair_fields_defaults(self):
        import argparse
        state = DaemonState(args=argparse.Namespace(), log=StageLogger())
        assert state.host_mac is None
        assert state.pair_sequence_ok is False
        assert state.pair_failure_step is None
        assert state.pair_responses == []
        assert state.command_response_subscribed is False

    def test_reset_session_clears_pair_fields(self):
        import argparse
        state = DaemonState(args=argparse.Namespace(), log=StageLogger())
        state.pair_sequence_ok = True
        state.pair_failure_step = "setmac"
        state.pair_responses.append("deadbeef")
        state.command_response_subscribed = True

        state.reset_session()
        assert state.pair_sequence_ok is False
        assert state.pair_failure_step is None
        assert state.pair_responses == []
        assert state.command_response_subscribed is False


# ═══════════════════════════════════════════════════════════════
# SetMAC payload builder
# ═══════════════════════════════════════════════════════════════

class TestBuildSetmacPayload:
    """Verify _build_setmac_payload helper."""

    def test_format(self):
        host_mac = "aa:bb:cc:dd:ee:ff"
        result = _build_setmac_payload(host_mac)
        assert result[:2] == b'\x00\x02'
        assert len(result) == 14  # 2 prefix + 6 mac + 6 mac

    def test_mac_repeated(self):
        host_mac = "11:22:33:44:55:66"
        result = _build_setmac_payload(host_mac)
        mac_bytes = bytes.fromhex('112233445566')
        assert result[2:8] == mac_bytes
        assert result[8:14] == mac_bytes
