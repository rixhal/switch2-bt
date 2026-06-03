/* test_adv_parse.c - Test scan_for_peer parser with synthetic LE Advertising Report bytes */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

/*
 * This test constructs a synthetic LE Advertising Report event
 * and verifies the parsing logic that scan_for_peer() uses.
 */

#define HCI_EVENT_PKT    0x04
#define EVT_LE_META      0x3e
#define EVT_LE_ADVERTISING_REPORT 0x02

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

/*
 * Build a synthetic HCI LE Advertising Report event:
 *   buf[0] = HCI_EVENT_PKT
 *   buf[1] = EVT_LE_META
 *   buf[2] = param_len
 *   buf[3] = EVT_LE_ADVERTISING_REPORT (subevent)
 *   buf[4] = num_reports
 *   buf[5+] = report(s)
 *
 * Report format (per BT spec):
 *   [0]   = event_type
 *   [1]   = address_type
 *   [2-7] = BDADDR (6 bytes)
 *   [8]   = data_length
 *   [9+]  = data
 */
int main(void)
{
    uint8_t buf[64];
    
    /* Synthetic report: random address DE:AD:BE:EF:CA:FE, type=0x03 (connectable undirected)
       addr_type=0x01 (random), with some advertising data */
    memset(buf, 0, sizeof(buf));
    buf[0] = HCI_EVENT_PKT;            /* pkt_type */
    buf[1] = EVT_LE_META;              /* event */
    /* param_len will go at buf[2] */
    
    uint8_t *ev = buf + 3;
    ev[0] = EVT_LE_ADVERTISING_REPORT; /* subevent */
    ev[1] = 1;                         /* num_reports */
    
    uint8_t *rep = ev + 2;
    rep[0] = 0x03;                     /* event_type: ADV_IND (connectable undirected) */
    rep[1] = 0x01;                     /* address_type: random */
    rep[2] = 0xDE; rep[3] = 0xAD; rep[4] = 0xBE; /* BDADDR */
    rep[5] = 0xEF; rep[6] = 0xCA; rep[7] = 0xFE;
    rep[8] = 5;                        /* data_length */
    /* Advertising data: flags + partial name */
    rep[9]  = 0x02;                    /* len=2 */
    rep[10] = 0x01;                    /* type=flags */
    rep[11] = 0x06;                    /* LE General Discoverable + BR/EDR Not Supported */
    rep[12] = 0x04;                    /* len=4 */
    rep[13] = 0x08;                    /* type=shortened local name */
    rep[14] = 'S';                     /* "SW2 " */
    rep[15] = 'W';
    rep[16] = '2';

    uint8_t param_len = (uint8_t)(2 + 9 + 7); /* subevent(1) + num(1) + report(9+7) */
    buf[2] = param_len;

    /* Now simulate the parsing logic from scan_for_peer() */
    int found = 0;
    uint8_t found_type = 0;
    char found_addr[18] = {0};

    int plen = param_len;
    uint8_t sub = ev[0];
    check("subevent", sub == EVT_LE_ADVERTISING_REPORT);

    uint8_t num_reports = ev[1];
    check("num_reports", num_reports == 1);

    for (int r = 0; r < num_reports && (rep - ev) < plen; r++) {
        uint8_t addr_type = rep[1];

        bdaddr_t rba;
        memcpy(&rba, rep + 2, 6);
        char rba_str[18];
        ba2str(&rba, rba_str);

        uint8_t data_len = rep[8];

        fprintf(stderr, "  adv: %s type=%u data_len=%u\n", rba_str, addr_type, data_len);

        if (!found) {
            found_type = addr_type;
            snprintf(found_addr, sizeof(found_addr), "%s", rba_str);
            found = 1;
        }

        rep += 9 + data_len;
    }

    check("found", found == 1);
    check("addr_type_random", found_type == 0x01);
    check("addr_value", strcmp(found_addr, "FE:CA:EF:BE:AD:DE") == 0);

    /* Test with a second synthetic report for a public address */
    memset(buf, 0, sizeof(buf));
    buf[0] = HCI_EVENT_PKT;
    buf[1] = EVT_LE_META;

    ev = buf + 3;
    ev[0] = EVT_LE_ADVERTISING_REPORT;
    ev[1] = 1;

    rep = ev + 2;
    rep[0] = 0x04;
    rep[1] = 0x00;
    rep[2] = 0x11; rep[3] = 0x22; rep[4] = 0x33;
    rep[5] = 0x44; rep[6] = 0x55; rep[7] = 0x66;
    rep[8] = 3;
    rep[9] = 0x02; rep[10] = 0x0a; rep[11] = 0xff;

    buf[2] = (uint8_t)(2 + 9 + 3);

    found = 0;
    sub = ev[0];
    num_reports = ev[1];
    plen = buf[2];

    for (int r = 0; r < num_reports && (rep - ev) < plen; r++) {
        uint8_t addr_type = rep[1];
        bdaddr_t rba;
        memcpy(&rba, rep + 2, 6);
        char rba_str[18];
        ba2str(&rba, rba_str);

        if (!found) {
            found_type = addr_type;
            snprintf(found_addr, sizeof(found_addr), "%s", rba_str);
            found = 1;
        }

        rep += 9 + rep[8];
    }

    check("public_addr_type", found_type == 0x00);
    check("public_addr_value", strcmp(found_addr, "66:55:44:33:22:11") == 0);

    if (failures) {
        fprintf(stderr, "\n%d tests FAILED\n", failures);
        return 1;
    }
    fprintf(stderr, "\nAll tests passed!\n");
    return 0;
}
