/*
 * sw2d_btstack.c - Switch 2 Pro Controller BLE HID Host
 */

#define BTSTACK_FILE__ "sw2d_btstack.c"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btstack_config.h"
#include "btstack.h"

#define NINTENDO_COMPANY_ID 0x0553
#define SWITCH_PRO2_VID 0x057E
#define SWITCH_PRO2_PID 0x2069

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

static const uint8_t adv_sid_broadcast_isochronous_group[] = { 0x18, 0x12 };
static btstack_packet_callback_registration_t hci_event_callback_registration;

static void parse_switch_report(const uint8_t *data, uint16_t size);

static inline uint16_t little_endian_read_16(const uint8_t *buffer, int pos) {
    return (uint16_t)buffer[pos] | ((uint16_t)buffer[pos + 1] << 8);
}

static inline uint32_t little_endian_read_32(const uint8_t *buffer, int pos) {
    return (uint32_t)buffer[pos] | ((uint32_t)buffer[pos + 1] << 8) |
           ((uint32_t)buffer[pos + 2] << 16) | ((uint32_t)buffer[pos + 3] << 24);
}

static void parse_switch_report(const uint8_t *data, uint16_t size) {
    if (size != 63) {
        printf("Invalid report size: %d\n", size);
        return;
    }

    // Bytes 4-7: buttons (32-bit little-endian)
    uint32_t buttons = little_endian_read_32(data, 4);
    
    // Bytes 10-15: 12-bit packed axes (LX, LY, RX, RY)
    uint16_t raw_lx = data[10] | ((data[11] & 0x0F) << 8);
    uint16_t raw_ly = (data[11] >> 4) | (data[12] << 4);
    uint16_t raw_rx = data[13] | ((data[14] & 0x0F) << 8);
    uint16_t raw_ry = (data[14] >> 4) | (data[15] << 4);
    
    // Convert 12-bit values to 8-bit (0-255)
    uint8_t lx = raw_lx >> 4;
    uint8_t ly = raw_ly >> 4;
    uint8_t rx = raw_rx >> 4;
    uint8_t ry = raw_ry >> 4;
    
    // Bytes 60-61: L2/R2 triggers
    uint8_t l2 = data[60];
    uint8_t r2 = data[61];
    
    printf("Buttons: 0x%08X, LX: %3d, LY: %3d, RX: %3d, RY: %3d, L2: %3d, R2: %3d\n",
           buttons, lx, ly, rx, ry, l2, r2);
}

static void start_scan(void) {
    state = W4_SCAN_RESULT;
    gap_set_scan_parameters(0, 0x0030, 0x0030);
    gap_start_scan();
}

