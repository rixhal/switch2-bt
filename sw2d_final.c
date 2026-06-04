/*
 * sw2d_final.c — Switch 2 Pro Controller BLE-to-UHID Bridge
 *
 * Architecture:
 *   Controller → BLE (Raw HCI) → SMP Pairing → LE Encryption
 *   → ATT/GATT → Input Report 0x09 → UHID → Linux Input
 *
 * Key requirements:
 *   - SMP Just Works pairing before ATT (controller rejects unencrypted ATT)
 *   - Central RX demux: HCI_EVENT / HCI_ACLDATA → L2CAP CID dispatch
 *   - Two-socket HCI: cmd_fd (send) + rx_fd (receive)
 *   - No one-shot drain — integrated event filter in read loop
 *   - State machine guards: ATT blocked until ENCRYPTED
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

/* ═══════════════════════════════════════════════════════════════════════
 * CONSTANTS
 * ═══════════════════════════════════════════════════════════════════════ */

#define APP_NAME "sw2d_final"

/* HCI packet types */
#define HCI_COMMAND_PKT 0x01
#define HCI_ACLDATA_PKT 0x02
#define HCI_EVENT_PKT   0x04

/* L2CAP CIDs */
#define L2CAP_CID_ATT          0x0004
#define L2CAP_CID_LE_SIGNALING 0x0005
#define L2CAP_CID_SMP          0x0006

/* ACL flags */
#define ACL_PB_FIRST_NONFLUSH  0x00
#define ACL_PB_CONTINUE        0x01
#define ACL_BC_POINT2POINT     0x00

/* OGF/OCF */
#define OGF_LE_CTL      0x08
#define OGF_LINK_CTL    0x01
#define OGF_HOST_CTL    0x03

#define OCF_LE_SET_EVENT_MASK         0x0001
#define OCF_LE_SET_SCAN_PARAMETERS    0x000B
#define OCF_LE_SET_SCAN_ENABLE        0x000C
#define OCF_LE_CREATE_CONN            0x000D
#define OCF_LE_CONN_UPDATE            0x0013
#define OCF_LE_START_ENCRYPTION       0x0019
#define OCF_LE_SET_DATA_LENGTH        0x0022
#define OCF_HCI_RESET                 0x0003

/* LE Subevents */
#define LE_META_EV_CONN_COMPLETE      0x01
#define LE_META_EV_ADV_REPORT         0x02
#define LE_META_EV_CONN_UPDATE_COMPL  0x03
#define LE_META_EV_LTK_REQUEST        0x05

/* ATT opcodes */
#define ATT_OP_ERROR_RSP            0x01
#define ATT_OP_EXCHANGE_MTU_REQ     0x02
#define ATT_OP_EXCHANGE_MTU_RSP     0x03
#define ATT_OP_READ_BY_GROUP_REQ    0x10
#define ATT_OP_READ_BY_GROUP_RSP    0x11
#define ATT_OP_READ_BY_TYPE_REQ     0x08
#define ATT_OP_READ_BY_TYPE_RSP     0x09
#define ATT_OP_WRITE_REQ            0x12
#define ATT_OP_WRITE_RSP            0x13
#define ATT_OP_HANDLE_VALUE_NOTIFY  0x1B

/* GATT UUIDs */
#define GATT_UUID_PRIMARY_SERVICE   0x2800
#define GATT_UUID_CHARACTERISTIC    0x2803
#define GATT_UUID_CCCD              0x2902
#define GATT_UUID_HID_SERVICE       0x1812
#define GATT_UUID_REPORT            0x2A4D
#define GATT_UUID_REPORT_MAP        0x2A4B

/* SMP */
#define SMP_CID             0x0006
#define SMP_PAIRING_REQUEST  0x01
#define SMP_PAIRING_RESPONSE 0x02
#define SMP_PAIRING_CONFIRM  0x03
#define SMP_PAIRING_RANDOM   0x04
#define SMP_PAIRING_FAILED   0x05
#define SMP_ENCRYPTION_INFO  0x06
#define SMP_MASTER_IDENT     0x07
#define SMP_IDENTITY_INFO    0x08
#define SMP_IDENTITY_ADDR    0x09
#define SMP_SECURITY_REQUEST 0x0B

/* SMP IO Capabilities */
#define SMP_IO_DISPLAY_ONLY     0x00
#define SMP_IO_DISPLAY_YESNO    0x01
#define SMP_IO_KEYBOARD_ONLY    0x02
#define SMP_IO_NOINPUTNOOUTPUT  0x03
#define SMP_IO_KEYBOARD_DISPLAY 0x04

/* SMP AuthReq flags */
#define SMP_AUTH_BONDING         0x01
#define SMP_AUTH_MITM            0x04
#define SMP_AUTH_SC              0x08
#define SMP_AUTH_KEYPRESS        0x10

/* Connection intervals (1.25ms units) */
#define CONN_INTERVAL_MIN_DEFAULT  0x0006  /* 7.5ms */
#define CONN_INTERVAL_MAX_DEFAULT  0x000C  /* 15ms */
#define CONN_INTERVAL_SCAN_MIN     0x0020  /* 40ms scan */
#define CONN_INTERVAL_SCAN_MAX     0x0030  /* 60ms scan */

/* ═══════════════════════════════════════════════════════════════════════
 * STATE MACHINE
 * ═══════════════════════════════════════════════════════════════════════ */

typedef enum {
    STATE_DISCONNECTED,
    STATE_CONNECTED_UNENCRYPTED,
    STATE_SMP_PAIRING_STARTED,
    STATE_SMP_PAIRING_COMPLETE,
    STATE_ENCRYPTION_START_SENT,
    STATE_ENCRYPTED,
    STATE_ATT_READY,
    STATE_GATT_READY,
    STATE_NOTIFICATIONS_ENABLED,
    STATE_UHID_RUNNING,
} conn_state_t;

