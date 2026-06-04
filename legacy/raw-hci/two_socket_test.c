/*
 * two_socket_test.c — Minimal proof that dual HCI sockets work on Pi5/LE
 *
 * Linux kernel hci_send_to_sock() does NOT dispatch events back to the
 * socket that sent the triggering command (skb→sk == sk → continue).
 * Therefore we need separate cmd_fd (send only) and evt_fd (receive only).
 *
 * Build: make two_socket_test
 * Usage: ./two_socket_test [BDADDR] [public|random]
 */

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/uio.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

/* ── helpers ────────────────────────────────────────────────────────── */

static void put_le16(void *dst, uint16_t val) {
    uint8_t *p = (uint8_t *)dst;
    p[0] = (uint8_t)(val);
    p[1] = (uint8_t)(val >> 8);
}

static uint16_t get_le16(const void *src) {
    const uint8_t *p = (const uint8_t *)src;
    return (uint16_t)(p[0] | (p[1] << 8));
}

static const char *addr_type_str(uint8_t t) {
    switch (t) {
        case 0x00: return "public";
        case 0x01: return "random";
        default:   return "?";
    }
}

/* ── event filter setup ─────────────────────────────────────────────── */

static int setup_event_filter(int fd) {
    struct hci_filter nf;

    /* Accept ALL events for debugging */
    hci_filter_clear(&nf);
    hci_filter_set_ptype(HCI_EVENT_PKT, &nf);
    nf.event_mask[0] = 0xFFFFFFFF;
    nf.event_mask[1] = 0xFFFFFFFF;

    if (setsockopt(fd, SOL_HCI, HCI_FILTER, &nf, sizeof(nf)) < 0) {
        perror("setsockopt HCI_FILTER on evt_fd");
        return -1;
    }

    /* Read back to verify */
    socklen_t len = sizeof(nf);
    struct hci_filter vf;
    memset(&vf, 0, sizeof(vf));
    if (getsockopt(fd, SOL_HCI, HCI_FILTER, &vf, &len) < 0)
        perror("getsockopt HCI_FILTER");
    else
        fprintf(stderr, "evt_fd filter VERIFY: type=0x%08x evt0=0x%08x evt1=0x%08x\n",
                vf.type_mask, vf.event_mask[0], vf.event_mask[1]);

    return 0;
}

/* ── open raw HCI sockets ───────────────────────────────────────────── */

static int open_cmd_socket(void) {
    int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_NONBLOCK, BTPROTO_HCI);
    if (fd < 0) { perror("cmd socket"); return -1; }

    struct sockaddr_hci addr;
    memset(&addr, 0, sizeof(addr));
    addr.hci_family = AF_BLUETOOTH;
    addr.hci_dev = 0;                /* hci0 */
    addr.hci_channel = HCI_CHANNEL_RAW;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind cmd_fd");
        close(fd);
        return -1;
    }
    return fd;
}

static int open_evt_socket(void) {
    int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_NONBLOCK, BTPROTO_HCI);
    if (fd < 0) { perror("evt socket"); return -1; }

    struct sockaddr_hci addr;
    memset(&addr, 0, sizeof(addr));
    addr.hci_family = AF_BLUETOOTH;
    addr.hci_dev = 0;
    addr.hci_channel = HCI_CHANNEL_RAW;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind evt_fd");
        close(fd);
        return -1;
    }

    if (setup_event_filter(fd) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ── send raw HCI command ───────────────────────────────────────────── */

static int send_cmd(int fd, uint16_t ogf, uint16_t ocf,
                    const uint8_t *params, uint8_t plen) {
    uint8_t pkt_type = HCI_COMMAND_PKT;
    uint16_t opcode = (uint16_t)((ocf & 0x03ff) | (ogf << 10));
    uint8_t hdr[3] = { pkt_type, (uint8_t)opcode, (uint8_t)(opcode >> 8) };
    hdr[2] = (uint8_t)plen;  /* opcode high byte is actually part of 2-byte opcode */

    /* Rebuild: opcode is 2 bytes, then plen */
    uint8_t frame[4];
    frame[0] = HCI_COMMAND_PKT;
    put_le16(frame + 1, opcode);
    frame[3] = plen;

    struct iovec iov[2] = {
        { frame, 4 },
        { (void *)params, plen }
    };

    struct msghdr msg = {0};
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    ssize_t n = sendmsg(fd, &msg, MSG_NOSIGNAL);
    if (n < 0) { perror("sendmsg cmd"); return -1; }
    fprintf(stderr, "  sent cmd: ogf=0x%02x ocf=0x%04x (%zd bytes)\n",
            ogf, ocf, n);
    return 0;
}

/* ── read one HCI packet from event socket ──────────────────────────── */

static int read_evt(int fd, uint8_t *buf, int bufsz, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };

    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) { perror("poll evt_fd"); return -1; }
    if (pr == 0) { fprintf(stderr, "  poll timeout (%dms)\n", timeout_ms); return 0; }

    /* CRITICAL: HCI sockets are SOCK_RAW (datagram). Each read() consumes
     * the entire skb. Read the full frame in ONE call! */
    ssize_t n = read(fd, buf, bufsz);
    if (n < 0) { perror("read evt"); return -1; }
    if (n < 3) { fprintf(stderr, "  short frame: %zd bytes\n", n); return 0; }

    return (int)n;
}

