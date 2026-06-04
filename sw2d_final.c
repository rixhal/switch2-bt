/*
 * sw2d_final.c — Switch 2 Pro Controller BLE-to-UHID Daemon
 *
 * Two-socket HCI architecture:
 *   cmd_fd — sends HCI commands (LE Create Connection, ACL data)
 *   evt_fd — receives HCI events (CMD_STATUS, LE Connection Complete, etc.)
 *
 * Why two sockets:
 *   Linux HCI sockets are SOCK_RAW (datagram). The kernel sets skb->sk on
 *   outgoing commands. For kernel-generated responses (e.g. mgmt), the check
 *   `skb->sk == sk` blocks delivery to the sending socket. Hardware events
 *   (CMD_STATUS from the controller) have skb->sk=NULL and are delivered
 *   to all bound sockets — but using separate sockets avoids edge cases
 *   with mgmt interference and filter conflicts.
 *
 * Filter patterns from: sowifi/hcimin, thewierdnut/asha_pipewire_sink
 * Reference: bleno#225, NXP SMP Pairing guide
 *
 * Build: make sw2d_final
 * Usage: sw2d_final [--bdaddr AA:BB:..] [--peer-addr-type public|random|auto]
 *                   [--host-bdaddr auto|AA:BB:..] [--usb-init] [--auto-scan]
 *                   [--exclusive-hci] [--kill-bluez-clients] [--reset-hci]
 *                   [--no-uhid] [--verbose]
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

/* ── Constants ──────────────────────────────────────────────────────── */

#define APP_NAME    "sw2d_final"
#define SW2_VID     0x057E
#define SW2_PID     0x2069

/* GATT handles (verified with gattdump) */
#define GATT_INPUT_REPORT_HANDLE   0x000E
#define GATT_CCCD_HANDLE           0x000F
#define GATT_SERVICE_START         0x0001
#define GATT_SERVICE_END           0xFFFF

/* HCI filter: EVT_LE_META_EVENT = 0x3e, defined as EVT_LE_META_EVENT in
 * modern headers, EVT_LE_META in older ones. */
#ifndef EVT_LE_META_EVENT
#define EVT_LE_META_EVENT  EVT_LE_META
#endif

/* ── Types ──────────────────────────────────────────────────────────── */

typedef enum {
    ADDR_PUBLIC = 0x00,
    ADDR_RANDOM = 0x01,
    ADDR_AUTO   = 0xFF
} addr_type_t;

/* ── Forward declarations ───────────────────────────────────────────── */

static int  open_cmd_socket(void);
static int  open_evt_socket(void);
static int  setup_event_filter(int fd);
static int  send_hci_cmd(int fd, uint16_t ogf, uint16_t ocf,
                         const uint8_t *params, uint8_t plen);
static int  read_evt_frame(int fd, uint8_t *buf, int bufsz, int timeout_ms);
static int  le_create_connection(int cmd_fd, int evt_fd,
                                 const bdaddr_t *peer, uint8_t peer_type,
                                 uint8_t own_type, uint16_t *outhandle);
static int  kill_bluez_clients(void);
static int  check_exclusive_hci(void);
static int  usb_prep_controller(const char *hidraw_path, const uint8_t host_bytes[6]);
static void parse_host_bdaddr(uint8_t host_bytes[6], const char *host_spec);
static void usage(const char *prog);

/* ── Globals ────────────────────────────────────────────────────────── */

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig) { (void)sig; g_running = 0; }

/* ── Little-endian helpers ──────────────────────────────────────────── */

static inline void put_le16(void *dst, uint16_t v) {
    uint8_t *p = dst; p[0] = v & 0xff; p[1] = v >> 8;
}
static inline uint16_t get_le16(const void *src) {
    const uint8_t *p = src; return (uint16_t)(p[0] | (p[1] << 8));
}

/* ── Address helpers ────────────────────────────────────────────────── */

static const char *addr_type_str(uint8_t t) {
    switch (t) {
        case 0x00: return "public";
        case 0x01: return "random";
        default:   return "?";
    }
}

static int parse_addr_type(const char *s) {
    if (!strcmp(s, "public")) return ADDR_PUBLIC;
    if (!strcmp(s, "random")) return ADDR_RANDOM;
    if (!strcmp(s, "auto"))   return ADDR_AUTO;
    return -1;
}