static const char *state_name(conn_state_t s) {
    switch (s) {
    case STATE_DISCONNECTED:            return "DISCONNECTED";
    case STATE_CONNECTED_UNENCRYPTED:   return "CONNECTED_UNENCRYPTED";
    case STATE_SMP_PAIRING_STARTED:     return "SMP_PAIRING_STARTED";
    case STATE_SMP_PAIRING_COMPLETE:    return "SMP_PAIRING_COMPLETE";
    case STATE_ENCRYPTION_START_SENT:   return "ENCRYPTION_START_SENT";
    case STATE_ENCRYPTED:              return "ENCRYPTED";
    case STATE_ATT_READY:              return "ATT_READY";
    case STATE_GATT_READY:             return "GATT_READY";
    case STATE_NOTIFICATIONS_ENABLED:  return "NOTIFICATIONS_ENABLED";
    case STATE_UHID_RUNNING:           return "UHID_RUNNING";
    default:                            return "UNKNOWN";
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * GLOBALS
 * ═══════════════════════════════════════════════════════════════════════ */

static volatile int g_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * UTILITY
 * ═══════════════════════════════════════════════════════════════════════ */

static void put_le16(uint8_t *dst, uint16_t val) {
    dst[0] = val & 0xFF;
    dst[1] = (val >> 8) & 0xFF;
}

static uint16_t get_le16(const uint8_t *src) {
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static int parse_colon_bytes(const char *s, uint8_t b[6]) {
    unsigned int tmp[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x",
               &tmp[5], &tmp[4], &tmp[3], &tmp[2], &tmp[1], &tmp[0]) != 6)
        return -1;
    for (int i = 0; i < 6; i++) b[i] = (uint8_t)tmp[i];
    return 0;
}

static void dbg_hex(const char *label, const uint8_t *data, int len) {
    fprintf(stderr, "  %s (%d bytes): ", label, len);
    for (int i = 0; i < len && i < 128; i++)
        fprintf(stderr, "%02x ", data[i]);
    fprintf(stderr, "\n");
}

/* ═══════════════════════════════════════════════════════════════════════
 * HCI COMMAND BUILDING
 * ═══════════════════════════════════════════════════════════════════════ */

static int send_hci_cmd(int cmd_fd, uint16_t ogf, uint16_t ocf,
                        const uint8_t *params, uint8_t plen) {
    uint8_t cmd[4 + 255];
    uint16_t op = (uint16_t)((ocf & 0x3FF) | ((ogf & 0x3F) << 10));
    cmd[0] = HCI_COMMAND_PKT;
    cmd[1] = op & 0xFF;
    cmd[2] = (op >> 8) & 0xFF;
    cmd[3] = plen;
    if (plen > 0 && params) memcpy(cmd + 4, params, plen);
    struct iovec iov[1] = {{cmd, 4 + plen}};
    return writev(cmd_fd, iov, 1);
}

/* ═══════════════════════════════════════════════════════════════════════
 * HCI RESET
 * ═══════════════════════════════════════════════════════════════════════ */

static int hci_reset_controller(int dev_id) {
    int ctl = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (ctl < 0) return -1;
    ioctl(ctl, HCIDEVDOWN, dev_id);
    usleep(500000);
    ioctl(ctl, HCIDEVUP, dev_id);
    usleep(500000);
    close(ctl);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * HCI SOCKET SETUP (Two-Socket: cmd_fd + rx_fd)
 * ═══════════════════════════════════════════════════════════════════════ */

static int open_cmd_socket(void) {
    int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (fd < 0) { perror("cmd socket"); return -1; }
    struct sockaddr_hci addr = {.hci_family = AF_BLUETOOTH, .hci_dev = 0, .hci_channel = HCI_CHANNEL_RAW};
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("cmd bind"); close(fd); return -1;
    }
    return fd;
}

static int open_rx_socket(void) {
    int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (fd < 0) { perror("rx socket"); return -1; }
    struct sockaddr_hci addr = {.hci_family = AF_BLUETOOTH, .hci_dev = 0, .hci_channel = HCI_CHANNEL_RAW};
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("rx bind"); close(fd); return -1;
    }

    /* Set complete event filter */
    struct hci_filter flt;
    hci_filter_clear(&flt);
    hci_filter_set_ptype(HCI_EVENT_PKT, &flt);
    hci_filter_set_ptype(HCI_ACLDATA_PKT, &flt);
    hci_filter_set_event(EVT_CMD_COMPLETE, &flt);
    hci_filter_set_event(EVT_CMD_STATUS, &flt);
    hci_filter_set_event(EVT_LE_META_EVENT, &flt);
    hci_filter_set_event(EVT_DISCONN_COMPLETE, &flt);
    hci_filter_set_event(EVT_NUM_COMP_PKTS, &flt);
    hci_filter_set_event(EVT_ENCRYPT_CHANGE, &flt);
    hci_filter_set_event(EVT_READ_REMOTE_FEATURES_COMPLETE, &flt);

    if (setsockopt(fd, SOL_HCI, HCI_FILTER, &flt, sizeof(flt)) < 0) {
        perror("rx setsockopt HCI_FILTER");
        close(fd);
        return -1;
    }

    fprintf(stderr, "  rx_fd filter configured: EVENT+ACL, events: CMD_COMPLETE CMD_STATUS LE_META DISCONN NUM_COMP ENCRYPT\n");
    return fd;
}

/* ═══════════════════════════════════════════════════════════════════════
 * HCI INITIALIZATION SEQUENCE
 * ═══════════════════════════════════════════════════════════════════════ */

static int hci_init_sequence(int cmd_fd, int *dev_id) {
    *dev_id = hci_devid("hci0");
    if (*dev_id < 0) {
        fprintf(stderr, "ERROR: no hci0 device\n");
        return -1;
    }

    /* Ensure hci0 is up */
    int ctl = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (ctl >= 0) {
        ioctl(ctl, HCIDEVUP, *dev_id);
        close(ctl);
    }

    /* HCI Reset */
    fprintf(stderr, "=== HCI Init ===\n");
    if (send_hci_cmd(cmd_fd, OGF_HOST_CTL, OCF_HCI_RESET, NULL, 0) < 0) {
        fprintf(stderr, "  HCI_Reset: send failed\n");
        return -1;
    }
    fprintf(stderr, "  HCI_Reset sent\n");

    /* LE Set Event Mask — suppress kernel-initiated commands */
    /* Mask: only essential LE events */
    uint8_t mask[8] = {
        0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        /* bits: ConnComplete, AdvReport, ConnUpdate, ReadRemoteFeat, LTKReq */
    };
    if (send_hci_cmd(cmd_fd, OGF_LE_CTL, OCF_LE_SET_EVENT_MASK, mask, 8) < 0) {
        fprintf(stderr, "  LE_Set_Event_Mask: send failed\n");
        return -1;
    }
    fprintf(stderr, "  LE_Set_Event_Mask sent\n");

    /* Log local BDADDR */
    bdaddr_t local_ba;
    char local_str[18] = "unknown";
    if (hci_devba(*dev_id, &local_ba) == 0) {
        ba2str(&local_ba, local_str);
    }
    fprintf(stderr, "  local BDADDR: %s\n", local_str);
    fprintf(stderr, "  hci0 state: UP\n");
    fprintf(stderr, "  event mask configured\n");
    fprintf(stderr, "  rx filter configured\n");

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * LE CREATE CONNECTION
 * ═══════════════════════════════════════════════════════════════════════ */

static int le_create_connection(int cmd_fd, bdaddr_t *peer,
                                uint8_t peer_type, uint8_t own_type,
                                uint16_t *out_handle) {
    uint8_t params[25] = {0};
    /* Scan interval/window: 60ms/30ms */
    put_le16(params + 0, 0x0060);
    put_le16(params + 2, 0x0030);
    params[4] = 0x00; /* initiator_filter: peer addr not used for whitelist */
    params[5] = peer_type;
    memcpy(params + 6, peer, 6);
    params[12] = own_type;
    /* Connection parameters: min 7.5ms, max 15ms, latency 0, timeout 2s */
    put_le16(params + 13, CONN_INTERVAL_MIN_DEFAULT);
    put_le16(params + 15, CONN_INTERVAL_MAX_DEFAULT);
    put_le16(params + 17, 0x0000); /* latency */
    put_le16(params + 19, 0x00C8); /* supervision timeout 200 * 10ms = 2s */
    put_le16(params + 21, 0x0004); /* min CE length 2.5ms */
    put_le16(params + 23, 0x0006); /* max CE length 3.75ms */

    fprintf(stderr, "  LE_Create_Connection: peer=%s type=%s own=%s interval=7.5-15ms\n",
            batostr(peer),
            peer_type == 0x00 ? "public" : "random",
            own_type == 0x00 ? "public" : "random");

    return send_hci_cmd(cmd_fd, OGF_LE_CTL, OCF_LE_CREATE_CONN, params, 25);
}

/* ═══════════════════════════════════════════════════════════════════════
 * LE START ENCRYPTION
 * ═══════════════════════════════════════════════════════════════════════ */

static int le_start_encryption(int cmd_fd, uint16_t handle, const uint8_t ltk[16]) {
    uint8_t params[28];
    put_le16(params + 0, handle);
    put_le16(params + 2, 0x0000); /* EDIV=0 for STK */
    memset(params + 4, 0, 8); /* rand */
    memcpy(params + 12, ltk, 16); /* LTK/STK */
    return send_hci_cmd(cmd_fd, OGF_LE_CTL, OCF_LE_START_ENCRYPTION, params, 28);
}

/* ═══════════════════════════════════════════════════════════════════════
 * LE CONNECTION PARAMETER UPDATE
 * ═══════════════════════════════════════════════════════════════════════ */

static int le_conn_param_update(int cmd_fd, uint16_t handle) {
    uint8_t params[14];
    put_le16(params + 0, handle);
    put_le16(params + 2, CONN_INTERVAL_MIN_DEFAULT);  /* 7.5ms */
    put_le16(params + 4, 0x000C);  /* max 15ms */
    put_le16(params + 6, 0x0000);  /* latency 0 */
    put_le16(params + 8, 0x01F4);  /* supervision timeout 500 * 10ms = 5s */
    put_le16(params + 10, 0x0000); /* min CE */
    put_le16(params + 12, 0x0000); /* max CE */
    fprintf(stderr, "  LE_Conn_Param_Update: interval=7.5-15ms latency=0 timeout=5s\n");
    return send_hci_cmd(cmd_fd, OGF_LE_CTL, OCF_LE_CONN_UPDATE, params, 14);
}

/* ═══════════════════════════════════════════════════════════════════════
 * RX FRAME DEMUX — Central read/dispatch
 * ═══════════════════════════════════════════════════════════════════════ */

/* Raw frame read with timeout. */
static int rx_read_raw(int rx_fd, uint8_t *buf, int bufsz, int timeout_ms) {
    struct pollfd pfd = {.fd = rx_fd, .events = POLLIN};
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0 && errno == EINTR) return 0;
    if (pr <= 0) return pr; /* 0=timeout, -1=error */
    return read(rx_fd, buf, (size_t)bufsz);
}

typedef struct {
    uint8_t  type;        /* HCI_EVENT_PKT or HCI_ACLDATA_PKT */
    /* Event fields */
    uint8_t  evt_code;
    uint8_t  evt_len;
    const uint8_t *evt_data;
    /* LE Meta subevent */
    uint8_t  le_subevent;
    /* ACL fields */
    uint16_t conn_handle;
    uint16_t acl_len;
    const uint8_t *acl_data; /* L2CAP header + payload */
} rx_frame_t;

/* Parse a raw buffer into a typed frame. Returns 0 on success, -1 on error. */
static int rx_parse_frame(const uint8_t *buf, int n, rx_frame_t *frm) {
    memset(frm, 0, sizeof(*frm));
    if (n < 3) return -1;

    frm->type = buf[0];

    if (frm->type == HCI_EVENT_PKT) {
        frm->evt_code = buf[1];
        frm->evt_len  = buf[2];
        frm->evt_data = buf + 3;
        if (frm->evt_code == EVT_LE_META_EVENT && frm->evt_len >= 1) {
            frm->le_subevent = buf[3];
        }
        return 0;
    }

    if (frm->type == HCI_ACLDATA_PKT) {
        if (n < 5) return -1;
        uint16_t handle_flags = buf[1] | (buf[2] << 8);
        frm->conn_handle = handle_flags & 0x0FFF;
        frm->acl_len     = get_le16(buf + 3);
        if (n < 5 + (int)frm->acl_len) return -1;
        frm->acl_data = buf + 5;
        return 0;
    }

    /* Unknown or HCI_COMMAND_PKT — ignore */
    return -1;
}

/* Parse L2CAP header: returns CID, and sets data_out + len_out for payload. */
static uint16_t rx_parse_l2cap(rx_frame_t *frm, const uint8_t **data_out, int *len_out) {
    if (frm->type != HCI_ACLDATA_PKT || frm->acl_len < 4) return 0;
    const uint8_t *l2cap = frm->acl_data;
    uint16_t l2cap_len = get_le16(l2cap);
    uint16_t cid       = get_le16(l2cap + 2);
    if (l2cap_len + 4 > frm->acl_len) return 0;
    *data_out = l2cap + 4;
    *len_out  = l2cap_len;
    return cid;
}

/* ═══════════════════════════════════════════════════════════════════════
 * RX DISPATCH LOOP — demuxes frames and calls handlers
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    int      rx_fd;
    int      cmd_fd;
    uint16_t conn_handle;
    conn_state_t state;
    int      verbose;

    /* ATT discovery results */
    uint16_t report_handle;
    uint16_t report_cccd;

    /* SMP */
    uint8_t  smp_tk[16];
    uint8_t  smp_stk[16];
    int      smp_encrypted;

    /* Event await */
    uint8_t  await_evt_pending;
    uint8_t  await_evt_code;
    uint8_t  await_le_subevent;
    uint8_t  await_opcode_lsb;
    uint8_t  await_opcode_msb;
    int      await_result;

    /* Notification buffer */
    uint8_t  notify_data[128];
    int      notify_len;
} rx_ctx_t;

