/*
 * hci_transport_user.h — HCI_CHANNEL_USER transport layer
 *
 * Provides exclusive, kernel-bypass Bluetooth controller access via
 * the Linux HCI User Channel (HCI_CHANNEL_USER).
 *
 * RATIONALE:
 *   HCI_CHANNEL_RAW (the standard approach) gives the kernel shared
 *   access to the controller. After a failed LE Create Connection,
 *   or after a disconnect, the kernel's own Bluetooth subsystem
 *   (BlueZ/MGMT) takes over connection management. A second
 *   LE Create Connection from userspace is then rejected with
 *   CMD_STATUS 0x0c ("Command Disallowed") because the kernel
 *   believes it owns the connection state.
 *
 *   HCI_CHANNEL_USER bypasses this entirely. When hci0 is brought
 *   DOWN (via ioctl HCIDEVDOWN), the socket with HCI_CHANNEL_USER
 *   gets exclusive, unfiltered access to the controller. No kernel
 *   commands leak in, no MGMT contamination, and — crucially —
 *   multiple connect/disconnect cycles work without "Command
 *   Disallowed" rejections.
 *
 *   This is the same pattern used by BlueKitchen's BTstack
 *   (platform/linux/hci_transport_linux.c).
 *
 * USAGE:
 *   1. hci_user_open() — bring hci0 down, open USER socket
 *   2. hci_user_init()  — HCI_Reset + LE Set Event Mask
 *   3. hci_user_send_cmd() / hci_user_read_frame() — raw HCI I/O
 *   4. hci_user_cancel_connect() — cancel pending LE Create Connection
 *   5. hci_user_close() — close socket, optionally restore hci0
 *
 * The USER socket uses SOCK_RAW datagram semantics:
 *   - Each read() consumes exactly one HCI frame
 *   - writev() with [pkt_type][hdr][params] to send
 *   - No HCI filter needed (controller gives us everything)
 *
 * Reference: BlueKitchen BTstack, platform/linux/hci_transport_linux.c
 *   https://github.com/bluekitchen/btstack
 */

#ifndef HCI_TRANSPORT_USER_H
#define HCI_TRANSPORT_USER_H

#include <stdint.h>
#include <sys/socket.h>

/* HCI packet types (matching BT spec) */
#define HCI_COMMAND_PKT  0x01
#define HCI_ACLDATA_PKT  0x02
#define HCI_EVENT_PKT    0x04

/* HCI event codes */
#define EVT_CMD_COMPLETE          0x0e
#define EVT_CMD_STATUS            0x0f
#define EVT_DISCONN_COMPLETE      0x05
#define EVT_NUM_COMP_PKTS         0x13
#define EVT_ENCRYPT_CHANGE        0x08
#define EVT_LE_META               0x3e
#define EVT_LE_CONN_COMPLETE      0x01

/* OGF/OCF for common commands */
#define OGF_LE_CTL                0x08
#define OCF_LE_SET_EVENT_MASK     0x0001
#define OCF_LE_CREATE_CONN        0x000d
#define OCF_LE_CREATE_CONN_CANCEL 0x000e
#define OCF_LE_START_ENCRYPTION   0x0019
#define OCF_LE_LTK_REQ_REPLY      0x001a

/* OCF for Device Under Test mode commands */
#define OCF_HCI_RESET             0x0003  /* OGF 0x03 */

/*
 * Parsed HCI frame from hci_user_read_frame().
 * Caller provides the buffer; we fill in pointers and metadata.
 */
