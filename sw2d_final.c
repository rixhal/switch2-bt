/* sw2d_final.c — Switch 2 Pro Controller Raw-HCI BLE Daemon (Golden Path)
 *
 * Architecture (v3 — HCI_CHANNEL_USER):
 *   Single HCI_CHANNEL_USER socket — direct controller access
 *   No kernel interference, no HCI filter, no MGMT contamination
 *   hci0 DOWN → bind USER channel → exclusive controller ownership
 *   Proven SMP Golden Path (c1/s1, Confirm/Random, STK, Encryption)
 *   Hard state machine: ATT blocked until ENCRYPTED
 *   USB prep with auto host BDADDR
 *   Peer address type auto-detection
 *
 * Build: gcc -O2 -s -Wall -o sw2d_final sw2d_final.c -lbluetooth -lcrypto
 *
 * Reference: BlueKitchen BTstack platform/linux/hci_transport_linux.c
 *   HCI_CHANNEL_USER gives direct, unfiltered controller access when
 *   hci0 is DOWN — no kernel commands leak in, no MGMT contamination.
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
#include <sys/socket.h>
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

/* ═══ constants ═══════════════════════════════════════════════════════════ */
#define HCI_COMMAND_PKT  0x01
#define HCI_ACLDATA_PKT  0x02
#define HCI_EVENT_PKT    0x04

#define EVT_CMD_COMPLETE          0x0e
#define EVT_CMD_STATUS            0x0f
#define EVT_DISCONN_COMPLETE      0x05
#define EVT_NUM_COMP_PKTS         0x13
#define EVT_ENCRYPT_CHANGE        0x08
#define EVT_LE_META               0x3e
#define EVT_LE_CONN_COMPLETE      0x01
#define EVT_LE_CONN_UPDATE        0x03
#define EVT_LE_ADV_REPORT         0x02
#define EVT_LE_DATA_LEN_CHANGE    0x07
#define EVT_ENCRYPT_KEY_REFRESH   0x30
#define EVT_LE_LTK_REQUEST        0x05

#define OGF_LE_CTL         0x08
#define OCF_LE_SET_EVENT_MASK    0x0001
#define OCF_LE_CREATE_CONN       0x000d
#define OCF_LE_CONN_UPDATE       0x0013
#define OCF_LE_START_ENCRYPTION  0x0019
#define OCF_LE_LTK_REQ_REPLY     0x001a
#define OCF_LE_LTK_REQ_NEG_REPLY 0x001b
#define OCF_LE_SET_SCAN_PARAMS   0x000b
#define OCF_LE_SET_SCAN_ENABLE   0x000c

#define ATT_CID             0x0004
#define L2CAP_LE_SIGNALING  0x0005
#define SMP_CID             0x0006

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

#define SMP_OP_PAIRING_REQ        0x01
#define SMP_OP_PAIRING_RSP        0x02
#define SMP_OP_CONFIRM            0x03
#define SMP_OP_RANDOM             0x04
#define SMP_OP_ENCRYPT_INFO       0x06
#define SMP_OP_MASTER_IDENT       0x07
#define SMP_OP_IDENT_INFO         0x08
#define SMP_OP_IDENT_ADDR_INFO    0x09
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

static const char *state_name(session_state_t s) {
    switch (s) {
    case STATE_DISCONNECTED:            return "DISCONNECTED";
    case STATE_CONNECTED_UNENCRYPTED:   return "CONNECTED_UNENCRYPTED";
    case STATE_SMP_PAIRING_SENT:        return "SMP_PAIRING_SENT";
    case STATE_SMP_CONFIRM_RANDOM:      return "SMP_CONFIRM_RANDOM";
    case STATE_ENCRYPTION_START_SENT:   return "ENCRYPTION_START_SENT";
    case STATE_ENCRYPTED:               return "ENCRYPTED";
    case STATE_ATT_READY:               return "ATT_READY";
    case STATE_GATT_READY:              return "GATT_READY";
    case STATE_NOTIFICATIONS_ENABLED:   return "NOTIFICATIONS_ENABLED";
    case STATE_UHID_RUNNING:            return "UHID_RUNNING";
    default:                            return "UNKNOWN";
    }
}

/* ═══ session context ════════════════════════════════════════════════════ */
typedef struct {
    int      hci_fd;        /* single HCI_CHANNEL_USER socket */
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
    uint8_t  smp_ltk[16];
    uint16_t smp_ediv;
    uint8_t  smp_rand[8];
    /* ATT/GATT */
    uint16_t att_mtu;
    uint16_t svc_start, svc_end;
    uint16_t report_handle, report_cccd;
    /* notification buffer */
    uint8_t  notify_data[256];
    uint16_t notify_len;
    uint16_t notify_handle;
    /* disconnect tracking */
    uint8_t  last_disconnect_reason;
} session_t;

/* ═══ globals ════════════════════════════════════════════════════════════ */
static volatile sig_atomic_t g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

/* ═══ helpers ════════════════════════════════════════════════════════════ */
static uint16_t get_le16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static void put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

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

/* ═══ read BDADDR from /sys (works when hci0 is DOWN) ════════════════════ */
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

/* ═══ HCI_CHANNEL_USER socket ════════════════════════════════════════════ */
static int open_hci_user_socket(void) {
    /* hci0 must be DOWN for HCI_CHANNEL_USER */
    system("hciconfig hci0 down 2>/dev/null");
    usleep(200000);

    int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (fd < 0) { perror("hci_user socket"); return -1; }

    struct sockaddr_hci addr = {
        .hci_family = AF_BLUETOOTH,
        .hci_dev    = 0,
        .hci_channel = HCI_CHANNEL_USER,
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("hci_user bind (is hci0 DOWN? is bluetoothd stopped?)");
        close(fd);
        return -1;
    }
    fprintf(stderr, "  HCI_CHANNEL_USER socket open (fd=%d, exclusive controller access)\n", fd);
    return fd;
}

