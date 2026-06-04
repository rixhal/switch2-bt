/* sw2d_final.c — Switch 2 Pro Controller BLE Daemon (Golden Path)
 *
 * SMP Golden Path: proven Just Works pairing with c1/s1 crypto,
 * Confirm/Random exchange, STK derivation, LE Encryption.
 *
 * Transport: HCI_CHANNEL_USER via hci_transport_user.c
 *   - hci0 brought DOWN via ioctl (kernel releases controller)
 *   - AF_BLUETOOTH SOCK_RAW + HCI_CHANNEL_USER = exclusive access
 *   - No kernel interference, no MGMT contamination
 *   - Multiple connect/disconnect cycles without CMD_STATUS 0x0c
 *   - LE Create Connection Cancel before retry on timeout
 *
 * Reference: BlueKitchen BTstack platform/linux/hci_transport_linux.c
 *
 * Build: gcc -O2 -s -Wall -o sw2d_final sw2d_final.c hci_transport_user.c \
 *            -lbluetooth -lcrypto
 */

#define _GNU_SOURCE
#include "hci_transport_user.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <linux/uhid.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <openssl/evp.h>

/* Alias transport frame type for brevity */
typedef hci_user_frame_t hci_frame_t;

/* ═══ constants (SMP/ATT/GATT — complementing transport header) ════════ */

/* LE Meta sub-events */
#define EVT_LE_CONN_UPDATE        0x03
#define EVT_LE_ADV_REPORT         0x02
#define EVT_LE_DATA_LEN_CHANGE    0x07
#define EVT_LE_LTK_REQUEST        0x05

/* OGF/OCF not in transport header */
#define OCF_LE_CONN_UPDATE        0x0013
#define OCF_LE_LTK_REQ_NEG_REPLY  0x001b
#define OCF_LE_SET_SCAN_PARAMS    0x000b
#define OCF_LE_SET_SCAN_ENABLE    0x000c

/* L2CAP CIDs */
#define ATT_CID             0x0004
#define L2CAP_LE_SIGNALING  0x0005
#define SMP_CID             0x0006

/* ATT opcodes */
#define ATT_OP_ERROR_RSP          0x01
#define ATT_OP_MTU_REQ            0x02
#define ATT_OP_MTU_RSP            0x03
#define ATT_OP_READ_BY_TYPE_REQ   0x08
#define ATT_OP_READ_BY_TYPE_RSP   0x09
#define ATT_OP_READ_BY_GROUP_REQ  0x10
#define ATT_OP_READ_BY_GROUP_RSP  0x11
#define ATT_OP_WRITE_REQ          0x12
#define ATT_OP_WRITE_RSP          0x13
#define ATT_OP_HANDLE_NOTIFY      0x1b

/* SMP opcodes */
#define SMP_OP_PAIRING_REQ        0x01
#define SMP_OP_PAIRING_RSP        0x02
#define SMP_OP_CONFIRM            0x03
#define SMP_OP_RANDOM             0x04
#define SMP_OP_PAIRING_FAILED     0x05
#define SMP_OP_SECURITY_REQUEST   0x0b

/* ═══ state machine ══════════════════════════════════════════════════════ */
typedef enum {
    STATE_DISCONNECTED,
    STATE_CONNECTED_UNENCRYPTED,
    STATE_SMP_PAIRING_SENT,
    STATE_SMP_CONFIRM_RANDOM,
    STATE_ENCRYPTION_START_SENT,
    STATE_ENCRYPTED,
    STATE_ATT_READY,
    STATE_GATT_READY,
    STATE_NOTIFICATIONS_ENABLED,
    STATE_UHID_RUNNING,
} session_state_t;

/* ═══ session context ════════════════════════════════════════════════════ */
typedef struct {
    int      hci_fd;        /* HCI_CHANNEL_USER socket from hci_user_open() */
    uint16_t conn_handle;
    session_state_t state;
    int      verbose;
    uint8_t  peer_addr_type;
    uint8_t  local_addr_type;
    uint8_t  peer_addr[6];
    uint8_t  local_addr[6];
    /* SMP */
    uint8_t  smp_tk[16];
    uint8_t  smp_preq[7];
    uint8_t  smp_pres[7];
    uint8_t  smp_mrand[16];
    uint8_t  smp_srand[16];
    uint8_t  smp_mconfirm[16];
    uint8_t  smp_sconfirm[16];
    uint8_t  smp_stk[16];
    /* ATT/GATT */
    uint16_t att_mtu;
    uint16_t svc_start, svc_end;
    uint16_t report_handle, report_cccd;
    /* disconnect tracking */
    uint8_t  last_disconnect_reason;
    /* retry: did we cancel before reconnect? */
    int      did_cancel;
} session_t;

/* ═══ globals ════════════════════════════════════════════════════════════ */
static volatile sig_atomic_t g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

/* ═══ helpers ════════════════════════════════════════════════════════════ */
static uint16_t get_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static void put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void hexdump(const char *label, const uint8_t *data, int len) {
    fprintf(stderr, "  %s (%d): ", label, len);
    for (int i = 0; i < len && i < 32; i++) fprintf(stderr, "%02x ", data[i]);
    if (len > 32) fprintf(stderr, "...");
    fprintf(stderr, "\n");
}

/* ═══ read BDADDR from /sys (works when hci0 is DOWN) ═══════════════════ */
static int read_local_bdaddr(uint8_t out[6]) {
    FILE *f = fopen("/sys/class/bluetooth/hci0/address", "r");
    if (!f) return -1;
    char line[32];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);
    line[strcspn(line, "\n")] = 0;
    unsigned int b[6];
    if (sscanf(line, "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[5], &b[4], &b[3], &b[2], &b[1], &b[0]) == 6) {
        for (int i = 0; i < 6; i++) out[i] = (uint8_t)b[i];
        return 0;
    }
    return -1;
}

/* ═══ LE Create Connection (wrapper over transport) ═════════════════════ */
static int le_create_conn(session_t *s) {
    uint8_t p[25];
    put_le16(p + 0, 0x0010);  /* scan interval (10ms) */
    put_le16(p + 2, 0x0010);  /* scan window (10ms) */
    p[4] = 0x00;               /* filter: no whitelist */
    p[5] = s->peer_addr_type;
    memcpy(p + 6, s->peer_addr, 6);
    p[12] = s->local_addr_type;
    put_le16(p + 13, 0x0028);  /* conn interval min (50ms) — Switch 2 sweet spot */
    put_le16(p + 15, 0x0038);  /* conn interval max (70ms) */
    put_le16(p + 17, 0x0000);  /* latency */
    put_le16(p + 19, 0x0C80);  /* supervision timeout (3.2s) */
    put_le16(p + 21, 0x0001);  /* min CE */
    put_le16(p + 23, 0x0001);  /* max CE */
    return hci_user_send_cmd(s->hci_fd, OGF_LE_CTL, OCF_LE_CREATE_CONN,
                             p, 25);
}

