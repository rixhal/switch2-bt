/* usb_inspect2.c — USB Subcommand Inspector (filtered)
 *
 * Sends subcommands and shows only non-input responses.
 * Input reports start with 0x09, subcommand ACKs start with 0x21-0x2F.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void hex(const uint8_t *d, int len) {
    for (int i = 0; i < len; i++) {
        if (i % 16 == 0) fprintf(stderr, "\n  %04x:", i);
        fprintf(stderr, " %02x", d[i]);
    }
    fprintf(stderr, "\n");
}

static int send_cmd(int fd, uint8_t subcmd, const uint8_t *data,
                    uint8_t dlen, uint8_t counter) {
    uint8_t buf[64];
    memset(buf, 0, 64);
    buf[0] = 0x01;
    buf[1] = counter & 0x0F;
    buf[10] = subcmd;
    if (data && dlen > 0 && dlen <= 53)
        memcpy(buf + 11, data, dlen);
    write(fd, buf, 64);
    fprintf(stderr, "\n>>> SEND subcmd=0x%02x counter=%u\n", subcmd, counter);
    return 0;
}

int main(int argc, char **argv) {
    const char *dev = (argc > 1) ? argv[1] : "/dev/hidraw0";
    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror(dev); return 1; }
    fprintf(stderr, "Opened %s\n", dev);

    /* Drain all pending input reports (0x09) at max speed */
    fprintf(stderr, "Draining input reports (max 2s)...\n");
    uint8_t buf[64];
    int drained = 0;
    time_t deadline = time(NULL) + 2;
    while (time(NULL) < deadline) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, 50) <= 0) { usleep(10000); continue; }
        if (read(fd, buf, 64) <= 0) break;
        if (buf[0] == 0x09) drained++;
    }
    fprintf(stderr, "Drained %d input reports (%lds)\n", drained, (long)(time(NULL)-deadline+2));

    uint8_t counter = 0;

    /* 1. Request Device Info (subcmd 0x02) */
    send_cmd(fd, 0x02, NULL, 0, counter++);
    for (int i = 0; i < 20; i++) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, 300) <= 0) break;
        int n = (int)read(fd, buf, 64);
        if (n <= 0) break;
        if (buf[0] != 0x09) { /* not an input report */
            fprintf(stderr, "  [RESP #%d] type=0x%02x", i+1, buf[0]);
            hex(buf, n);
        }
    }

    /* 2. SPI Flash Read at bond storage area */
    fprintf(stderr, "\n=== SPI Flash Read 0x00060000 len 0x20 ===\n");
    {
        uint32_t addr = 0x00060000;
        uint8_t d[5] = {addr&0xFF, (addr>>8)&0xFF, (addr>>16)&0xFF, (addr>>24)&0xFF, 0x20};
        send_cmd(fd, 0x10, d, 5, counter++);
        for (int i = 0; i < 30; i++) {
            struct pollfd pfd = {.fd = fd, .events = POLLIN};
            if (poll(&pfd, 1, 500) <= 0) break;
            int n = (int)read(fd, buf, 64);
            if (n <= 0) break;
            if (buf[0] != 0x09) {
                fprintf(stderr, "  [SPI #%d] type=0x%02x", i+1, buf[0]);
                hex(buf, n);
            }
        }
    }

    /* 3. Try factory reset subcmd (0x48) — undocumented, may not work */
    fprintf(stderr, "\n=== Reset subcmd 0x48 (may be ignored) ===\n");
    send_cmd(fd, 0x48, NULL, 0, counter++);
    for (int i = 0; i < 10; i++) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, 500) <= 0) break;
        int n = (int)read(fd, buf, 64);
        if (n <= 0) break;
        if (buf[0] != 0x09) {
            fprintf(stderr, "  [RST #%d] type=0x%02x", i+1, buf[0]);
            hex(buf, n);
        }
    }

    close(fd);
    return 0;
}
