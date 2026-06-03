/* test_bdaddr.c - Test parse_colon_bytes + get_local_bdaddr_text roundtrip */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

static int parse_colon_bytes(const char *s, uint8_t out[6])
{
    unsigned int v[6];
    if (!s)
        return -1;
    if (sscanf(s, "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return -1;
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xff)
            return -1;
        out[i] = (uint8_t)v[i];
    }
    return 0;
}

static int get_local_bdaddr_text(int dev_id, char *txt, size_t txtsz)
{
    bdaddr_t ba;
    if (hci_devba(dev_id, &ba) < 0) {
        return -1;
    }
    ba2str(&ba, txt);
    if (strlen(txt) >= txtsz) {
        return -1;
    }
    return 0;
}

static int failures = 0;

static void check(const char *name, int condition)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    } else {
        fprintf(stderr, "PASS: %s\n", name);
    }
}

int main(void)
{
    uint8_t out[6];
    char txt[18];

    /* Test parse_colon_bytes */
    check("parse AA:BB:CC:DD:EE:FF",
          parse_colon_bytes("AA:BB:CC:DD:EE:FF", out) == 0 &&
          out[0] == 0xAA && out[1] == 0xBB && out[2] == 0xCC &&
          out[3] == 0xDD && out[4] == 0xEE && out[5] == 0xFF);

    check("parse 00:00:00:00:00:00",
          parse_colon_bytes("00:00:00:00:00:00", out) == 0 &&
          out[0] == 0 && out[1] == 0 && out[2] == 0 &&
          out[3] == 0 && out[4] == 0 && out[5] == 0);

    check("parse 0a:1b:2c:3d:4e:5f",
          parse_colon_bytes("0a:1b:2c:3d:4e:5f", out) == 0 &&
          out[0] == 0x0a && out[1] == 0x1b && out[2] == 0x2c &&
          out[3] == 0x3d && out[4] == 0x4e && out[5] == 0x5f);

    check("parse lowercase",
          parse_colon_bytes("a0:b1:c2:d3:e4:f5", out) == 0 &&
          out[0] == 0xa0 && out[1] == 0xb1 && out[2] == 0xc2 &&
          out[3] == 0xd3 && out[4] == 0xe4 && out[5] == 0xf5);

    check("parse invalid", parse_colon_bytes("not:valid", out) != 0);
    check("parse NULL", parse_colon_bytes(NULL, out) != 0);
    check("parse bad val", parse_colon_bytes("AA:BB:1FF:DD:EE:FF", out) != 0);

    /* Test roundtrip: bytes -> str2ba -> ba2str -> bytes */
    {
        uint8_t orig[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
        bdaddr_t ba;
        memcpy(&ba, orig, 6);
        ba2str(&ba, txt);
        fprintf(stderr, "DEBUG: roundtrip txt=%s\n", txt);

        bdaddr_t ba2;
        str2ba(txt, &ba2);
        uint8_t out2[6];
        memcpy(out2, &ba2, 6);
        check("roundtrip bytes",
              memcmp(orig, out2, 6) == 0);
    }

    /* Try get_local_bdaddr_text if hci0 is available */
    if (get_local_bdaddr_text(0, txt, sizeof(txt)) == 0) {
        fprintf(stderr, "PASS: local BDADDR = %s\n", txt);
        /* Verify it can be parsed back */
        bdaddr_t ba;
        check("local str2ba", str2ba(txt, &ba) >= 0);
    } else {
        fprintf(stderr, "SKIP: no hci0 device available (expected in CI without BT adapter)\n");
    }

    if (failures) {
        fprintf(stderr, "\n%d tests FAILED\n", failures);
        return 1;
    }
    fprintf(stderr, "\nAll tests passed!\n");
    return 0;
}
