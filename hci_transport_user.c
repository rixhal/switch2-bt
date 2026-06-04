/*
 * hci_transport_user.c — HCI_CHANNEL_USER transport implementation
 *
 * See hci_transport_user.h for rationale and API documentation.
 *
 * This file implements:
 *   1. Bringing hci0 DOWN via ioctl (so the kernel releases the controller)
 *   2. Opening an AF_BLUETOOTH SOCK_RAW socket with HCI_CHANNEL_USER
 *   3. Raw HCI command/event/ACL I/O over the USER channel
 *   4. Controller initialization (HCI_Reset + LE Set Event Mask)
 *   5. LE Create Connection Cancel for clean retry
 *
 * Kernel-behaviour note (CMD_STATUS 0x0c):
 *   When hci0 is UP, the kernel's hci_core tracks connection state.
 *   If a previous LE Create Connection didn't complete cleanly
 *   (timeout, remote reject, disconnect), the kernel considers the
 *   "connect" operation still active and rejects a second
 *   LE Create Connection with CMD_STATUS 0x0c ("Command Disallowed").
 *   HCI_CHANNEL_USER bypasses this because hci0 is DOWN — the kernel
 *   has no visibility into our connection state. WE are the host.
 *
 *   However, the CONTROLLER itself may still have a pending
 *   LE Create Connection in its command queue. If we got a timeout
 *   or error from the first attempt, we MUST send
 *   LE_Create_Connection_Cancel before retrying, otherwise the
 *   controller — not the kernel — rejects the second command.
 *
 * Build: gcc -c hci_transport_user.c
 * Link:  gcc ... hci_transport_user.o -lbluetooth
 */

#define _GNU_SOURCE
#include "hci_transport_user.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

/* ── helpers ──────────────────────────────────────────────────────────── */
static uint16_t get_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* ── bring hci0 down via ioctl ────────────────────────────────────────── */

/*
 * We need a temporary HCI socket to issue the HCIDEVDOWN ioctl.
 * This is a standard HCI_CHANNEL_RAW socket — it only exists for
 * the duration of the ioctl call, then we close it and open the
 * real HCI_CHANNEL_USER socket.
 */
static int hci_bring_down(int dev_id) {
    int ctl = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
    if (ctl < 0) {
        perror("hci_bring_down: socket");
        return -1;
    }

    /* Bind to the device so ioctl targets the right one */
    struct sockaddr_hci addr = {
        .hci_family = AF_BLUETOOTH,
        .hci_dev    = (uint16_t)dev_id,
        .hci_channel = HCI_CHANNEL_RAW,
    };
    if (bind(ctl, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        /* EBUSY or EALREADY means it's already down — that's fine */
        if (errno != EBUSY && errno != EALREADY) {
            perror("hci_bring_down: bind");
            close(ctl);
            return -1;
        }
        /* Already down or busy — try ioctl anyway */
    }

    /* HCIDEVDOWN = 0 (see hciconfig.c from bluez) */
    if (ioctl(ctl, HCIDEVDOWN, dev_id) < 0) {
        /* EALREADY = already down (not an error for us) */
        if (errno != EALREADY) {
            perror("hci_bring_down: HCIDEVDOWN");
            close(ctl);
            return -1;
        }
    }

    close(ctl);
    usleep(100000); /* give kernel time to release */
    return 0;
}

/* ── open HCI_CHANNEL_USER socket ─────────────────────────────────────── */

int hci_user_open(void) {
    int dev_id = hci_get_route(NULL);
    if (dev_id < 0) {
        /* hci0 might not be registered yet — try dev_id 0 */
        dev_id = 0;
    }

    fprintf(stderr, "  [transport] bringing hci%d DOWN for USER channel\n", dev_id);
    if (hci_bring_down(dev_id) < 0) {
        fprintf(stderr, "  [transport] WARNING: could not bring hci%d down "
                        "(may already be down)\n", dev_id);
    }

    int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (fd < 0) {
        perror("hci_user_open: socket");
        return -1;
    }

    struct sockaddr_hci addr = {
        .hci_family  = AF_BLUETOOTH,
        .hci_dev     = (uint16_t)dev_id,
        .hci_channel = HCI_CHANNEL_USER,
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "  [transport] bind HCI_CHANNEL_USER failed: %s\n",
                strerror(errno));
        fprintf(stderr, "  [transport] Make sure:\n");
        fprintf(stderr, "  [transport]   - bluetoothd is stopped\n");
        fprintf(stderr, "  [transport]   - no other process holds /dev/hci0\n");
        fprintf(stderr, "  [transport]   - hci0 is DOWN (not UP)\n");
        close(fd);
        return -1;
    }

    fprintf(stderr, "  [transport] HCI_CHANNEL_USER socket open "
                    "(fd=%d, exclusive access to hci%d)\n", fd, dev_id);
    return fd;
}

