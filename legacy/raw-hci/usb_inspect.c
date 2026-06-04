/* usb_inspect.c — Switch 2 Pro Controller USB HID Inspector
 *
 * Reads USB HID input reports from the controller after sending
 * subcommands. Use this to inspect bond state, BDADDR, firmware,
 * and pairing configuration.
 *
 * Build: gcc -O2 -Wall -o usb_inspect usb_inspect.c
 * Run:   sudo ./usb_inspect [/dev/hidrawX]
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Known subcommand IDs (Switch 2 Pro Controller) */
#define SUBCMD_REQ_DEV_INFO      0x02
#define SUBCMD_SET_HCI_STATE     0x06
#define SUBCMD_SET_SHIPMENT      0x08
#define SUBCMD_SPI_FLASH_READ    0x10
#define SUBCMD_SET_NFC_IR_MCU    0x22
#define SUBCMD_RESET             0x48  /* Factory reset (rumored) */

static void hexdump(const char *label, const uint8_t *d, int len) {
    fprintf(stderr, "%s (%d bytes):\n", label, len);
    for (int i = 0; i < len; i++) {
        if (i % 16 == 0) fprintf(stderr, "  %04x:", i);
        fprintf(stderr, " %02x", d[i]);
        if (i % 16 == 15 || i == len - 1) fprintf(stderr, "\n");
    }
}

/* Send a subcommand and read response(s) */
static int send_subcmd(int fd, uint8_t subcmd, const uint8_t *data,
                       uint8_t dlen, uint8_t counter) {
    uint8_t buf[64];
    memset(buf, 0, 64);
    buf[0] = 0x01;            /* Report ID */
    buf[1] = counter & 0x0F;  /* Subcommand counter (nibble) */
    buf[10] = subcmd;         /* Subcommand ID */
    if (data && dlen > 0 && dlen <= 53)
        memcpy(buf + 11, data, dlen);

    ssize_t n = write(fd, buf, 64);
    if (n < 0) {
        perror("write subcmd");
        return -1;
    }
    fprintf(stderr, "\n>>> SEND subcmd=0x%02x counter=%u data_len=%u\n",
            subcmd, counter, dlen);
    hexdump("  OUT", buf, 64);
    return 0;
}

/* Read one input report with timeout */
static int read_report(int fd, uint8_t *buf, int timeout_ms) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) { perror("poll"); return -1; }
    if (pr == 0) return 0; /* timeout */

    ssize_t n = read(fd, buf, 64);
    if (n < 0) { perror("read"); return -1; }
    return (int)n;
}

int main(int argc, char **argv) {
    const char *dev = (argc > 1) ? argv[1] : "/dev/hidraw0";

    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s. Try: sudo %s /dev/hidraw0\n",
                dev, argv[0]);
        /* List available hidraw devices */
        fprintf(stderr, "\nAvailable hidraw devices:\n");
        system("ls -la /dev/hidraw* 2>/dev/null || echo '  (none)'");
        return 1;
    }
    fprintf(stderr, "Opened %s (fd=%d)\n\n", dev, fd);

    /* Drain any pending input reports */
    fprintf(stderr, "=== Draining pending reports ===\n");
    {
        uint8_t buf[64];
        int n;
        while ((n = read_report(fd, buf, 100)) > 0) {
            hexdump("PRE-EXISTING", buf, n);
        }
    }

    uint8_t counter = 0;
    uint8_t inp[64];

    /* ── 1. Request Device Info ───────────────────────────────── */
    fprintf(stderr, "\n=== 1. Request Device Info (0x%02x) ===\n",
            SUBCMD_REQ_DEV_INFO);
    send_subcmd(fd, SUBCMD_REQ_DEV_INFO, NULL, 0, counter++);
    for (int i = 0; i < 5; i++) {
        int n = read_report(fd, inp, 500);
        if (n <= 0) break;
        fprintf(stderr, "  [IN #%d] ", i + 1);
        hexdump("", inp, n);
    }

    /* ── 2. SPI Flash Read — read bond data ───────────────────── */
    /* SPI Flash Read: subcmd 0x10, address is 4 bytes LE starting at byte 11
     * Bond data is typically at offset 0x00060000 (Switch 1).
     * Switch 2 may differ. Try a few known addresses. */
    fprintf(stderr, "\n=== 2. SPI Flash Read (0x%02x) ===\n",
            SUBCMD_SPI_FLASH_READ);

    /* Try reading at offset 0x00060000, length 0x20 (Switch 1 bond area) */
    {
        uint32_t addr = 0x00060000;
        uint8_t spidata[5];
        spidata[0] = (uint8_t)(addr & 0xFF);
        spidata[1] = (uint8_t)((addr >> 8) & 0xFF);
        spidata[2] = (uint8_t)((addr >> 16) & 0xFF);
        spidata[3] = (uint8_t)((addr >> 24) & 0xFF);
        spidata[4] = 0x20; /* read length */
        send_subcmd(fd, SUBCMD_SPI_FLASH_READ, spidata, 5, counter++);
        for (int i = 0; i < 5; i++) {
            int n = read_report(fd, inp, 500);
            if (n <= 0) break;
            fprintf(stderr, "  [IN #%d] ", i + 1);
            hexdump("", inp, n);
        }
    }

    /* ── 3. SetHCIState — query current state ─────────────────── */
    fprintf(stderr, "\n=== 3. SetHCIState (0x%02x) — query ===\n",
            SUBCMD_SET_HCI_STATE);
    {
        uint8_t hci = 0x00; /* 0x00 = query? 0x01 = enable */
        send_subcmd(fd, SUBCMD_SET_HCI_STATE, &hci, 1, counter++);
        for (int i = 0; i < 5; i++) {
            int n = read_report(fd, inp, 500);
            if (n <= 0) break;
            fprintf(stderr, "  [IN #%d] ", i + 1);
            hexdump("", inp, n);
        }
    }

    /* ── 4. Enable HCI state (if it was off) ──────────────────── */
    fprintf(stderr, "\n=== 4. SetHCIState — ENABLE ===\n");
    {
        uint8_t hci = 0x01;
        send_subcmd(fd, SUBCMD_SET_HCI_STATE, &hci, 1, counter++);
        for (int i = 0; i < 5; i++) {
            int n = read_report(fd, inp, 500);
            if (n <= 0) break;
            fprintf(stderr, "  [IN #%d] ", i + 1);
            hexdump("", inp, n);
        }
    }

    /* ── 5. Drain any remaining reports ───────────────────────── */
    fprintf(stderr, "\n=== Draining final reports ===\n");
    {
        uint8_t buf[64];
        int n;
        while ((n = read_report(fd, buf, 200)) > 0) {
            hexdump("FINAL", buf, n);
        }
    }

    close(fd);
    fprintf(stderr, "\nDone.\n");
    return 0;
}
