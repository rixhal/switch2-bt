#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <linux/uhid.h>

static volatile int running = 1;
static void on_sig(int s) { running = 0; }

static int parse_type(const char *s, uint8_t *out)
{
    if (!s) return -1;
    if (strcmp(s, "public") == 0) { *out = LE_PUBLIC_ADDRESS; return 0; }
    if (strcmp(s, "random") == 0) { *out = LE_RANDOM_ADDRESS; return 0; }
    return -1;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s BDADDR public|random\n", argv[0]);
        fprintf(stderr, "  example: %s AA:BB:CC:DD:EE:FF random\n", argv[0]);
        return 1;
    }

    const char *bdaddr_s = argv[1];
    const char *type_s = argv[2];
    uint8_t peer_type = LE_PUBLIC_ADDRESS;

    if (parse_type(type_s, &peer_type) < 0) {
        fprintf(stderr, "invalid type: %s (use public or random)\n", type_s);
        return 1;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    
    fprintf(stderr, "sw2d-lib: opening hci0\n");
    int dd = hci_open_dev(0);
    if (dd < 0) { perror("hci_open_dev"); return 1; }
    
    bdaddr_t peer;
    if (str2ba(bdaddr_s, &peer) < 0) {
        fprintf(stderr, "invalid BDADDR: %s\n", bdaddr_s);
        close(dd);
        return 1;
    }
    
    fprintf(stderr, "creating LE connection to %s (type=%s)\n", bdaddr_s, type_s);
    uint16_t handle = 0;
    int rc = hci_le_create_conn(dd, 0x0004, 0x0004, 0, peer_type, peer,
                                LE_PUBLIC_ADDRESS,
                                0x000F, 0x000F, 0, 0x0C80, 0x0001, 0x0001,
                                &handle, 6000);
    if (rc < 0) {
        fprintf(stderr, "le_create_conn: %s (%d)\n", strerror(-rc), -rc);
        close(dd); return 1;
    }
    fprintf(stderr, "connected handle=0x%04x\n", handle);
    
    // Send ATT MTU request via ACL
    uint16_t hf = (handle & 0x0FFF) | (2 << 12);
    uint8_t pkt[32] = {
        HCI_ACLDATA_PKT,
        hf & 0xFF, (hf >> 8) & 0xFF,
        7, 0,           // ACL len
        3, 0,           // L2CAP len
        4, 0,           // ATT CID
        0x02, 0xFF, 0x02 // ATT MTU req
    };
    write(dd, pkt, 12);
    fprintf(stderr, "MTU sent\n");
    
    // Read responses
    uint8_t buf[512];
    struct pollfd pfd = {dd, POLLIN, 0};
    for (int i = 0; i < 30 && running; i++) {
        if (poll(&pfd, 1, 100) > 0) {
            int n = read(dd, buf, 512);
            if (n > 0) {
                fprintf(stderr, "recv %d: %02X", n, buf[0]);
                for (int j = 1; j < n && j < 14; j++) fprintf(stderr, " %02X", buf[j]);
                fprintf(stderr, "\n");
            }
        }
    }
    
    close(dd);
    return 0;
}
