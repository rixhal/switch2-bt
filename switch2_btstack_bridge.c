/*
 * switch2_btstack_bridge.c — Phase B: BTstack GATT Bridge
 *
 * BLE scan → connect → GATT discovery → CareyScott pair commands →
 * input report notifications → hexdump
 *
 * NO SMP. Uses custom GATT pair writes per CareyScott/switch2controllerpc.
 * BTstack HCI transport (HCI_CHANNEL_USER or h4), no BlueZ D-Bus runtime.
 *
 * Build: cmake .. && make switch2_btstack_bridge
 * Run:   hciconfig hci0 down && sudo ./switch2_btstack_bridge
 */

#define BTSTACK_FILE__ "switch2_btstack_bridge.c"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

#include "btstack_config.h"
#include "btstack.h"

/* ── Protocol constants ──────────────────────────────────────────────────── */

#define NINTENDO_MANUFACTURER  0x0553

/* 128-bit GATT UUIDs (kept for reference; filtering done at app level) */
static const uint8_t UUID_INPUT_REPORT[16] = {
    0xab, 0x7d, 0xe9, 0xbe, 0x89, 0xfe, 0x49, 0xad,
    0x82, 0x8f, 0x11, 0x8f, 0x09, 0xdf, 0x7f, 0xd2
};
static const uint8_t UUID_COMMAND_WRITE[16] = {
    0x64, 0x9d, 0x4a, 0xc9, 0x8e, 0xb7, 0x4e, 0x6c,
    0xaf, 0x44, 0x1e, 0xa5, 0x4f, 0xe5, 0xf0, 0x05
};

/* Pair keys (17 bytes each, 0x00 prefix + 16 bytes) */
static const uint8_t PAIR_KEY_1[17] = {
    0x00, 0xea, 0xbd, 0x47, 0x13, 0x89, 0x35, 0x42,
    0xc6, 0x79, 0xee, 0x07, 0xf2, 0x53, 0x2c, 0x6c, 0x31
};
static const uint8_t PAIR_KEY_2[17] = {
    0x00, 0x40, 0xb0, 0x8a, 0x5f, 0xcd, 0x1f, 0x9b,
    0x41, 0x12, 0x5c, 0xac, 0xc6, 0x3f, 0x38, 0xa0, 0x73
};

static const uint8_t HOST_MAC[6] = {0xD8, 0x3A, 0xDD, 0xE5, 0x69, 0xED};

/* ── State ───────────────────────────────────────────────────────────────── */

typedef enum {
    STATE_SCANNING,
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_SERVICE_DISCOVERING,
    STATE_CHAR_DISCOVERING,
    STATE_WAIT_ENCRYPTION,
    STATE_PAIRING,
    STATE_NOTIFYING,
    STATE_READY,
} app_state_t;

static app_state_t  app_state;
static bd_addr_t    remote_addr;
static bd_addr_type_t remote_addr_type;
static hci_con_handle_t conn_handle = HCI_CON_HANDLE_INVALID;

static uint16_t discover_service_index;
static uint16_t discover_services[32];
static uint16_t discover_service_end[32];
static int      discover_service_count;

static gatt_client_characteristic_t cmd_write_char;
static gatt_client_characteristic_t input_report_char;
static gatt_client_notification_t   input_notification;
static int pair_step;

/* ── Logging ─────────────────────────────────────────────────────────────── */

static void bridge_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    fflush(stderr);
}

static void hex_dump(const char *label, const uint8_t *data, int len) {
    fprintf(stderr, "  %s (%d): ", label, len);
    for (int i = 0; i < len; i++) fprintf(stderr, "%02x ", data[i]);
    fprintf(stderr, "\n");
}

/* ── Command builder ─────────────────────────────────────────────────────── */

static int build_cmd(uint8_t cmd_id, uint8_t sub_id,
                     const uint8_t *data, uint8_t data_len,
                     uint8_t *out) {
    out[0] = cmd_id;
    out[1] = 0x91;
    out[2] = 0x01;
    out[3] = sub_id;
    out[4] = 0x00;
    out[5] = data_len;
    out[6] = 0x00;
    out[7] = 0x00;
    if (data_len) memcpy(out + 8, data, data_len);
    return 8 + data_len;
}

/* ── Forward declarations ────────────────────────────────────────────────── */

