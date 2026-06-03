#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/uhid.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

/* ── HCI / ATT constants ──────────────────────────────────────────────── */
#define HCI_COMMAND_PKT  0x01
#define HCI_ACLDATA_PKT  0x02
#define HCI_EVENT_PKT    0x04

#define EVT_DISCONN_COMPLETE       0x05
#define EVT_NUM_COMP_PKTS          0x13
#define EVT_LE_META                0x3e
#define EVT_LE_CONN_COMPLETE       0x01
#define EVT_LE_CONN_UPDATE_COMPLETE 0x03

#define OGF_LE_CTL         0x08
#define OCF_LE_CONN_UPDATE 0x0013

#define ATT_CID             0x0004
#define ATT_ERROR_RSP       0x01
#define ATT_EXCHANGE_MTU_REQ  0x02
#define ATT_EXCHANGE_MTU_RSP  0x03
#define ATT_READ_BY_TYPE_REQ  0x08
#define ATT_READ_BY_TYPE_RSP  0x09
#define ATT_READ_BY_GROUP_REQ 0x10
#define ATT_READ_BY_GROUP_RSP 0x11
#define ATT_WRITE_REQ        0x12
#define ATT_WRITE_RSP        0x13
#define ATT_HANDLE_NOTIFY    0x1b

#define DEFAULT_BDADDR          "E0:EF:BF:3B:C6:76"
#define DEFAULT_HIDRAW          "/dev/hidraw0"
#define SW2_INPUT_VALUE_HANDLE  0x000e
#define SW2_INPUT_CCCD_HANDLE   0x000f

/* ── globals ──────────────────────────────────────────────────────────── */
static volatile sig_atomic_t running = 1;

/* ── helpers ──────────────────────────────────────────────────────────── */
static void on_signal(int sig)
{
    (void)sig;
    running = 0;
}

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void hexdump_prefix(const char *prefix, const uint8_t *buf,
                           size_t len, size_t max)
{
    fprintf(stderr, "%s", prefix);
    for (size_t i = 0; i < len && i < max; i++)
        fprintf(stderr, " %02x", buf[i]);
    if (len > max)
        fprintf(stderr, " ...");
    fprintf(stderr, "\n");
}

static ssize_t write_full(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)n;
    }
    return (ssize_t)done;
}

static ssize_t read_full(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = read(fd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            break;
        done += (size_t)n;
    }
    return (ssize_t)done;
}

/* ── HCI filter (works on libbluetooth fd too) ─────────────────────────── */
static int set_acl_runtime_filter(int fd)
{
    uint8_t f[16] = {0};
    uint32_t event0 = (1u << EVT_DISCONN_COMPLETE) |
                      (1u << EVT_CMD_COMPLETE) |
                      (1u << EVT_CMD_STATUS) |
                      (1u << EVT_NUM_COMP_PKTS);

    f[0] = (uint8_t)((1u << HCI_ACLDATA_PKT) | (1u << HCI_EVENT_PKT));
    f[4] = (uint8_t)event0;
    f[5] = (uint8_t)(event0 >> 8);
    f[6] = (uint8_t)(event0 >> 16);
    f[7] = (uint8_t)(event0 >> 24);
    f[10] = 0x00;
    f[11] = 0x40;

    if (setsockopt(fd, SOL_HCI, HCI_FILTER, f, sizeof(f)) < 0) {
        perror("setsockopt runtime HCI_FILTER");
        return -1;
    }
    return 0;
}

/* ── raw HCI command send (writev to libbluetooth fd) ─────────────────── */
static int raw_hci_send_cmd(int fd, uint16_t ogf, uint16_t ocf,
                        const uint8_t *params, uint8_t plen)
{
    uint8_t pkt_type = HCI_COMMAND_PKT;
    uint8_t hdr[3];
    uint16_t opcode = (uint16_t)((ocf & 0x03ff) | (ogf << 10));
    put_le16(hdr + 0, opcode);
    hdr[2] = plen;

    struct iovec iov[3] = {
        {&pkt_type, 1},
        {hdr, 3},
        {(void *)params, plen}
    };

    ssize_t n = writev(fd, iov, 3);
    if (n < 0) {
        perror("writev HCI command");
        return -1;
    }
    if (n != 1 + 3 + plen) {
        fprintf(stderr, "writev short: %zd vs %u\n", n, 1 + 3 + plen);
        return -1;
    }
    return 0;
}