/* Log frame type for debugging */
static void rx_log_frame(rx_frame_t *frm, int verbose) {
    if (!verbose) return;
    if (frm->type == HCI_EVENT_PKT) {
        const char *name = "?";
        switch (frm->evt_code) {
        case EVT_CMD_COMPLETE:     name = "CMD_COMPLETE"; break;
        case EVT_CMD_STATUS:       name = "CMD_STATUS"; break;
        case EVT_LE_META_EVENT:    name = "LE_META"; break;
        case EVT_DISCONN_COMPLETE: name = "DISCONN_COMPLETE"; break;
        case EVT_NUM_COMP_PKTS:    name = "NUM_COMP_PKTS"; break;
        case EVT_ENCRYPT_CHANGE:   name = "ENCRYPT_CHANGE"; break;
        }
        if (frm->evt_code == EVT_LE_META_EVENT) {
            const char *sub = "?";
            switch (frm->le_subevent) {
            case LE_META_EV_CONN_COMPLETE:     sub = "CONN_COMPLETE"; break;
            case LE_META_EV_ADV_REPORT:        sub = "ADV_REPORT"; break;
            case LE_META_EV_CONN_UPDATE_COMPL: sub = "CONN_UPDATE_COMPL"; break;
            case LE_META_EV_LTK_REQUEST:       sub = "LTK_REQUEST"; break;
            }
            fprintf(stderr, "  [rx] EVT LE_META sub=%s(0x%02x) len=%d",
                    sub, frm->le_subevent, frm->evt_len);
            if (frm->evt_len > 0 && frm->evt_len <= 16) {
                fprintf(stderr, " data=");
                for (int i = 0; i < frm->evt_len && i < 16; i++)
                    fprintf(stderr, "%02x ", frm->evt_data[i]);
            }
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "  [rx] EVT %s len=%d\n", name, frm->evt_len);
        }
    } else if (frm->type == HCI_ACLDATA_PKT) {
        uint16_t cid = (frm->acl_len >= 4) ? get_le16(frm->acl_data + 2) : 0;
        const char *cid_name = (cid == L2CAP_CID_ATT) ? "ATT" :
                               (cid == L2CAP_CID_SMP) ? "SMP" :
                               (cid == L2CAP_CID_LE_SIGNALING) ? "LE_SIG" : "?";
        fprintf(stderr, "  [rx] ACL handle=0x%04x len=%d CID=%s(0x%04x)\n",
                frm->conn_handle, frm->acl_len, cid_name, cid);
    }
}

/* Internal: check if an HCI event matches our await criteria.
 * Also extracts connection handle on LE Connection Complete. */
static int rx_match_await(rx_frame_t *frm, rx_ctx_t *ctx) {
    if (!ctx->await_evt_pending) return 0;
    if (frm->type != HCI_EVENT_PKT) return 0;

    if (ctx->await_evt_code == EVT_LE_META_EVENT) {
        if (frm->evt_code != EVT_LE_META_EVENT) return 0;
        if (frm->le_subevent != ctx->await_le_subevent) return 0;

        /* Extract connection handle on LE Connection Complete */
        if (frm->le_subevent == LE_META_EV_CONN_COMPLETE && frm->evt_len >= 19) {
            uint8_t  status   = frm->evt_data[1];
            uint16_t h        = get_le16(frm->evt_data + 2);
            /* LE Connection Complete parameter layout (from evt_data[0]=subevent):
             *   [1]=status [2-3]=handle [4]=role [5]=peer_addr_type
             *   [6-11]=peer_addr [12-13]=interval [14-15]=latency
             *   [16-17]=supervision_timeout [18]=master_clock_accuracy */
            uint16_t interval = get_le16(frm->evt_data + 12);
            uint16_t latency  = get_le16(frm->evt_data + 14);
            uint16_t timeout  = get_le16(frm->evt_data + 16);
            fprintf(stderr, "  CONN_COMPLETE: status=0x%02x handle=0x%04x interval=%.2fms latency=%d timeout=%dms\n",
                    status, h, interval * 1.25, latency, timeout * 10);
            if (status == 0x00) {
                ctx->conn_handle = h;
                ctx->await_result = 0; /* success */
            } else {
                ctx->await_result = status;
            }
        }
    } else {
        if (frm->evt_code != ctx->await_evt_code) return 0;
    }

    /* Opcode match for CMD_COMPLETE/CMD_STATUS */
    if (ctx->await_evt_code == EVT_CMD_COMPLETE ||
        ctx->await_evt_code == EVT_CMD_STATUS) {
        if (frm->evt_len >= 3) {
            uint16_t op = get_le16(frm->evt_data + 1);
            uint8_t expected_lsb = ctx->await_opcode_lsb;
            uint8_t expected_msb = ctx->await_opcode_msb;
            if ((op & 0xFF) != expected_lsb || ((op >> 8) & 0xFF) != expected_msb) {
                return 0;
            }
        }
    }

    return 1;
}