static void gatt_client_handler(uint8_t packet_type, uint16_t channel,
                                 uint8_t *packet, uint16_t size);
static void packet_handler(uint8_t packet_type, uint16_t channel,
                            uint8_t *packet, uint16_t size);

/* ── Pair sequence ───────────────────────────────────────────────────────── */

static void write_pair_step(void) {
    uint8_t payload[32];
    int plen;

    switch (pair_step) {
    case 0: { /* SetMAC */
        uint8_t setmac[14];
        setmac[0] = 0x00; setmac[1] = 0x02;
        memcpy(setmac + 2,  HOST_MAC, 6);
        memcpy(setmac + 8,  HOST_MAC, 6);
        plen = build_cmd(0x15, 0x01, setmac, 14, payload);
        bridge_log("PAIR SetMAC");
        hex_dump("  payload", payload, plen);
        break;
    }
    case 1:
        plen = build_cmd(0x15, 0x04, PAIR_KEY_1, 17, payload);
        bridge_log("PAIR Key1");
        hex_dump("  payload", payload, plen);
        break;
    case 2:
        plen = build_cmd(0x15, 0x02, PAIR_KEY_2, 17, payload);
        bridge_log("PAIR Key2");
        hex_dump("  payload", payload, plen);
        break;
    case 3: {
        uint8_t finish = 0x00;
        plen = build_cmd(0x15, 0x03, &finish, 1, payload);
        bridge_log("PAIR Finish");
        hex_dump("  payload", payload, plen);
        break;
    }
    default:
        return;
    }

    gatt_client_write_value_of_characteristic(
        gatt_client_handler, conn_handle,
        cmd_write_char.value_handle, plen, payload);
}

/* ── GATT Client handler ─────────────────────────────────────────────────── */

static void gatt_client_handler(uint8_t packet_type, uint16_t channel,
                                 uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);
    /* Accept all packet types — GATT events may use non-HCI types */

    uint8_t event = hci_event_packet_get_type(packet);
    /* DEBUG: log every event */
    bridge_log("GATT-HANDLER event=0x%02x", event);

    switch (event) {

    case GATT_EVENT_SERVICE_QUERY_RESULT: {
        gatt_client_service_t svc;
        gatt_event_service_query_result_get_service(packet, &svc);
        if (discover_service_count < 32) {
            discover_services[discover_service_count] = svc.start_group_handle;
            discover_service_end[discover_service_count] = svc.end_group_handle;
            discover_service_count++;
        }
        bridge_log("  Svc 0x%04x-0x%04x uuid=0x%04x",
                   svc.start_group_handle, svc.end_group_handle, svc.uuid16);
        break;
    }

    case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
        gatt_client_characteristic_t c;
        gatt_event_characteristic_query_result_get_characteristic(packet, &c);
        bridge_log("  Char val=0x%04x props=0x%02x", c.value_handle, c.properties);

        /* First discovery: input report, second: command write */
        if (!input_report_char.value_handle) {
            memcpy(&input_report_char, &c, sizeof(input_report_char));
            bridge_log("    ^ INPUT REPORT");
        } else if (!cmd_write_char.value_handle) {
            memcpy(&cmd_write_char, &c, sizeof(cmd_write_char));
            bridge_log("    ^ CMD WRITE");
        }
        break;
    }

    case GATT_EVENT_QUERY_COMPLETE: {
        uint8_t att = gatt_event_query_complete_get_att_status(packet);
        if (att != ATT_ERROR_SUCCESS) {
            bridge_log("GATT query error: 0x%02x", att);
            gap_disconnect(conn_handle);
            return;
        }

        if (app_state == STATE_CHAR_DISCOVERING) {
            /* Phase 1: discover INPUT_REPORT by UUID128 */
            if (!input_report_char.value_handle) {
                bridge_log("Phase 1: discovering INPUT_REPORT by UUID128...");
                gatt_client_discover_characteristics_for_handle_range_by_uuid128(
                    gatt_client_handler, conn_handle, 0x0001, 0xFFFF, UUID_INPUT_REPORT);
                break;
            }
            /* Phase 2: discover COMMAND_WRITE by UUID128 */
            if (!cmd_write_char.value_handle) {
                bridge_log("Phase 2: discovering COMMAND_WRITE by UUID128...");
                gatt_client_discover_characteristics_for_handle_range_by_uuid128(
                    gatt_client_handler, conn_handle, 0x0001, 0xFFFF, UUID_COMMAND_WRITE);
                break;
            }
            bridge_log("Char discovery complete. cmd=0x%04x input=0x%04x",
                       cmd_write_char.value_handle, input_report_char.value_handle);
            if (!cmd_write_char.value_handle || !input_report_char.value_handle) {
                bridge_log("ERROR: Missing characteristics");
                gap_disconnect(conn_handle);
                return;
            }
            app_state = STATE_PAIRING;
            pair_step = 0;
            bridge_log("=== CareyScott pair sequence ===");
            write_pair_step();
        }
        else if (app_state == STATE_PAIRING) {
            pair_step++;
            if (pair_step < 4) {
                write_pair_step();
            } else {
                bridge_log("Pair done. Enabling notifications...");
                app_state = STATE_NOTIFYING;
                gatt_client_write_client_characteristic_configuration(
                    gatt_client_handler, conn_handle,
                    &input_report_char,
                    GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
            }
        }
        else if (app_state == STATE_NOTIFYING) {
            gatt_client_listen_for_characteristic_value_updates(
                &input_notification, gatt_client_handler,
                conn_handle, &input_report_char);
            app_state = STATE_READY;
            bridge_log("=== READY — waiting for reports ===");
        }
        break;
    }

    case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT: {
        uint16_t val_handle = gatt_event_characteristic_value_query_result_get_value_handle(packet);
        uint16_t val_len    = gatt_event_characteristic_value_query_result_get_value_length(packet);
        const uint8_t *val  = gatt_event_characteristic_value_query_result_get_value(packet);
        if (val_handle == input_report_char.value_handle) {
            hex_dump("REPORT", val, val_len);
        }
        break;
    }

    default:
        break;
    }
}