static void hci_request_conn_update(int fd, uint16_t handle)
{
    uint8_t p[14];
    put_le16(p + 0, handle);
    put_le16(p + 2, 0x0006);
    put_le16(p + 4, 0x0006);
    put_le16(p + 6, 0x0000);
    put_le16(p + 8, 0x002a);
    put_le16(p + 10, 0x0000);
    put_le16(p + 12, 0x0000);

    if (raw_hci_send_cmd(fd, OGF_LE_CTL, OCF_LE_CONN_UPDATE, p, sizeof(p)) == 0)
        fprintf(stderr, "requested LE connection update: 7.5ms interval\n");
}

/* ── HCI packet reader (raw poll+read on libbluetooth fd) ─────────────── */
static int hci_read_packet(int fd, uint8_t *buf, size_t bufsz, int timeout_ms)
{
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0)
        return pr;

    if (bufsz < 1)
        return -1;
    if (read_full(fd, buf, 1) != 1) {
        perror("read HCI packet type");
        return -1;
    }

    if (buf[0] == HCI_EVENT_PKT) {
        if (bufsz < 3)
            return -1;
        if (read_full(fd, buf + 1, 2) != 2)
            return -1;
        if (3u + buf[2] > bufsz)
            return -1;
        if (read_full(fd, buf + 3, buf[2]) != buf[2])
            return -1;
        return 3 + buf[2];
    }

    if (buf[0] == HCI_ACLDATA_PKT) {
        uint16_t dlen;
        if (bufsz < 5)
            return -1;
        if (read_full(fd, buf + 1, 4) != 4)
            return -1;
        dlen = get_le16(buf + 3);
        if (5u + dlen > bufsz)
            return -1;
        if (read_full(fd, buf + 5, dlen) != dlen)
            return -1;
        return 5 + dlen;
    }

    return 1;
}

/* ── ACL / ATT framing ────────────────────────────────────────────────── */
static int acl_send_att(int fd, uint16_t handle, const uint8_t *att,
                        uint16_t att_len)
{
    uint8_t pkt[5 + 4 + 1024];
    uint16_t hflags = (uint16_t)((handle & 0x0fff) | (2u << 12));
    uint16_t acl_len = (uint16_t)(4 + att_len);

    if (att_len > 1024) {
        errno = EMSGSIZE;
        return -1;
    }

    pkt[0] = HCI_ACLDATA_PKT;
    put_le16(pkt + 1, hflags);
    put_le16(pkt + 3, acl_len);
    put_le16(pkt + 5, att_len);
    put_le16(pkt + 7, ATT_CID);
    memcpy(pkt + 9, att, att_len);

    if (write_full(fd, pkt, 9u + att_len) < 0) {
        perror("write ACL ATT");
        return -1;
    }
    return 0;
}

static int read_acl_att(int fd, uint16_t conn_handle, uint8_t *att,
                        size_t attsz, uint16_t *out_handle, int timeout_ms)
{
    uint8_t pkt[2048];

    for (;;) {
        int n = hci_read_packet(fd, pkt, sizeof(pkt), timeout_ms);
        if (n <= 0)
            return n;

        if (pkt[0] == HCI_EVENT_PKT && n >= 3) {
            uint8_t evt = pkt[1];
            const uint8_t *ev = pkt + 3;
            if (evt == EVT_DISCONN_COMPLETE && n >= 7) {
                uint16_t h = get_le16(ev + 1) & 0x0fff;
                fprintf(stderr, "disconnect complete: handle=0x%04x reason=0x%02x\n",
                        h, ev[3]);
                return -2;
            }
            if (evt == EVT_LE_META && n >= 6 &&
                ev[0] == EVT_LE_CONN_UPDATE_COMPLETE) {
                fprintf(stderr, "connection update complete: status=0x%02x interval=%u\n",
                        ev[1], get_le16(ev + 4));
            }
            continue;
        }

        if (pkt[0] != HCI_ACLDATA_PKT || n < 9)
            continue;

        uint16_t hflags = get_le16(pkt + 1);
        uint16_t h = hflags & 0x0fff;
        uint16_t acl_len = get_le16(pkt + 3);
        uint16_t l2_len = get_le16(pkt + 5);
        uint16_t cid = get_le16(pkt + 7);

        if (h != conn_handle || cid != ATT_CID)
            continue;
        if (acl_len + 5u > (uint16_t)n || l2_len + 4u > acl_len)
            continue;
        if (l2_len > attsz) {
            fprintf(stderr, "dropping oversized ATT frame len=%u\n", l2_len);
            continue;
        }

        memcpy(att, pkt + 9, l2_len);
        if (out_handle)
            *out_handle = h;
        return l2_len;
    }
}