/* ═══ HCI command send ═══════════════════════════════════════════════════ */
static int hci_send_cmd_(int fd, uint16_t ogf, uint16_t ocf,
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
    ssize_t n = writev(fd, iov, params ? 3 : 2);
    if (n < 0) { perror("writev HCI cmd"); return -1; }
    return 0;
}

/* ═══ HCI init sequence ══════════════════════════════════════════════════ */
static int hci_init_sequence(int fd) {
    fprintf(stderr, "=== HCI Init ===\n");
    /* HCI_Reset */
    if (hci_send_cmd_(fd, 0x03, 0x0003, NULL, 0) < 0) return -1;
    fprintf(stderr, "  HCI_Reset sent\n");
    usleep(50000);

    /* LE Set Event Mask */
    uint8_t evtmask[8] = {0xFF, 0xFF, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00};
    if (hci_send_cmd_(fd, OGF_LE_CTL, OCF_LE_SET_EVENT_MASK, evtmask, 8) < 0)
        return -1;
    fprintf(stderr, "  LE_Set_Event_Mask sent\n");
    usleep(50000);
    return 0;
}

/* ═══ ACL send ═══════════════════════════════════════════════════════════ */
static int acl_send(int fd, uint16_t handle, uint16_t cid,
                    const uint8_t *data, uint16_t len) {
    uint8_t buf[5 + 4 + 512];
    uint16_t hf = (uint16_t)((handle & 0x0fff) | (2u << 12)); /* PB=start */
    uint16_t acl_len = (uint16_t)(4 + len);
    buf[0] = HCI_ACLDATA_PKT;
    put_le16(buf + 1, hf);
    put_le16(buf + 3, acl_len);
    put_le16(buf + 5, len);
    put_le16(buf + 7, cid);
    if (len > 512) return -1;
    memcpy(buf + 9, data, len);
    struct iovec iov[1] = {{buf, 9u + len}};
    ssize_t n = writev(fd, iov, 1);
    if (n < 0) { perror("acl_send"); return -1; }
    return 0;
}

static int smp_send(session_t *s, const uint8_t *data, uint16_t len) {
    return acl_send(s->hci_fd, s->conn_handle, SMP_CID, data, len);
}

static int att_send(session_t *s, const uint8_t *data, uint16_t len) {
    return acl_send(s->hci_fd, s->conn_handle, ATT_CID, data, len);
}

/* ═══ LE Create Connection ═══════════════════════════════════════════════ */
static int le_create_conn(session_t *s) {
    uint8_t p[25];
    put_le16(p + 0, 0x0010);  /* scan interval (10ms) */
    put_le16(p + 2, 0x0010);  /* scan window (10ms) */
    p[4] = 0x00;               /* filter: no whitelist */
    p[5] = s->peer_addr_type;
    memcpy(p + 6, s->peer_addr, 6);
    p[12] = s->local_addr_type;
    put_le16(p + 13, 0x000C);  /* conn interval min (15ms) */
    put_le16(p + 15, 0x000C);  /* conn interval max (15ms) */
    put_le16(p + 17, 0x0000);  /* latency */
    put_le16(p + 19, 0x0C80);  /* supervision timeout (3.2s) */
    put_le16(p + 21, 0x0001);  /* min CE */
    put_le16(p + 23, 0x0001);  /* max CE */
    if (hci_send_cmd_(s->hci_fd, OGF_LE_CTL, OCF_LE_CREATE_CONN, p, 25) < 0)
        return -1;
    fprintf(stderr, "  LE_Create_Connection: peer=%02x:%02x:%02x:%02x:%02x:%02x type=%s\n",
            s->peer_addr[5], s->peer_addr[4], s->peer_addr[3],
            s->peer_addr[2], s->peer_addr[1], s->peer_addr[0],
            s->peer_addr_type == LE_RANDOM_ADDRESS ? "random" : "public");
    return 0;
}

/* ═══ SMP: AES-128 via OpenSSL ═══════════════════════════════════════════ */
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

/* SMP c1: confirm value (BLUETOOTH CORE SPEC 5.4 Vol 3 Part H 2.2.6) */
static void smp_c1(const uint8_t k[16], const uint8_t r[16],
                   const uint8_t preq[7], const uint8_t pres[7],
                   uint8_t iat, const uint8_t ia[6],
                   uint8_t rat, const uint8_t ra[6],
                   uint8_t confirm[16]) {
    uint8_t p1[16], p2[16], tmp[16];
    /* p1 = pres || preq || rat || iat */
    memcpy(p1, pres, 7);
    memcpy(p1 + 7, preq, 7);
    p1[14] = rat;
    p1[15] = iat;
    /* tmp = r XOR p1 */
    for (int i = 0; i < 16; i++) tmp[i] = r[i] ^ p1[i];
    aes_128(k, tmp, tmp);
    /* p2 = padding || ia || ra */
    memset(p2, 0, 4);
    memcpy(p2 + 4, ia, 6);
    memcpy(p2 + 10, ra, 6);
    for (int i = 0; i < 16; i++) tmp[i] ^= p2[i];
    aes_128(k, tmp, confirm);
}

/* SMP s1: STK derivation */
static void smp_s1(const uint8_t k[16], const uint8_t r1[16],
                   const uint8_t r2[16], uint8_t stk[16]) {
    uint8_t d[16];
    memcpy(d, r1, 8);
    memcpy(d + 8, r2, 8);
    aes_128(k, d, stk);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * RX FRAME — read from single HCI_CHANNEL_USER socket
 * ═══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    uint8_t  pkt_type;
    uint8_t  evt_code;
    uint8_t  subevent;
    uint8_t  status;
    uint16_t opcode;
    uint16_t handle;
    uint16_t cid;
    uint16_t acl_len;
    uint16_t l2cap_len;
    const uint8_t *payload;
    uint16_t payload_len;
    uint8_t  raw[512];
    uint16_t raw_len;
} hci_frame_t;