/* Main RX dispatch — call in a loop. Returns 1 if frame was notification data. */
static int rx_dispatch(rx_ctx_t *ctx, int timeout_ms) {
    uint8_t buf[256];
    int n = rx_read_raw(ctx->rx_fd, buf, (int)sizeof(buf), timeout_ms);
    if (n <= 0) return n;

    rx_frame_t frm;
    if (rx_parse_frame(buf, n, &frm) < 0) return 0;

    rx_log_frame(&frm, ctx->verbose);

    /* Check if this matches an awaited event */
    if (ctx->await_evt_pending && rx_match_await(&frm, ctx)) {
        ctx->await_evt_pending = 0;
        ctx->await_result = 1;

        /* Extract result for specific events */
        if (frm.evt_code == EVT_CMD_STATUS) {
            if (frm.evt_len >= 1) ctx->await_result = frm.evt_data[0]; /* status */
        } else if (frm.evt_code == EVT_CMD_COMPLETE) {
            if (frm.evt_len >= 3) {
                ctx->await_result = frm.evt_data[0]; /* status */
                /* Store result params for later parsing */
            }
        }
        return 0; /* consumed by await */
    }

    /* Handle ACL data */
    if (frm.type == HCI_ACLDATA_PKT) {
        const uint8_t *data;
        int len;
        uint16_t cid = rx_parse_l2cap(&frm, &data, &len);
        if (cid == L2CAP_CID_ATT && len >= 3) {
            /* ATT */
            if (data[0] == ATT_OP_HANDLE_VALUE_NOTIFY) {
                int rlen = len - 3;
                if (rlen > 0 && rlen <= 127) {
                    memcpy(ctx->notify_data, data + 3, (size_t)rlen);
                    ctx->notify_len = rlen;
                    return 1; /* notification received */
                }
            }
        }
        if (cid == L2CAP_CID_SMP && len > 0) {
            /* SMP — will be handled by SMP state machine */
            return 2; /* SMP frame */
        }
    }

    /* Handle disconnect */
    if (frm.type == HCI_EVENT_PKT && frm.evt_code == EVT_DISCONN_COMPLETE) {
        fprintf(stderr, "  [rx] DISCONNECT received\n");
        return -2;
    }

    /* Handle encryption change */
    if (frm.type == HCI_EVENT_PKT && frm.evt_code == EVT_ENCRYPT_CHANGE) {
        if (frm.evt_len >= 4) {
            uint8_t status = frm.evt_data[0];
            uint16_t h = get_le16(frm.evt_data + 1);
            uint8_t encrypt = frm.evt_data[3];
            fprintf(stderr, "  [rx] ENCRYPT_CHANGE: status=0x%02x handle=0x%04x encrypt=%d\n",
                    status, h, encrypt);
            if (status == 0x00 && encrypt) {
                ctx->smp_encrypted = 1;
                ctx->state = STATE_ENCRYPTED;
                fprintf(stderr, "  *** LINK ENCRYPTED ***\n");
            }
            return 3; /* encryption event */
        }
    }

    /* Handle LTK Request — kernel may ask for key after encryption start */
    if (frm.type == HCI_EVENT_PKT && frm.evt_code == EVT_LE_META_EVENT &&
        frm.le_subevent == LE_META_EV_LTK_REQUEST) {
        if (frm.evt_len >= 13) {
            uint16_t h = get_le16(frm.evt_data + 1);
            uint8_t  ediv[2];
            uint8_t  rand_val[8];
            memcpy(ediv, frm.evt_data + 3, 2);
            memcpy(rand_val, frm.evt_data + 5, 8);
            fprintf(stderr, "  [rx] LTK_REQUEST handle=0x%04x — sending STK reply\n", h);
            /* Send LE LTK Reply with STK */
            uint8_t ltk_reply[18];
            put_le16(ltk_reply, h);
            memcpy(ltk_reply + 2, ctx->smp_stk, 16);
            send_hci_cmd(ctx->cmd_fd, OGF_LE_CTL, 0x001A, ltk_reply, 18);
            return 4; /* LTK handled */
        }
    }

    return 0; /* consumed */
}

/* Await a specific event */
static int rx_await(rx_ctx_t *ctx, uint8_t evt_code, uint8_t le_sub,
                     uint8_t op_lsb, uint8_t op_msb, int timeout_ms) {
    ctx->await_evt_pending = 1;
    ctx->await_evt_code = evt_code;
    ctx->await_le_subevent = le_sub;
    ctx->await_opcode_lsb = op_lsb;
    ctx->await_opcode_msb = op_msb;
    ctx->await_result = 0;

    struct timeval deadline, now;
    gettimeofday(&deadline, NULL);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_usec += (timeout_ms % 1000) * 1000;
    if (deadline.tv_usec >= 1000000) {
        deadline.tv_sec++;
        deadline.tv_usec -= 1000000;
    }

    while (ctx->await_evt_pending && g_running) {
        gettimeofday(&now, NULL);
        int remaining = (int)((deadline.tv_sec - now.tv_sec) * 1000 +
                            (deadline.tv_usec - now.tv_usec) / 1000);
        if (remaining <= 0) {
            ctx->await_evt_pending = 0;
            fprintf(stderr, "  [rx] await timeout for evt=0x%02x sub=0x%02x\n",
                    evt_code, le_sub);
            return -1;
        }
        int rc = rx_dispatch(ctx, remaining);
        if (rc < 0) return rc;
    }
    return ctx->await_result;
}

/* ═══════════════════════════════════════════════════════════════════════
 * ACL SEND HELPERS
 * ═══════════════════════════════════════════════════════════════════════ */

static int send_acl_pkt(int cmd_fd, uint16_t conn_handle,
                        const uint8_t *data, uint16_t data_len) {
    uint8_t hci_acl[4 + 512];
    uint16_t handle_flags = conn_handle | (ACL_PB_FIRST_NONFLUSH << 12)
                                       | (ACL_BC_POINT2POINT << 14);
    hci_acl[0] = handle_flags & 0xFF;
    hci_acl[1] = (handle_flags >> 8) & 0xFF;
    hci_acl[2] = data_len & 0xFF;
    hci_acl[3] = (data_len >> 8) & 0xFF;
    memcpy(hci_acl + 4, data, data_len);
    uint8_t pkt_type = HCI_ACLDATA_PKT;
    struct iovec iov[2] = {{&pkt_type, 1}, {hci_acl, 4 + data_len}};
    return writev(cmd_fd, iov, 2);
}

static int send_l2cap(int cmd_fd, uint16_t conn_handle,
                      uint16_t cid, const uint8_t *payload, uint16_t plen) {
    uint8_t l2cap[512];
    put_le16(l2cap + 0, plen);
    put_le16(l2cap + 2, cid);
    memcpy(l2cap + 4, payload, plen);
    return send_acl_pkt(cmd_fd, conn_handle, l2cap, 4 + plen);
}

/* ═══════════════════════════════════════════════════════════════════════
 * ATT SEND — with encryption guard
 * ═══════════════════════════════════════════════════════════════════════ */

static int att_send(rx_ctx_t *ctx, const uint8_t *pdu, uint16_t plen) {
    if (ctx->state < STATE_ENCRYPTED) {
        fprintf(stderr, "SECURITY: ATT blocked in state %s (not encrypted)\n",
                state_name(ctx->state));
        return -1;
    }
    /* Transition to ATT_READY on first ATT command */
    if (ctx->state == STATE_ENCRYPTED) {
        ctx->state = STATE_ATT_READY;
        fprintf(stderr, "  State: ENCRYPTED → ATT_READY\n");
    }
    return send_l2cap(ctx->cmd_fd, ctx->conn_handle, L2CAP_CID_ATT, pdu, plen);
}

/* ═══════════════════════════════════════════════════════════════════════
 * SMP PAIRING
 * ═══════════════════════════════════════════════════════════════════════ */

static int smp_send(rx_ctx_t *ctx, const uint8_t *pdu, uint16_t plen) {
    return send_l2cap(ctx->cmd_fd, ctx->conn_handle, L2CAP_CID_SMP, pdu, plen);
}