static int att_request_response(int fd, uint16_t handle, const uint8_t *req,
                                uint16_t req_len, uint8_t *rsp, size_t rspsz,
                                uint8_t expect, int timeout_ms)
{
    if (acl_send_att(fd, handle, req, req_len) < 0)
        return -1;

    for (;;) {
        int n = read_acl_att(fd, handle, rsp, rspsz, NULL, timeout_ms);
        if (n <= 0)
            return n ? n : -1;
        if (rsp[0] == ATT_HANDLE_NOTIFY)
            continue;
        if (rsp[0] == expect)
            return n;
        if (rsp[0] == ATT_ERROR_RSP && n >= 5) {
            fprintf(stderr, "ATT error: req=0x%02x handle=0x%04x code=0x%02x\n",
                    rsp[1], get_le16(rsp + 2), rsp[4]);
            return n;
        }
        hexdump_prefix("unexpected ATT response:", rsp, (size_t)n, 24);
        return n;
    }
}

/* ── GATT probing ─────────────────────────────────────────────────────── */
static void gatt_probe(int fd, uint16_t handle)
{
    uint8_t rsp[768];
    uint8_t mtu_req[3] = {ATT_EXCHANGE_MTU_REQ, 0x05, 0x02};
    int n = att_request_response(fd, handle, mtu_req, sizeof(mtu_req),
                                 rsp, sizeof(rsp), ATT_EXCHANGE_MTU_RSP, 2000);
    if (n >= 3 && rsp[0] == ATT_EXCHANGE_MTU_RSP) {
        fprintf(stderr, "ATT MTU response: %u\n", get_le16(rsp + 1));
    } else {
        fprintf(stderr, "ATT MTU exchange did not complete; continuing\n");
    }

    uint8_t svc_req[7] = {ATT_READ_BY_GROUP_REQ, 0x01, 0x00,
                          0xff, 0xff, 0x00, 0x28};
    n = att_request_response(fd, handle, svc_req, sizeof(svc_req),
                             rsp, sizeof(rsp), ATT_READ_BY_GROUP_RSP, 2000);
    if (n >= 2 && rsp[0] == ATT_READ_BY_GROUP_RSP) {
        int elen = rsp[1];
        int count = elen ? (n - 2) / elen : 0;
        fprintf(stderr, "primary services: %d entries, element length=%d\n",
                count, elen);
        for (int i = 0; i < count; i++) {
            const uint8_t *e = rsp + 2 + i * elen;
            fprintf(stderr, "  service 0x%04x-0x%04x\n",
                    get_le16(e), get_le16(e + 2));
        }
    }

    uint8_t chr_req[7] = {ATT_READ_BY_TYPE_REQ, 0x08, 0x00,
                          0x2a, 0x00, 0x03, 0x28};
    n = att_request_response(fd, handle, chr_req, sizeof(chr_req),
                             rsp, sizeof(rsp), ATT_READ_BY_TYPE_RSP, 2000);
    if (n >= 2 && rsp[0] == ATT_READ_BY_TYPE_RSP) {
        int elen = rsp[1];
        int count = elen ? (n - 2) / elen : 0;
        fprintf(stderr, "service characteristics: %d entries, element length=%d\n",
                count, elen);
        for (int i = 0; i < count; i++) {
            const uint8_t *e = rsp + 2 + i * elen;
            if (elen >= 7) {
                fprintf(stderr, "  decl=0x%04x props=0x%02x value=0x%04x\n",
                        get_le16(e), e[2], get_le16(e + 3));
            }
        }
    }
}

