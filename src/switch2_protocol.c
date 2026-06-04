/*
 * switch2_protocol.c — Payload builder implementation
 *
 * License: MIT
 */

#include "switch2_protocol.h"
#include <string.h>

/* ── Command Payload Builder ────────────────────────────────────
 *
 * CareyScott wire format:
 *   [0]     cmd_id
 *   [1]     0x91        (magic constant)
 *   [2]     0x01        (magic constant)
 *   [3]     sub_id
 *   [4]     0x00
 *   [5]     data_len    (subcommand payload length)
 *   [6]     0x00
 *   [7]     0x00
 *   [8..]   data        (subcommand payload)
 *
 * Total header = 8 bytes, total = 8 + data_len
 */
int switch2_build_command(uint8_t cmd_id, uint8_t sub_id,
                          const uint8_t *data, uint8_t data_len,
                          uint8_t *out_buf, uint8_t *out_len)
{
    if (!out_buf || !out_len) return -1;
    if (data_len > 0 && !data) return -1;

    out_buf[0] = cmd_id;
    out_buf[1] = 0x91;
    out_buf[2] = 0x01;
    out_buf[3] = sub_id;
    out_buf[4] = 0x00;
    out_buf[5] = data_len;
    out_buf[6] = 0x00;
    out_buf[7] = 0x00;

    if (data_len > 0) {
        memcpy(out_buf + 8, data, data_len);
    }

    *out_len = 8 + data_len;
    return 0;
}

/* ── SetMAC Pair Subcommand ────────────────────────────────────
 *
 * CareyScott payload:
 *   [0..1]  0x00 0x02   (prefix)
 *   [2..7]  host_mac    (6 bytes little-endian)
 *   [8..13] host_mac    (6 bytes little-endian, repeated)
 *
 * Total: 14 bytes
 */
int switch2_build_pair_setmac(const uint8_t host_mac[6],
                              uint8_t out_buf[14], uint8_t *out_len)
{
    if (!host_mac || !out_buf || !out_len) return -1;

    out_buf[0] = 0x00;
    out_buf[1] = 0x02;
    memcpy(out_buf + 2,  host_mac, 6);
    memcpy(out_buf + 8,  host_mac, 6);

    *out_len = 14;
    return 0;
}

/* ── Pair Key Validation ──────────────────────────────────────── */

int switch2_validate_pair_keys(void)
{
    static const uint8_t expected_key1[17] = {
        0x00, 0xea, 0xbd, 0x47, 0x13, 0x89, 0x35, 0x42,
        0xc6, 0x79, 0xee, 0x07, 0xf2, 0x53, 0x2c, 0x6c, 0x31
    };
    static const uint8_t expected_key2[17] = {
        0x00, 0x40, 0xb0, 0x8a, 0x5f, 0xcd, 0x1f, 0x9b,
        0x41, 0x12, 0x5c, 0xac, 0xc6, 0x3f, 0x38, 0xa0, 0x73
    };

    if (memcmp(SWITCH2_PAIR_KEY_1, expected_key1, 17) != 0) return -1;
    if (memcmp(SWITCH2_PAIR_KEY_2, expected_key2, 17) != 0) return -1;
    return 0;
}