/* ── Advertisement filter ────────────────────────────────────────────────── */

static bool adv_is_nintendo(const uint8_t *packet) {
    const uint8_t *ad = gap_event_advertising_report_get_data(packet);
    uint8_t len = gap_event_advertising_report_get_data_length(packet);
    uint8_t pos = 0;
    while (pos + 1 < len) {
        uint8_t elen = ad[pos];
        uint8_t type = ad[pos + 1];
        if (elen == 0 || pos + 1 + elen > len) break;
        if (type == 0xFF && elen >= 3) {
            uint16_t cid = ad[pos + 2] | (ad[pos + 3] << 8);
            if (cid == NINTENDO_MANUFACTURER) return true;
        }
        pos += 1 + elen;
    }
    return false;
}

/* ── SM event handler ─────────────────────────────────────────────────────── */

static void sm_packet_handler(uint8_t packet_type, uint16_t channel,
                               uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {

    case SM_EVENT_NUMERIC_COMPARISON_REQUEST: {
        uint32_t passkey = sm_event_numeric_comparison_request_get_passkey(packet);
        bridge_log("SM Numeric Comparison: %06" PRIu32 " — auto-confirming", passkey);
        sm_numeric_comparison_confirm(sm_event_numeric_comparison_request_get_handle(packet));
        break;
    }

    case SM_EVENT_JUST_WORKS_REQUEST:
        bridge_log("SM Just Works request from controller — accepting, wait encryption");
        sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
        app_state = STATE_WAIT_ENCRYPTION;
        break;

    case SM_EVENT_PAIRING_COMPLETE: {
        uint8_t s = sm_event_pairing_complete_get_status(packet);
        bridge_log("SM Pairing %s (0x%02x)", s == ERROR_CODE_SUCCESS ? "OK" : "FAILED", s);
        /* Start GATT regardless — controller may accept after Just Works */
        if (app_state == STATE_WAIT_ENCRYPTION) {
            bridge_log("Proceeding to GATT (encryption may not be active)");
            app_state = STATE_CHAR_DISCOVERING;
            discover_service_index = 0;
            discover_service_count = 0;
            memset(&cmd_write_char, 0, sizeof(cmd_write_char));
            memset(&input_report_char, 0, sizeof(input_report_char));
            /* Start Phase 1: discover INPUT_REPORT */
            gatt_client_discover_characteristics_for_handle_range_by_uuid128(
                gatt_client_handler, conn_handle, 0x0001, 0xFFFF, UUID_INPUT_REPORT);
        }
        break;
    }

    default:
        break;
    }
}