static int parse_colon_bytes(const char *s, uint8_t out[6]) {
    unsigned int b[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x",
               &b[5],&b[4],&b[3],&b[2],&b[1],&b[0]) != 6)
        return -1;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)b[i];
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * HCI SOCKET LAYER
 * ═══════════════════════════════════════════════════════════════════════ */

static int open_cmd_socket(void) {
    int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (fd < 0) { perror("cmd socket"); return -1; }

    struct sockaddr_hci addr;
    memset(&addr, 0, sizeof(addr));
    addr.hci_family = AF_BLUETOOTH;
    addr.hci_dev = 0;               /* hci0 */
    addr.hci_channel = HCI_CHANNEL_RAW;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind cmd_fd");
        close(fd);
        return -1;
    }
    return fd;
}

static int open_evt_socket(void) {
    int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
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
    return fd;
}

static int setup_event_filter(int fd) {
    struct hci_filter nf;

    hci_filter_clear(&nf);
    hci_filter_set_ptype(HCI_EVENT_PKT, &nf);
    /* Use permissive filter first — the specific event filter may
     * be AND-ed down by kernel security policy on some configs. */
    memset(nf.event_mask, 0xFF, sizeof(nf.event_mask));

    if (setsockopt(fd, SOL_HCI, HCI_FILTER, &nf, sizeof(nf)) < 0) {
        perror("evt_fd HCI_FILTER");
        return -1;
    }

    /* Verify filter was applied */
    struct hci_filter vf;
    socklen_t len = sizeof(vf);
    memset(&vf, 0, sizeof(vf));
    if (getsockopt(fd, SOL_HCI, HCI_FILTER, &vf, &len) == 0) {
        fprintf(stderr, "evt_fd filter: type=0x%08x evt0=0x%08x evt1=0x%08x\n",
                vf.type_mask, vf.event_mask[0], vf.event_mask[1]);
    }
    return 0;
}

/* ── Send HCI command via cmd_fd ─────────────────────────────────── */

static int send_hci_cmd(int fd, uint16_t ogf, uint16_t ocf,
                        const uint8_t *params, uint8_t plen) {
    uint16_t opcode = (uint16_t)((ocf & 0x03ff) | (ogf << 10));
    uint8_t frame[4];
    frame[0] = HCI_COMMAND_PKT;
    put_le16(frame + 1, opcode);
    frame[3] = plen;

    struct iovec iov[2];
    iov[0].iov_base = frame;
    iov[0].iov_len  = 4;
    iov[1].iov_base = (void *)params;
    iov[1].iov_len  = plen;

    struct msghdr msg = {0};
    msg.msg_iov    = iov;
    msg.msg_iovlen = plen ? 2 : 1;

    ssize_t n = sendmsg(fd, &msg, MSG_NOSIGNAL);
    if (n < 0) {
        perror("sendmsg cmd");
        return -1;
    }

    if (g_running)  /* verbose only if not shutting down */
        fprintf(stderr, "  HCI cmd: ogf=0x%02x ocf=0x%04x plen=%d (%zd bytes)\n",
                ogf, ocf, plen, n);
    return 0;
}

/* ── Read one HCI event frame from evt_fd ───────────────────────── */

static int read_evt_frame(int fd, uint8_t *buf, int bufsz, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);

    if (pr < 0) {
        if (errno == EINTR) return 0;
        perror("poll evt_fd");
        return -1;
    }
    if (pr == 0) return 0;  /* timeout */

    /* CRITICAL: HCI sockets are SOCK_RAW (datagram).
     * Read the ENTIRE frame in one call. Byte-by-byte reads
     * discard the remaining datagram bytes. */
    ssize_t n = read(fd, buf, bufsz);
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) return 0;
        perror("read evt_fd");
        return -1;
    }
    if (n < 3) {  /* min frame: type(1) + event_hdr(2) */
        if (n > 0) fprintf(stderr, "  short HCI frame: %zd bytes\n", n);
        return 0;
    }
    return (int)n;
}

/* ═══════════════════════════════════════════════════════════════════════
 * LE CREATE CONNECTION
 * ═══════════════════════════════════════════════════════════════════════ */

