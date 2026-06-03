/* acltest.c - hcitool lecc + manual ACL send test */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <poll.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_HCI 1
#define SOL_HCI 0
#define HCI_FILTER 2

int main() {
    // Step 1: Run hcitool lecc
    printf("Running hcitool lecc...\n");
    FILE *fp = popen("hcitool lecc E0:EF:BF:3B:C6:76 2>&1", "r");
    char line[64];
    uint16_t handle = 0;
    while (fgets(line, sizeof(line), fp)) {
        printf("  %s", line);
        unsigned int h;
        if (sscanf(line, "Connection handle %u", &h) == 1)
            handle = h;
    }
    pclose(fp);
    printf("Handle: 0x%04X\n", handle);
    if (!handle) { printf("No connection\n"); return 1; }

    // Step 2: Open HCI socket for ACL
    struct sockaddr_hci {
        sa_family_t hci_family;
        unsigned short hci_dev;
        unsigned short hci_channel;
    } addr = {AF_BLUETOOTH, 0, 0};
    
    int s = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
    if (s < 0) { perror("socket"); return 1; }
    bind(s, (struct sockaddr*)&addr, sizeof(addr));
    
    // Set filter for ACL + events
    uint8_t flt[16] = {0};
    flt[0] = (1 << 2) | (1 << 4); // ACL data + events
    flt[10] = 0x00; flt[11] = 0x40; // LE Meta
    setsockopt(s, SOL_HCI, HCI_FILTER, flt, 16);
    printf("Filter set\n");

    // Step 3: Send MTU request via ACL
    printf("Sending MTU request...\n");
    uint16_t hf = (handle & 0x0FFF) | (2 << 12); // PB=first auto-flushable
    uint8_t pkt[32];
    pkt[0] = 0x02; // ACL
    pkt[1] = hf & 0xFF; pkt[2] = (hf >> 8) & 0xFF;
    pkt[3] = 7; pkt[4] = 0; // ACL len = 3 + 4 = 7
    pkt[5] = 3; pkt[6] = 0; // L2CAP len = 3
    pkt[7] = 4; pkt[8] = 0; // CID = ATT (0x0004)
    pkt[9] = 0x02; pkt[10] = 0xFF; pkt[11] = 0x02; // ATT MTU req (op=2, mtu=767)
    
    int w = write(s, pkt, 12);
    printf("Wrote %d bytes: %s\n", w, w < 0 ? strerror(errno) : "OK");
    
    // Step 4: Try to read response
    printf("Reading response...\n");
    struct pollfd pfd = {s, POLLIN, 0};
    for (int i = 0; i < 30; i++) {
        int r = poll(&pfd, 1, 100);
        if (r <= 0) continue;
        
        uint8_t pt;
        r = read(s, &pt, 1);
        if (r <= 0) { perror("read type"); continue; }
        
        uint8_t buf[260];
        r = read(s, buf, 260);
        if (r <= 0) { perror("read data"); continue; }
        
        printf("Got type=0x%02X len=%d: ", pt, r);
        for (int j = 0; j < r && j < 12; j++) printf("%02X ", buf[j]);
        printf("\n");
        
        if (pt == 0x02) { // ACL data
            if (r >= 4 && buf[2]==4 && buf[3]==0) { // ATT CID
                printf("  ATT: op=0x%02X\n", buf[4]);
                break;
            }
        }
    }
    
    close(s);
    return 0;
}