/* ── LE Create Connection + wait for complete ────────────────────────── */

static int le_create_conn(int cmd_fd, int evt_fd,
                          const bdaddr_t *peer, uint8_t peer_type,
                          uint8_t own_type) {
    uint8_t params[25];
    memset(params, 0, sizeof(params));

    /* LE Create Connection parameters (HCI spec 7.8.12) */
    put_le16(params + 0, 0x0010);  /* scan_interval (10ms) */
    put_le16(params + 2, 0x0010);  /* scan_window   (10ms) */
    params[4] = 0x00;              /* initiator_filter: peer addr */
    params[5] = peer_type;         /* peer address type */
    memcpy(params + 6, peer, 6);   /* peer BDADDR */
    params[12] = own_type;         /* own address type */
    put_le16(params + 13, 0x0028); /* conn_interval_min (50ms) */
    put_le16(params + 15, 0x0038); /* conn_interval_max (70ms) */
    put_le16(params + 17, 0x0000); /* conn_latency */
    put_le16(params + 19, 0x07D0); /* supervision_timeout (20s) */
    put_le16(params + 21, 0x0001); /* min_ce_length */
    put_le16(params + 23, 0x0001); /* max_ce_length */

    uint16_t opcode = (uint16_t)((OCF_LE_CREATE_CONN & 0x03ff) | (OGF_LE_CTL << 10));

    if (send_cmd(cmd_fd, OGF_LE_CTL, OCF_LE_CREATE_CONN, params, 25) < 0)
        return -1;

    /* Small delay to ensure kernel has processed any immediate responses */
    usleep(5000);  /* 5ms */
    int max_iters = 60; /* 6 seconds total */
    for (int i = 0; i < max_iters; i++) {
        if (i < 5) fprintf(stderr, "  iter %d: calling read_evt...\n", i);
        uint8_t buf[256];
        int n = read_evt(evt_fd, buf, sizeof(buf), 100);
        if (i < 5) fprintf(stderr, "  iter %d: read_evt returned %d\n", i, n);
        if (n <= 0) continue;

        /* Dump ALL received events */
        fprintf(stderr, "  EVENT: type=0x%02x code=0x%02x plen=%d data=",
                buf[0], buf[1], buf[2]);
        for (int j = 0; j < buf[2] && j < 16; j++)
            fprintf(stderr, "%02x", buf[3+j]);
        fprintf(stderr, "\n");

        /* Skip non-event packets */
        if (buf[0] != HCI_EVENT_PKT) continue;

        uint8_t evt = buf[1];
        uint8_t plen = buf[2];
        const uint8_t *ev = buf + 3;

        if (evt == EVT_CMD_STATUS && plen >= 4) {
            uint16_t resp_opcode = get_le16(ev + 1);
            if (resp_opcode == opcode) {
                uint8_t status = ev[0];
                fprintf(stderr, "  CMD_STATUS: opcode=0x%04x status=0x%02x\n",
                        resp_opcode, status);
                if (status != 0) {
                    fprintf(stderr, "  FAIL: CMD_STATUS error 0x%02x\n", status);
                    return -1;
                }
            }
            continue;
        }

        if (evt == EVT_LE_META_EVENT && plen >= 1) {
            uint8_t sub = ev[0];
            if (sub == EVT_LE_CONN_COMPLETE && plen >= 19) {
                uint8_t status = ev[1];
                uint16_t handle = get_le16(ev + 2);
                uint8_t role = ev[8];
                uint8_t pt = ev[9];
                bdaddr_t paddr;
                memcpy(&paddr, ev + 10, 6);
                char pstr[18];
                ba2str(&paddr, pstr);

                uint16_t interval = get_le16(ev + 16);
                uint16_t latency = get_le16(ev + 18);
                uint16_t timeout = get_le16(ev + 20);

                fprintf(stderr, "  CONN_COMPLETE: status=0x%02x handle=0x%04x role=%s\n",
                        status, handle, role ? "slave" : "central");
                fprintf(stderr, "    peer=%s type=%s interval=%.2fms latency=%u timeout=%ums\n",
                        pstr, addr_type_str(pt),
                        interval * 1.25, latency, timeout * 10);

                if (status == 0x00) return (int)handle;

                fprintf(stderr, "  FAIL: connection status=0x%02x\n", status);

                /* Wait for disconnect */
                for (int j = 0; j < 20; j++) {
                    int n2 = read_evt(evt_fd, buf, sizeof(buf), 100);
                    if (n2 > 0 && buf[1] == EVT_DISCONN_COMPLETE) {
                        uint8_t reason = buf[3 + 2];
                        fprintf(stderr, "  DISCONN: reason=0x%02x\n", reason);
                        break;
                    }
                }
                return -1;
            }
        }
    }

    fprintf(stderr, "  FAIL: timeout waiting for LE Connection Complete\n");
    return -1;
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    const char *bdaddr_s = "E0:EF:BF:3B:C6:76";
    uint8_t peer_type = 0x00; /* public */
    uint8_t own_type = 0x00;  /* public */

    if (argc >= 2) bdaddr_s = argv[1];
    if (argc >= 3) {
        if (strcmp(argv[2], "random") == 0) peer_type = 0x01;
    }

    bdaddr_t peer;
    if (str2ba(bdaddr_s, &peer) < 0) {
        fprintf(stderr, "invalid BDADDR: %s\n", bdaddr_s);
        return 2;
    }

    fprintf(stderr, "=== two_socket_test ===\n");
    fprintf(stderr, "peer=%s type=%s\n", bdaddr_s, addr_type_str(peer_type));

    /* Ensure hci0 is up */
    int dev_id = hci_devid("hci0");
    if (dev_id < 0) {
        fprintf(stderr, "no hci0 device\n");
        return 1;
    }

    /* Open sockets */
    fprintf(stderr, "opening cmd_fd...\n");
    int cmd_fd = open_cmd_socket();
    if (cmd_fd < 0) return 1;
    fprintf(stderr, "  cmd_fd=%d\n", cmd_fd);

    fprintf(stderr, "opening evt_fd...\n");
    int evt_fd = open_evt_socket();
    if (evt_fd < 0) { close(cmd_fd); return 1; }
    fprintf(stderr, "  evt_fd=%d\n", evt_fd);

    /* Verify hci0 is up */
    {
        bdaddr_t local;
        if (hci_devba(dev_id, &local) < 0) {
            fprintf(stderr, "hci0 not ready (hci_devba failed)\n");
        } else {
            char lstr[18];
            ba2str(&local, lstr);
            fprintf(stderr, "local hci0 BDADDR: %s\n", lstr);
        }
    }

    /* Connect */
    fprintf(stderr, "sending LE Create Connection...\n");
    int handle = le_create_conn(cmd_fd, evt_fd, &peer, peer_type, own_type);

    if (handle > 0) {
        fprintf(stderr, "\n*** SUCCESS: connection handle 0x%04x ***\n", handle);
    } else {
        fprintf(stderr, "\n*** FAILED: handle=%d ***\n", handle);
    }

    close(cmd_fd);
    close(evt_fd);
    return (handle > 0) ? 0 : 1;
}