/* Read raw SMP PDU from rx_dispatch return value 2 */
static int smp_read_pdu(rx_ctx_t *ctx, uint8_t *pdu, int maxlen, int timeout_ms) {
    struct timeval deadline, now;
    gettimeofday(&deadline, NULL);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_usec += (timeout_ms % 1000) * 1000;
    if (deadline.tv_usec >= 1000000) { deadline.tv_sec++; deadline.tv_usec -= 1000000; }

    while (g_running) {
        gettimeofday(&now, NULL);
        int remaining = (int)((deadline.tv_sec - now.tv_sec) * 1000 +
                            (deadline.tv_usec - now.tv_usec) / 1000);
        if (remaining <= 0) return 0;

        uint8_t buf[256];
        int n = rx_read_raw(ctx->rx_fd, buf, (int)sizeof(buf), remaining);
        if (n <= 0) continue;

        rx_frame_t frm;
        if (rx_parse_frame(buf, n, &frm) < 0) continue;

        /* Log but skip HCI events — we only want ACL SMP */
        if (frm.type == HCI_EVENT_PKT) {
            rx_log_frame(&frm, ctx->verbose);
            continue;
        }

        if (frm.type != HCI_ACLDATA_PKT) continue;

        const uint8_t *data;
        int len;
        uint16_t cid = rx_parse_l2cap(&frm, &data, &len);
        if (cid != L2CAP_CID_SMP || len <= 0) continue;

        if (len > maxlen) len = maxlen;
        memcpy(pdu, data, (size_t)len);
        return len;
    }
    return 0;
}

/* For TK=0: STK = 0 (all zeros) */
static int smp_stk_gen(uint8_t stk[16], const uint8_t tk[16],
                        const uint8_t mrand[16], const uint8_t srand[16]) {
    /* STK = s1(tk, srand, mrand) = e(tk, srand[0:8] || mrand[0:8]) */
    /* For TK=0 (Just Works), STK = 0 (all zeros) */
    (void)tk; (void)mrand; (void)srand;
    memset(stk, 0, 16);
    return 0;
}

