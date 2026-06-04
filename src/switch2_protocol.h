/*
 * switch2_protocol.h — Switch 2 Pro Controller GATT Protocol Definitions
 *
 * Derived from CareyScott/switch2controllerpc reverse engineering.
 * These values are neutral protocol constants; do NOT assume they
 * are classical SMP LTK keys. They may be vendor-specific GATT
 * app-layer pairing credentials.
 *
 * License: MIT
 */

#ifndef SWITCH2_PROTOCOL_H
#define SWITCH2_PROTOCOL_H

#include <stdint.h>

/* ── Manufacturer & Device Identification ─────────────────────── */

#define SWITCH2_NINTENDO_MANUFACTURER_ID  0x0553
#define SWITCH2_NINTENDO_VENDOR_ID        0x057E
#define SWITCH2_PRO_CONTROLLER2_PID       0x2069
#define SWITCH2_JOYCON2_RIGHT_PID         0x2066
#define SWITCH2_JOYCON2_LEFT_PID          0x2067
#define SWITCH2_NSO_GAMECUBE_PID          0x2073

/* ── GATT Service / Characteristic UUIDs ─────────────────────── */

/* Input Report Notifications — controller → host */
#define SWITCH2_UUID_INPUT_REPORT \
    "ab7de9be-89fe-49ad-828f-118f09df7fd2"

/* Command Write — host → controller */
#define SWITCH2_UUID_COMMAND_WRITE \
    "649d4ac9-8eb7-4e6c-af44-1ea54fe5f005"

/* Command Response — controller → host (notifications) */
#define SWITCH2_UUID_COMMAND_RESPONSE \
    "c765a961-d9d8-4d36-a20a-5315b111836a"

/* Vibration — host → controller (Pro Controller) */
#define SWITCH2_UUID_VIBRATION_PRO \
    "cc483f51-9258-427d-a939-630c31f72b05"

/* ── Pair Command Constants ───────────────────────────────────── */

#define SWITCH2_PAIR_CMD             0x15

#define SWITCH2_PAIR_SUB_SET_MAC     0x01
#define SWITCH2_PAIR_SUB_KEY_1       0x04
#define SWITCH2_PAIR_SUB_KEY_2       0x02
#define SWITCH2_PAIR_SUB_FINISH      0x03

/* ── Pair Keys (vendor-specific, 17 bytes each incl. 0x00 prefix) ── */

static const uint8_t SWITCH2_PAIR_KEY_1[17] = {
    0x00, 0xea, 0xbd, 0x47, 0x13, 0x89, 0x35, 0x42,
    0xc6, 0x79, 0xee, 0x07, 0xf2, 0x53, 0x2c, 0x6c,
    0x31
};

static const uint8_t SWITCH2_PAIR_KEY_2[17] = {
    0x00, 0x40, 0xb0, 0x8a, 0x5f, 0xcd, 0x1f, 0x9b,
    0x41, 0x12, 0x5c, 0xac, 0xc6, 0x3f, 0x38, 0xa0,
    0x73
};

/* ── Command/Subcommand Families ──────────────────────────────── */

#define SWITCH2_CMD_LEDS             0x09
#define SWITCH2_CMD_VIBRATION        0x0A
#define SWITCH2_CMD_MEMORY           0x02
#define SWITCH2_CMD_FEATURE          0x0C

#define SWITCH2_SUB_LEDS_SET_PLAYER  0x07
#define SWITCH2_SUB_VIBRATION_PRESET 0x02
#define SWITCH2_SUB_MEMORY_READ      0x04
#define SWITCH2_SUB_FEATURE_INIT     0x02
#define SWITCH2_SUB_FEATURE_ENABLE   0x04

/* Feature flags */
#define SWITCH2_FEATURE_MOTION       0x04
#define SWITCH2_FEATURE_MOUSE        0x10
#define SWITCH2_FEATURE_MAGNOMETER   0x80

/* ── Payload Builder Declarations ─────────────────────────────── */

/*
 * Build a GATT command write payload.
 *
 * Format: [cmd_id] 0x91 0x01 [sub_id] 0x00 [data_len] 0x00 0x00 [data…]
 *
 * @param cmd_id       Command family (e.g. 0x15 for Pair)
 * @param sub_id       Subcommand ID
 * @param data         Subcommand payload
 * @param data_len     Length of subcommand payload
 * @param out_buf      Output buffer (must be at least data_len + 8 bytes)
 * @param out_len      Output: total payload length written
 * @return             0 on success, -1 if buffer too small
 */
int switch2_build_command(uint8_t cmd_id, uint8_t sub_id,
                          const uint8_t *data, uint8_t data_len,
                          uint8_t *out_buf, uint8_t *out_len);

/*
 * Build the SetMAC pair subcommand payload.
 *
 * @param host_mac    6-byte Host Bluetooth MAC (little-endian)
 * @param out_buf     Output buffer (>= 14 bytes: 2 prefix + 12 MAC)
 * @param out_len     Output: 14
 */
int switch2_build_pair_setmac(const uint8_t host_mac[6],
                              uint8_t out_buf[14], uint8_t *out_len);

/* Validate that the pair key arrays match the canonical values. */
int switch2_validate_pair_keys(void);

#endif /* SWITCH2_PROTOCOL_H */
