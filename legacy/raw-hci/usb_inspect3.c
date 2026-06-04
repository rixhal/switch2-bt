/* usb_inspect3.c — Show first reports after each subcommand */
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
    for (int i = 0; i < len && i < 32; i++) {
        if (i % 16 == 0) fprintf(stderr, "\n    ");
        fprintf(stderr, "%02x ", d[i]);
    }
    fprintf(stderr, "\n");
}

static int send_cmd(int fd, uint8_t subcmd, const uint8_t *data,
                    uint8_t dlen, uint8_t counter) {
    uint8_t buf[64];
    memset(buf, 0, 64);
    buf[0] = 0x01; buf[1] = counter & 0x0F; buf[10] = subcmd;
    if (data && dlen > 0 && dlen <= 53)
        memcpy(buf + 11, data, dlen);
    write(fd, buf, 64);
    fprintf(stderr, "\n>>> SEND subcmd=0x%02x counter=%u\n", subcmd, counter);
    hex(buf, 64);
    return 0;
}

int main(int argc, char **argv) {
    const char *dev = (argc > 1) ? argv[1] : "/dev/hidraw0";
    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror(dev); return 1; }

    /* Quick drain (500ms) */
    fprintf(stderr, "Quick drain...\n");
    uint8_t buf[64];
    time_t dl = time(NULL) + 1;
    while (time(NULL) < dl) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, 20) <= 0) continue;
        read(fd, buf, 64);
    }

    uint8_t counter = 0;

    /* 1. Request Device Info */
    send_cmd(fd, 0x02, NULL, 0, counter++);
    fprintf(stderr, "  Waiting for responses...\n");
    for (int i = 0; i < 8; i++) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, 300) <= 0) break;
        int n = (int)read(fd, buf, 64);
        if (n > 0) {
            fprintf(stderr, "  [#%d] id=0x%02x subcmd_reply=%02x%02x",
                    i, buf[0], buf[13], buf[14]);
            hex(buf, n);
        }
    }

    /* 2. SetHCIState enable */
    {
        uint8_t d = 0x01;
        send_cmd(fd, 0x06, &d, 1, counter++);
        fprintf(stderr, "  Waiting for responses...\n");
        for (int i = 0; i < 8; i++) {
            struct pollfd pfd = {.fd = fd, .events = POLLIN};
            if (poll(&pfd, 1, 300) <= 0) break;
            int n = (int)read(fd, buf, 64);
            if (n > 0) {
                fprintf(stderr, "  [#%d] id=0x%02x subcmd_reply=%02x%02x",
                        i, buf[0], buf[13], buf[14]);
                hex(buf, n);
            }
        }
    }

    /* 3. BluetoothManualPair */
    {
        uint8_t d[7] = {0x01, 0xD8, 0x3A, 0xDD, 0xE5, 0x69, 0xED};
        send_cmd(fd, 0x01, d, 7, counter++);
        fprintf(stderr, "  Waiting for responses...\n");
        for (int i = 0; i < 8; i++) {
            struct pollfd pfd = {.fd = fd, .events = POLLIN};
            if (poll(&pfd, 1, 300) <= 0) break;
            int n = (int)read(fd, buf, 64);
            if (n > 0) {
                fprintf(stderr, "  [#%d] id=0x%02x subcmd_reply=%02x%02x",
                        i, buf[0], buf[13], buf[14]);
                hex(buf, n);
            }
        }
    }

    close(fd);
    return 0;
}