static int le_create_connection(int cmd_fd, int evt_fd,
                                const bdaddr_t *peer, uint8_t peer_type,
                                uint8_t own_type, uint16_t *outhandle) {
    /* Build LE Create Connection parameters (HCI spec 7.8.12) */
    uint8_t params[25];
    memset(params, 0, sizeof(params));
    put_le16(params + 0,  0x0010);  /* scan_interval  10ms */
    put_le16(params + 2,  0x0010);  /* scan_window    10ms */
    params[4]  = 0x00;              /* initiator_filter: use peer addr */
    params[5]  = peer_type;
    memcpy(params + 6, peer, 6);
    params[12] = own_type;
    put_le16(params + 13, 0x0028);  /* conn_interval_min 50ms */
    put_le16(params + 15, 0x0038);  /* conn_interval_max 70ms */
    put_le16(params + 17, 0x0000);  /* conn_latency      0   */
    put_le16(params + 19, 0x07D0);  /* supervision_timeout 20s */
    put_le16(params + 21, 0x0001);  /* min_ce_length */
    put_le16(params + 23, 0x0001);  /* max_ce_length */

    uint16_t opcode = (uint16_t)((OCF_LE_CREATE_CONN & 0x03ff) | (OGF_LE_CTL << 10));

    if (send_hci_cmd(cmd_fd, OGF_LE_CTL, OCF_LE_CREATE_CONN, params, 25) < 0)
        return -1;

    /* Wait for CMD_STATUS, then LE Connection Complete.
     * Pattern from hcimin hci_send_req(): poll+read loop, parse events. */
    int max_iters = 60;  /* 6 seconds at 100ms each */
    for (int i = 0; i < max_iters && g_running; i++) {
        uint8_t buf[256];
        int n = read_evt_frame(evt_fd, buf, sizeof(buf), 100);
        if (n <= 0) continue;

        /* Frame format: [type_byte][event_hdr][event_data] */
        if (buf[0] != HCI_EVENT_PKT) continue;

        uint8_t  evt  = buf[1];
        uint8_t  plen = buf[2];
        const uint8_t *ev = buf + 3;

        /* Verify plen matches what we received */
        if (n < 3 + plen) {
            fprintf(stderr, "  truncated event: got %d, expected %d+%d\n", n, 3, plen);
            continue;
        }

        switch (evt) {
        case EVT_CMD_STATUS: {
            if (plen < 4) break;
            uint8_t  status = ev[0];
            uint16_t resp_op = get_le16(ev + 1);
            if (resp_op == opcode) {
                fprintf(stderr, "  CMD_STATUS: status=0x%02x\n", status);
                if (status != 0x00) {
                    fprintf(stderr, "  FAIL: CMD_STATUS error 0x%02x\n", status);
                    return -1;
                }
                /* status=0 → command pending, continue waiting */
            }
            break;
        }
        case EVT_LE_META_EVENT: {
            if (plen < 1) break;
            uint8_t sub = ev[0];
            if (sub == EVT_LE_CONN_COMPLETE && plen >= 19) {
                uint8_t  status = ev[1];
                uint16_t handle = get_le16(ev + 2);
                uint8_t  pt     = ev[9];
                uint16_t interval = get_le16(ev + 16);
                char pstr[18];
                ba2str((bdaddr_t *)(ev + 10), pstr);

                fprintf(stderr, "  CONN_COMPLETE: status=0x%02x handle=0x%04x\n",
                        status, handle);
                fprintf(stderr, "    peer=%s type=%s interval=%.2fms\n",
                        pstr, addr_type_str(pt), interval * 1.25);

                if (status == 0x00) {
                    *outhandle = handle;
                    return 0;  /* SUCCESS */
                }
                fprintf(stderr, "  FAIL: connection status=0x%02x\n", status);
                return -1;
            }
            break;
        }
        case EVT_DISCONN_COMPLETE: {
            if (plen >= 4) {
                uint8_t reason = ev[2];
                fprintf(stderr, "  DISCONN: reason=0x%02x\n", reason);
            }
            break;
        }
        }
    }

    fprintf(stderr, "  FAIL: timeout waiting for LE Connection Complete\n");
    return -1;
}

/* ═══════════════════════════════════════════════════════════════════════
 * BLUEZ CLIENT GUARD
 * ═══════════════════════════════════════════════════════════════════════ */

static const char *BLUEZ_PROCS[] = {
    "bluetoothd", "bluetoothctl", "btmgmt", "hcitool",
    "gatttool", "hciattach", "btmon", NULL
};