static int enable_notifications(int fd, uint16_t handle)
{
    uint8_t req[5] = {ATT_WRITE_REQ, 0x0f, 0x00, 0x01, 0x00};
    uint8_t rsp[64];
    int n;

    put_le16(req + 1, SW2_INPUT_CCCD_HANDLE);
    n = att_request_response(fd, handle, req, sizeof(req),
                             rsp, sizeof(rsp), ATT_WRITE_RSP, 2000);
    if (n >= 1 && rsp[0] == ATT_WRITE_RSP) {
        fprintf(stderr, "enabled notifications on CCCD 0x%04x\n",
                SW2_INPUT_CCCD_HANDLE);
        return 0;
    }
    fprintf(stderr, "failed to enable notifications on CCCD 0x%04x\n",
            SW2_INPUT_CCCD_HANDLE);
    return -1;
}

/* ── HID report descriptor: Switch 2 Pro Controller ───────────────────── */
static const uint8_t gamepad_rdesc[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x18, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x18, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32,
    0x09, 0x35, 0x15, 0x00, 0x26, 0xff, 0x0f, 0x75,
    0x0c, 0x95, 0x04, 0x81, 0x02, 0x09, 0x39, 0x15,
    0x00, 0x25, 0x08, 0x35, 0x00, 0x46, 0x3b, 0x01,
    0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x03, 0x05, 0x02,
    0x09, 0xc5, 0x09, 0xc4, 0x15, 0x00,
    0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x02, 0x81,
    0x02, 0xc0
};

/* ── UHID ─────────────────────────────────────────────────────────────── */
static int uhid_create_device(void)
{
    int fd = open("/dev/uhid", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open /dev/uhid");
        return -1;
    }

    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_CREATE2;
    snprintf((char *)ev.u.create2.name, sizeof(ev.u.create2.name),
             "Nintendo Switch 2 Pro Controller");
    snprintf((char *)ev.u.create2.uniq, sizeof(ev.u.create2.uniq),
             DEFAULT_BDADDR);
    ev.u.create2.bus = BUS_BLUETOOTH;
    ev.u.create2.vendor = 0x057e;
    ev.u.create2.product = 0x2069;
    ev.u.create2.version = 0x0100;
    ev.u.create2.country = 0;
    ev.u.create2.rd_size = sizeof(gamepad_rdesc);
    memcpy(ev.u.create2.rd_data, gamepad_rdesc, sizeof(gamepad_rdesc));

    if (write_full(fd, &ev, sizeof(ev)) < 0) {
        perror("UHID_CREATE2");
        close(fd);
        return -1;
    }

    fprintf(stderr, "created UHID gamepad\n");
    return fd;
}

static void uhid_destroy_device(int fd)
{
    if (fd < 0)
        return;
    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_DESTROY;
    (void)write_full(fd, &ev, sizeof(ev));
    close(fd);
}

static int uhid_send_report(int fd, const uint8_t *data, size_t len)
{
    if (fd < 0)
        return 0;

    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_INPUT2;
    if (len > sizeof(ev.u.input2.data))
        len = sizeof(ev.u.input2.data);
    ev.u.input2.size = (uint16_t)len;
    memcpy(ev.u.input2.data, data, len);

    if (write_full(fd, &ev, sizeof(ev)) < 0) {
        perror("UHID_INPUT2");
        return -1;
    }
    return 0;
}

/* ── report 0x09 → standard HID ───────────────────────────────────────── */
static uint8_t dpad_from_buttons(uint16_t b)
{
    int up = 0;
    int down = 0;
    int left = 0;
    int right = 0;

    (void)b;

    if (up && right) return 1;
    if (right && down) return 3;
    if (down && left) return 5;
    if (left && up) return 7;
    if (up) return 0;
    if (right) return 2;
    if (down) return 4;
    if (left) return 6;
    return 8;
}