static int rx_read_frame(session_t *s, hci_frame_t *f, int timeout_ms) {
    struct pollfd pfd = {.fd = s->hci_fd, .events = POLLIN};
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) return -1;
    if (pr == 0) return 0;

    uint8_t buf[512];
    ssize_t n = read(s->hci_fd, buf, sizeof(buf));
    if (n < 1) return n < 0 ? -1 : 0;

    memcpy(f->raw, buf, (size_t)n);
    f->raw_len = (uint16_t)n;
    f->pkt_type = buf[0];

    if (buf[0] == HCI_EVENT_PKT && n >= 3) {
        f->evt_code = buf[1];
        f->payload = buf + 3;
        f->payload_len = (uint16_t)buf[2];
        if (f->evt_code == EVT_LE_META && f->payload_len >= 2)
            f->subevent = f->payload[0];

        if (f->evt_code == EVT_CMD_STATUS && f->payload_len >= 4) {
            f->status = f->payload[0];
            f->opcode = get_le16(f->payload + 2);
        }
        if (f->evt_code == EVT_DISCONN_COMPLETE && f->payload_len >= 4) {
            f->handle = get_le16(f->payload + 1) & 0x0fff;
            f->status = f->payload[3];  /* reason code */
        }
        if (f->evt_code == EVT_ENCRYPT_CHANGE && f->payload_len >= 4) {
            f->status = f->payload[2];  /* 0x00 = success */
            f->handle = get_le16(f->payload + 0) & 0x0fff;
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

    return 1; /* unknown but consumed */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * RX DEMUX — dispatch and wait for specific events
 * ═══════════════════════════════════════════════════════════════════════════ */

/* rx_dispatch: read one frame, log + handle global events, return 1=event 2=ACL */
static int rx_dispatch(session_t *s, hci_frame_t *f, int timeout_ms) {
    int rc = rx_read_frame(s, f, timeout_ms);
    if (rc <= 0) return rc;

    if (f->pkt_type == HCI_EVENT_PKT) {
        if (s->verbose) {
            const char *names[] = {
                [EVT_CMD_COMPLETE] = "CMD_COMPLETE",
                [EVT_CMD_STATUS] = "CMD_STATUS",
                [EVT_DISCONN_COMPLETE] = "DISCONN_COMPLETE",
                [EVT_NUM_COMP_PKTS] = "NUM_COMP_PKTS",
                [EVT_ENCRYPT_CHANGE] = "ENCRYPT_CHANGE",
                [EVT_LE_META] = "LE_META",
                [EVT_ENCRYPT_KEY_REFRESH] = "ENCRYPT_KEY_REFRESH",
            };
            const char *nm = (f->evt_code < 0x40 && names[f->evt_code])
                                 ? names[f->evt_code]
                                 : "?";
            fprintf(stderr, "  [rx] EVT %s", nm);
            if (f->evt_code == EVT_CMD_STATUS)
                fprintf(stderr, " status=0x%02x opcode=0x%04x", f->status,
                        f->opcode);
            if (f->evt_code == EVT_LE_META)
                fprintf(stderr, " sub=0x%02x", f->subevent);
            if (f->evt_code == EVT_DISCONN_COMPLETE)
                fprintf(stderr, " handle=0x%04x reason=0x%02x", f->handle,
                        f->status);
            if (f->evt_code == EVT_ENCRYPT_CHANGE)
                fprintf(stderr, " handle=0x%04x status=0x%02x", f->handle,
                        f->status);
            fprintf(stderr, "\n");
        }

        /* Record disconnect reason */
        if (f->evt_code == EVT_DISCONN_COMPLETE)
            s->last_disconnect_reason = f->status;

        /* Auto-update encryption state */
        if (f->evt_code == EVT_ENCRYPT_CHANGE && f->status == 0x00 &&
            f->handle == s->conn_handle)
            s->state = STATE_ENCRYPTED;

        return 1; /* event */
    }

    if (f->pkt_type == HCI_ACLDATA_PKT) {
        if (s->verbose && f->cid != ATT_CID)
            fprintf(stderr, "  [rx] ACL handle=0x%04x cid=0x%04x len=%u\n",
                    f->handle, f->cid, f->payload_len);
        /* Buffer ATT notifications */
        if (f->handle == s->conn_handle && f->cid == ATT_CID &&
            f->payload_len >= 1 && f->payload[0] == ATT_OP_HANDLE_NOTIFY &&
            f->payload_len >= 3) {
            s->notify_handle = get_le16(f->payload + 1);
            s->notify_len = f->payload_len - 3;
            if (s->notify_len <= sizeof(s->notify_data))
                memcpy(s->notify_data, f->payload + 3, s->notify_len);
            return 3; /* notification */
        }
        return 2; /* ACL */
    }

    return 1;
}

/*
 * rx_await: wait for specific event pattern, return 0=got_it, -1=timeout,
 * -2=disconnect
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
        int rc = rx_read_frame(s, &f, (int)(rem > 500 ? 500 : rem));
        if (rc <= 0) continue;

        if (f.pkt_type == HCI_EVENT_PKT) {
            /* Always handle disconnects */
            if (f.evt_code == EVT_DISCONN_COMPLETE) {
                s->last_disconnect_reason = f.status;
                fprintf(stderr, "  [rx] DISCONNECT handle=0x%04x reason=0x%02x\n",
                        f.handle, f.status);
                return -2;
            }
            /* CMD_STATUS for our commands */
            if (f.evt_code == EVT_CMD_STATUS) {
                if (s->verbose)
                    fprintf(stderr, "  [rx] CMD_STATUS status=0x%02x opcode=0x%04x\n",
                            f.status, f.opcode);
            }
            /* Auto-detect encryption success */
            if (f.evt_code == EVT_ENCRYPT_CHANGE && f.status == 0x00 &&
                f.handle == s->conn_handle) {
                s->state = STATE_ENCRYPTED;
                fprintf(stderr, "  [rx] ENCRYPT_CHANGE success ✓\n");
            }
            /* Extract conn_handle from LE Connection Complete */
            if (f.evt_code == EVT_LE_META && f.subevent == EVT_LE_CONN_COMPLETE &&
                f.payload_len >= 18) {
                uint8_t st = f.payload[1];
                if (st == 0x00) {
                    s->conn_handle = get_le16(f.payload + 3) & 0x0fff;
                    fprintf(stderr, "  [rx] LE_CONN_COMPLETE handle=0x%04x interval=%.2fms\n",
                            s->conn_handle, get_le16(f.payload + 14) * 1.25);
                } else {
                    fprintf(stderr, "  [rx] LE_CONN_COMPLETE FAILED status=0x%02x\n", st);
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
                    size_t cp =
                        f.payload_len < *out_len ? f.payload_len : *out_len;
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
 * rx_await_acl: wait for ACL data on specific CID, return 0=got_it,
 * -1=timeout, -2=disconnect
 */
static int rx_await_acl(session_t *s, uint16_t cid, uint8_t *out,
                        uint16_t *out_len, int timeout_ms) {
    int64_t deadline = now_ms() + timeout_ms;
    hci_frame_t f;

    while (g_running) {
        int64_t rem = deadline - now_ms();
        if (rem <= 0) return -1;

        int rc = rx_read_frame(s, &f, (int)(rem > 500 ? 500 : rem));
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
    if (fd < 0) {
        perror(hidraw);
        return -1;
    }

    uint8_t r[64];

    /* SetHCIState */
    memset(r, 0, sizeof(r));
    r[0] = 0x01;
    r[1] = 0x01;
    r[10] = 0x06;
    r[11] = 0x01;
    if (write(fd, r, sizeof(r)) < 0) {
        perror("SetHCIState");
        close(fd);
        return -1;
    }
    fprintf(stderr, "  SetHCIState sent\n");

    /* SetShipment */
    memset(r, 0, sizeof(r));
    r[0] = 0x01;
    r[1] = 0x02;
    r[10] = 0x08;
    r[11] = 0x00;
    if (write(fd, r, sizeof(r)) < 0) {
        perror("SetShipment");
        close(fd);
        return -1;
    }
    fprintf(stderr, "  SetShipment sent\n");

    /* BluetoothManualPair */
    memset(r, 0, sizeof(r));
    r[0] = 0x01;
    r[1] = 0x03;
    r[10] = 0x01;
    memcpy(r + 11, host, 6);
    if (write(fd, r, sizeof(r)) < 0) {
        perror("BluetoothManualPair");
        close(fd);
        return -1;
    }
    fprintf(stderr,
            "  BluetoothManualPair "
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
                !strcmp(buf, "hcitool") || strstr(buf, "python") ||
                strstr(buf, "bleak")) {
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
    system(
        "killall -9 bluetoothd bluetoothctl btmgmt gatttool hcitool "
        "2>/dev/null");
    system("systemctl stop bluetooth 2>/dev/null");
    usleep(500000);
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
            "  --hidraw PATH                 Hidraw device (default: "
            "/dev/hidraw0)\n"
            "  --exclusive-hci               Check no other BT clients running\n"
            "  --kill-bluez-clients          Kill interfering BT processes\n"
            "  --preflight                   Only check, then exit\n"
            "  --verbose                     Verbose output\n"
            "  --scan-timeout MS             Scan timeout (default 10000)\n"
            "\n"
            "Architecture: HCI_CHANNEL_USER (exclusive controller access)\n"
            "  hci0 must be DOWN. BlueZ must be stopped.\n"
            "  No kernel interference, no MGMT contamination.\n",
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
    int kill_bluez = 0, preflight = 0, verbose = 0;
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
        else if (!strcmp(argv[i], "--exclusive-hci"))
            exclusive = 1;
        else if (!strcmp(argv[i], "--kill-bluez-clients"))
            kill_bluez = 1;
        else if (!strcmp(argv[i], "--preflight"))
            preflight = 1;
        else if (!strcmp(argv[i], "--verbose"))
            verbose = 1;
        else {
            usage(argv[0]);
            return 2;
        }
    }

    /* Resolve peer type */
    if (!strcmp(peer_type_s, "random"))
        peer_type = LE_RANDOM_ADDRESS;
    else if (!strcmp(peer_type_s, "auto")) {
        peer_type = LE_RANDOM_ADDRESS;
        auto_scan = 1;
    }
    if (!strcmp(own_type_s, "random")) own_type = LE_RANDOM_ADDRESS;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    setvbuf(stderr, NULL, _IOLBF, 0);

    /* ── BlueZ guard ─────────────────────────────────────────────── */
    if (exclusive || kill_bluez) {
        int c = check_bluez_clients();
        if (c > 0 && kill_bluez) {
            kill_bluez_clients();
            c = check_bluez_clients();
        }
        if (c > 0 && exclusive) {
            fprintf(stderr,
                    "ERROR: %d BT client(s) running. Use "
                    "--kill-bluez-clients.\n",
                    c);
            return 1;
        }
        if (preflight) {
            fprintf(stderr, "Pre-flight OK.\n");
            return 0;
        }
    }

    /* ── Resolve BDADDR ─────────────────────────────────────────── */
    if (bdaddr_s && str2ba(bdaddr_s, (bdaddr_t *)peer) < 0) {
        fprintf(stderr, "ERROR: invalid BDADDR: %s\n", bdaddr_s);
        return 2;
    }

    /* ── Resolve host BDADDR ────────────────────────────────────── */
    {
        uint8_t local_buf[6];
        if (!strcmp(host_spec, "auto")) {
            if (read_local_bdaddr(local_buf) == 0) {
                memcpy(host_bytes, local_buf, 6);
                fprintf(stderr,
                        "host BDADDR (sysfs): "
                        "%02x:%02x:%02x:%02x:%02x:%02x\n",
                        host_bytes[5], host_bytes[4], host_bytes[3],
                        host_bytes[2], host_bytes[1], host_bytes[0]);
            } else {
                /* Fallback: known crackbery5 BDADDR */
                uint8_t fb[6] = {0xD8, 0x3A, 0xDD, 0xE5, 0x69, 0xED};
                memcpy(host_bytes, fb, 6);
                fprintf(stderr, "host BDADDR (fallback): D8:3A:DD:E5:69:ED\n");
            }
        } else {
            if (str2ba(host_spec, (bdaddr_t *)host_bytes) < 0) {
                fprintf(stderr, "ERROR: invalid host BDADDR\n");
                return 2;
            }
            fprintf(stderr,
                    "host BDADDR (manual): "
                    "%02x:%02x:%02x:%02x:%02x:%02x\n",
                    host_bytes[5], host_bytes[4], host_bytes[3],
                    host_bytes[2], host_bytes[1], host_bytes[0]);
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

    /* ── Auto-scan (uses libbluetooth, needs hci0 UP briefly) ────── */
    if (auto_scan && !bdaddr_s) {
        /* Bring hci0 UP just for scan */
        system("hciconfig hci0 up 2>/dev/null");
        usleep(200000);

        int dev_id = hci_get_route(NULL);
        if (dev_id < 0) dev_id = 0;

        int scan_fd = hci_open_dev(dev_id);
        if (scan_fd >= 0) {
            /* Set filter for LE Meta events */
            struct hci_filter of, nf;
            socklen_t flen = sizeof(of);
            getsockopt(scan_fd, SOL_HCI, HCI_FILTER, &of, &flen);
            hci_filter_clear(&nf);
            hci_filter_set_ptype(HCI_EVENT_PKT, &nf);
            hci_filter_set_event(EVT_LE_META, &nf);
            setsockopt(scan_fd, SOL_HCI, HCI_FILTER, &nf, sizeof(nf));

            /* Passive scan (active rejected by CYW43455) */
            hci_le_set_scan_parameters(scan_fd, 0x00, 0x0010, 0x0010,
                                       LE_PUBLIC_ADDRESS, 0x00, 1000);
            hci_le_set_scan_enable(scan_fd, 0x01, 0x00, 1000);

            fprintf(stderr,
                    "=== Scanning for Nintendo (0x057E) advertisers "
                    "===\n");
            int64_t deadline = now_ms() + scan_timeout;
            int found = 0;

            while (g_running && !found && now_ms() < deadline) {
                struct pollfd pfd = {.fd = scan_fd, .events = POLLIN};
                int pr = poll(&pfd, 1, 500);
                if (pr <= 0) continue;

                uint8_t buf[256];
                int n = (int)read(scan_fd, buf, sizeof(buf));
                if (n < 4 || buf[0] != HCI_EVENT_PKT ||
                    buf[1] != EVT_LE_META)
                    continue;

                const uint8_t *ev = buf + 3;
                uint8_t plen = buf[2];
                if (plen < 3 || ev[0] != EVT_LE_ADV_REPORT) continue;

                uint8_t nrep = ev[1];
                const uint8_t *rep = ev + 2;
                for (int ri = 0;
                     ri < nrep && (rep - ev) + 9 <= plen; ri++) {
                    uint8_t atype = rep[1];
                    memcpy(peer, rep + 2, 6);
                    uint8_t dlen = rep[8];
                    /* Search for Nintendo manufacturer data */
                    for (int di = 0; di + 2 < dlen;) {
                        uint8_t el = rep[9 + di], ty = rep[10 + di];
                        if (!el || di + 1 + (int)el > dlen) break;
                        if (ty == 0xFF && el >= 3) {
                            uint16_t cid = rep[11+di] | (rep[12+di] << 8);
                            if (cid == 0x057E || cid == 0x0553) {
                                char a[18];
                                ba2str((bdaddr_t *)peer, a);
                                fprintf(stderr,
                                        "  Found: %s type=%s (Nintendo "
                                        "0x057E)\n",
                                        a,
                                        atype == LE_RANDOM_ADDRESS
                                            ? "random"
                                            : "public");
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
                fprintf(stderr,
                        "No Switch 2 controller found in scan.\n");
                if (scan_only) return 1;
            }
        } else {
            fprintf(stderr, "WARNING: can't open hci0 for scan\n");
        }

        /* Bring hci0 back DOWN for USER channel */
        system("hciconfig hci0 down 2>/dev/null");
        usleep(200000);

        if (!peer[0] && !peer[1] && !peer[2] && !peer[3] && !peer[4] &&
            !peer[5]) {
            fprintf(stderr, "ERROR: no peer address found\n");
            return 1;
        }
        char a[18];
        ba2str((bdaddr_t *)peer, a);
        fprintf(stderr,
                "peer: %s type=%s\n", a,
                peer_type == LE_RANDOM_ADDRESS ? "random" : "public");
        if (scan_only) return 0;
    }

    /* ── Open HCI_CHANNEL_USER socket ────────────────────────────── */
    int hci_fd = open_hci_user_socket();
    if (hci_fd < 0) return 1;

    /* ── HCI Init ────────────────────────────────────────────────── */
    if (hci_init_sequence(hci_fd) < 0) {
        close(hci_fd);
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

    fprintf(stderr, "=== sw2d_final (HCI_CHANNEL_USER v3) ===\n");
    {
        char lstr[18], pstr[18];
        ba2str((bdaddr_t *)s.local_addr, lstr);
        ba2str((bdaddr_t *)peer, pstr);
        fprintf(stderr,
                "local:  %s\n"
                "peer:   %s type=%s\n"
                "socket: HCI_CHANNEL_USER (exclusive)\n",
                lstr, pstr,
                peer_type == LE_RANDOM_ADDRESS ? "random" : "public");
    }

    /* ═══════════════════════════════════════════════════════════════
     * MAIN RECONNECT LOOP
     * ═══════════════════════════════════════════════════════════════ */
    int backoff = 1;
    while (g_running) {
        fprintf(stderr, "─────────────────── session start "
                        "────────────────────\n");
        s.state = STATE_DISCONNECTED;
        s.conn_handle = 0;
        s.att_mtu = 0;
        s.last_disconnect_reason = 0;

        /* ── Phase 1: LE Create Connection ────────────────────────── */
        if (le_create_conn(&s) < 0) goto reconnect;
        {
            int rc = rx_await(&s, EVT_LE_META, EVT_LE_CONN_COMPLETE, NULL,
                              NULL, 10000);
            if (rc != 0 || !s.conn_handle) {
                fprintf(stderr, "Connection failed (rc=%d)\n", rc);
                goto reconnect;
            }
        }
        s.state = STATE_CONNECTED_UNENCRYPTED;
        fprintf(stderr, "*** CONNECTED handle=0x%04x ***\n", s.conn_handle);

        /* ── Phase 2: SMP Golden Path ──────────────────────────────── */
        fprintf(stderr, "=== SMP Just Works Pairing ===\n");

        /* 2a: Send Security Request first */
        {
            uint8_t sec_req = SMP_OP_SECURITY_REQUEST;
            uint8_t auth_req = 0x01; /* Bonding */
            uint8_t pdu[2] = {sec_req, auth_req};
            if (smp_send(&s, pdu, 2) < 0) goto reconnect;
            fprintf(stderr, "  Security Request sent (Bonding)\n");
        }

        /* 2b: Send Pairing Request (Golden Path) */
        {
            uint8_t preq[] = {SMP_OP_PAIRING_REQ, /* opcode */
                              0x03,  /* IO: NoInputNoOutput */
                              0x00,  /* OOB: not present */
                              0x01,  /* AuthReq: Bonding (no MITM, no SC) */
                              0x10,  /* MaxEncKeySize: 16 */
                              0x03,  /* InitiatorKeyDist: EncKey | IdKey */
                              0x03}; /* ResponderKeyDist: EncKey | IdKey */
            memcpy(s.smp_preq, preq, sizeof(preq));
            if (smp_send(&s, preq, sizeof(preq)) < 0) goto reconnect;
            s.state = STATE_SMP_PAIRING_SENT;
            fprintf(stderr, "  Pairing Request sent\n");
            if (verbose) hexdump("pairing_req", preq, sizeof(preq));
        }

        /* 2c: Wait for Pairing Response on SMP_CID */
        {
            uint8_t buf[32];
            uint16_t blen = sizeof(buf);
            int rc = rx_await_acl(&s, SMP_CID, buf, &blen, 5000);
            if (rc == -2) {
                fprintf(stderr,
                        "  Disconnect (0x%02x) before Pairing "
                        "Response\n",
                        s.last_disconnect_reason);
                goto reconnect;
            }
            if (rc != 0 || blen < 7) {
                fprintf(stderr,
                        "  No SMP Pairing Response (rc=%d, len=%u)\n", rc,
                        blen);
                goto reconnect;
            }
            memcpy(s.smp_pres, buf, 7);
            fprintf(stderr,
                    "  Pairing Response: IO=0x%02x AuthReq=0x%02x "
                    "MaxKey=%u\n",
                    buf[1], buf[3], buf[4]);
            if (buf[0] == SMP_OP_PAIRING_RSP) {
                /* Pairing Response received — good */
            } else if (buf[0] == SMP_OP_SECURITY_REQUEST) {
                /* Controller echoed security request — continue */
                fprintf(stderr, "  (Controller security request echo)\n");
                /* Re-read for actual Pairing Response */
                blen = sizeof(buf);
                rc = rx_await_acl(&s, SMP_CID, buf, &blen, 5000);
                if (rc != 0 || blen < 7 || buf[0] != SMP_OP_PAIRING_RSP) {
                    fprintf(stderr,
                            "  No Pairing Response after security req\n");
                    goto reconnect;
                }
                memcpy(s.smp_pres, buf, 7);
                fprintf(stderr,
                        "  Pairing Response: IO=0x%02x AuthReq=0x%02x "
                        "MaxKey=%u\n",
                        buf[1], buf[3], buf[4]);
            }
            s.state = STATE_SMP_CONFIRM_RANDOM;
        }

        /* 2d: Generate Mrand and Mconfirm via c1() */
        {
            for (int i = 0; i < 16; i++)
                s.smp_mrand[i] = (uint8_t)(rand() & 0xFF);
            smp_c1(s.smp_tk, s.smp_mrand, s.smp_preq, s.smp_pres,
                   s.local_addr_type, s.local_addr, s.peer_addr_type,
                   s.peer_addr, s.smp_mconfirm);
            if (verbose)
                hexdump("Mconfirm", s.smp_mconfirm, 16);
        }

        /* 2e: Master sends Confirm */
        {
            uint8_t c[17];
            c[0] = SMP_OP_CONFIRM;
            memcpy(c + 1, s.smp_mconfirm, 16);
            if (smp_send(&s, c, 17) < 0) goto reconnect;
            fprintf(stderr, "  Confirm sent\n");
        }

        /* 2f: Read Slave Confirm */
        {
            uint8_t buf[32];
            uint16_t blen = sizeof(buf);
            int rc = rx_await_acl(&s, SMP_CID, buf, &blen, 5000);
            if (rc == -2)
                goto reconnect;
            if (rc != 0 || blen != 17 || buf[0] != SMP_OP_CONFIRM) {
                fprintf(stderr, "  No slave confirm (rc=%d, len=%u, "
                                "op=0x%02x)\n",
                        rc, blen, blen > 0 ? buf[0] : 0);
                goto reconnect;
            }
            memcpy(s.smp_sconfirm, buf + 1, 16);
            if (verbose)
                hexdump("Sconfirm", s.smp_sconfirm, 16);
        }

        /* 2g: Master sends Random */
        {
            uint8_t r[17];
            r[0] = SMP_OP_RANDOM;
            memcpy(r + 1, s.smp_mrand, 16);
            if (smp_send(&s, r, 17) < 0) goto reconnect;
            fprintf(stderr, "  Random sent\n");
        }

        /* 2h: Read Slave Random */
        {
            uint8_t buf[32];
            uint16_t blen = sizeof(buf);
            int rc = rx_await_acl(&s, SMP_CID, buf, &blen, 5000);
            if (rc == -2)
                goto reconnect;
            if (rc != 0 || blen != 17 || buf[0] != SMP_OP_RANDOM) {
                fprintf(stderr, "  No slave random (rc=%d, len=%u, "
                                "op=0x%02x)\n",
                        rc, blen, blen > 0 ? buf[0] : 0);
                goto reconnect;
            }
            memcpy(s.smp_srand, buf + 1, 16);
        }

        /* 2i: Verify slave confirm */
        {
            uint8_t expected[16];
            smp_c1(s.smp_tk, s.smp_srand, s.smp_preq, s.smp_pres,
                   s.peer_addr_type, s.peer_addr, s.local_addr_type,
                   s.local_addr, expected);
            if (memcmp(expected, s.smp_sconfirm, 16) != 0) {
                fprintf(stderr, "  CONFIRM MISMATCH — abort\n");
                if (verbose) {
                    hexdump("expected", expected, 16);
                    hexdump("received", s.smp_sconfirm, 16);
                }
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
            memset(enc + 2, 0, 8);    /* Random = 0 */
            put_le16(enc + 10, 0);    /* EDIV = 0 */
            memcpy(enc + 12, s.smp_stk, 16); /* LTK = STK */
            if (hci_send_cmd_(hci_fd, OGF_LE_CTL, OCF_LE_START_ENCRYPTION, enc,
                             28) < 0)
                goto reconnect;
            s.state = STATE_ENCRYPTION_START_SENT;
            fprintf(stderr, "  LE Start Encryption sent\n");
        }

        /* 3a: Handle LTK Request if kernel sends it */
        /* 3b: Wait for Encryption Change */
        {
            int encrypted = 0;
            int64_t dl = now_ms() + 5000;
            while (now_ms() < dl && g_running) {
                hci_frame_t f;
                int rc = rx_read_frame(&s, &f, 500);
                if (rc <= 0) continue;

                if (f.pkt_type == HCI_EVENT_PKT) {
                    if (f.evt_code == EVT_DISCONN_COMPLETE) {
                        fprintf(stderr,
                                "  Disconnect (0x%02x) during "
                                "encryption\n",
                                f.status);
                        goto reconnect;
                    }
                    if (f.evt_code == EVT_ENCRYPT_CHANGE) {
                        fprintf(stderr,
                                "  Encryption change: status=0x%02x "
                                "handle=0x%04x\n",
                                f.payload_len >= 3 ? f.payload[2] : 0xFF,
                                f.handle);
                        if (f.payload_len >= 3 && f.payload[2] == 0x00 &&
                            f.handle == s.conn_handle) {
                            encrypted = 1;
                            break;
                        }
                    }
                    /* Handle LTK Request */
                    if (f.evt_code == EVT_LE_META &&
                        f.subevent == EVT_LE_LTK_REQUEST &&
                        f.payload_len >= 10) {
                        uint16_t h = get_le16(f.payload + 2) & 0x0fff;
                        fprintf(stderr,
                                "  LTK Request handle=0x%04x — "
                                "replying\n",
                                h);
                        uint8_t ltk_reply[18];
                        put_le16(ltk_reply, h);
                        memcpy(ltk_reply + 2, s.smp_stk, 16);
                        hci_send_cmd_(hci_fd, OGF_LE_CTL,
                                     OCF_LE_LTK_REQ_REPLY, ltk_reply, 18);
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

        /* ── Phase 4: ATT/GATT after encryption ────────────────────── */
        fprintf(stderr, "=== ATT/GATT ===\n");

        /* 4a: ATT MTU Exchange */
        {
            uint8_t mtu[] = {ATT_OP_MTU_REQ, 0xFF, 0x00};
            if (att_send(&s, mtu, sizeof(mtu)) < 0) goto reconnect;
            fprintf(stderr, "  ATT MTU req sent\n");

            uint8_t buf[32];
            uint16_t blen = sizeof(buf);
            int rc = rx_await_acl(&s, ATT_CID, buf, &blen, 2000);
            if (rc == 0 && blen >= 3 && buf[0] == ATT_OP_MTU_RSP) {
                s.att_mtu = get_le16(buf + 1);
                fprintf(stderr, "  ATT MTU: %u\n", s.att_mtu);
            } else {
                s.att_mtu = 23; /* default */
                fprintf(stderr, "  ATT MTU: default %u\n", s.att_mtu);
            }
            s.state = STATE_ATT_READY;
        }

        /* 4b: GATT discover primary services */
        uint16_t svc_start = 0, svc_end = 0;
        {
            uint8_t req[] = {ATT_OP_READ_BY_GROUP_REQ, 0x01, 0x00, 0xFF,
                             0xFF, 0x00, 0x28};
            if (att_send(&s, req, sizeof(req)) < 0) goto reconnect;

            uint8_t buf[64];
            uint16_t blen = sizeof(buf);
            int rc = rx_await_acl(&s, ATT_CID, buf, &blen, 3000);
            if (rc == 0 && blen >= 6 &&
                buf[0] == ATT_OP_READ_BY_GROUP_RSP) {
                svc_start = get_le16(buf + 2);
                svc_end = get_le16(buf + 4);
                fprintf(stderr, "  Service: 0x%04x-0x%04x\n", svc_start,
                        svc_end);
            } else {
                /* Fallback: use full range */
                svc_start = 0x0001;
                svc_end = 0xFFFF;
                fprintf(stderr,
                        "  Using full range for char discovery\n");
            }
            s.svc_start = svc_start;
            s.svc_end = svc_end;
        }

        /* 4c: GATT discover characteristics */
        {
            uint8_t req[] = {
                ATT_OP_READ_BY_TYPE_REQ,
                (uint8_t)(svc_start & 0xFF),
                (uint8_t)(svc_start >> 8),
                (uint8_t)(svc_end & 0xFF),
                (uint8_t)(svc_end >> 8),
                0x03,
                0x28}; /* UUID for characteristic */
            if (att_send(&s, req, sizeof(req)) < 0) goto reconnect;

            int64_t dl = now_ms() + 3000;
            while (now_ms() < dl && g_running) {
                uint8_t buf[64];
                uint16_t blen = sizeof(buf);
                int rc = rx_await_acl(&s, ATT_CID, buf, &blen,
                                      (int)(dl - now_ms()));
                if (rc == -2) goto reconnect;
                if (rc != 0) continue;

                if (buf[0] == ATT_OP_READ_BY_TYPE_RSP && blen >= 7) {
                    uint8_t props = buf[4];
                    uint16_t val_handle = get_le16(buf + 5);
                    fprintf(stderr,
                            "  Char: props=0x%02x value=0x%04x\n", props,
                            val_handle);
                    if (props & 0x10) { /* notify */
                        s.report_handle = val_handle;
                        s.report_cccd = val_handle + 1;
                        fprintf(stderr,
                                "    -> INPUT (notify) "
                                "handle=0x%04x "
                                "cccd=0x%04x\n",
                                s.report_handle, s.report_cccd);
                    }
                }
                if (buf[0] == ATT_OP_ERROR_RSP) break;
            }
            if (!s.report_handle) {
                fprintf(stderr,
                        "  WARNING: no notify characteristic found\n");
                s.report_handle = 0x000E;
                s.report_cccd = 0x000F;
            }
            s.state = STATE_GATT_READY;
        }

        /* 4d: Enable notifications (CCCD write 0x0001) */
        {
            uint8_t req[] = {
                ATT_OP_WRITE_REQ, (uint8_t)(s.report_cccd & 0xFF),
                (uint8_t)(s.report_cccd >> 8), 0x01, 0x00};
            if (att_send(&s, req, sizeof(req)) < 0) goto reconnect;

            uint8_t buf[16];
            uint16_t blen = sizeof(buf);
            int rc = rx_await_acl(&s, ATT_CID, buf, &blen, 2000);
            if (rc == 0 && blen >= 1 && buf[0] == ATT_OP_WRITE_RSP) {
                fprintf(stderr,
                        "  CCCD write OK — notifications enabled\n");
            } else {
                fprintf(stderr,
                        "  CCCD write: no explicit response (rc=%d), "
                        "assuming enabled\n",
                        rc);
            }
            s.state = STATE_NOTIFICATIONS_ENABLED;
        }

        /* ── Phase 5: Input Report Loop ─────────────────────────────── */
        fprintf(stderr, "=== Receiving input reports ===\n");
        {
            int count = 0;
            while (g_running && s.state == STATE_NOTIFICATIONS_ENABLED) {
                hci_frame_t f;
                int rc = rx_read_frame(&s, &f, 1000);
                if (rc <= 0) continue;

                if (f.pkt_type == HCI_EVENT_PKT &&
                    f.evt_code == EVT_DISCONN_COMPLETE) {
                    fprintf(stderr,
                            "  Disconnected (%d reports, "
                            "reason=0x%02x)\n",
                            count, f.status);
                    s.last_disconnect_reason = f.status;
                    break;
                }
                if (f.pkt_type == HCI_ACLDATA_PKT &&
                    f.handle == s.conn_handle && f.cid == ATT_CID &&
                    f.payload_len >= 3 &&
                    f.payload[0] == ATT_OP_HANDLE_NOTIFY) {
                    uint16_t vh = get_le16(f.payload + 1);
                    uint16_t vl = f.payload_len - 3;
                    const uint8_t *vd = f.payload + 3;
                    if (vh == s.report_handle && vl >= 1 &&
                        vd[0] == 0x09) {
                        count++;
                        if (count <= 10 || count % 50 == 0) {
                            fprintf(stderr,
                                    "REPORT #%d (%d bytes): ", count, vl);
                            for (int i = 0; i < vl && i < 16; i++)
                                fprintf(stderr, "%02x ", vd[i]);
                            fprintf(stderr, "\n");
                        }
                    }
                }
            }
        }

    reconnect:
        if (!g_running) break;
        fprintf(stderr, "reconnecting in %ds...\n", backoff);
        sleep((unsigned)backoff);
        if (backoff < 16) backoff *= 2;
    }

    close(hci_fd);
    fprintf(stderr, "sw2d_final: stopped\n");
    return 0;
}
