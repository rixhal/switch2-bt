/*
 * sw2d_btstack.c
 */

#include <inttypes.h>
#include <stdio.h>
#include <btstack_tlv.h>
#include <string.h>

#include "btstack_config.h"
#include "btstack.h"

/* Stub: BCM patchram buffer (required by btstack_chipset_bcm but we don't use firmware patching) */
const uint8_t  brcm_patchram_buf[1]   = {0};
const uint32_t brcm_patch_ram_length  = 0;
const uint8_t  brcm_patch_version     = 0;

#define NINTENDO_COMPANY_ID 0x0553
#define SWITCH_PRO2_VID     0x057E
#define SWITCH_PRO2_PID     0x2069

static enum {
    W4_WORKING,
    W4_SCAN_RESULT,
    W4_CONNECT,
    W4_SERVICE_RESULT,
    W4_CHARACTERISTIC_RESULT,
    W4_ENABLE_NOTIFICATIONS_COMPLETE,
    READY
} state = W4_WORKING;

static bd_addr_t remote_addr;
static bd_addr_type_t remote_addr_type;
static hci_con_handle_t connection_handle;
static uint8_t battery_level = 0;

static gatt_client_service_t hid_service;
static gatt_client_characteristic_t hid_report_input_characteristic;
static gatt_client_notification_t hid_report_notification;

static btstack_packet_callback_registration_t hci_event_callback_registration;

static void parse_switch_input_report(const uint8_t *data, uint16_t len) {
    if (len != 63) return;
    
    // Extract buttons (bytes 4-7, 32-bit little-endian)
    uint32_t buttons = little_endian_read_32(data, 4);
    
    // Extract axes (bytes 10-15, 12-bit packed)
    uint16_t lx = ((data[11] & 0x0F) << 8) | data[10];
    uint16_t ly = (data[12] << 4) | ((data[11] & 0xF0) >> 4);
    uint16_t rx = ((data[14] & 0x0F) << 8) | data[13];
    uint16_t ry = (data[15] << 4) | ((data[14] & 0xF0) >> 4);
    
    // Convert 12-bit to 8-bit
    uint8_t lx_8 = lx >> 4;
    uint8_t ly_8 = ly >> 4;
    uint8_t rx_8 = rx >> 4;
    uint8_t ry_8 = ry >> 4;
    
    // Extract triggers (bytes 60-61)
    uint8_t l2 = data[60];
    uint8_t r2 = data[61];
    
    printf("Buttons: 0x%08X, LX: %3d, LY: %3d, RX: %3d, RY: %3d, L2: %3d, R2: %3d\n",
           (unsigned int)buttons, lx_8, ly_8, rx_8, ry_8, l2, r2);
}

static void hid_report_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);
    
    if (hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION) return;
    
    uint16_t value_length = gatt_event_notification_get_value_length(packet);
    const uint8_t * value = gatt_event_notification_get_value(packet);
    
    parse_switch_input_report(value, value_length);
}

static void handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);
    
    switch(hci_event_packet_get_type(packet)){
        case GATT_EVENT_SERVICE_QUERY_RESULT:
            gatt_event_service_query_result_get_service(packet, &hid_service);
            break;
        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
            gatt_event_characteristic_query_result_get_characteristic(packet, &hid_report_input_characteristic);
            break;
        case GATT_EVENT_QUERY_COMPLETE:
            switch (state){
                case W4_SERVICE_RESULT:
                    state = W4_CHARACTERISTIC_RESULT;
                    gatt_client_discover_characteristics_for_service_by_uuid16(handle_gatt_client_event, connection_handle, &hid_service, ORG_BLUETOOTH_CHARACTERISTIC_REPORT);
                    break;
                case W4_CHARACTERISTIC_RESULT:
                    state = W4_ENABLE_NOTIFICATIONS_COMPLETE;
                    gatt_client_listen_for_characteristic_value_updates(&hid_report_notification, &hid_report_handler, connection_handle, &hid_report_input_characteristic);
                    gatt_client_write_client_characteristic_configuration(handle_gatt_client_event, connection_handle, &hid_report_input_characteristic, GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
                    break;
                case W4_ENABLE_NOTIFICATIONS_COMPLETE:
                    state = READY;
                    printf("HID notifications enabled, ready to receive input reports\n");
                    break;
                default:
                    break;
            }
            break;
        case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT:
            break;
        default:
            break;
    }
}

static void hci_event_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                state = W4_SCAN_RESULT;
                gap_set_scan_parameters(0, 0x0030, 0x0030);
                gap_start_scan();
                printf("Scanning for Nintendo Switch Pro Controller 2...\n");
            }
            break;
        case GAP_EVENT_ADVERTISING_REPORT: {
            bd_addr_t address;
            gap_event_advertising_report_get_address(address, packet);
            bd_addr_type_t address_type = gap_event_advertising_report_get_address_type(packet);
            uint16_t data_length = gap_event_advertising_report_get_data_length(packet);
            const uint8_t * data = gap_event_advertising_report_get_data(packet);
            
            // Look for manufacturer data with Nintendo company ID
            ad_context_t context;
            bool found_nintendo = false;
            uint16_t vid = 0, pid = 0;
            
            for (ad_iterator_init(&context, data_length, data); ad_iterator_has_more(&context); ad_iterator_next(&context)) {
                uint8_t data_type = ad_iterator_get_data_type(&context);
                uint8_t size = ad_iterator_get_data_len(&context);
                const uint8_t * element_data = ad_iterator_get_data(&context);
                
                if (data_type == BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA && size >= 6) {
                    uint16_t company_id = little_endian_read_16(element_data, 0);
                    if (company_id == NINTENDO_COMPANY_ID) {
                        found_nintendo = true;
                        vid = little_endian_read_16(element_data, 2);
                        pid = little_endian_read_16(element_data, 4);
                        break;
                    }
                }
            }
            
            if (found_nintendo && vid == SWITCH_PRO2_VID && pid == SWITCH_PRO2_PID) {
                printf("Found Switch Pro Controller 2: %s\n", bd_addr_to_str(address));
                memcpy(remote_addr, address, 6);
                remote_addr_type = address_type;
                state = W4_CONNECT;
                gap_stop_scan();
                gap_connect(address, address_type);
            }
            break;
        }
        case HCI_EVENT_LE_META:
            switch (hci_event_le_meta_get_subevent_code(packet)) {
                case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
                    connection_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
                    printf("Connection established\n");
                    state = W4_SERVICE_RESULT;
                    gatt_client_discover_primary_services_by_uuid16(handle_gatt_client_event, connection_handle, ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE);
                    break;
                default:
                    break;
            }
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            printf("Disconnected, resuming scan\n");
            connection_handle = HCI_CON_HANDLE_INVALID;
            state = W4_SCAN_RESULT;
            gap_start_scan();
            break;
        default:
            break;
    }
}

int main(int argc, const char * argv[]) {
    (void)argc;
    (void)argv;

    hci_event_callback_registration.callback = &hci_event_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    l2cap_init();
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING);

    gatt_client_init();
    
    hci_power_control(HCI_POWER_ON);
    
    btstack_run_loop_execute();
    
    return 0;
}