static uint16_t stick12_at(const uint8_t *r, size_t len, size_t off)
{
    if (off + 1 >= len)
        return 2048;
    return get_le16(r + off) & 0x0fff;
}

static int report09_to_uhid(const uint8_t *r, size_t len,
                            uint8_t *out, size_t outsz)
{
    if (len < 11 || r[0] != 0x09 || outsz < 13)
        return -1;

    uint16_t b = get_le16(r + 1);
    uint16_t lx = stick12_at(r, len, 3);
    uint16_t ly = stick12_at(r, len, 5);
    uint16_t rx = stick12_at(r, len, 7);
    uint16_t ry = stick12_at(r, len, 9);

    memset(out, 0, 13);
    out[0] = 0x01;
    out[1] = (uint8_t)b;
    out[2] = (uint8_t)(b >> 8);
    out[3] = (uint8_t)(((b & (1u << 6)) ? 1u : 0u) |
                       ((b & (1u << 7)) ? 2u : 0u));
    out[4] = (uint8_t)lx;
    out[5] = (uint8_t)((lx >> 8) | (ly << 4));
    out[6] = (uint8_t)(ly >> 4);
    out[7] = (uint8_t)rx;
    out[8] = (uint8_t)((rx >> 8) | (ry << 4));
    out[9] = (uint8_t)(ry >> 4);
    out[10] = dpad_from_buttons(b) & 0x0f;
    out[11] = (b & (1u << 6)) ? 0xff : 0x00;
    out[12] = (b & (1u << 7)) ? 0xff : 0x00;
    return 13;
}

/* ── USB init subcommands (over hidraw) ────────────────────────────────── */
static int usb_send_subcmd(int fd, uint8_t counter, uint8_t subcmd,
                           const uint8_t *arg, size_t arg_len)
{
    uint8_t report[64];
    memset(report, 0, sizeof(report));
    report[0] = 0x01;
    report[1] = counter;
    report[10] = subcmd;
    if (arg_len > sizeof(report) - 11)
        arg_len = sizeof(report) - 11;
    if (arg_len)
        memcpy(report + 11, arg, arg_len);

    if (write_full(fd, report, sizeof(report)) < 0) {
        perror("write hidraw subcommand");
        return -1;
    }
    return 0;
}

static int usb_init_controller(const char *hidraw)
{
    int fd = open(hidraw, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror(hidraw);
        return -1;
    }

    uint8_t one = 0x01;
    uint8_t zero = 0x00;
    uint8_t host[6] = {0xd8, 0x3a, 0xdd, 0xe5, 0x69, 0xed};

    fprintf(stderr, "USB init on %s\n", hidraw);
    if (usb_send_subcmd(fd, 0, 0x06, &one, 1) < 0 ||      /* SetHCIState */
        usb_send_subcmd(fd, 1, 0x08, &zero, 1) < 0 ||     /* SetShipment */
        usb_send_subcmd(fd, 2, 0x01, host, sizeof(host)) < 0) { /* BluetoothManualPair */
        close(fd);
        return -1;
    }

    close(fd);
    fprintf(stderr, "USB prep sent; unplug the controller now\n");
    sleep(8);
    return 0;
}

/* ── usage ────────────────────────────────────────────────────────────── */
static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [bdaddr] [--usb-init] [--no-uhid] [--hidraw PATH]\n",
            argv0);
}