static int kill_bluez_clients(void) {
    int killed = 0;
    for (const char **p = BLUEZ_PROCS; *p; p++) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "killall -9 %s 2>/dev/null", *p);
        int rc = system(cmd);
        if (rc == 0 || rc == 256) killed++;  /* 256 = signal 1 */
        fprintf(stderr, "  kill %s: %s\n", *p, (rc == 0 || rc == 256) ? "ok" : "-");
    }
    /* Also stop bluetooth.service */
    system("systemctl stop bluetooth 2>/dev/null");
    return killed;
}

static int check_exclusive_hci(void) {
    int found = 0;
    for (const char **p = BLUEZ_PROCS; *p; p++) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "pgrep -x %s >/dev/null 2>&1", *p);
        if (system(cmd) == 0) {
            fprintf(stderr, "  CONFLICT: %s is running\n", *p);
            found++;
        }
    }
    return found;
}

/* ═══════════════════════════════════════════════════════════════════════
 * USB PREP (SetHCIState + BluetoothManualPair)
 * ═══════════════════════════════════════════════════════════════════════ */

static int usb_prep_controller(const char *hidraw_path,
                               const uint8_t host_bytes[6]) {
    int fd = open(hidraw_path, O_RDWR);
    if (fd < 0) {
        perror(hidraw_path);
        return -1;
    }

    uint8_t report[64];

    /* Subcommand 0x06: SetHCIState (enable Bluetooth module) */
    memset(report, 0, sizeof(report));
    report[0]  = 0x01;   /* output report ID */
    report[10] = 0x06;   /* SetHCIState */
    report[11] = 0x01;   /* BT on */
    if (write(fd, report, sizeof(report)) < 0) {
        perror("write SetHCIState");
        close(fd);
        return -1;
    }
    fprintf(stderr, "  SetHCIState (BT ON) sent\n");

    /* Subcommand 0x01: BluetoothManualPair */
    memset(report, 0, sizeof(report));
    report[0]  = 0x01;
    report[10] = 0x01;   /* BluetoothManualPair */
    memcpy(report + 11, host_bytes, 6);
    if (write(fd, report, sizeof(report)) < 0) {
        perror("write BluetoothManualPair");
        close(fd);
        return -1;
    }
    fprintf(stderr, "  BluetoothManualPair sent (host=%02x:%02x:%02x:%02x:%02x:%02x)\n",
            host_bytes[5], host_bytes[4], host_bytes[3],
            host_bytes[2], host_bytes[1], host_bytes[0]);

    close(fd);
    return 0;
}

static void parse_host_bdaddr(uint8_t host_bytes[6], const char *host_spec) {
    char txt[18] = "auto";
    if (strcmp(host_spec, "auto") == 0) {
        bdaddr_t ba;
        if (hci_devba(0, &ba) == 0) {
            memcpy(host_bytes, &ba, 6);
            ba2str(&ba, txt);
        } else {
            /* Fallback: use well-known crackberry5 BDADDR */
            uint8_t fallback[6] = {0xD8,0x3A,0xDD,0xE5,0x69,0xED};
            memcpy(host_bytes, fallback, 6);
            strcpy(txt, "D8:3A:DD:E5:69:ED");
            fprintf(stderr, "  hci_devba failed, using fallback BDADDR\n");
        }
        fprintf(stderr, "  host BDADDR (auto): %s\n", txt);
    } else {
        if (parse_colon_bytes(host_spec, host_bytes) < 0)
            str2ba(host_spec, (bdaddr_t *)host_bytes);
        ba2str((bdaddr_t *)host_bytes, txt);
        fprintf(stderr, "  host BDADDR (manual): %s\n", txt);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════ */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n\n"
        "Options:\n"
        "  --bdaddr AA:BB:CC:DD:EE:FF   Controller BDADDR\n"
        "  --peer-addr-type TYPE         public | random | auto\n"
        "  --own-addr-type TYPE           public | random\n"
        "  --host-bdaddr auto|AA:BB:..   Host BDADDR for USB prep\n"
        "  --auto-scan                   Scan for Switch 2 advertisers\n"
        "  --scan-timeout MS             Scan timeout (default: 10000)\n"
        "  --scan-only                   Scan, print, and exit\n"
        "  --usb-init                    Run USB init sequence first\n"
        "  --hidraw PATH                 Hidraw device (default: /dev/hidraw0)\n"
        "  --exclusive-hci               Check no other BT clients running\n"
        "  --kill-bluez-clients          Kill interfering BT processes\n"
        "  --preflight                   Only check, then exit\n"
        "  --reset-hci                   Reset hci0 (down/up) before start\n"
        "  --no-uhid                     Don't create UHID device\n"
        "  --verbose                     Verbose output\n",
        prog);
}