/* ═══ ACL send wrappers ═════════════════════════════════════════════════ */
static int smp_send(session_t *s, const uint8_t *data, uint16_t len) {
    return hci_user_acl_send(s->hci_fd, s->conn_handle, SMP_CID, data, len);
}

static int att_send(session_t *s, const uint8_t *data, uint16_t len) {
    return hci_user_acl_send(s->hci_fd, s->conn_handle, ATT_CID, data, len);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SMP: AES-128 + c1/s1 (PROVEN — DO NOT MODIFY WITHOUT TRACE COMPARISON)
 * ═══════════════════════════════════════════════════════════════════════════ */

static int aes_128(const uint8_t key[16], const uint8_t in[16],
                   uint8_t out[16]) {
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int ol = 0, tl = 0;
    EVP_EncryptInit_ex(c, EVP_aes_128_ecb(), NULL, key, NULL);
    EVP_CIPHER_CTX_set_padding(c, 0);
    EVP_EncryptUpdate(c, out, &ol, in, 16);
    EVP_EncryptFinal_ex(c, out + ol, &tl);
    EVP_CIPHER_CTX_free(c);
    return 0;
}

/*
 * SMP c1: Confirm value generation
 * BLUETOOTH CORE SPEC 5.4 Vol 3 Part H Section 2.2.6
 *
 * c1(k, r, preq, pres, iat, ia, rat, ra) = e(k, e(k, r XOR p1) XOR p2)
 * where p1 = pres || preq || rat || iat
 *       p2 = padding(4) || ia || ra
 */
static void smp_c1(const uint8_t k[16], const uint8_t r[16],
                   const uint8_t preq[7], const uint8_t pres[7],
                   uint8_t iat, const uint8_t ia[6],
                   uint8_t rat, const uint8_t ra[6],
                   uint8_t confirm[16]) {
    uint8_t p1[16], p2[16], tmp[16];
    memcpy(p1, pres, 7);
    memcpy(p1 + 7, preq, 7);
    p1[14] = rat;
    p1[15] = iat;
    for (int i = 0; i < 16; i++) tmp[i] = r[i] ^ p1[i];
    aes_128(k, tmp, tmp);
    memset(p2, 0, 4);
    memcpy(p2 + 4, ia, 6);
    memcpy(p2 + 10, ra, 6);
    for (int i = 0; i < 16; i++) tmp[i] ^= p2[i];
    aes_128(k, tmp, confirm);
}

/*
 * SMP s1: STK derivation
 * s1(k, r1, r2) = e(k, r1' || r2')
 */
static void smp_s1(const uint8_t k[16], const uint8_t r1[16],
                   const uint8_t r2[16], uint8_t stk[16]) {
    uint8_t d[16];
    memcpy(d, r1, 8);
    memcpy(d + 8, r2, 8);
    aes_128(k, d, stk);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * RX AWAIT — wait for specific events or ACL data
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Wait for a specific HCI event pattern.
 * Returns:  0 = got the event
 *          -1 = timeout
 *          -2 = disconnect (reason in s->last_disconnect_reason)
 */
static int rx_await(session_t *s, uint8_t evt, uint8_t sub,
                    uint8_t *out_data, uint16_t *out_len,
                    int timeout_ms) {
    int64_t deadline = now_ms() + timeout_ms;
    hci_frame_t f;

    while (g_running) {
        int64_t rem = deadline - now_ms();
        if (rem <= 0) {
            fprintf(stderr, "  [rx] await timeout evt=0x%02x sub=0x%02x\n",
                    evt, sub);
            return -1;
        }
        int rc = hci_user_read_frame(s->hci_fd, &f,
                                     (int)(rem > 500 ? 500 : rem));
        if (rc <= 0) continue;

        if (f.pkt_type == HCI_EVENT_PKT) {
            /* Always handle disconnects */
            if (f.evt_code == EVT_DISCONN_COMPLETE) {
                s->last_disconnect_reason = f.status;
                fprintf(stderr, "  [rx] DISCONNECT handle=0x%04x reason=0x%02x\n",
                        f.handle, f.status);
                return -2;
            }
            /* Log CMD_STATUS for debugging */
            if (f.evt_code == EVT_CMD_STATUS) {
                if (s->verbose)
                    fprintf(stderr, "  [rx] CMD_STATUS status=0x%02x opcode=0x%04x\n",
                            f.status, f.opcode);
                /* Detect Command Disallowed — suggest cancel */
                if (f.status == 0x0c && f.opcode == 0x200d) {
                    fprintf(stderr, "  [rx] *** CMD_STATUS 0x0c: LE Create "
                                    "Connection DISALLOWED ***\n");
                    fprintf(stderr, "  [rx] *** Previous connect still "
                                    "pending in controller. ***\n");
                    fprintf(stderr, "  [rx] *** Need "
                                    "LE_Create_Connection_Cancel before "
                                    "retry. ***\n");
                }
            }
            /* Auto-detect encryption success */
            if (f.evt_code == EVT_ENCRYPT_CHANGE && f.status == 0x00 &&
                f.handle == s->conn_handle) {
                s->state = STATE_ENCRYPTED;
                fprintf(stderr, "  [rx] ENCRYPT_CHANGE success ✓\n");
            }
            /* Extract conn_handle from LE Connection Complete */
            if (f.evt_code == EVT_LE_META &&
                f.subevent == EVT_LE_CONN_COMPLETE &&
                f.payload_len >= 18) {
                uint8_t st = f.payload[1];
                uint16_t h  = get_le16(f.payload + 2) & 0x0fff;
                uint16_t ci = get_le16(f.payload + 12);
                /* Raw hexdump + diagnostics */
                fprintf(stderr, "  [rx] LE_CONN_COMPLETE st=0x%02x handle=0x%04x "
                                "ci=%.2fms raw[0..18]: ",
                        st, h, ci * 1.25);
                for (int i = 0; i < 19 && i < f.payload_len; i++)
                    fprintf(stderr, "%02x ", f.payload[i]);
                fprintf(stderr, "\n");
                if (st == 0x00) {
                    s->conn_handle = h;
                    fprintf(stderr, "  [rx] LE_CONN_COMPLETE SUCCESS "
                                    "handle=0x%04x interval=%.2fms\n",
                            s->conn_handle, ci * 1.25);
                } else {
                    fprintf(stderr, "  [rx] LE_CONN_COMPLETE FAILED "
                                    "status=0x%02x\n", st);
                }
            }
            /* Check if this matches our awaited event */
            int match = 0;
            if (evt == EVT_LE_META)
                match = (f.evt_code == EVT_LE_META && f.subevent == sub);
            else
                match = (f.evt_code == evt);
            if (match) {
                if (out_data && f.payload_len > 0) {
                    size_t cp = f.payload_len < *out_len ? f.payload_len
                                                          : *out_len;
                    memcpy(out_data, f.payload, cp);
                    *out_len = (uint16_t)cp;
                }
                return 0;
            }
            continue;
        }
    }
    return -1;
}

/*
 * Wait for ACL data on a specific CID.
 * Returns:  0 = got data (in out/out_len)
 *          -1 = timeout
 *          -2 = disconnect
 */
static int rx_await_acl(session_t *s, uint16_t cid, uint8_t *out,
                        uint16_t *out_len, int timeout_ms) {
    int64_t deadline = now_ms() + timeout_ms;
    hci_frame_t f;

    while (g_running) {
        int64_t rem = deadline - now_ms();
        if (rem <= 0) return -1;

        int rc = hci_user_read_frame(s->hci_fd, &f,
                                     (int)(rem > 500 ? 500 : rem));
        if (rc <= 0) continue;

        if (f.pkt_type == HCI_EVENT_PKT) {
            if (f.evt_code == EVT_DISCONN_COMPLETE) {
                s->last_disconnect_reason = f.status;
                fprintf(stderr, "  [rx] DISCONNECT reason=0x%02x\n",
                        f.status);
                return -2;
            }
            if (f.evt_code == EVT_ENCRYPT_CHANGE && f.status == 0x00)
                s->state = STATE_ENCRYPTED;
            if (s->verbose)
                fprintf(stderr, "  [rx] %s while waiting for ACL\n",
                        f.evt_code == EVT_LE_META ? "LE_META" : "EVENT");
            continue;
        }

        if (f.pkt_type == HCI_ACLDATA_PKT && f.handle == s->conn_handle &&
            f.cid == cid) {
            if (out && out_len && f.payload_len <= *out_len) {
                memcpy(out, f.payload, f.payload_len);
                *out_len = f.payload_len;
            }
            return 0;
        }
    }
    return -1;
}

/* ═══ USB prep with auto host BDADDR ══════════════════════════════════════ */
static int usb_prep(const char *hidraw, const uint8_t host[6]) {
    int fd = open(hidraw, O_RDWR);
    if (fd < 0) { perror(hidraw); return -1; }

    uint8_t r[64];

    /* SetHCIState */
    memset(r, 0, sizeof(r));
    r[0] = 0x01; r[1] = 0x01; r[10] = 0x06; r[11] = 0x01;
    if (write(fd, r, sizeof(r)) < 0) {
        perror("SetHCIState"); close(fd); return -1;
    }
    fprintf(stderr, "  SetHCIState sent\n");

    /* SetShipment */
    memset(r, 0, sizeof(r));
    r[0] = 0x01; r[1] = 0x02; r[10] = 0x08; r[11] = 0x00;
    if (write(fd, r, sizeof(r)) < 0) {
        perror("SetShipment"); close(fd); return -1;
    }
    fprintf(stderr, "  SetShipment sent\n");

    /* BluetoothManualPair */
    memset(r, 0, sizeof(r));
    r[0] = 0x01; r[1] = 0x03; r[10] = 0x01;
    memcpy(r + 11, host, 6);
    if (write(fd, r, sizeof(r)) < 0) {
        perror("BluetoothManualPair"); close(fd); return -1;
    }
    fprintf(stderr, "  BluetoothManualPair "
                    "(host=%02x:%02x:%02x:%02x:%02x:%02x)\n",
            host[5], host[4], host[3], host[2], host[1], host[0]);
    close(fd);
    return 0;
}

/* ═══ BlueZ ownership guard ═══════════════════════════════════════════════ */
static int check_bluez_clients(void) {
    int count = 0;
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_type != DT_DIR) continue;
        char path[256], buf[256];
        snprintf(path, sizeof(path), "/proc/%s/comm", ent->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(buf, sizeof(buf), f)) {
            buf[strcspn(buf, "\n")] = 0;
            if (!strcmp(buf, "bluetoothd") || !strcmp(buf, "bluetoothctl") ||
                !strcmp(buf, "btmgmt") || !strcmp(buf, "gatttool") ||
                !strcmp(buf, "hcitool") ||
                strstr(buf, "python") || strstr(buf, "bleak")) {
                fprintf(stderr, "  CONFLICT: %s (pid=%s)\n", buf, ent->d_name);
                count++;
            }
        }
        fclose(f);
    }
    closedir(d);
    return count;
}