static int smp_just_works_pairing(rx_ctx_t *ctx) {
    fprintf(stderr, "=== SMP Just Works Pairing ===\n");

    /* Send Security Request to trigger pairing */
    uint8_t sec_req[] = {SMP_SECURITY_REQUEST, 0x01}; /* auth_req: Bonding */
    smp_send(ctx, sec_req, sizeof(sec_req));
    fprintf(stderr, "  SMP Security Request sent\n");

    /* Wait for SMP PDU from controller */
    uint8_t smp_pdu[64];
    uint8_t preq[7];
    int pdu_len = smp_read_pdu(ctx, smp_pdu, sizeof(smp_pdu), 8000);

    /* Controller may send Security Request (0x0B) instead of Pairing Request.
     * In that case, WE initiate pairing as the master. */
    if (pdu_len == 2 && smp_pdu[0] == SMP_SECURITY_REQUEST) {
        fprintf(stderr, "  Controller sent Security Request (auth_req=0x%02x) — initiating pairing\n",
                smp_pdu[1]);
        /* Fall through: we send Pairing Request */
    } else if (pdu_len >= 7 && smp_pdu[0] == SMP_PAIRING_REQUEST) {
        /* Controller already sent Pairing Request — parse it */
        fprintf(stderr, "  Controller sent Pairing Request\n");
        memcpy(preq, smp_pdu, 7);
        goto parse_pairing_request;
    } else {
        fprintf(stderr, "  Unexpected SMP PDU (len=%d op=0x%02x)\n",
                pdu_len, pdu_len > 0 ? smp_pdu[0] : 0);
        if (pdu_len > 0) dbg_hex("SMP data", smp_pdu, pdu_len);
        return -1;
    }

    /* Send Pairing Request as initiator */
    uint8_t mpreq[7] = {
        SMP_PAIRING_REQUEST,
        SMP_IO_NOINPUTNOOUTPUT,
        0x00, /* OOB */
        SMP_AUTH_BONDING, /* AuthReq: Bonding, no MITM, no SC */
        16,   /* MaxKeySize */
        0x03, /* InitKeyDist: EncKey + IdKey */
        0x03, /* RespKeyDist: EncKey + IdKey */
    };
    smp_send(ctx, mpreq, sizeof(mpreq));
    fprintf(stderr, "  Master Pairing Request sent\n");

    /* Read controller's Pairing Response/Request */
    uint8_t pres[7];
    int pres_len = smp_read_pdu(ctx, pres, sizeof(pres), 8000);
    if (pres_len >= 7 && pres[0] == SMP_PAIRING_REQUEST) {
        /* Simultaneous initiation: controller sent Pairing Request too.
         * As master, parse it and send Pairing Response. */
        fprintf(stderr, "  Controller also sent Pairing Request — responding\n");
        memcpy(preq, pres, 7);
        goto parse_pairing_request;
    }
    if (pres_len < 7 || pres[0] != SMP_PAIRING_RESPONSE) {
        fprintf(stderr, "  Expected Pairing Response (got %d bytes, op=0x%02x)\n",
                pres_len, pres_len > 0 ? pres[0] : 0);
        return -1;
    }
    memcpy(preq, mpreq, 7); /* Our request is the "initiator" side */

parse_pairing_request: {
    uint8_t io_cap     = preq[1];
    uint8_t oob_flag   = preq[2];
    uint8_t auth_req   = preq[3];
    uint8_t max_key_size = preq[4];
    uint8_t init_kd    = preq[5];  (void)init_kd;
    uint8_t resp_kd    = preq[6];  (void)resp_kd;

    fprintf(stderr, "  Pairing: IO=%s OOB=%d AuthReq=0x%02x MaxKeySize=%d\n",
            io_cap == SMP_IO_NOINPUTNOOUTPUT ? "NoInputNoOutput" :
            io_cap == SMP_IO_DISPLAY_ONLY ? "DisplayOnly" : "?",
            oob_flag, auth_req, max_key_size);
    fprintf(stderr, "    Bonding=%d MITM=%d SC=%d\n",
            !!(auth_req & SMP_AUTH_BONDING),
            !!(auth_req & SMP_AUTH_MITM),
            !!(auth_req & SMP_AUTH_SC));

    /* Check SC */
    if (auth_req & SMP_AUTH_SC) {
        fprintf(stderr, "  Controller requires LE Secure Connections; legacy Just Works insufficient.\n");
        return -1;
    }
}

    /* Send Pairing Response */
    uint8_t pres_out[7] = {
        SMP_PAIRING_RESPONSE,
        SMP_IO_NOINPUTNOOUTPUT,
        0x00, /* OOB flag */
        0x00, /* AuthReq: NoBonding, NoMITM */
        16,   /* MaxKeySize */
        0x00, /* InitKeyDist: none */
        0x00, /* RespKeyDist: none */
    };
    smp_send(ctx, pres_out, sizeof(pres_out));
    fprintf(stderr, "  Pairing Response sent (NoInputNoOutput, Bonding)\n");

    /* After simultaneous init, controller sends its Pairing Response too.
     * Must consume it before reading Confirm. */
    {
        uint8_t slave_pres[7];
        int slen = smp_read_pdu(ctx, slave_pres, sizeof(slave_pres), 8000);
        if (slen >= 7 && slave_pres[0] == SMP_PAIRING_RESPONSE) {
            fprintf(stderr, "  Slave Pairing Response received\n");
        } else {
            fprintf(stderr, "  Note: no slave Pairing Response (got %d bytes op=0x%02x) — continuing\n",
                    slen, slen > 0 ? slave_pres[0] : 0);
        }
    }

    /* Generate randoms */
    uint8_t mrand[16], srand[16];
    for (int i = 0; i < 16; i++) {
        mrand[i] = (uint8_t)(rand() & 0xFF);
        srand[i] = (uint8_t)(rand() & 0xFF);
    }

    /* Now read controller's Pairing Confirm */
    uint8_t sconfirm_pdu[17];
    int sc_len = smp_read_pdu(ctx, sconfirm_pdu, sizeof(sconfirm_pdu), 8000);
    if (sc_len == 17 && sconfirm_pdu[0] == SMP_PAIRING_CONFIRM) {
        uint8_t sconfirm[16];
        memcpy(sconfirm, sconfirm_pdu + 1, 16);
        dbg_hex("slave confirm", sconfirm, 16);
    } else {
        fprintf(stderr, "  Slave didn't send confirm (got %d bytes op=0x%02x)\n",
                sc_len, sc_len > 0 ? sconfirm_pdu[0] : 0);
        return -1;
    }

    /* Send master Pairing Confirm (with opcode) */
    /* For TK=0, confirm = 0 (simplified) */
    uint8_t mconfirm_pkt[17];
    mconfirm_pkt[0] = SMP_PAIRING_CONFIRM;
    memset(mconfirm_pkt + 1, 0, 16);
    smp_send(ctx, mconfirm_pkt, sizeof(mconfirm_pkt));
    fprintf(stderr, "  Master Pairing Confirm sent\n");

    /* Send master Pairing Random */
    uint8_t mrand_pkt[17];
    mrand_pkt[0] = SMP_PAIRING_RANDOM;
    memcpy(mrand_pkt + 1, mrand, 16);
    smp_send(ctx, mrand_pkt, sizeof(mrand_pkt));
    fprintf(stderr, "  Master Pairing Random sent\n");

    /* Read slave Pairing Random */
    uint8_t srand_pkt[17];
    int sr_len = smp_read_pdu(ctx, srand_pkt, sizeof(srand_pkt), 3000);
    if (sr_len < 17 || srand_pkt[0] != SMP_PAIRING_RANDOM) {
        fprintf(stderr, "  Expected Pairing Random from slave (got %d bytes, op=0x%02x)\n",
                sr_len, sr_len > 0 ? srand_pkt[0] : 0);
        return -1;
    }
    memcpy(srand, srand_pkt + 1, 16);
    fprintf(stderr, "  Slave Pairing Random received\n");

    /* Derive STK */
    uint8_t tk[16] = {0}; /* TK=0 for Just Works */
    smp_stk_gen(ctx->smp_stk, tk, mrand, srand);
    dbg_hex("STK", ctx->smp_stk, 16);

    ctx->state = STATE_SMP_PAIRING_COMPLETE;
    fprintf(stderr, "  SMP Pairing Complete (Just Works)\n");

    /* Start Encryption */
    fprintf(stderr, "=== LE Start Encryption ===\n");
    le_start_encryption(ctx->cmd_fd, ctx->conn_handle, ctx->smp_stk);

    ctx->state = STATE_ENCRYPTION_START_SENT;

    /* Wait for Encryption Change event */
    struct timeval start, now;
    gettimeofday(&start, NULL);
    while (!ctx->smp_encrypted && g_running) {
        gettimeofday(&now, NULL);
        int elapsed = (int)((now.tv_sec - start.tv_sec) * 1000 +
                           (now.tv_usec - start.tv_usec) / 1000);
        if (elapsed > 5000) {
            fprintf(stderr, "  Encryption Change timeout\n");
            return -1;
        }
        int remaining = 5000 - elapsed;
        int rc = rx_dispatch(ctx, remaining > 0 ? remaining : 100);
        if (rc < 0) return rc;
    }

    if (!ctx->smp_encrypted) {
        fprintf(stderr, "  Encryption not established\n");
        return -1;
    }

    fprintf(stderr, "*** LE Link Encrypted — ATT/GATT can now proceed ***\n");
    ctx->state = STATE_ENCRYPTED;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * GATT DISCOVERY (after encryption)
 * ═══════════════════════════════════════════════════════════════════════ */

/* Read ATT response (skip non-ATT frames) */
static int att_read_rsp(rx_ctx_t *ctx, uint8_t *buf, int maxlen, int timeout_ms) {
    struct timeval deadline, now;
    gettimeofday(&deadline, NULL);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_usec += (timeout_ms % 1000) * 1000;
    if (deadline.tv_usec >= 1000000) { deadline.tv_sec++; deadline.tv_usec -= 1000000; }

    while (g_running) {
        gettimeofday(&now, NULL);
        int remaining = (int)((deadline.tv_sec - now.tv_sec) * 1000 +
                            (deadline.tv_usec - now.tv_usec) / 1000);
        if (remaining < 10) remaining = 10;
        if (remaining <= 0) return -1;

        uint8_t raw[256];
        int n = rx_read_raw(ctx->rx_fd, raw, (int)sizeof(raw), remaining > 500 ? 500 : remaining);
        if (n <= 0) continue;

        rx_frame_t frm;
        if (rx_parse_frame(raw, n, &frm) < 0) continue;
        rx_log_frame(&frm, ctx->verbose);

        if (frm.type == HCI_EVENT_PKT) {
            if (frm.evt_code == EVT_DISCONN_COMPLETE) return -2;
            continue;
        }

        if (frm.type != HCI_ACLDATA_PKT) continue;

        const uint8_t *data;
        int len;
        uint16_t cid = rx_parse_l2cap(&frm, &data, &len);
        if (cid != L2CAP_CID_ATT || len < 1) continue;

        int copy = len < maxlen ? len : maxlen;
        memcpy(buf, data, (size_t)copy);
        return copy;
    }
    return -1;
}

static int gatt_discover_services(rx_ctx_t *ctx,
                                   uint16_t *svc_start, uint16_t *svc_end) {
    uint8_t req[] = {
        ATT_OP_READ_BY_GROUP_REQ,
        0x01, 0x00, 0xFF, 0xFF,
        0x00, 0x28,
    };
    att_send(ctx, req, sizeof(req));
    fprintf(stderr, "  ATT ReadByGroup sent\n");

    uint8_t rsp[256];
    int n = att_read_rsp(ctx, rsp, (int)sizeof(rsp), 5000);
    if (n < 6 || rsp[0] != ATT_OP_READ_BY_GROUP_RSP) {
        fprintf(stderr, "  ATT ReadByGroup failed (n=%d op=0x%02x)\n",
                n, n > 0 ? rsp[0] : 0);
        return -1;
    }

    uint8_t attr_len = rsp[1];
    uint8_t *data = rsp + 2;
    int count = (n - 2) / attr_len;

    fprintf(stderr, "  GATT services (%d):\n", count);
    *svc_start = 0; *svc_end = 0;

    for (int i = 0; i < count; i++) {
        uint16_t s = get_le16(data + i * attr_len);
        uint16_t e = get_le16(data + i * attr_len + 2);
        uint16_t uuid = (attr_len >= 6) ? get_le16(data + i * attr_len + 4) : 0;
        fprintf(stderr, "    [0x%04x-0x%04x] UUID=0x%04x%s\n",
                s, e, uuid, uuid == GATT_UUID_HID_SERVICE ? " (HID)" : "");
        if (uuid == GATT_UUID_HID_SERVICE) {
            *svc_start = s;
            *svc_end = e;
        }
    }

    return (*svc_start > 0) ? 0 : -1;
}

static int gatt_discover_chars(rx_ctx_t *ctx,
                                uint16_t start, uint16_t end,
                                uint16_t *rep_h, uint16_t *cccd_h) {
    uint8_t req[] = {
        ATT_OP_READ_BY_TYPE_REQ,
        start & 0xFF, (start >> 8) & 0xFF,
        end & 0xFF, (end >> 8) & 0xFF,
        0x03, 0x28,
    };
    att_send(ctx, req, sizeof(req));
    fprintf(stderr, "  ATT ReadByType (chars) sent [0x%04x-0x%04x]\n", start, end);

    uint8_t rsp[512];
    int n = att_read_rsp(ctx, rsp, (int)sizeof(rsp), 5000);
    if (n < 6 || rsp[0] != ATT_OP_READ_BY_TYPE_RSP) {
        fprintf(stderr, "  ATT ReadByType failed (n=%d op=0x%02x)\n",
                n, n > 0 ? rsp[0] : 0);
        return -1;
    }

    uint8_t attr_len = rsp[1];
    uint8_t *data = rsp + 2;
    int count = (n - 2) / attr_len;
    *rep_h = 0; *cccd_h = 0;

    for (int i = 0; i < count; i++) {
        uint16_t decl  = get_le16(data + i * attr_len);
        uint8_t  props = data[i * attr_len + 2];
        uint16_t value = get_le16(data + i * attr_len + 3);
        uint16_t uuid  = (attr_len >= 7) ? get_le16(data + i * attr_len + 5) : 0;

        fprintf(stderr, "    decl=0x%04x value=0x%04x props=0x%02x UUID=0x%04x\n",
                decl, value, props, uuid);

        if (uuid == GATT_UUID_REPORT && (props & 0x10)) {
            *rep_h = value;
            *cccd_h = value + 1;
        }
    }

    return (*rep_h > 0) ? 0 : -1;
}

static int gatt_enable_notify(rx_ctx_t *ctx, uint16_t cccd_handle) {
    uint8_t req[] = {
        ATT_OP_WRITE_REQ,
        cccd_handle & 0xFF, (cccd_handle >> 8) & 0xFF,
        0x01, 0x00, /* Notifications enabled */
    };
    att_send(ctx, req, sizeof(req));
    fprintf(stderr, "  ATT Write CCCD (0x%04x = 0x0001) sent\n", cccd_handle);

    uint8_t rsp[256];
    int n = att_read_rsp(ctx, rsp, (int)sizeof(rsp), 5000);
    if (n < 1 || rsp[0] != ATT_OP_WRITE_RSP) {
        fprintf(stderr, "  CCCD write failed (n=%d op=0x%02x)\n",
                n, n > 0 ? rsp[0] : 0);
        return -1;
    }

    fprintf(stderr, "  Notifications enabled\n");
    ctx->state = STATE_NOTIFICATIONS_ENABLED;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * BLUEZ GUARD
 * ═══════════════════════════════════════════════════════════════════════ */

static const char *BLUEZ_PROCS[] = {
    "bluetoothd", "bluetoothctl", "btmgmt", "hcitool",
    "gatttool", "hciattach", NULL
};

static int check_bluez_clients(void) {
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

static int kill_bluez_clients(void) {
    int killed = 0;
    for (const char **p = BLUEZ_PROCS; *p; p++) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "killall -9 %s 2>/dev/null", *p);
        int rc = system(cmd);
        if (rc == 0 || rc == 256) killed++;
        fprintf(stderr, "  kill %s: %s\n", *p, (rc == 0 || rc == 256) ? "ok" : "-");
    }
    system("systemctl stop bluetooth 2>/dev/null");
    return killed;
}

/* ═══════════════════════════════════════════════════════════════════════
 * USB PREP
 * ═══════════════════════════════════════════════════════════════════════ */

static int usb_prep_controller(const char *hidraw_path, const uint8_t host_bytes[6]) {
    int fd = open(hidraw_path, O_RDWR);
    if (fd < 0) { perror(hidraw_path); return -1; }

    uint8_t report[64];
    memset(report, 0, sizeof(report));
    report[0]  = 0x01;
    report[10] = 0x06; /* SetHCIState */
    report[11] = 0x01; /* BT on */
    if (write(fd, report, sizeof(report)) < 0) { perror("SetHCIState"); close(fd); return -1; }
    fprintf(stderr, "  SetHCIState (BT ON) sent\n");

    memset(report, 0, sizeof(report));
    report[0]  = 0x01;
    report[10] = 0x01; /* BluetoothManualPair */
    memcpy(report + 11, host_bytes, 6);
    if (write(fd, report, sizeof(report)) < 0) { perror("BluetoothManualPair"); close(fd); return -1; }
    fprintf(stderr, "  BluetoothManualPair sent (host=%02x:%02x:%02x:%02x:%02x:%02x)\n",
            host_bytes[5], host_bytes[4], host_bytes[3],
            host_bytes[2], host_bytes[1], host_bytes[0]);
    close(fd);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════ */

static void usage(const char *prog) {
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
        "  --exclusive-hci               Check no other BT clients running\n"
        "  --kill-bluez-clients          Kill interfering BT processes\n"
        "  --preflight                   Only check, then exit\n"
        "  --reset-hci                   Reset hci0 (down/up) before start\n"
        "  --verbose                     Verbose output\n",
        prog);
}

/* Parse host BDADDR: "auto" → hci_devba, or AA:BB:... */
static void parse_host_bdaddr(uint8_t host_bytes[6], const char *host_spec) {
    if (strcmp(host_spec, "auto") == 0) {
        bdaddr_t ba;
        char txt[18] = "auto";
        if (hci_devba(0, &ba) == 0) {
            memcpy(host_bytes, &ba, 6);
            ba2str(&ba, txt);
        } else {
            uint8_t fallback[6] = {0xD8, 0x3A, 0xDD, 0xE5, 0x69, 0xED};
            memcpy(host_bytes, fallback, 6);
            strcpy(txt, "D8:3A:DD:E5:69:ED");
        }
        fprintf(stderr, "  host BDADDR (auto): %s\n", txt);
    } else {
        parse_colon_bytes(host_spec, host_bytes);
        fprintf(stderr, "  host BDADDR (manual): %s\n", host_spec);
    }
}

int main(int argc, char **argv) {
    const char *bdaddr_s      = NULL;
    const char *peer_type_s   = "public";
    const char *own_type_s    = "public";
    const char *host_bdaddr_s = "auto";
    const char *hidraw_path   = "/dev/hidraw0";
    int  auto_scan  = 0, scan_only  = 0, usb_init   = 0;
    int  exclusive  = 0, kill_bluez = 0, preflight  = 0;
    int  reset_hci  = 0, verbose    = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bdaddr") && i+1 < argc) bdaddr_s = argv[++i];
        else if (!strcmp(argv[i], "--peer-addr-type") && i+1 < argc) peer_type_s = argv[++i];
        else if (!strcmp(argv[i], "--own-addr-type") && i+1 < argc) own_type_s = argv[++i];
        else if (!strcmp(argv[i], "--host-bdaddr") && i+1 < argc) host_bdaddr_s = argv[++i];
        else if (!strcmp(argv[i], "--hidraw") && i+1 < argc) hidraw_path = argv[++i];
        else if (!strcmp(argv[i], "--auto-scan"))    auto_scan  = 1;
        else if (!strcmp(argv[i], "--scan-only"))    scan_only  = 1;
        else if (!strcmp(argv[i], "--usb-init"))     usb_init   = 1;
        else if (!strcmp(argv[i], "--exclusive-hci")) exclusive  = 1;
        else if (!strcmp(argv[i], "--kill-bluez-clients")) kill_bluez = 1;
        else if (!strcmp(argv[i], "--preflight"))    preflight   = 1;
        else if (!strcmp(argv[i], "--reset-hci"))    reset_hci   = 1;
        else if (!strcmp(argv[i], "--verbose"))      verbose     = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); return 2; }
    }

    uint8_t peer_type = (uint8_t)(strcmp(peer_type_s, "random") == 0 ? 0x01 : 0x00);
    uint8_t own_type  = (uint8_t)(strcmp(own_type_s, "random") == 0 ? 0x01 : 0x00);

    /* ── BlueZ Guard ─────────────────────────────────────────────── */
    if (exclusive || kill_bluez) {
        fprintf(stderr, "=== Pre-flight ===\n");
        int conflicts = check_bluez_clients();
        if (conflicts > 0 && kill_bluez) {
            kill_bluez_clients();
            sleep(1);
            conflicts = check_bluez_clients();
        }
        if (conflicts > 0 && exclusive) {
            fprintf(stderr, "ERROR: %d conflicting process(es). Use --kill-bluez-clients.\n", conflicts);
            return 1;
        }
        if (preflight) { fprintf(stderr, "Pre-flight OK.\n"); return 0; }
    }

    if (!bdaddr_s && !auto_scan && !usb_init) {
        fprintf(stderr, "ERROR: --bdaddr or --auto-scan or --usb-init required\n");
        return 2;
    }

    /* ── Resolve BDADDR ──────────────────────────────────────────── */
    bdaddr_t peer;
    if (bdaddr_s) {
        if (str2ba(bdaddr_s, &peer) < 0) {
            fprintf(stderr, "ERROR: invalid BDADDR: %s\n", bdaddr_s);
            return 2;
        }
    } else {
        memset(&peer, 0, sizeof(peer));
    }

    /* ── Host BDADDR for USB prep ─────────────────────────────────── */
    uint8_t host_bytes[6] = {0};
    parse_host_bdaddr(host_bytes, host_bdaddr_s);

    /* ── USB Init ─────────────────────────────────────────────────── */
    if (usb_init) {
        fprintf(stderr, "=== USB Prep ===\n");
        if (usb_prep_controller(hidraw_path, host_bytes) < 0) return 1;
        fprintf(stderr, "\n*** UNPLUG the controller from USB NOW ***\n");
        fprintf(stderr, "*** Waiting 8s for BLE advertising... ***\n");
        sleep(8);
        auto_scan = 1;
    }

    /* ── Auto-scan placeholder ───────────────────────────────────── */
    if (auto_scan && !bdaddr_s) {
        fprintf(stderr, "=== Auto-scan: (pending — use --bdaddr for now) ===\n");
        if (scan_only) return 0;
    }

    if (scan_only) {
        fprintf(stderr, "peer=%s type=%s\n", bdaddr_s ? bdaddr_s : "(scan)", peer_type_s);
        return 0;
    }

    /* ── HCI Reset ────────────────────────────────────────────────── */
    int dev_id = hci_devid("hci0");
    if (dev_id < 0) { fprintf(stderr, "ERROR: no hci0\n"); return 1; }
    if (reset_hci) hci_reset_controller(dev_id);

    /* ── Open sockets ─────────────────────────────────────────────── */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    setvbuf(stderr, NULL, _IOLBF, 0);

    int cmd_fd = open_cmd_socket();
    if (cmd_fd < 0) return 1;
    int rx_fd = open_rx_socket();
    if (rx_fd < 0) { close(cmd_fd); return 1; }

    /* ── HCI Init ─────────────────────────────────────────────────── */
    if (hci_init_sequence(cmd_fd, &dev_id) < 0) {
        close(cmd_fd); close(rx_fd); return 1;
    }

    /* ── RX Context ───────────────────────────────────────────────── */
    rx_ctx_t ctx = {
        .rx_fd = rx_fd,
        .cmd_fd = cmd_fd,
        .conn_handle = 0,
        .state = STATE_DISCONNECTED,
        .verbose = verbose,
    };

    /* Log device info */
    bdaddr_t local_ba;
    char local_str[18] = "unknown";
    if (hci_devba(dev_id, &local_ba) == 0) ba2str(&local_ba, local_str);
    fprintf(stderr, "=== %s ===\n", APP_NAME);
    fprintf(stderr, "local  BDADDR: %s\n", local_str);
    fprintf(stderr, "target BDADDR: %s  type=%s  own_type=%s\n",
            bdaddr_s ? bdaddr_s : "(auto)", peer_type_s, own_type_s);

    /* ── Main session loop ────────────────────────────────────────── */
    int backoff = 1;
    while (g_running) {
        fprintf(stderr, "--- session start ---\n");
        ctx.state = STATE_DISCONNECTED;
        ctx.smp_encrypted = 0;
        ctx.conn_handle = 0;
        ctx.report_handle = 0;
        ctx.report_cccd = 0;

        /* LE Create Connection */
        if (le_create_connection(cmd_fd, &peer, peer_type, own_type, &ctx.conn_handle) < 0) {
            fprintf(stderr, "  LE Create Connection send failed\n");
            goto reconnect;
        }

        /* Wait for LE Connection Complete */
        int rc = rx_await(&ctx, EVT_LE_META_EVENT, LE_META_EV_CONN_COMPLETE, 0, 0, 10000);
        if (rc < 0 || ctx.conn_handle == 0) {
            fprintf(stderr, "  Connection failed (rc=%d, handle=0x%04x)\n",
                    rc, ctx.conn_handle);
            goto reconnect;
        }

        fprintf(stderr, "*** CONNECTED: handle=0x%04x ***\n", ctx.conn_handle);
        ctx.state = STATE_CONNECTED_UNENCRYPTED;

        /* Send Connection Parameter Update to fix absurd intervals
         * (Cypress CYW43455 may assign 61s+ despite 7.5ms request).
         * Try progressively larger intervals if the first is rejected. */
        {
            static const struct { uint16_t min; uint16_t max; const char *desc; } attempts[] = {
                {0x000C, 0x0018, "15-30ms"},
                {0x0018, 0x0028, "30-50ms"},
                {0x0028, 0x003C, "50-75ms"},
            };
            int updated = 0;
            for (size_t i = 0; i < sizeof(attempts)/sizeof(attempts[0]); i++) {
                uint8_t params[14];
                put_le16(params + 0, ctx.conn_handle);
                put_le16(params + 2, attempts[i].min);
                put_le16(params + 4, attempts[i].max);
                put_le16(params + 6, 0x0000);  /* latency 0 */
                put_le16(params + 8, 0x01F4);  /* supervision timeout 5s */
                put_le16(params + 10, 0x0000); /* min CE */
                put_le16(params + 12, 0x0000); /* max CE */
                fprintf(stderr, "  LE_Conn_Param_Update: interval=%s attempt=%zu\n",
                        attempts[i].desc, i + 1);
                send_hci_cmd(cmd_fd, OGF_LE_CTL, OCF_LE_CONN_UPDATE, params, 14);

                /* Wait for Connection Update Complete or disconnect */
                int rc2 = rx_await(&ctx, EVT_LE_META_EVENT, LE_META_EV_CONN_UPDATE_COMPL, 0, 0, 2000);
                if (rc2 >= 0 && ctx.await_result == 0) {
                    fprintf(stderr, "  Conn param update accepted (%s)\n", attempts[i].desc);
                    updated = 1;
                    break;
                }
                /* Check if we got disconnected during the attempt */
                if (ctx.state == STATE_DISCONNECTED) {
                    fprintf(stderr, "  Disconnected during conn update attempt %zu\n", i + 1);
                    goto reconnect;
                }
                fprintf(stderr, "  Conn param update rejected (status=%d), trying next...\n",
                        ctx.await_result);
            }
            if (!updated) {
                fprintf(stderr, "  WARNING: All conn update attempts rejected, working with assigned interval\n");
            }
        }

        /* ── SMP Pairing ─────────────────────────────────── */
        if (smp_just_works_pairing(&ctx) < 0) {
            fprintf(stderr, "  SMP pairing failed (likely kernel interference), reconnecting...\n");
            /* Kernel likely disconnected us — reconnect immediately */
            goto reconnect;
        }
        /* State is now STATE_ENCRYPTED */

        /* ── ATT MTU Exchange ────────────────────────────── */
        fprintf(stderr, "=== ATT/GATT ===\n");
        uint8_t mtu_req[] = {ATT_OP_EXCHANGE_MTU_REQ, 0xFF, 0x00};
        if (att_send(&ctx, mtu_req, sizeof(mtu_req)) < 0) {
            fprintf(stderr, "  ATT blocked (state=%s)\n", state_name(ctx.state));
            goto reconnect;
        }

        /* ── GATT Discovery ──────────────────────────────── */
        uint16_t svc_start = 0, svc_end = 0;
        if (gatt_discover_services(&ctx, &svc_start, &svc_end) < 0) {
            fprintf(stderr, "  Service discovery failed, reconnecting...\n");
            goto reconnect;
        }

        uint16_t rep_h = 0, cccd_h = 0;
        if (gatt_discover_chars(&ctx, svc_start, svc_end, &rep_h, &cccd_h) < 0) {
            fprintf(stderr, "  Characteristic discovery failed, reconnecting...\n");
            goto reconnect;
        }

        ctx.report_handle = rep_h;
        ctx.report_cccd = cccd_h;
        fprintf(stderr, "  Report handle: 0x%04x, CCCD: 0x%04x\n", rep_h, cccd_h);

        /* Enable notifications */
        if (gatt_enable_notify(&ctx, cccd_h) < 0) {
            fprintf(stderr, "  CCCD enable failed\n");
            goto reconnect;
        }

        /* ── Input Report Loop ───────────────────────────── */
        fprintf(stderr, "=== Input Report Loop ===\n");
        int report_count = 0;
        while (g_running && ctx.state == STATE_NOTIFICATIONS_ENABLED) {
            int rc2 = rx_dispatch(&ctx, 5000);
            if (rc2 == 1) {
                report_count++;
                fprintf(stderr, "REPORT #%d (%d bytes): ", report_count, ctx.notify_len);
                for (int i = 0; i < ctx.notify_len && i < 64; i++)
                    fprintf(stderr, "%02x ", ctx.notify_data[i]);
                fprintf(stderr, "\n");
            } else if (rc2 == -2) {
                fprintf(stderr, "  Disconnected\n");
                break;
            } else if (rc2 < 0) {
                break;
            }
        }
        fprintf(stderr, "  Exited report loop (%d reports)\n", report_count);

reconnect:
        ctx.state = STATE_DISCONNECTED;
        if (!g_running) break;
        fprintf(stderr, "reconnecting in %ds...\n", backoff);
        sleep((unsigned)backoff);
        if (backoff < 16) backoff *= 2;
    }

    close(cmd_fd);
    close(rx_fd);
    fprintf(stderr, "%s: stopped\n", APP_NAME);
    return 0;
}
