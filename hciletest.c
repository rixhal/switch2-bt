/* hciletest.c - Test using libbluetooth for LE connection */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

int main() {
    int dd = hci_open_dev(0);
    if (dd < 0) { perror("hci_open_dev"); return 1; }
    printf("Opened hci0 (fd=%d)\n", dd);
    
    bdaddr_t peer;
    str2ba("E0:EF:BF:3B:C6:76", &peer);
    
    printf("Creating LE connection to E0:EF:BF:3B:C6:76...\n");
    uint16_t handle = 0;
    int rc = hci_le_create_conn(dd, 0x0004, 0x0004, 0x00,
                                LE_PUBLIC_ADDRESS, peer, LE_PUBLIC_ADDRESS,
                                0x000F, 0x000F, 0x0000, 0x0C80, 0x0001, 0x0001,
                                &handle, 6000);
    if (rc < 0) {
        fprintf(stderr, "hci_le_create_conn failed: %s (%d)\n", strerror(-rc), -rc);
        close(dd);
        return 1;
    }
    printf("Connected! handle=0x%04X\n", handle);
    
    // Now send ACL ATT MTU request
    printf("Sending ATT MTU request...\n");
    uint16_t hf = (handle & 0x0FFF) | (2 << 12);
    uint8_t pkt[32];
    pkt[0] = HCI_ACLDATA_PKT;
    pkt[1] = hf & 0xFF; pkt[2] = (hf >> 8) & 0xFF;
    pkt[3] = 7; pkt[4] = 0; // ACL len = 7
    pkt[5] = 3; pkt[6] = 0; // L2CAP len = 3  
    pkt[7] = 4; pkt[8] = 0; // CID = ATT (0x0004)
    pkt[9] = 0x02; pkt[10] = 0xFF; pkt[11] = 0x02; // ATT MTU req
    
    int w = write(dd, pkt, 12);
    printf("Wrote %d bytes\n", w);
    
    // Read response
    printf("Reading response...\n");
    uint8_t buf[260];
    int n = read(dd, buf, 260);
    if (n > 0) {
        printf("Got %d bytes: type=%02X ", n, buf[0]);
        for (int i = 1; i < n && i < 16; i++) printf("%02X ", buf[i]);
        printf("\n");
    } else {
        perror("read");
    }
    
    hci_disconnect(dd, handle, HCI_OE_USER_ENDED_CONNECTION, 1000);
    close(dd);
    return 0;
}