static void kill_bluez_clients(void) {
    system("killall -9 bluetoothd bluetoothctl btmgmt gatttool hcitool "
           "2>/dev/null");
    system("systemctl stop bluetooth 2>/dev/null");
    usleep(500000);
}

static int file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[512];
    int found = 0;
    while (fgets(buf, sizeof(buf), f)) {
        if (strstr(buf, needle)) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static int command_exists(const char *cmd) {
    char line[128];
    snprintf(line, sizeof(line), "command -v %s >/dev/null 2>&1", cmd);
    return system(line) == 0;
}

static int path_exists(const char *path) {
    return access(path, F_OK) == 0;
}

static int run_preflight(const char *hidraw, int exclusive) {
    int rc = 0;
    uint8_t local[6];

    fprintf(stderr, "=== Preflight ===\n");

    if (geteuid() == 0) {
        fprintf(stderr, "  uid: root OK\n");
    } else {
        fprintf(stderr, "  WARN: not running as root; raw HCI/UHID will likely fail\n");
        rc = 1;
    }

    if (path_exists("/sys/class/bluetooth/hci0")) {
        fprintf(stderr, "  hci0: present\n");
    } else {
        fprintf(stderr, "  ERROR: /sys/class/bluetooth/hci0 missing\n");
        rc = 1;
    }

    if (read_local_bdaddr(local) == 0) {
        fprintf(stderr, "  host BDADDR: %02x:%02x:%02x:%02x:%02x:%02x\n",
                local[5], local[4], local[3], local[2], local[1], local[0]);
    } else {
        fprintf(stderr, "  WARN: could not read host BDADDR from sysfs\n");
    }

    if (path_exists("/dev/uhid")) {
        fprintf(stderr, "  /dev/uhid: present\n");
    } else {
        fprintf(stderr, "  WARN: /dev/uhid missing; UHID input creation will fail\n");
    }

    if (path_exists(hidraw)) {
        fprintf(stderr, "  %s: present\n", hidraw);
    } else {
        fprintf(stderr, "  NOTE: %s missing; OK if not using --usb-init yet\n", hidraw);
    }

    fprintf(stderr, "  btmon: %s\n", command_exists("btmon") ? "present" : "missing");
    fprintf(stderr, "  hciconfig: %s\n", command_exists("hciconfig") ? "present" : "missing");

    int clients = check_bluez_clients();
    if (clients > 0) {
        fprintf(stderr, "  WARN: %d possible Bluetooth client(s) running\n", clients);
        if (exclusive) rc = 1;
    } else if (clients == 0) {
        fprintf(stderr, "  BlueZ clients: none detected\n");
    } else {
        fprintf(stderr, "  WARN: could not inspect /proc for BlueZ clients\n");
    }

    if (file_contains("/proc/cmdline", "bluetooth.disable_mgmt=1")) {
        fprintf(stderr, "  kernel mgmt: bluetooth.disable_mgmt=1 set\n");
    } else {
        fprintf(stderr, "  NOTE: bluetooth.disable_mgmt=1 not seen in /proc/cmdline\n");
        fprintf(stderr, "        If HCI_CHANNEL_USER misses connection events on Pi, test this kernel arg.\n");
    }

    fprintf(stderr, "Preflight %s\n", rc ? "completed with warnings" : "OK");
    return rc;
}

/* ═══ usage ═══════════════════════════════════════════════════════════════ */
static void usage(const char *p) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n\n"
        "Options:\n"
        "  --bdaddr AA:BB:CC:DD:EE:FF   Controller BDADDR\n"
        "  --peer-addr-type public|random|auto\n"
        "  --own-addr-type public|random\n"
        "  --host-bdaddr auto|AA:BB:..   Host BDADDR for USB prep\n"
        "  --auto-scan                   Scan for Switch 2 advertisers\n"
        "  --scan-only                   Scan, print, exit\n"
        "  --usb-init                    Run USB init sequence first\n"
        "  --hidraw PATH                 Hidraw device (default: /dev/hidraw0)\n"
        "  --send-security-request       Send SMP Security Request before Pairing Request\n"
        "  --exclusive-hci               Check no other BT clients running\n"
        "  --kill-bluez-clients          Kill interfering BT processes\n"
        "  --preflight                   Only check, then exit\n"
        "  --verbose                     Verbose output\n"
        "  --scan-timeout MS             Scan timeout (default 10000)\n"
        "\n"
        "Transport: HCI_CHANNEL_USER (hci_transport_user.c)\n"
        "  - ioctl HCIDEVDOWN before socket open\n"
        "  - Single AF_BLUETOOTH SOCK_RAW with HCI_CHANNEL_USER\n"
        "  - Exclusive controller access, no kernel interference\n"
        "  - LE Create Connection Cancel on retry (no CMD_STATUS 0x0c)\n"
        "\n"
        "SMP: Golden Path Just Works (proven c1/s1 crypto)\n"
        "  - Pairing Request → Pairing Response → Confirm → Random\n"
        "  - STK derivation → LE Start Encryption\n"
        "  - ATT/GATT only after ENCRYPTED state\n",
        p);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════════ */