typedef struct {
    uint8_t  pkt_type;        /* HCI_COMMAND_PKT, HCI_EVENT_PKT, HCI_ACLDATA_PKT */
    uint8_t  evt_code;        /* valid when pkt_type == HCI_EVENT_PKT */
    uint8_t  subevent;        /* valid when evt_code == EVT_LE_META */
    uint8_t  status;          /* CMD_STATUS status, or DISCONN reason, or ENCRYPT status */
    uint16_t opcode;          /* CMD_STATUS opcode */
    uint16_t handle;          /* ACL handle or DISCONN handle */
    uint16_t cid;             /* L2CAP CID (valid when ACL) */
    uint16_t acl_len;         /* ACL data length (HCI header) */
    uint16_t l2cap_len;       /* L2CAP payload length */
    const uint8_t *payload;   /* points into caller's buffer */
    uint16_t payload_len;     /* length of payload */
    /* Raw buffer */
    uint8_t  raw[512];
    uint16_t raw_len;
} hci_user_frame_t;

/*
 * Open HCI_CHANNEL_USER socket.
 *
 * Prerequisites: bluetoothd must be stopped, no other BT clients
 * holding /dev/hci0 or the MGMT socket.
 *
 * Side effects:
 *   - Brings hci0 DOWN via ioctl(SIOCDEVPRIVATE) / HCIDEVDOWN
 *   - Opens AF_BLUETOOTH SOCK_RAW BTPROTO_HCI socket
 *   - Binds with HCI_CHANNEL_USER on hci0
 *
 * Returns fd >= 0 on success, -1 on error (message to stderr).
 */
int hci_user_open(void);

/*
 * Close the USER socket.
 * Does NOT restore hci0 state (leave it for the kernel or next user).
 */
void hci_user_close(int fd);

/*
 * Initialize controller for our session.
 * Sends HCI_Reset then LE Set Event Mask.
 *
 * Must be called after hci_user_open() and before any connection.
 * The Reset ensures the controller is in a clean state regardless
 * of what BlueZ or a previous session left behind.
 *
 * Returns 0 on success, -1 on error.
 */
int hci_user_init(int fd);

/*
 * Send a raw HCI command through the USER socket.
 *
 * fd:     USER socket from hci_user_open()
 * ogf:    OpCode Group Field
 * ocf:    OpCode Command Field
 * params: command parameters (can be NULL if plen == 0)
 * plen:   parameter length
 *
 * Uses writev() to send [pkt_type=0x01][opcode|plen][params] in
 * a single system call. The USER channel does not add framing.
 *
 * Returns 0 on success, -1 on error (message to stderr).
 */
int hci_user_send_cmd(int fd, uint16_t ogf, uint16_t ocf,
                      const uint8_t *params, uint8_t plen);

/*
 * Read one HCI frame from the USER socket.
 *
 * Blocks for up to timeout_ms milliseconds (poll-based).
 * Returns:  1 = frame read and parsed into *f
 *            0 = timeout (no data)
 *           -1 = error
 *
 * The frame's payload pointer points into f->raw.
 * f->raw_len bytes are valid for the full frame.
 */
int hci_user_read_frame(int fd, hci_user_frame_t *f, int timeout_ms);

/*
 * Cancel a pending LE Create Connection.
 *
 * After an LE Create Connection that timed out or was rejected,
 * the controller may still have the command in its queue. A second
 * LE Create Connection will be rejected with CMD_STATUS 0x0c
 * ("Command Disallowed") unless we explicitly cancel the first one.
 *
 * Call this before retrying LE Create Connection if:
 *   - The previous attempt timed out (controller not advertising)
 *   - The previous LE Connection Complete had status != 0x00
 *   - You got CMD_STATUS 0x0c on retry
 *
 * Returns 0 on success, -1 on error.
 */
int hci_user_cancel_connect(int fd);

/* ── ACL data send (convenience, uses hci_user_send_cmd internally) ──── */

/*
 * Send ACL data on a specific L2CAP CID.
 *
 * fd:       USER socket
 * handle:   connection handle (12-bit)
 * cid:      L2CAP channel ID (ATT=0x0004, SMP=0x0006, etc.)
 * data:     L2CAP payload
 * len:      payload length (max 512)
 *
 * Returns 0 on success, -1 on error.
 */
int hci_user_acl_send(int fd, uint16_t handle, uint16_t cid,
                      const uint8_t *data, uint16_t len);

#endif /* HCI_TRANSPORT_USER_H */
