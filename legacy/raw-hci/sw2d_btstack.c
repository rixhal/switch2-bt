/*
 * sw2d_btstack.c - Switch 2 Pro Controller BLE HID Host
 * Uses BTstack Linux HCI socket transport (no raw UART needed)
 */

#define BTSTACK_FILE__ "sw2d_btstack.c"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btstack_config.h"
#include "btstack.h"
#include "btstack_run_loop.h"
#include "hci_transport.h"
#include "hci_transport_linux.h"

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
/* NOTE: do NOT include hci_lib.h — conflicts with BTstack internal symbols */

#define NINTENDO_COMPANY_ID 0x0553
#define SWITCH_PRO2_VID     0x057E
#define SWITCH_PRO2_PID     0x2069

typedef enum {
    W4_WORKING,
    W4_SCAN_RESULT,
    W4_CONNECT,
    W4_SERVICE_RESULT,
    W4_CHARACTERISTIC_RESULT,
    W4_ENABLE_NOTIFICATIONS_COMPLETE,
    READY
} state_t;

static state_t state = W4_WORKING;
static bd_addr_t remote_addr;
static bd_addr_type_t remote_addr_type;
static hci_con_handle_t connection_handle;
static gatt_client_service_t hid_service;
static gatt_client_characteristic_t hid_report_characteristic;
static gatt_client_notification_t notification_listener;
static btstack_packet_callback_registration_t hci_event_callback_registration;

static inline uint16_t le16(const uint8_t *b, int p) {
    return (uint16_t)b[p] | ((uint16_t)b[p+1] << 8);
}
static inline uint32_t le32(const uint8_t *b, int p) {
    return (uint32_t)b[p] | ((uint32_t)b[p+1] << 8) |
           ((uint32_t)b[p+2] << 16) | ((uint32_t)b[p+3] << 24);
}

static void parse_switch_report(const uint8_t *data, uint16_t size) {
    if (size < 63) return;
    uint32_t buttons = le32(data, 4);
    uint16_t raw_lx = data[10] | ((data[11] & 0x0F) << 8);
    uint16_t raw_ly = (data[11] >> 4) | (data[12] << 4);
    uint16_t raw_rx = data[13] | ((data[14] & 0x0F) << 8);
    uint16_t raw_ry = (data[14] >> 4) | (data[15] << 4);
    printf("BTN:%08X LX:%3d LY:%3d RX:%3d RY:%3d L2:%3d R2:%3d\n",
           buttons, raw_lx>>4, raw_ly>>4, raw_rx>>4, raw_ry>>4, data[60], data[61]);
}

static void start_scan(void) {
    state = W4_SCAN_RESULT;
    gap_set_scan_parameters(0, 0x0030, 0x0030);
    gap_start_scan();
}