int main(int argc, char **argv) {
    const char *bdaddr_s = NULL, *hidraw = "/dev/hidraw0";
    const char *host_spec = "auto", *peer_type_s = "public",
               *own_type_s = "public";
    int usb_init = 0, auto_scan = 0, scan_only = 0, exclusive = 0;
    int kill_bluez = 0, preflight = 0, verbose = 0, send_security_req = 0;
    int scan_timeout = 10000;
    uint8_t peer[6] = {0}, host_bytes[6] = {0};
    uint8_t peer_type = LE_PUBLIC_ADDRESS, own_type = LE_PUBLIC_ADDRESS;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bdaddr") && i + 1 < argc)
            bdaddr_s = argv[++i];
        else if (!strcmp(argv[i], "--peer-addr-type") && i + 1 < argc)
            peer_type_s = argv[++i];
        else if (!strcmp(argv[i], "--own-addr-type") && i + 1 < argc)
            own_type_s = argv[++i];
        else if (!strcmp(argv[i], "--host-bdaddr") && i + 1 < argc)
            host_spec = argv[++i];
        else if (!strcmp(argv[i], "--hidraw") && i + 1 < argc)
            hidraw = argv[++i];
        else if (!strcmp(argv[i], "--scan-timeout") && i + 1 < argc)
            scan_timeout = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--auto-scan"))
            auto_scan = 1;
        else if (!strcmp(argv[i], "--scan-only"))
            scan_only = 1;
        else if (!strcmp(argv[i], "--usb-init"))
            usb_init = 1;
        else if (!strcmp(argv[i], "--send-security-request"))
            send_security_req = 1;
        else if (!strcmp(argv[i], "--exclusive-hci"))
            exclusive = 1;
        else if (!strcmp(argv[i], "--kill-bluez-clients"))
            kill_bluez = 1;
        else if (!strcmp(argv[i], "--preflight"))
            preflight = 1;
        else if (!strcmp(argv[i], "--verbose"))
            verbose = 1;
        else { usage(argv[0]); return 2; }
    }

    if (!strcmp(peer_type_s, "random"))
        peer_type = LE_RANDOM_ADDRESS;
    else if (!strcmp(peer_type_s, "auto")) {
        peer_type = LE_RANDOM_ADDRESS; auto_scan = 1;
    }
    if (!strcmp(own_type_s, "random")) own_type = LE_RANDOM_ADDRESS;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    setvbuf(stderr, NULL, _IOLBF, 0);

    if (preflight)
        return run_preflight(hidraw, exclusive);

    /* ── BlueZ guard ─────────────────────────────────────────────── */
    if (exclusive || kill_bluez) {
        int c = check_bluez_clients();
        if (c > 0 && kill_bluez) { kill_bluez_clients(); c = check_bluez_clients(); }
        if (c > 0 && exclusive) {
            fprintf(stderr, "ERROR: %d BT client(s) running. Use --kill-bluez-clients.\n", c);
            return 1;
        }
    }

    /* ── Resolve BDADDR ─────────────────────────────────────────── */
    if (bdaddr_s && str2ba(bdaddr_s, (bdaddr_t *)peer) < 0) {
        fprintf(stderr, "ERROR: invalid BDADDR: %s\n", bdaddr_s); return 2;
    }

    /* ── Resolve host BDADDR ────────────────────────────────────── */
    {
        uint8_t lb[6];
        if (!strcmp(host_spec, "auto")) {
            if (read_local_bdaddr(lb) == 0) {
                memcpy(host_bytes, lb, 6);
                fprintf(stderr, "host BDADDR (sysfs): %02x:%02x:%02x:%02x:%02x:%02x\n",
                        host_bytes[5], host_bytes[4], host_bytes[3],
                        host_bytes[2], host_bytes[1], host_bytes[0]);
            } else {
                uint8_t fb[6] = {0xD8, 0x3A, 0xDD, 0xE5, 0x69, 0xED};
                memcpy(host_bytes, fb, 6);
                fprintf(stderr, "host BDADDR (fallback): D8:3A:DD:E5:69:ED\n");
            }
        } else {
            if (str2ba(host_spec, (bdaddr_t *)host_bytes) < 0) {
                fprintf(stderr, "ERROR: invalid host BDADDR\n"); return 2;
            }
        }
    }

    /* ── USB Init ────────────────────────────────────────────────── */
    if (usb_init) {
        fprintf(stderr, "=== USB Prep ===\n");
        if (usb_prep(hidraw, host_bytes) < 0) return 1;
        fprintf(stderr, "\n*** UNPLUG controller from USB NOW ***\n");
        fprintf(stderr, "*** Waiting 8s for BLE advertising... ***\n");
        sleep(8);
        auto_scan = 1;
    }

    /* ── Auto-scan (libbluetooth, needs hci0 UP briefly) ─────────── */
    if (auto_scan && !bdaddr_s) {
        system("hciconfig hci0 up 2>/dev/null");
        usleep(200000);

        int dev_id = hci_get_route(NULL);
        if (dev_id < 0) dev_id = 0;

        int scan_fd = hci_open_dev(dev_id);
        if (scan_fd >= 0) {
            struct hci_filter of, nf;
            socklen_t flen = sizeof(of);
            getsockopt(scan_fd, SOL_HCI, HCI_FILTER, &of, &flen);
            hci_filter_clear(&nf);
            hci_filter_set_ptype(HCI_EVENT_PKT, &nf);
            hci_filter_set_event(EVT_LE_META, &nf);
            setsockopt(scan_fd, SOL_HCI, HCI_FILTER, &nf, sizeof(nf));

            /* Passive scan (CYW43455 rejects active) */
            hci_le_set_scan_parameters(scan_fd, 0x00, 0x0010, 0x0010,
                                       LE_PUBLIC_ADDRESS, 0x00, 1000);
            hci_le_set_scan_enable(scan_fd, 0x01, 0x00, 1000);

            fprintf(stderr, "=== Scanning for Nintendo advertisers ===\n");
            int64_t deadline = now_ms() + scan_timeout;
            int found = 0;

            while (g_running && !found && now_ms() < deadline) {
                struct pollfd pfd = {.fd = scan_fd, .events = POLLIN};
                if (poll(&pfd, 1, 500) <= 0) continue;

                uint8_t buf[256];
                int n = (int)read(scan_fd, buf, sizeof(buf));
                if (n < 4 || buf[0] != HCI_EVENT_PKT || buf[1] != EVT_LE_META)
                    continue;

                const uint8_t *ev = buf + 3;
                uint8_t plen = buf[2];
                if (plen < 3 || ev[0] != EVT_LE_ADV_REPORT) continue;

                uint8_t nrep = ev[1];
                const uint8_t *rep = ev + 2;
                for (int ri = 0; ri < nrep && (rep - ev) + 9 <= plen; ri++) {
                    uint8_t atype = rep[1];
                    memcpy(peer, rep + 2, 6);
                    uint8_t dlen = rep[8];
                    for (int di = 0; di + 2 < dlen;) {
                        uint8_t el = rep[9 + di], ty = rep[10 + di];
                        if (!el || di + 1 + (int)el > dlen) break;
                        if (ty == 0xFF && el >= 3) {
                            uint16_t cid = rep[11+di] | (rep[12+di] << 8);
                            /* Nintendo: 0x057E (BT SIG) or 0x0553 (observed) */
                            if (cid == 0x057E || cid == 0x0553) {
                                char a[18]; ba2str((bdaddr_t *)peer, a);
                                fprintf(stderr, "  Found: %s type=%s (Nintendo 0x%04x)\n",
                                        a, atype == LE_RANDOM_ADDRESS ? "random" : "public", cid);
                                peer_type = atype;
                                found = 1;
                                break;
                            }
                        }
                        di += 1 + el;
                    }
                    if (found) break;
                    rep += 9 + dlen;
                }
            }
            hci_le_set_scan_enable(scan_fd, 0x00, 0x00, 1000);
            setsockopt(scan_fd, SOL_HCI, HCI_FILTER, &of, sizeof(of));
            close(scan_fd);

            if (!found) {
                fprintf(stderr, "No Switch 2 controller found in scan.\n");
                if (scan_only) return 1;
            }
        }

        /* Bring hci0 back DOWN for USER channel */
        system("hciconfig hci0 down 2>/dev/null");
        usleep(200000);

        if (!peer[0] && !peer[1] && !peer[2] && !peer[3] && !peer[4] && !peer[5]) {
            fprintf(stderr, "ERROR: no peer address found\n");
            return 1;
        }
        char a[18]; ba2str((bdaddr_t *)peer, a);
        fprintf(stderr, "peer: %s type=%s\n", a,
                peer_type == LE_RANDOM_ADDRESS ? "random" : "public");
        if (scan_only) return 0;
    }

    /* ═══════════════════════════════════════════════════════════════
     * TRANSPORT: Open HCI_CHANNEL_USER socket
     *   hci_user_open() brings hci0 DOWN via ioctl, opens USER socket
     *   hci_user_init() sends HCI_Reset + LE Set Event Mask
     * ═══════════════════════════════════════════════════════════════ */
    int hci_fd = hci_user_open();
    if (hci_fd < 0) return 1;

    if (hci_user_init(hci_fd) < 0) {
        hci_user_close(hci_fd);
        return 1;
    }

    /* ── Session context ──────────────────────────────────────────── */
    session_t s;
    memset(&s, 0, sizeof(s));
    s.hci_fd = hci_fd;
    s.state = STATE_DISCONNECTED;
    s.verbose = verbose;
    s.peer_addr_type = peer_type;
    s.local_addr_type = own_type;
    memcpy(s.peer_addr, peer, 6);
    memcpy(s.local_addr, host_bytes, 6);
    memset(s.smp_tk, 0, 16); /* TK = 0 for Just Works */

    fprintf(stderr, "=== sw2d_final (HCI_CHANNEL_USER transport) ===\n");
    {
        char lstr[18], pstr[18];
        ba2str((bdaddr_t *)s.local_addr, lstr);
        ba2str((bdaddr_t *)peer, pstr);
        fprintf(stderr, "local:  %s\npeer:   %s type=%s\nsocket: HCI_CHANNEL_USER\n",
                lstr, pstr,
                peer_type == LE_RANDOM_ADDRESS ? "random" : "public");
    }

    /* ═══════════════════════════════════════════════════════════════
     * MAIN RECONNECT LOOP
     *
     * KEY FIX (v3 transport):
     *   Before retrying LE Create Connection, we call
     *   hci_user_cancel_connect() to cancel any pending connect
     *   in the controller. Without this, the controller rejects
     *   a second LE Create Connection with CMD_STATUS 0x0c
     *   ("Command Disallowed").
     *
     *   This was NOT needed with HCI_CHANNEL_RAW because the kernel
     *   managed controller state — but also rejected retries itself.
     *   With HCI_CHANNEL_USER, WE manage controller state, so WE
     *   must cancel explicitly.
     * ═══════════════════════════════════════════════════════════════ */
    int backoff = 1;
    while (g_running) {
        fprintf(stderr, "──────────────── session start ────────────────\n");
        s.state = STATE_DISCONNECTED;
        s.conn_handle = 0;
        s.att_mtu = 0;
        s.last_disconnect_reason = 0;

        /*
         * If previous attempt left a pending connect in the controller,
         * cancel it before trying again. We detect this by checking
         * if we had a timeout or disconnect in the last cycle.
         */
        if (s.did_cancel || s.last_disconnect_reason) {
            hci_user_cancel_connect(s.hci_fd);
            usleep(50000);
        }
        s.did_cancel = 1; /* assume we'll need cancel next time unless success */

        /* ── Phase 1: LE Create Connection ────────────────────────── */
        if (le_create_conn(&s) < 0) goto reconnect;
        {
            int rc = rx_await(&s, EVT_LE_META, EVT_LE_CONN_COMPLETE,
                              NULL, NULL, 10000);
            if (rc != 0) {
                fprintf(stderr, "Connection failed (rc=%d)\n", rc);
                goto reconnect;
            }
            /* conn_handle may be 0x0000 — valid per BT spec */
            fprintf(stderr, "  connection handle: 0x%04x\n", s.conn_handle);
        }
        s.state = STATE_CONNECTED_UNENCRYPTED;
        fprintf(stderr, "*** CONNECTED handle=0x%04x ***\n", s.conn_handle);

        /* ── Phase 2: SMP Golden Path (PROVEN — byte-identical) ────── */
        fprintf(stderr, "=== SMP Just Works Pairing ===\n");

        /* 2a: Optional Security Request.
         * In normal BLE SMP, the peripheral sends Security Request and the
         * central initiates pairing with Pairing Request. Keep this off by
         * default, but allow trace experiments with old behavior. */
        if (send_security_req) {
            uint8_t pdu[2] = {SMP_OP_SECURITY_REQUEST, 0x01};
            if (smp_send(&s, pdu, 2) < 0) goto reconnect;
            fprintf(stderr, "  Security Request sent\n");
        }

        /* 2b: Pairing Request */
        {
            uint8_t preq[] = {SMP_OP_PAIRING_REQ, 0x03, 0x00, 0x01, 0x10, 0x03, 0x03};
            memcpy(s.smp_preq, preq, sizeof(preq));
            if (smp_send(&s, preq, sizeof(preq)) < 0) goto reconnect;
            s.state = STATE_SMP_PAIRING_SENT;
            fprintf(stderr, "  Pairing Request sent\n");
            if (verbose) hexdump("pairing_req", preq, sizeof(preq));
        }

        /* 2c: Wait for Pairing Response */
        {
            uint8_t buf[32]; uint16_t blen = sizeof(buf);
            int rc = rx_await_acl(&s, SMP_CID, buf, &blen, 5000);
            if (rc == -2) {
                fprintf(stderr, "  Disconnect (0x%02x) before Pairing Response\n",
                        s.last_disconnect_reason);
                goto reconnect;
            }
            if (rc != 0 || blen < 7 || buf[0] != SMP_OP_PAIRING_RSP) {
                if (blen >= 2 && buf[0] == SMP_OP_PAIRING_FAILED) {
                    fprintf(stderr, "  SMP Pairing Failed reason=0x%02x\n", buf[1]);
                    goto reconnect;
                }
                /* Maybe security request echo — retry once */
                if (blen >= 2 && buf[0] == SMP_OP_SECURITY_REQUEST) {
                    fprintf(stderr, "  (Controller security request echo)\n");
                    blen = sizeof(buf);
                    rc = rx_await_acl(&s, SMP_CID, buf, &blen, 5000);
                    if (rc != 0 || blen < 7 || buf[0] != SMP_OP_PAIRING_RSP) {
                        if (blen >= 2 && buf[0] == SMP_OP_PAIRING_FAILED)
                            fprintf(stderr, "  SMP Pairing Failed reason=0x%02x\n", buf[1]);
                        fprintf(stderr, "  No Pairing Response after sec req\n");
                        goto reconnect;
                    }
                } else {
                    fprintf(stderr, "  No SMP Pairing Response (rc=%d, len=%u)\n", rc, blen);
                    goto reconnect;
                }
            }
            memcpy(s.smp_pres, buf, 7);
            fprintf(stderr, "  Pairing Response: IO=0x%02x AuthReq=0x%02x MaxKey=%u\n",
                    buf[1], buf[3], buf[4]);
            s.state = STATE_SMP_CONFIRM_RANDOM;
        }

        /* 2d: Generate Mrand → Mconfirm (c1) */
        {
            for (int i = 0; i < 16; i++)
                s.smp_mrand[i] = (uint8_t)(rand() & 0xFF);
            smp_c1(s.smp_tk, s.smp_mrand, s.smp_preq, s.smp_pres,
                   s.local_addr_type, s.local_addr,
                   s.peer_addr_type, s.peer_addr,
                   s.smp_mconfirm);
            if (verbose) hexdump("Mconfirm", s.smp_mconfirm, 16);
        }

        /* 2e: Master sends Confirm */
        {
            uint8_t c[17]; c[0] = SMP_OP_CONFIRM; memcpy(c + 1, s.smp_mconfirm, 16);
            if (smp_send(&s, c, 17) < 0) goto reconnect;
            fprintf(stderr, "  Confirm sent\n");
        }

        /* 2f: Read Slave Confirm */
        {
            uint8_t buf[32]; uint16_t blen = sizeof(buf);
            int rc = rx_await_acl(&s, SMP_CID, buf, &blen, 5000);
            if (rc == -2) goto reconnect;
            if (rc != 0 || blen != 17 || buf[0] != SMP_OP_CONFIRM) {
                fprintf(stderr, "  No slave confirm (rc=%d, len=%u, op=0x%02x)\n",
                        rc, blen, blen > 0 ? buf[0] : 0);
                goto reconnect;
            }
            memcpy(s.smp_sconfirm, buf + 1, 16);
        }

        /* 2g: Master sends Random */
        {
            uint8_t r[17]; r[0] = SMP_OP_RANDOM; memcpy(r + 1, s.smp_mrand, 16);
            if (smp_send(&s, r, 17) < 0) goto reconnect;
            fprintf(stderr, "  Random sent\n");
        }

        /* 2h: Read Slave Random */
        {
            uint8_t buf[32]; uint16_t blen = sizeof(buf);
            int rc = rx_await_acl(&s, SMP_CID, buf, &blen, 5000);
            if (rc == -2) goto reconnect;
            if (rc != 0 || blen != 17 || buf[0] != SMP_OP_RANDOM) {
                fprintf(stderr, "  No slave random (rc=%d, len=%u, op=0x%02x)\n",
                        rc, blen, blen > 0 ? buf[0] : 0);
                goto reconnect;
            }
            memcpy(s.smp_srand, buf + 1, 16);
        }

        /* 2i: Verify slave confirm */
        {
            uint8_t expected[16];
            smp_c1(s.smp_tk, s.smp_srand, s.smp_preq, s.smp_pres,
                   s.peer_addr_type, s.peer_addr, s.local_addr_type, s.local_addr,
                   expected);
            if (memcmp(expected, s.smp_sconfirm, 16) != 0) {
                fprintf(stderr, "  CONFIRM MISMATCH — abort\n");
                if (verbose) { hexdump("expected", expected, 16); hexdump("received", s.smp_sconfirm, 16); }
                goto reconnect;
            }
            fprintf(stderr, "  Confirm MATCH ✓\n");
        }

        /* 2j: Derive STK */
        smp_s1(s.smp_tk, s.smp_mrand, s.smp_srand, s.smp_stk);
        if (verbose) hexdump("STK", s.smp_stk, 16);

        /* ── Phase 3: LE Start Encryption ──────────────────────────── */
        {
            uint8_t enc[28];
            put_le16(enc + 0, s.conn_handle);
            memset(enc + 2, 0, 8);
            put_le16(enc + 10, 0);
            memcpy(enc + 12, s.smp_stk, 16);
            if (hci_user_send_cmd(hci_fd, OGF_LE_CTL, OCF_LE_START_ENCRYPTION,
                                  enc, 28) < 0)
                goto reconnect;
            s.state = STATE_ENCRYPTION_START_SENT;
            fprintf(stderr, "  LE Start Encryption sent\n");
        }

        /* 3a: Wait for Encryption Change + handle LTK Request */
        {
            int encrypted = 0;
            int64_t dl = now_ms() + 10000;
            while (now_ms() < dl && g_running && !encrypted) {
                hci_frame_t f;
                int rc = hci_user_read_frame(hci_fd, &f, 500);
                if (rc <= 0) continue;

                if (f.pkt_type == HCI_EVENT_PKT) {
                    if (f.evt_code == EVT_DISCONN_COMPLETE) {
                        fprintf(stderr, "  Disconnect (0x%02x) during encryption\n", f.status);
                        goto reconnect;
                    }
                    if (f.evt_code == EVT_ENCRYPT_CHANGE) {
                        fprintf(stderr, "  Encryption change: status=0x%02x handle=0x%04x\n",
                                f.status, f.handle);
                        if (f.status == 0x00 && f.handle == s.conn_handle) {
                            encrypted = 1;
                        }
                    }
                    if (f.evt_code == EVT_LE_META &&
                        f.subevent == EVT_LE_LTK_REQUEST &&
                        f.payload_len >= 13) {
                        uint16_t h = get_le16(f.payload + 1) & 0x0fff;
                        fprintf(stderr, "  LTK Request handle=0x%04x — replying\n", h);
                        uint8_t ltk_reply[18];
                        put_le16(ltk_reply, h);
                        memcpy(ltk_reply + 2, s.smp_stk, 16);
                        hci_user_send_cmd(hci_fd, OGF_LE_CTL, OCF_LE_LTK_REQ_REPLY,
                                          ltk_reply, 18);
                    }
                }
            }
            if (!encrypted) {
                fprintf(stderr, "  Encryption failed\n");
                goto reconnect;
            }
            s.state = STATE_ENCRYPTED;
            fprintf(stderr, "*** ENCRYPTED ***\n");
        }

        /* Connection cycle succeeded — no cancel needed next time */
        s.did_cancel = 0;

        /* ── Phase 4: ATT/GATT after encryption ────────────────────── */
        fprintf(stderr, "=== ATT/GATT ===\n");

        /* 4a: ATT MTU Exchange */
        {
            uint8_t mtu[] = {ATT_OP_MTU_REQ, 0xFF, 0x00};
            if (att_send(&s, mtu, sizeof(mtu)) < 0) goto reconnect;
            fprintf(stderr, "  ATT MTU req sent\n");
            uint8_t buf[32]; uint16_t blen = sizeof(buf);
            int rc = rx_await_acl(&s, ATT_CID, buf, &blen, 2000);
            if (rc == 0 && blen >= 3 && buf[0] == ATT_OP_MTU_RSP) {
                s.att_mtu = get_le16(buf + 1);
                fprintf(stderr, "  ATT MTU: %u\n", s.att_mtu);
            } else {
                s.att_mtu = 23;
                fprintf(stderr, "  ATT MTU: default %u\n", s.att_mtu);
            }
            s.state = STATE_ATT_READY;
        }

        /* 4b: GATT discover primary services */
        uint16_t svc_start = 0, svc_end = 0;
        {
            uint8_t req[] = {ATT_OP_READ_BY_GROUP_REQ, 0x01, 0x00, 0xFF, 0xFF, 0x00, 0x28};
            if (att_send(&s, req, sizeof(req)) < 0) goto reconnect;
            uint8_t buf[64]; uint16_t blen = sizeof(buf);
            int rc = rx_await_acl(&s, ATT_CID, buf, &blen, 3000);
            if (rc == 0 && blen >= 6 && buf[0] == ATT_OP_READ_BY_GROUP_RSP) {
                svc_start = get_le16(buf + 2);
                svc_end = get_le16(buf + 4);
                fprintf(stderr, "  Service: 0x%04x-0x%04x\n", svc_start, svc_end);
            } else {
                svc_start = 0x0001; svc_end = 0xFFFF;
                fprintf(stderr, "  Using full range for char discovery\n");
            }
            s.svc_start = svc_start; s.svc_end = svc_end;
        }

        /* 4c: GATT discover characteristics */
        {
            uint8_t req[] = {ATT_OP_READ_BY_TYPE_REQ,
                             (uint8_t)(svc_start & 0xFF), (uint8_t)(svc_start >> 8),
                             (uint8_t)(svc_end & 0xFF), (uint8_t)(svc_end >> 8),
                             0x03, 0x28};
            if (att_send(&s, req, sizeof(req)) < 0) goto reconnect;
            int64_t dl = now_ms() + 3000;
            while (now_ms() < dl && g_running) {
                uint8_t buf[64]; uint16_t blen = sizeof(buf);
                int rc = rx_await_acl(&s, ATT_CID, buf, &blen, (int)(dl - now_ms()));
                if (rc == -2) goto reconnect;
                if (rc != 0) continue;
                if (buf[0] == ATT_OP_READ_BY_TYPE_RSP && blen >= 7) {
                    uint8_t props = buf[4];
                    uint16_t val_handle = get_le16(buf + 5);
                    fprintf(stderr, "  Char: props=0x%02x value=0x%04x\n", props, val_handle);
                    if (props & 0x10) {
                        s.report_handle = val_handle;
                        s.report_cccd = val_handle + 1;
                        fprintf(stderr, "    -> INPUT notify handle=0x%04x cccd=0x%04x\n",
                                s.report_handle, s.report_cccd);
                    }
                }
                if (buf[0] == ATT_OP_ERROR_RSP) break;
            }
            if (!s.report_handle) {
                fprintf(stderr, "  WARNING: no notify characteristic found\n");
                s.report_handle = 0x000E; s.report_cccd = 0x000F;
            }
            s.state = STATE_GATT_READY;
        }

        /* 4d: Enable notifications (CCCD write 0x0001) */
        {
            uint8_t req[] = {ATT_OP_WRITE_REQ,
                             (uint8_t)(s.report_cccd & 0xFF), (uint8_t)(s.report_cccd >> 8),
                             0x01, 0x00};
            if (att_send(&s, req, sizeof(req)) < 0) goto reconnect;
            uint8_t buf[16]; uint16_t blen = sizeof(buf);
            int rc = rx_await_acl(&s, ATT_CID, buf, &blen, 2000);
            if (rc == 0 && blen >= 1 && buf[0] == ATT_OP_WRITE_RSP)
                fprintf(stderr, "  CCCD write OK — notifications enabled\n");
            else
                fprintf(stderr, "  CCCD write: no explicit response (rc=%d)\n", rc);
            s.state = STATE_NOTIFICATIONS_ENABLED;
        }

        /* ── Phase 5: Input Report Loop ─────────────────────────────── */
        fprintf(stderr, "=== Receiving input reports ===\n");
        {
            int count = 0;
            while (g_running && s.state == STATE_NOTIFICATIONS_ENABLED) {
                hci_frame_t f;
                int rc = hci_user_read_frame(hci_fd, &f, 1000);
                if (rc <= 0) continue;
                if (f.pkt_type == HCI_EVENT_PKT &&
                    f.evt_code == EVT_DISCONN_COMPLETE) {
                    fprintf(stderr, "  Disconnected (%d reports, reason=0x%02x)\n",
                            count, f.status);
                    s.last_disconnect_reason = f.status;
                    break;
                }
                if (f.pkt_type == HCI_ACLDATA_PKT &&
                    f.handle == s.conn_handle && f.cid == ATT_CID &&
                    f.payload_len >= 3 && f.payload[0] == ATT_OP_HANDLE_NOTIFY) {
                    uint16_t vh = get_le16(f.payload + 1);
                    uint16_t vl = f.payload_len - 3;
                    const uint8_t *vd = f.payload + 3;
                    if (vh == s.report_handle && vl >= 1 && vd[0] == 0x09) {
                        count++;
                        if (count <= 10 || count % 50 == 0) {
                            fprintf(stderr, "REPORT #%d (%d bytes): ", count, vl);
                            for (int i = 0; i < vl && i < 16; i++)
                                fprintf(stderr, "%02x ", vd[i]);
                            fprintf(stderr, "\n");
                        }
                    }
                }
            }
        }

        /*
         * After successful connect+encrypt+GATT, the controller will be
         * disconnected (either by us or by timeout/idle). We DON'T need
         * cancel before the next connect because the disconnect properly
         * closed the connection.
         *
         * But if we got here via a disconnect in the report loop,
         * last_disconnect_reason is set and we WON'T cancel — which is
         * correct: the disconnect is clean.
         */
        s.did_cancel = 0;

    reconnect:
        if (!g_running) break;
        fprintf(stderr, "reconnecting in %ds...\n", backoff);
        sleep((unsigned)backoff);
        if (backoff < 16) backoff *= 2;
    }

    hci_user_close(hci_fd);
    fprintf(stderr, "sw2d_final: stopped\n");
    return 0;
}