/* ── single session (libbluetooth fd, full GATT+UHID) ──────────────────── */
static int run_session(const bdaddr_t *peer, int no_uhid)
{
    int dd = -1;
    int uhid = -1;
    uint16_t handle = 0;
    uint8_t att[1024];
    uint8_t hid[32];

    /* 1. Open HCI device via libbluetooth */
    dd = hci_open_dev(0);
    if (dd < 0) {
        perror("hci_open_dev");
        return -1;
    }

    /* 2. LE Create Connection (params from sw2d_lib.c) */
    fprintf(stderr, "creating LE connection via libbluetooth...\n");
    int rc = hci_le_create_conn(dd,
                                0x0004,    /* scan_interval   */
                                0x0004,    /* scan_window     */
                                0,         /* initiator_filter */
                                0,         /* peer_bdaddr_type (public) */
                                *peer,     /* peer BD_ADDR    */
                                0,         /* own_bdaddr_type */
                                0x000F,    /* conn_interval_min (15 * 1.25ms = 18.75ms) */
                                0x000F,    /* conn_interval_max */
                                0,         /* latency         */
                                0x0C80,    /* supervision_timeout (3200 * 10ms) */
                                0x0001,    /* min_ce_length   */
                                0x0001,    /* max_ce_length   */
                                &handle,
                                25000);    /* timeout ms      */
    if (rc < 0) {
        fprintf(stderr, "hci_le_create_conn: %s (%d)\n", strerror(-rc), -rc);
        close(dd);
        return -1;
    }
    fprintf(stderr, "connected: handle=0x%04x\n", handle);

    /* 3. Set runtime filter for ACL + events */
    if (set_acl_runtime_filter(dd) < 0) {
        close(dd);
        return -1;
    }

    /* 4. Request faster connection update (7.5ms) */
    hci_request_conn_update(dd, handle);

    /* 5. GATT service/characteristic discovery */
    gatt_probe(dd, handle);

    /* 6. Create UHID device */
    if (!no_uhid) {
        uhid = uhid_create_device();
        if (uhid < 0) {
            close(dd);
            return -1;
        }
    }

    /* 7. Enable notifications on input report */
    if (enable_notifications(dd, handle) < 0) {
        uhid_destroy_device(uhid);
        close(dd);
        return -1;
    }

    /* 8. Main receive loop: read ACL/ATT from libbluetooth fd */
    fprintf(stderr, "receiving controller notifications\n");
    while (running) {
        int n = read_acl_att(dd, handle, att, sizeof(att), NULL, 1000);
        if (n == -2)   /* disconnect */
            break;
        if (n <= 0)
            continue;
        if (n >= 3 && att[0] == ATT_HANDLE_NOTIFY) {
            uint16_t value_handle = get_le16(att + 1);
            const uint8_t *report = att + 3;
            size_t report_len = (size_t)n - 3;
            if (value_handle != SW2_INPUT_VALUE_HANDLE)
                continue;
            int hlen = report09_to_uhid(report, report_len,
                                        hid, sizeof(hid));
            if (hlen > 0)
                (void)uhid_send_report(uhid, hid, (size_t)hlen);
        }
    }

    uhid_destroy_device(uhid);
    close(dd);
    return 0;
}

/* ── main ─────────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    const char *bdaddr_s = DEFAULT_BDADDR;
    const char *hidraw = DEFAULT_HIDRAW;
    int usb_init = 0;
    int no_uhid = 0;
    bdaddr_t peer;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--usb-init") == 0) {
            usb_init = 1;
        } else if (strcmp(argv[i], "--no-uhid") == 0) {
            no_uhid = 1;
        } else if (strcmp(argv[i], "--hidraw") == 0) {
            if (++i >= argc) {
                usage(argv[0]);
                return 2;
            }
            hidraw = argv[i];
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 2;
        } else {
            bdaddr_s = argv[i];
        }
    }

    /* Parse BD_ADDR via libbluetooth's str2ba */
    if (str2ba(bdaddr_s, &peer) < 0) {
        fprintf(stderr, "invalid controller BD_ADDR: %s\n", bdaddr_s);
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    setvbuf(stderr, NULL, _IOLBF, 0);

    fprintf(stderr, "sw2d_final: target=%s hci=hci0 uhid=%s\n",
            bdaddr_s, no_uhid ? "disabled" : "enabled");

    if (usb_init && usb_init_controller(hidraw) < 0)
        return 1;

    /* Reconnect loop with exponential backoff */
    int backoff = 1;
    while (running) {
        int rc = run_session(&peer, no_uhid);
        if (!running)
            break;
        fprintf(stderr, "session ended rc=%d; reconnecting in %ds\n",
                rc, backoff);
        sleep((unsigned int)backoff);
        if (backoff < 16)
            backoff *= 2;
    }

    fprintf(stderr, "sw2d_final: stopped\n");
    return 0;
}