void hci_user_close(int fd) {
    if (fd >= 0) {
        close(fd);
        fprintf(stderr, "  [transport] HCI_CHANNEL_USER socket closed\n");
    }
}

/* ── send HCI command ────────────────────────────────────────────────── */

int hci_user_send_cmd(int fd, uint16_t ogf, uint16_t ocf,
                      const uint8_t *params, uint8_t plen) {
    uint8_t pkt_type = HCI_COMMAND_PKT;
    uint8_t hdr[3];
    uint16_t opcode = (uint16_t)((ocf & 0x03ff) | (ogf << 10));
    put_le16(hdr, opcode);
    hdr[2] = plen;

    struct iovec iov[3] = {
        {&pkt_type, 1},
        {hdr, 3},
        {(void *)params, plen},
    };
    int nvec = (params && plen > 0) ? 3 : 2;
    ssize_t n = writev(fd, iov, nvec);
    if (n < 0) {
        perror("hci_user_send_cmd: writev");
        return -1;
    }
    return 0;
}

/* ── cancel pending LE Create Connection ─────────────────────────────── */

int hci_user_cancel_connect(int fd) {
    /*
     * LE Create Connection Cancel (OCF 0x000E, OGF 0x08)
     * No parameters.
     * The controller responds with CMD_STATUS, then CMD_COMPLETE
     * for the cancelled Create Connection (status=cancelled).
     *
     * Important: we MUST drain the CMD_STATUS and CMD_COMPLETE
     * events after sending cancel, otherwise they'll be read as
     * the response to our next command.
     */
    fprintf(stderr, "  [transport] sending LE_Create_Connection_Cancel\n");
    if (hci_user_send_cmd(fd, OGF_LE_CTL, OCF_LE_CREATE_CONN_CANCEL,
                          NULL, 0) < 0) {
        return -1;
    }

    /* Drain the cancel response (CMD_STATUS + CMD_COMPLETE).
     * We don't care about the actual values — just clear the pipe. */
    hci_user_frame_t f;
    int drained = 0;
    for (int i = 0; i < 5 && drained < 2; i++) {
        int rc = hci_user_read_frame(fd, &f, 200);
        if (rc <= 0) break;
        if (f.pkt_type == HCI_EVENT_PKT &&
            (f.evt_code == EVT_CMD_STATUS || f.evt_code == EVT_CMD_COMPLETE)) {
            drained++;
        }
    }
    fprintf(stderr, "  [transport] cancel complete (drained %d events)\n", drained);
    return 0;
}

/* ── controller initialization ────────────────────────────────────────── */