int main(int argc, char **argv) {
    /* ── Parse CLI ────────────────────────────────────────────────── */
    const char *bdaddr_s       = NULL;
    const char *peer_type_s    = "public";
    const char *own_type_s     = "public";
    const char *host_bdaddr_s  = "auto";
    const char *hidraw_path    = "/dev/hidraw0";
    int   auto_scan   = 0;
    int   scan_only   = 0;
    int   usb_init    = 0;
    int   exclusive   = 0;
    int   kill_bluez  = 0;
    int   preflight   = 0;
    int   reset_hci   = 0;
    int   no_uhid     = 0;
    int   verbose     = 0;
    int   scan_timeout_ms = 10000;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bdaddr") && i+1 < argc)
            bdaddr_s = argv[++i];
        else if (!strcmp(argv[i], "--peer-addr-type") && i+1 < argc)
            peer_type_s = argv[++i];
        else if (!strcmp(argv[i], "--own-addr-type") && i+1 < argc)
            own_type_s = argv[++i];
        else if (!strcmp(argv[i], "--host-bdaddr") && i+1 < argc)
            host_bdaddr_s = argv[++i];
        else if (!strcmp(argv[i], "--hidraw") && i+1 < argc)
            hidraw_path = argv[++i];
        else if (!strcmp(argv[i], "--scan-timeout") && i+1 < argc)
            scan_timeout_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--auto-scan"))    auto_scan   = 1;
        else if (!strcmp(argv[i], "--scan-only"))    scan_only   = 1;
        else if (!strcmp(argv[i], "--usb-init"))     usb_init    = 1;
        else if (!strcmp(argv[i], "--exclusive-hci")) exclusive   = 1;
        else if (!strcmp(argv[i], "--kill-bluez-clients")) kill_bluez = 1;
        else if (!strcmp(argv[i], "--preflight"))    preflight   = 1;
        else if (!strcmp(argv[i], "--reset-hci"))    reset_hci   = 1;
        else if (!strcmp(argv[i], "--no-uhid"))      no_uhid     = 1;
        else if (!strcmp(argv[i], "--verbose"))      verbose     = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]); return 0;
        } else {
            /* Assume positional BDADDR */
            if (!bdaddr_s) bdaddr_s = argv[i];
            else { fprintf(stderr, "unknown option: %s\n", argv[i]); return 2; }
        }
    }

    /* ── Pre-flight checks ────────────────────────────────────────── */
    if (exclusive || kill_bluez) {
        fprintf(stderr, "=== Pre-flight ===\n");
        int conflicts = check_exclusive_hci();
        if (conflicts > 0 && kill_bluez) {
            fprintf(stderr, "Killing conflicting processes...\n");
            kill_bluez_clients();
            sleep(1);
            conflicts = check_exclusive_hci();
        }
        if (conflicts > 0 && exclusive) {
            fprintf(stderr, "ERROR: %d conflicting process(es) detected. "
                    "Use --kill-bluez-clients or stop them manually.\n",
                    conflicts);
            return 1;
        }
        if (preflight) {
            fprintf(stderr, "Pre-flight OK.\n");
            return 0;
        }
    }

    /* ── Parse BDADDRs ────────────────────────────────────────────── */
    bdaddr_t peer;
    uint8_t peer_type = (uint8_t)parse_addr_type(peer_type_s);
    uint8_t own_type  = (uint8_t)parse_addr_type(own_type_s);

    if (!bdaddr_s && !auto_scan && !usb_init) {
        fprintf(stderr, "ERROR: --bdaddr or --auto-scan or --usb-init required\n");
        usage(argv[0]);
        return 2;
    }

    if (bdaddr_s) {
        if (str2ba(bdaddr_s, &peer) < 0) {
            fprintf(stderr, "ERROR: invalid BDADDR: %s\n", bdaddr_s);
            return 2;
        }
    } else {
        memset(&peer, 0, sizeof(peer));
    }

    /* Resolve host BDADDR for USB prep */
    uint8_t host_bytes[6] = {0};
    parse_host_bdaddr(host_bytes, host_bdaddr_s);

    /* ── USB init ─────────────────────────────────────────────────── */
    if (usb_init) {
        fprintf(stderr, "=== USB Prep ===\n");
        if (usb_prep_controller(hidraw_path, host_bytes) < 0)
            return 1;
        fprintf(stderr, "\n*** UNPLUG the controller from USB NOW ***\n");
        fprintf(stderr, "*** Waiting 8 seconds for BLE advertising to start... ***\n");
        fflush(stderr);
        sleep(8);
        /* After USB init, auto-scan is implied */
        auto_scan = 1;
    }

    /* ── hci0 reset ───────────────────────────────────────────────── */
    if (reset_hci) {
        fprintf(stderr, "=== Reset hci0 ===\n");
        int dev_id = hci_devid("hci0");
        if (dev_id >= 0) {
            int ctl = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
            if (ctl >= 0) {
                ioctl(ctl, HCIDEVDOWN, dev_id);
                usleep(500000);
                ioctl(ctl, HCIDEVUP, dev_id);
                usleep(500000);
                close(ctl);
            }
        }
    }

    /* ── Auto-scan for peer ───────────────────────────────────────── */
    /*
     * TODO: Raw HCI scan using LE Set Scan Parameters/Enable,
     * parsing advertising reports for Nintendo Manufacturer Data (0x057E).
     * For now, require explicit --bdaddr.
     */
    if (auto_scan && !bdaddr_s) {
        fprintf(stderr, "=== Auto-scan: (pending implementation, use --bdaddr for now) ===\n");
        /* Will implement raw scan here */
        if (scan_only) return 0;
    }

    if (!bdaddr_s && !auto_scan) {
        fprintf(stderr, "ERROR: no peer specified\n");
        return 2;
    }

    if (scan_only) {
        fprintf(stderr, "peer=%s type=%s\n", bdaddr_s ? bdaddr_s : "(scan)",
                addr_type_str(peer_type));
        return 0;
    }

    /* ── Ensure hci0 is up ────────────────────────────────────────── */
    int dev_id = hci_devid("hci0");
    if (dev_id < 0) {
        fprintf(stderr, "ERROR: no hci0 device\n");
        return 1;
    }

    if (!reset_hci) {
        int ctl = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
        if (ctl >= 0) {
            ioctl(ctl, HCIDEVUP, dev_id);
            close(ctl);
        }
    }

    /* ── Log info ─────────────────────────────────────────────────── */
    bdaddr_t local_ba;
    char local_str[18] = "unknown";
    if (hci_devba(dev_id, &local_ba) == 0)
        ba2str(&local_ba, local_str);

    fprintf(stderr, "=== %s ===\n", APP_NAME);
    fprintf(stderr, "local  BDADDR: %s\n", local_str);
    fprintf(stderr, "target BDADDR: %s  type=%s  own_type=%s\n",
            bdaddr_s ? bdaddr_s : "(auto)", addr_type_str(peer_type),
            addr_type_str(own_type));

    /* ── Open sockets ─────────────────────────────────────────────── */
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    setvbuf(stderr, NULL, _IOLBF, 0);

    /* Reconnect loop */
    int backoff = 1;
    while (g_running) {
        fprintf(stderr, "--- session start ---\n");

        int cmd_fd = open_cmd_socket();
        if (cmd_fd < 0) { sleep(1); continue; }

        int evt_fd = open_evt_socket();
        if (evt_fd < 0) { close(cmd_fd); sleep(1); continue; }

        if (setup_event_filter(evt_fd) < 0) {
            close(cmd_fd); close(evt_fd); sleep(1); continue;
        }

        uint16_t handle = 0;
        int rc = le_create_connection(cmd_fd, evt_fd, &peer,
                                      peer_type, own_type, &handle);

        if (rc == 0) {
            fprintf(stderr, "*** CONNECTED: handle=0x%04x ***\n", handle);
            /*
             * TODO: GATT discovery, CCCD, input reports, UHID
             * Will be implemented in the next phase.
             */
            fprintf(stderr, "  (GATT/UHID pending — connection established)\n");
            /* Keep connection alive briefly for verification */
            sleep(5);
        } else {
            fprintf(stderr, "session failed (rc=%d)\n", rc);
        }

        close(cmd_fd);
        close(evt_fd);

        if (!g_running) break;

        fprintf(stderr, "reconnecting in %ds...\n", backoff);
        sleep((unsigned)backoff);
        if (backoff < 16) backoff *= 2;
    }

    fprintf(stderr, "%s: stopped\n", APP_NAME);
    return 0;
}