static void handle_gatt_client_event(uint8_t pt, uint16_t ch, uint8_t *pkt, uint16_t sz) {
    (void)pt; (void)ch; (void)sz;
    switch (hci_event_packet_get_type(pkt)) {
        case GATT_EVENT_SERVICE_QUERY_RESULT:
            gatt_event_service_query_result_get_service(pkt, &hid_service);
            break;
        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
            gatt_event_characteristic_query_result_get_characteristic(pkt, &hid_report_characteristic);
            break;
        case GATT_EVENT_QUERY_COMPLETE:
            if (state == W4_SERVICE_RESULT) {
                state = W4_CHARACTERISTIC_RESULT;
                gatt_client_discover_characteristics_for_service_by_uuid16(
                    handle_gatt_client_event, connection_handle,
                    &hid_service, 0x2A4D /* Report */);
            } else if (state == W4_CHARACTERISTIC_RESULT) {
                state = W4_ENABLE_NOTIFICATIONS_COMPLETE;
                gatt_client_listen_for_characteristic_value_updates(
                    &notification_listener, handle_gatt_client_event,
                    connection_handle, &hid_report_characteristic);
                gatt_client_write_client_characteristic_configuration(
                    handle_gatt_client_event, connection_handle,
                    &hid_report_characteristic,
                    GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
            } else if (state == W4_ENABLE_NOTIFICATIONS_COMPLETE) {
                state = READY;
                printf("=== HID READY ===\n");
            }
            break;
        case GATT_EVENT_NOTIFICATION:
            parse_switch_report(gatt_event_notification_get_value(pkt),
                               gatt_event_notification_get_value_length(pkt));
            break;
    }
}

static void packet_handler(uint8_t pt, uint16_t ch, uint8_t *pkt, uint16_t sz) {
    (void)ch; (void)sz;
    switch (pt) {
        case HCI_EVENT_PACKET:
            switch (hci_event_packet_get_type(pkt)) {
                case BTSTACK_EVENT_STATE:
                    if (btstack_event_state_get_state(pkt) == HCI_STATE_WORKING) {
                        printf("BTstack ready, scanning...\n");
                        start_scan();
                    }
                    break;
                case GAP_EVENT_ADVERTISING_REPORT:
                    if (state != W4_SCAN_RESULT) break;
                    if (gap_event_advertising_report_get_advertising_event_type(pkt) != 0) break;
                    {
                        bd_addr_t a; uint8_t at, el;
                        gap_event_advertising_report_get_address(pkt, a);
                        at = gap_event_advertising_report_get_address_type(pkt);
                        el = gap_event_advertising_report_get_data_length(pkt);
                        ad_context_t ctx;
                        for (ad_iterator_init(&ctx, el, gap_event_advertising_report_get_data(pkt));
                             ad_iterator_has_more(&ctx); ad_iterator_next(&ctx)) {
                            if (ad_iterator_get_data_type(&ctx) != BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA)
                                continue;
                            uint8_t dl = ad_iterator_get_data_len(&ctx);
                            const uint8_t *d = ad_iterator_get_data(&ctx);
                            if (dl >= 6 && le16(d,0) == NINTENDO_COMPANY_ID
                                && le16(d,2) == SWITCH_PRO2_VID
                                && le16(d,4) == SWITCH_PRO2_PID) {
                                printf("Found Switch Pro 2\n");
                                memcpy(remote_addr, a, 6);
                                remote_addr_type = at;
                                state = W4_CONNECT;
                                gap_stop_scan();
                                gap_connect(remote_addr, remote_addr_type);
                                return;
                            }
                        }
                    }
                    break;
                case HCI_EVENT_LE_META:
                    if (hci_event_le_meta_get_subevent_code(pkt) == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
                        if (hci_subevent_le_connection_complete_get_status(pkt)) {
                            printf("Connect failed\n"); start_scan(); break;
                        }
                        connection_handle = hci_subevent_le_connection_complete_get_connection_handle(pkt);
                        printf("Connected, discovering HID...\n");
                        state = W4_SERVICE_RESULT;
                        gatt_client_discover_primary_services_by_uuid16(
                            handle_gatt_client_event, connection_handle,
                            ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE);
                    }
                    break;
                case HCI_EVENT_DISCONNECTION_COMPLETE:
                    printf("Disconnected\n");
                    start_scan();
                    break;
            }
            break;
    }
}

int btstack_main(int argc, const char *argv[]) {
    (void)argc; (void)argv;

    fprintf(stderr, "BTstack init...\n");

    /* Init Linux HCI socket transport FIRST */
    fprintf(stderr, "  hci_transport_linux_instance...\n");
    hci_transport_config_linux_t config = {
        .type = HCI_TRANSPORT_CONFIG_LINUX,
        .device_id = 0,
    };
    const hci_transport_t *transport = hci_transport_linux_instance();
    fprintf(stderr, "  hci_init...\n");
    hci_init(transport, &config);
    fprintf(stderr, "  hci_init done\n");

    /* Now register event handlers and init subsystems */
    fprintf(stderr, "  hci_add_event_handler...\n");
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    fprintf(stderr, "  l2cap_init...\n");
    l2cap_init();
    fprintf(stderr, "  sm_init...\n");
    sm_init();
    /* Switch 2 controller uses LEGACY pairing (no SC, no MITM) */
    fprintf(stderr, "  sm_set_io...\n");
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    fprintf(stderr, "  sm_set_auth...\n");
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING);
    fprintf(stderr, "  gatt_client_init...\n");
    gatt_client_init();

    /* Bring up HCI */
    fprintf(stderr, "  hci_power_control...\n");
    hci_power_control(HCI_POWER_ON);

    return 0;
}

int main(int argc, const char *argv[]) {
    /* Run btstack_main, then enter the run loop */
    btstack_main(argc, argv);
    btstack_run_loop_execute();
    return 0;
}