int hci_user_init(int fd) {
    fprintf(stderr, "  [transport] === Controller Init ===\n");

    /*
     * HCI_Reset:
     *   Full controller reset. Clears all connection state, pairing
     *   data (volatile), and pending commands. Essential because we
     *   don't know what state the kernel/BlueZ left the controller in.
     *   OGF=0x03 (Link Control), OCF=0x0003
     */
    if (hci_user_send_cmd(fd, 0x03, OCF_HCI_RESET, NULL, 0) < 0)
        return -1;
    fprintf(stderr, "  [transport]   HCI_Reset sent\n");

    /*
     * Wait for CMD_COMPLETE for HCI_Reset.
     * The reset takes ~100-500ms on most controllers.
     */
    hci_user_frame_t f;
    int got_reset = 0;
    for (int i = 0; i < 10 && !got_reset; i++) {
        int rc = hci_user_read_frame(fd, &f, 200);
        if (rc <= 0) continue;
        if (f.pkt_type == HCI_EVENT_PKT &&
            f.evt_code == EVT_CMD_COMPLETE &&
            f.payload_len >= 3) {
            uint16_t op = get_le16(f.payload + 1);
            uint8_t  st = f.payload[0];
            if (op == 0x0C03) { /* HCI_Reset opcode in CMD_COMPLETE */
                fprintf(stderr, "  [transport]   HCI_Reset complete (status=0x%02x)\n", st);
                got_reset = 1;
            }
        }
    }
    if (!got_reset) {
        fprintf(stderr, "  [transport]   WARNING: no CMD_COMPLETE for HCI_Reset, continuing\n");
    }

    /*
     * LE Set Event Mask:
     *   Tell the controller which LE Meta events we want.
     *   We enable: Connection Complete, Connection Update, Advertising
     *   Report, Data Length Change, LTK Request.
     *   Mask: 0x0000000000001FFF (bits 0-12 set)
     */
    uint8_t evtmask[8] = {0xFF, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    if (hci_user_send_cmd(fd, OGF_LE_CTL, OCF_LE_SET_EVENT_MASK,
                          evtmask, 8) < 0)
        return -1;
    fprintf(stderr, "  [transport]   LE_Set_Event_Mask sent\n");
    usleep(50000);

    fprintf(stderr, "  [transport] Controller init complete\n");
    return 0;
}

/* ── read HCI frame ───────────────────────────────────────────────────── */

int hci_user_read_frame(int fd, hci_user_frame_t *f, int timeout_ms) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) return -1;
    if (pr == 0) return 0; /* timeout */

    uint8_t buf[512];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 1) return n < 0 ? -1 : 0;

    memcpy(f->raw, buf, (size_t)n);
    f->raw_len = (uint16_t)n;
    f->pkt_type = buf[0];

    if (buf[0] == HCI_EVENT_PKT && n >= 3) {
        f->evt_code = buf[1];
        f->payload = buf + 3;
        f->payload_len = (uint16_t)buf[2];
        f->subevent = 0;
        f->status = 0;
        f->opcode = 0;

        if (f->evt_code == EVT_LE_META && f->payload_len >= 2)
            f->subevent = f->payload[0];

        if (f->evt_code == EVT_CMD_STATUS && f->payload_len >= 4) {
            f->status = f->payload[0];
            f->opcode = get_le16(f->payload + 2);
        }
        if (f->evt_code == EVT_DISCONN_COMPLETE && f->payload_len >= 4) {
            f->handle = get_le16(f->payload + 1) & 0x0fff;
            f->status = f->payload[3]; /* reason code */
        }
        if (f->evt_code == EVT_ENCRYPT_CHANGE && f->payload_len >= 4) {
            f->handle = get_le16(f->payload + 0) & 0x0fff;
            f->status = f->payload[2]; /* 0x00 = success */
        }
        return 1;
    }

    if (buf[0] == HCI_ACLDATA_PKT && n >= 9) {
        f->handle = get_le16(buf + 1) & 0x0fff;
        f->acl_len = get_le16(buf + 3);
        f->l2cap_len = get_le16(buf + 5);
        f->cid = get_le16(buf + 7);
        f->payload = buf + 9;
        f->payload_len = f->l2cap_len;
        return 1;
    }

    return 1; /* consumed, unknown type */
}

/* ── ACL data send ────────────────────────────────────────────────────── */

int hci_user_acl_send(int fd, uint16_t handle, uint16_t cid,
                      const uint8_t *data, uint16_t len) {
    if (len > 512) return -1;

    uint8_t buf[5 + 4 + 512];
    /* Handle + PB flag: PB=10 (first fragment, auto-flush) */
    uint16_t hf = (uint16_t)((handle & 0x0fff) | (2u << 12));
    uint16_t acl_len = (uint16_t)(4 + len); /* L2CAP header + payload */

    buf[0] = HCI_ACLDATA_PKT;
    put_le16(buf + 1, hf);
    put_le16(buf + 3, acl_len);
    /* L2CAP header */
    put_le16(buf + 5, len);     /* L2CAP payload length */
    put_le16(buf + 7, cid);     /* L2CAP Channel ID */
    memcpy(buf + 9, data, len);

    struct iovec iov[1] = {{buf, 9u + len}};
    ssize_t n = writev(fd, iov, 1);
    if (n < 0) {
        perror("hci_user_acl_send: writev");
        return -1;
    }
    return 0;
}
