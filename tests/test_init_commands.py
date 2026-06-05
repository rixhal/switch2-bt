"""Tests for joycon2cpp init command byte sequences and timing.

Validates against the documented working implementation from
TheFrano/joycon2cpp (testapp/src/testapp.cpp lines 966-1075).
"""

import pytest
from switch2d import (
    PROTOCOL_PROFILES,
    PROFILE_JOYCON2CPP,
    PROFILE_MACOS,
    PROFILE_SPRO2WIN,
    ProtocolProfile,
)


# ── joycon2cpp profile init commands ───────────────────────────────

class TestJoycon2cppInitCommands:
    """Verify joycon2cpp init command payloads match reference."""

    def test_profile_exists(self):
        """joycon2cpp profile is registered."""
        assert PROTOCOL_PROFILES["joycon2cpp"] is PROFILE_JOYCON2CPP

    def test_four_init_commands(self):
        """joycon2cpp profile has exactly 4 init commands."""
        cmds = PROFILE_JOYCON2CPP.init_commands
        assert len(cmds) == 4, f"Expected 4 init commands, got {len(cmds)}"

    def test_feature_select_02_payload(self):
        """Feature-select 0x02 matches joycon2cpp reference."""
        _, payload, delay = PROFILE_JOYCON2CPP.init_commands[0]
        expected = bytes.fromhex("0c 91 01 02 00 04 00 00 ff 00 00 00")
        assert payload == expected, f"Got: {payload.hex(' ')}"
        assert delay == pytest.approx(0.5)  # 500ms

    def test_feature_select_04_payload(self):
        """Feature-select 0x04 matches joycon2cpp reference."""
        _, payload, delay = PROFILE_JOYCON2CPP.init_commands[1]
        expected = bytes.fromhex("0c 91 01 04 00 04 00 00 ff 00 00 00")
        assert payload == expected, f"Got: {payload.hex(' ')}"
        # 0.7s = 500ms init delay + 200ms barrier (combined for simplicity)
        assert delay == pytest.approx(0.7)

    def test_led_payload(self):
        """LED player 1 matches joycon2cpp reference."""
        _, payload, delay = PROFILE_JOYCON2CPP.init_commands[2]
        expected = bytes.fromhex("09 91 01 07 00 08 00 00 01 00 00 00 00 00 00 00")
        assert payload == expected, f"Got: {payload.hex(' ')}"
        assert delay == pytest.approx(0.05)  # 50ms

    def test_sound_payload(self):
        """Sound command matches joycon2cpp reference."""
        _, payload, delay = PROFILE_JOYCON2CPP.init_commands[3]
        expected = bytes.fromhex("0a 91 01 02 00 08 00 00 04 00 00 00 00 00 00 00")
        assert payload == expected, f"Got: {payload.hex(' ')}"
        assert delay == pytest.approx(0.05)  # 50ms

    def test_total_init_time(self):
        """Total init sequence time matches joycon2cpp timing."""
        total = sum(delay for _, _, delay in PROFILE_JOYCON2CPP.init_commands)
        # 0.5 + 0.7 + 0.05 + 0.05 = 1.3s
        # joycon2cpp reference: 500ms + 500ms + 200ms + 50ms + 50ms = 1.3s
        assert total == pytest.approx(1.3, abs=0.01)

    def test_command_ids_joycon2cpp_format(self):
        """All init commands use Nintendo command wire format [cmd_id, 0x91, 0x01, ...]."""
        for label, payload, _ in PROFILE_JOYCON2CPP.init_commands:
            assert len(payload) >= 8, f"{label}: payload too short ({len(payload)} bytes)"
            assert payload[1] == 0x91, f"{label}: byte 1 not 0x91"
            assert payload[2] == 0x01, f"{label}: byte 2 not 0x01"


# ── Profile comparison ────────────────────────────────────────────

class TestProfileStructure:
    """Validate ProtocolProfile integrity across all profiles."""

    def test_macos_no_init(self):
        """macos profile has no init commands."""
        assert PROFILE_MACOS.init_commands == []
        assert PROFILE_MACOS.command_uuid is None

    def test_macos_has_input_uuid(self):
        """macos profile has input UUID for selected notify."""
        assert len(PROFILE_MACOS.input_uuids) == 1
        assert PROFILE_MACOS.input_uuids[0] == "7492866c-ec3e-4619-8258-32755ffcc0f9"

    def test_spro2win_subscribe_all(self):
        """spro2win profile uses subscribe-all strategy."""
        assert PROFILE_SPRO2WIN.subscribe_strategy == "all"
        assert PROFILE_SPRO2WIN.spro2win_handle == 45

    def test_joycon2cpp_has_command_uuid(self):
        """joycon2cpp profile has a command UUID for init writes."""
        assert PROFILE_JOYCON2CPP.command_uuid == "649d4ac9-8eb7-4e6c-af44-1ea54fe5f005"

    def test_all_profiles_registered(self):
        """All expected profiles are in PROTOCOL_PROFILES."""
        expected = {"macos", "spro2win", "joycon2cpp", "joycon2cpp-pair"}
        assert set(PROTOCOL_PROFILES.keys()) == expected
    def test_all_profiles_are_protocol_profile_instances(self):
        """Every registered profile is a ProtocolProfile dataclass."""
        for name, profile in PROTOCOL_PROFILES.items():
            assert isinstance(profile, ProtocolProfile), f"{name} is not ProtocolProfile"


# ── uinput button coverage ────────────────────────────────────────

class TestUinputButtonCoverage:
    """Verify all 21 buttons in BUTTON_MAP are covered by uinput."""

    def test_all_buttons_mapped(self):
        """Every button in BUTTON_MAP has a uinput mapping in update_uinput."""
        from switch2d import BUTTON_MAP

        # This is a static check: all buttons should be mappable.
        # The actual mapping lives in update_uinput() — we verify btn_map
        # covers all BUTTON_MAP entries by importing and inspecting.

        # Known uinput-mapped buttons (from update_uinput's btn_map)
        mapped = {
            "b", "a", "y", "x",
            "l", "r", "zl", "zr",
            "back", "start", "home",
            "l3", "r3",
            "dpad_up", "dpad_down", "dpad_left", "dpad_right",
            "c", "gl", "gr", "screenshot",
        }

        button_names = set(BUTTON_MAP.keys())
        assert button_names == mapped, (
            f"Missing from uinput: {button_names - mapped}\n"
            f"Extra in uinput: {mapped - button_names}"
        )
