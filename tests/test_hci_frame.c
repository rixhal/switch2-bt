#include "../hci_transport_user.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

static void check(const char *name, int cond) {
    if (cond) {
        printf("PASS: %s\n", name);
    } else {
        printf("FAIL: %s\n", name);
        failures++;
    }
}

static int feed_and_read(const uint8_t *pkt, size_t len, hci_user_frame_t *f) {
    int p[2];
    if (pipe(p) < 0) return -1;
    if (write(p[1], pkt, len) != (ssize_t)len) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    close(p[1]);
    int rc = hci_user_read_frame(p[0], f, 100);
    close(p[0]);
    return rc;
}

int main(void) {
    hci_user_frame_t f;

    const uint8_t enc_change[] = {
        0x04, 0x08, 0x04,             /* HCI event: Encryption Change */
        0x00,                         /* status */
        0x40, 0x00,                   /* handle */
        0x01                          /* encryption enabled */
    };
    memset(&f, 0, sizeof(f));
    check("read enc_change", feed_and_read(enc_change, sizeof(enc_change), &f) == 1);
    check("enc pkt type", f.pkt_type == HCI_EVENT_PKT);
    check("enc event code", f.evt_code == EVT_ENCRYPT_CHANGE);
    check("enc status offset", f.status == 0x00);
    check("enc handle offset", f.handle == 0x0040);
    check("enc payload stable", f.payload == f.raw + 3 && f.payload[0] == 0x00);

    const uint8_t ltk_req[] = {
        0x04, 0x3e, 0x0d,             /* HCI LE Meta event */
        0x05,                         /* LE Long Term Key Request */
        0x40, 0x00,                   /* handle */
        0, 1, 2, 3, 4, 5, 6, 7,       /* random number */
        0x34, 0x12                    /* EDIV */
    };
    memset(&f, 0, sizeof(f));
    check("read ltk_req", feed_and_read(ltk_req, sizeof(ltk_req), &f) == 1);
    check("ltk event code", f.evt_code == EVT_LE_META);
    check("ltk subevent", f.subevent == 0x05);
    check("ltk handle payload offset", f.payload == f.raw + 3 && f.payload[1] == 0x40);

    const uint8_t acl_smp[] = {
        0x02,                         /* ACL packet */
        0x40, 0x20,                   /* handle 0x40, PB=2 */
        0x07, 0x00,                   /* ACL length */
        0x03, 0x00,                   /* L2CAP payload length */
        0x06, 0x00,                   /* SMP CID */
        0x0b, 0x01, 0x00              /* SMP payload */
    };
    memset(&f, 0, sizeof(f));
    check("read acl_smp", feed_and_read(acl_smp, sizeof(acl_smp), &f) == 1);
    check("acl pkt type", f.pkt_type == HCI_ACLDATA_PKT);
    check("acl handle", f.handle == 0x0040);
    check("acl cid", f.cid == 0x0006);
    check("acl payload stable", f.payload == f.raw + 9 && f.payload[0] == 0x0b);

    if (failures) {
        printf("\n%d tests FAILED\n", failures);
        return 1;
    }
    printf("\nAll tests passed!\n");
    return 0;
}