/* ── Main packet handler ─────────────────────────────────────────────────── */

static void packet_handler(uint8_t packet_type, uint16_t channel,
                            uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {

    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) break;
        bridge_log("BTstack running. Scanning for Nintendo 0x%04x...",
                   NINTENDO_MANUFACTURER);
        app_state = STATE_SCANNING;
        gap_set_connection_parameters(48, 24, 6, 8, 0, 50, 0, 0);
        gap_set_scan_parameters(0, 48, 48);
        gap_start_scan();
        break;

    case GAP_EVENT_ADVERTISING_REPORT:
        if (app_state != STATE_SCANNING) break;
        if (!adv_is_nintendo(packet)) break;
        gap_event_advertising_report_get_address(packet, remote_addr);
        remote_addr_type = gap_event_advertising_report_get_address_type(packet);
        bridge_log("Found: %s rssi=%d type=%d",
                   bd_addr_to_str(remote_addr),
                   gap_event_advertising_report_get_rssi(packet),
                   remote_addr_type);
        gap_stop_scan();
        app_state = STATE_CONNECTING;
        bridge_log("Connecting...");
        gap_connect(remote_addr, remote_addr_type);
        break;

    case HCI_EVENT_META_GAP: {
        uint8_t sub = hci_event_gap_meta_get_subevent_code(packet);
        if (sub == GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
            conn_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
            uint8_t st = gap_subevent_le_connection_complete_get_status(packet);
            if (st != ERROR_CODE_SUCCESS) {
                bridge_log("Connect failed: 0x%02x", st);
                app_state = STATE_SCANNING;
                gap_start_scan();
                break;
            }
            bridge_log("Connected handle=0x%04x — SMP Just Works then GATT", conn_handle);
            app_state = STATE_CONNECTED;
            sm_request_pairing(conn_handle);
        }
        break;
    }

    case HCI_EVENT_ENCRYPTION_CHANGE:
        if (app_state == STATE_WAIT_ENCRYPTION) {
            uint8_t enc_status = hci_event_encryption_change_get_status(packet);
            bridge_log("Encryption change: 0x%02x — starting GATT", enc_status);
            app_state = STATE_CHAR_DISCOVERING;
            discover_service_index = 0;
            discover_service_count = 0;
            memset(&cmd_write_char, 0, sizeof(cmd_write_char));
            memset(&input_report_char, 0, sizeof(input_report_char));
            gatt_client_discover_characteristics_for_handle_range_by_uuid128(
                gatt_client_handler, conn_handle, 0x0001, 0xFFFF, UUID_INPUT_REPORT);
        }
        break;

    case HCI_EVENT_DISCONNECTION_COMPLETE:
        bridge_log("Disconnected. Reconnecting...");
        conn_handle = HCI_CON_HANDLE_INVALID;
        memset(&cmd_write_char, 0, sizeof(cmd_write_char));
        memset(&input_report_char, 0, sizeof(input_report_char));
        discover_service_count = 0;
        sleep(1);
        app_state = STATE_SCANNING;
        gap_set_scan_parameters(0, 48, 48);
        gap_start_scan();
        break;

    default:
        /* Route all other events to GATT handler */
        gatt_client_handler(packet_type, channel, packet, size);
        break;
    }
}

static btstack_packet_callback_registration_t hci_event_callback_reg;
static btstack_packet_callback_registration_t sm_event_callback_reg;

/* ── btstack_main ────────────────────────────────────────────────────────── */

int btstack_main(int argc, const char *argv[]);

int btstack_main(int argc, const char *argv[]) {
    (void)argc;
    (void)argv;

    bridge_log("=== Switch 2 BTstack GATT Bridge (Phase B) ===");
    bridge_log("Protocol: CareyScott custom GATT pairing (no SMP)");

    l2cap_init();

    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_NO_BONDING);
    sm_set_secure_connections_only_mode(false);
    gatt_client_init();

    hci_event_callback_reg.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_reg);

    sm_event_callback_reg.callback = &sm_packet_handler;
    sm_add_event_handler(&sm_event_callback_reg);

    (void)UUID_INPUT_REPORT;
    (void)UUID_COMMAND_WRITE;
    (void)HOST_MAC;

    setvbuf(stderr, NULL, _IONBF, 0);

    hci_power_control(HCI_POWER_ON);
    return 0;
}
