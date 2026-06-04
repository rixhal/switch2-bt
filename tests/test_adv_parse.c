/* test_adv_parse.c - Test Switch 2 Pro LE Advertising Report parsing */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <bluetooth/bluetooth.h>

#define HCI_EVENT_PKT             0x04
#define EVT_LE_META               0x3e
#define EVT_LE_ADVERTISING_REPORT 0x02

static int failures = 0;

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void check(const char *name, int condition)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    } else {
        fprintf(stderr, "PASS: %s\n", name);
    }
}

static int adv_data_has_switch2_pro(const uint8_t *data, uint8_t dlen)
{
    for (int di = 0; di + 2 < dlen;) {
        uint8_t el = data[di];
        uint8_t ty = data[di + 1];
        if (!el || di + 1 + (int)el > dlen) break;
        if (ty == 0xff && el >= 7) {
            uint16_t cid = get_le16(data + di + 2);
            uint16_t vid = get_le16(data + di + 4);
            uint16_t pid = get_le16(data + di + 6);
            if (cid == 0x0553 && vid == 0x057e && pid == 0x2069)
                return 1;
            if (cid == 0x0553 && el >= 10) {
                vid = get_le16(data + di + 7);
                pid = get_le16(data + di + 9);
                if (vid == 0x057e && pid == 0x2069)
                    return 1;
            }
        }
        di += 1 + el;
    }
    return 0;
}

static int parse_one_report(const uint8_t *ev, uint8_t plen,
                            uint8_t out_addr[6], uint8_t *out_type)
{
    if (plen < 3 || ev[0] != EVT_LE_ADVERTISING_REPORT) return 0;

    uint8_t nrep = ev[1];
    size_t off = 2;
    for (int ri = 0; ri < nrep && off + 10 <= plen; ri++) {
        const uint8_t *rep = ev + off;
        uint8_t dlen = rep[8];
        if (off + 10 + dlen > plen) break;

        if (adv_data_has_switch2_pro(rep + 9, dlen)) {
            *out_type = rep[1];
            memcpy(out_addr, rep + 2, 6);
            return 1;
        }
        off += 10 + dlen;
    }
    return 0;
}

static void build_report(uint8_t *buf, uint8_t addr_type,
                         const uint8_t addr[6],
                         const uint8_t *adv, uint8_t adv_len)
{
    memset(buf, 0, 128);
    buf[0] = HCI_EVENT_PKT;
    buf[1] = EVT_LE_META;

    uint8_t *ev = buf + 3;
    ev[0] = EVT_LE_ADVERTISING_REPORT;
    ev[1] = 1;

    uint8_t *rep = ev + 2;
    rep[0] = 0x00;       /* ADV_IND */
    rep[1] = addr_type;
    memcpy(rep + 2, addr, 6);
    rep[8] = adv_len;
    memcpy(rep + 9, adv, adv_len);
    rep[9 + adv_len] = 0xc3; /* RSSI = -61 */

    buf[2] = (uint8_t)(2 + 10 + adv_len);
}

int main(void)
{
    uint8_t buf[128], found[6], found_type = 0;
    char addr_txt[18];

    const uint8_t addr[6] = {0x76, 0xc6, 0x3b, 0xbf, 0xef, 0xe0};
    const uint8_t phase_a_mfg[] = {
        0x01, 0x00, 0x03, 0x7e, 0x05, 0x69, 0x20, 0x00,
        0x01, 0x00, 0x31, 0xf8, 0x94, 0x55, 0xe2, 0x98,
        0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    uint8_t adv[40];
    adv[0] = 0x02; adv[1] = 0x01; adv[2] = 0x06;
    adv[3] = (uint8_t)(sizeof(phase_a_mfg) + 3);
    adv[4] = 0xff;
    adv[5] = 0x53; adv[6] = 0x05;
    memcpy(adv + 7, phase_a_mfg, sizeof(phase_a_mfg));

    check("switch2 payload match",
          adv_data_has_switch2_pro(adv, (uint8_t)(7 + sizeof(phase_a_mfg))) == 1);

    build_report(buf, 0x01, addr, adv, (uint8_t)(7 + sizeof(phase_a_mfg)));
    check("parse switch2 report",
          parse_one_report(buf + 3, buf[2], found, &found_type) == 1);
    ba2str((bdaddr_t *)found, addr_txt);
    check("addr type random", found_type == 0x01);
    check("addr value", strcmp(addr_txt, "E0:EF:BF:3B:C6:76") == 0);

    /* Same Nintendo manufacturer and VID, wrong PID must not match. */
    adv[12] = 0x66;
    adv[13] = 0x20;
    check("wrong pid rejected",
          adv_data_has_switch2_pro(adv, (uint8_t)(7 + sizeof(phase_a_mfg))) == 0);

    if (failures) {
        fprintf(stderr, "\n%d tests FAILED\n", failures);
        return 1;
    }
    fprintf(stderr, "\nAll tests passed!\n");
    return 0;
}