static void handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    switch (hci_event_packet_get_type(packet)) {
        case GATT_EVENT_SERVICE_QUERY_RESULT:
            gatt_event_service_query_result_get_service(packet, &hid_service);
            break;
        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
            gatt_event_characteristic_query_result_get_characteristic(packet, &hid_report_characteristic);
            break;
        case GATT_EVENT_QUERY_COMPLETE:
            switch (state) {
                case W4_SERVICE_RESULT:
                    state = W4_CHARACTERISTIC_RESULT;
                    gatt_client_discover_characteristics_for_service_by_uuid16(handle_gatt_client_event, connection_handle, &hid_service, GATT_CHARACTERISTIC_REPORT);
                    break;
                case W4_CHARACTERISTIC_RESULT:
                    state = W4_ENABLE_NOTIFICATIONS_COMPLETE;
                    gatt_client_listen_for_characteristic_value_updates(&notification_listener, handle_gatt_client_event, connection_handle, &hid_report_characteristic);
                    gatt_client_write_client_characteristic_configuration(handle_gatt_client_event, connection_handle, &hid_report_characteristic, GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
                    break;
                case W4_ENABLE_NOTIFICATIONS_COMPLETE:
                    state = READY;
                    printf("HID notifications enabled, ready for input\n");
                    break;
                default:
                    break;
            }
            break;
        case GATT_EVENT_NOTIFICATION:
            parse_switch_report(gatt_event_notification_get_value(packet), gatt_event_notification_get_value_length(packet));
            break;
        default:
            break;
    }
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);
    
    bd_addr_t addr;
    uint8_t addr_type;
    uint8_t event_type;
    uint8_t status;

    switch (packet_type) {
        case HCI_EVENT_PACKET:
            switch (hci_event_packet_get_type(packet)) {
                case BTSTACK_EVENT_STATE:
                    if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                        start_scan();
                    }
                    break;
                case GAP_EVENT_ADVERTISING_REPORT:
                    if (state != W4_SCAN_RESULT) break;
                    
                    gap_event_advertising_report_get_address(packet, addr);
                    addr_type = gap_event_advertising_report_get_address_type(packet);
                    event_type = gap_event_advertising_report_get_advertising_event_type(packet);
                    
                    if (event_type != 0) break; // Only connectable advertising
                    
                    // Check manufacturer data for Nintendo company ID and Switch Pro 2 PID
                    ad_context_t context;
                    uint16_t company_id;
                    uint8_t *manufacturer_data;
                    uint8_t manufacturer_data_len;
                    
                    for (ad_iterator_init(&context, gap_event_advertising_report_get_data_length(packet), gap_event_advertising_report_get_data(packet));
                         ad_iterator_has_more(&context); ad_iterator_next(&context)) {
                        uint8_t data_type = ad_iterator_get_data_type(&context);
                        uint8_t data_len = ad_iterator_get_data_len(&context);
                        uint8_t *data = ad_iterator_get_data(&context);
                        
                        if (data_type == BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA && data_len >= 4) {
                            company_id = little_endian_read_16(data, 0);
                            if (company_id == NINTENDO_COMPANY_ID) {
                                uint16_t vid = little_endian_read_16(data, 2);
                                if (data_len >= 6) {
                                    uint16_t pid = little_endian_read_16(data, 4);
                                    if (vid == SWITCH_PRO2_VID && pid == SWITCH_PRO2_PID) {
                                        printf("Found Switch Pro 2 Controller\n");
                                        memcpy(remote_addr, addr, 6);
                                        remote_addr_type = addr_type;
                                        state = W4_CONNECT;
                                        gap_stop_scan();
                                        gap_connect(remote_addr, remote_addr_type);
                                        return;
                                    }
                                }
                            }
                        }
                    }
                    break;
                case HCI_EVENT_LE_META:
                    switch (hci_event_le_meta_get_subevent_code(packet)) {
                        case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
                            status = hci_subevent_le_connection_complete_get_status(packet);
                            if (status != ERROR_CODE_SUCCESS) {
                                printf("Connection failed, status 0x%02x\n", status);
                                start_scan();
                                break;
                            }
                            connection_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
                            printf("Connected, discovering HID service\n");
                            state = W4_SERVICE_RESULT;
                            gatt_client_discover_primary_services_by_uuid16(handle_gatt_client_event, connection_handle, ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE);
                            break;
                    }
                    break;
                case HCI_EVENT_DISCONNECTION_COMPLETE:
                    printf("Disconnected, restarting scan\n");
                    start_scan();
                    break;
            }
            break;
    }
}

int btstack_main(int argc, const char * argv[]) {
    UNUSED(argc);
    UNUSED(argv);

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    l2cap_init();
    sm_init();
    gatt_client_init();

    hci_power_control(HCI_POWER_ON);
    
    return 0;
}

int main(int argc, const char * argv[]) {
    return btstack_main(argc, argv);
}
```

```c
/*
 * btstack_config.h
 */

#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

// Port related features
#define HAVE_MALLOC
#define HAVE_POSIX_FILE_IO
#define HAVE_BTSTACK_STDIN
#define HAVE_POSIX_TIME

// BTstack features that can be enabled
#define ENABLE_BLE
#define ENABLE_LE_CENTRAL
#define ENABLE_LOG_INFO 
#define ENABLE_LOG_ERROR
#define ENABLE_PRINTF_HEXDUMP

// BTstack configuration. buffers, sizes, ...
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_INCOMING_PRE_BUFFER_SIZE 14 // sizeof benep heade, avoid memcpy

// Limit number of connections to save memory
#define MAX_NR_HCI_CONNECTIONS   1
#define MAX_NR_L2CAP_SERVICES    3
#define MAX_NR_L2CAP_CHANNELS    3
#define MAX_NR_RFCOMM_MULTIPLEXERS 0
#define MAX_NR_RFCOMM_SERVICES     0
#define MAX_NR_RFCOMM_CHANNELS     0
#define MAX_NR_BTSTACK_LINK_KEY_DB_MEMORY_ENTRIES  2
#define MAX_NR_BNEP_SERVICES       0
#define MAX_NR_BNEP_CHANNELS       0
#define MAX_NR_HID_HOST_CONNECTIONS 1
#define MAX_NR_GATT_CLIENTS        1

#define MAX_NR_SM_LOOKUP_ENTRIES   3
#define MAX_NR_SERVICE_RECORD_ITEMS 1
#define MAX_NR_WHITELIST_ENTRIES   1
#define MAX_NR_LE_DEVICE_DB_ENTRIES 1

// ATT DB Size
#define ATT_DB_UTIL_NUM_PERSISTENT_CLIENTS 1
#define ATT_DB_UTIL_CAPACITY 512

#endif
