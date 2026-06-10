/*
 * sw2d_btstack.c - Switch 2 Pro Controller BLE bridge using BTstack
 */

#define BTSTACK_FILE__ "sw2d_btstack.c"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "btstack_config.h"
#include "btstack.h"

#define NINTENDO_COMPANY_ID 0x0553
#define SWITCH_PRO2_VID     0x057e
#define SWITCH_PRO2_PID     0x2069

#define SW2D_LOG(fmt, ...)    printf("[SW2D] " fmt "\n", ##__VA_ARGS__)
#define SW2D_SM_LOG(fmt, ...) printf("[SW2D-SM] " fmt "\n", ##__VA_ARGS__)

/*
 * BTstack UUID byte arrays use the same most-significant-byte-first order as
 * the canonical UUID string.
 */
static const uint8_t switch_service_uuid[16] = {
    0xab, 0x7d, 0xe9, 0xbe, 0x89, 0xfe, 0x49, 0xad,
    0x82, 0x8f, 0x11, 0x8f, 0x09, 0xdf, 0x7f, 0xd0
};

static const uint8_t input_report_uuid[16] = {
    0xab, 0x7d, 0xe9, 0xbe, 0x89, 0xfe, 0x49, 0xad,
    0x82, 0x8f, 0x11, 0x8f, 0x09, 0xdf, 0x7f, 0xd2
};

static const uint8_t command_write_uuid[16] = {
    0x64, 0x9d, 0x4a, 0xc9, 0x8e, 0xb7, 0x4e, 0x6c,
    0xaf, 0x44, 0x1e, 0xa5, 0x4f, 0xe5, 0xf0, 0x05
};

/*
 * Donor LTKs extracted from Nadeflore/switch2-controllers @ controller.py:380-383.
 * The Switch 2 console sends TWO 17-byte LTK payloads via 0x15 PAIR subcommands
 * (dual-bluetooth adapter). The first byte is a pairing flag; the remaining 16
 * bytes form the actual AES-128 LTK for BLE encryption.
 *
 * LTK1 (subcmd 0x04 PAIR_LTK1): Primary LTK used for initial link encryption.
 * LTK2 (subcmd 0x02 PAIR_LTK2): Secondary LTK for the console's second adapter.
 *
 * For a single-adapter host (Pi + CSR8510), start with LTK1. If re-encryption
 * fails with PIN_OR_KEY_MISSING, try LTK2.
 */
static const uint8_t donor_ltk1[16] = {
    0xea, 0xbd, 0x47, 0x13, 0x89, 0x35, 0x42, 0xc6,
    0x79, 0xee, 0x07, 0xf2, 0x53, 0x2c, 0x6c, 0x31
};
static const uint8_t donor_ltk2[16] = {
    0x40, 0xb0, 0x8a, 0x5f, 0xcd, 0x1f, 0x9b, 0x41,
    0x12, 0x5c, 0xac, 0xc6, 0x3f, 0x38, 0xa0, 0x73
};

/*
 * Init commands sent after encryption+notifications are established.
 * Format (from Nadeflore write_command): [cmd_id] [0x91 0x01] [subcmd] [0x00] [data_len] [0x00 0x00] [data]
 *
 * Order matched from Nadeflore's controller.py: feature init → feature enable → set LED → vibration preset.
 * FEATURE flags: 0x04=MOTION | 0x10=MOUSE | 0x80=MAGNOMETER. Using minimal set (0x04 motion only)
 * to avoid triggering mouse emulation on the controller.
 */
static const uint8_t init_feature_select_02[] = {
    0x0c, 0x91, 0x01, 0x02, 0x00, 0x04, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00
};
static const uint8_t init_feature_select_04[] = {
    0x0c, 0x91, 0x01, 0x04, 0x00, 0x04, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00
};
static const uint8_t init_set_led_player1[] = {
    0x09, 0x91, 0x01, 0x07, 0x00, 0x04, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00
};
static const uint8_t init_play_vibration_preset[] = {
    0x0a, 0x91, 0x01, 0x02, 0x00, 0x04, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00
};

typedef struct {
    const char *name;
    const uint8_t *data;
    uint16_t size;
    uint16_t delay_after_ms;
} init_command_t;

static const init_command_t init_commands[] = {
    { "feature-select 0x02", init_feature_select_02, sizeof(init_feature_select_02), 500 },
    { "feature-select 0x04", init_feature_select_04, sizeof(init_feature_select_04), 700 },
    { "set LED player 1",    init_set_led_player1,   sizeof(init_set_led_player1),     50 },
    { "vibration preset",    init_play_vibration_preset, sizeof(init_play_vibration_preset), 50 },
};

typedef enum {
    APP_W4_WORKING,
    APP_SCANNING,
    APP_CONNECTING,
    APP_W4_REENCRYPTION,
    APP_DISCOVERING_SERVICE,
    APP_DISCOVERING_INPUT,
    APP_DISCOVERING_COMMAND,
    APP_ENABLING_NOTIFICATIONS,
    APP_INITIALIZING,
    APP_READY
} app_state_t;

static app_state_t app_state = APP_W4_WORKING;
static bd_addr_t remote_addr;
static bd_addr_type_t remote_addr_type;
static hci_con_handle_t connection_handle = HCI_CON_HANDLE_INVALID;

static gatt_client_service_t switch_service;
static gatt_client_characteristic_t input_report_characteristic;
static gatt_client_characteristic_t command_write_characteristic;
static gatt_client_notification_t notification_listener;

static btstack_packet_callback_registration_t hci_event_registration;
static btstack_packet_callback_registration_t sm_event_registration;
static btstack_context_callback_registration_t init_write_registration;
static btstack_timer_source_t init_timer;
static uint8_t init_command_index;
static bool service_found;
static bool input_found;
static bool command_found;

static void gatt_client_handler(uint8_t packet_type, uint16_t channel,
                                uint8_t *packet, uint16_t size);

static uint16_t read_le16(const uint8_t *buffer) {
    return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
}

static uint32_t read_le32(const uint8_t *buffer) {
    return (uint32_t)buffer[0] |
           ((uint32_t)buffer[1] << 8) |
           ((uint32_t)buffer[2] << 16) |
           ((uint32_t)buffer[3] << 24);
}

static void reset_connection_state(void) {
    btstack_run_loop_remove_timer(&init_timer);
    connection_handle = HCI_CON_HANDLE_INVALID;
    memset(&switch_service, 0, sizeof(switch_service));
    memset(&input_report_characteristic, 0, sizeof(input_report_characteristic));
    memset(&command_write_characteristic, 0, sizeof(command_write_characteristic));
    memset(&notification_listener, 0, sizeof(notification_listener));
    service_found = false;
    input_found = false;
    command_found = false;
    init_command_index = 0;
}

static void start_scan(void) {
    reset_connection_state();
    app_state = APP_SCANNING;
    gap_set_scan_parameters(0, 0x0030, 0x0030);
    SW2D_LOG("Scanning for Nintendo VID 0x%04x PID 0x%04x",
             SWITCH_PRO2_VID, SWITCH_PRO2_PID);
    gap_start_scan();
}

static void disconnect_with_error(const char *stage, uint8_t status) {
    SW2D_LOG("%s failed, status 0x%02x; disconnecting", stage, status);
    if (connection_handle != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(connection_handle);
    } else {
        start_scan();
    }
}

static bool advertisement_is_switch_pro2(const uint8_t *packet) {
    ad_context_t context;
    uint8_t data_length = gap_event_advertising_report_get_data_length(packet);
    const uint8_t *data = gap_event_advertising_report_get_data(packet);

    for (ad_iterator_init(&context, data_length, data);
         ad_iterator_has_more(&context);
         ad_iterator_next(&context)) {
        if (ad_iterator_get_data_type(&context) !=
            BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA) {
            continue;
        }

        uint8_t field_length = ad_iterator_get_data_len(&context);
        const uint8_t *field = ad_iterator_get_data(&context);
        if (field_length < 6) {
            continue;
        }

        if (read_le16(field) == NINTENDO_COMPANY_ID &&
            read_le16(field + 2) == SWITCH_PRO2_VID &&
            read_le16(field + 4) == SWITCH_PRO2_PID) {
            return true;
        }
    }
    return false;
}

static void emit_uinput_placeholder(uint32_t buttons, uint16_t lx, uint16_t ly,
                                    uint16_t rx, uint16_t ry,
                                    uint8_t l2, uint8_t r2) {
    SW2D_LOG("INPUT buttons=0x%08" PRIx32
             " lx=%4u ly=%4u rx=%4u ry=%4u l2=%3u r2=%3u",
             buttons, lx, ly, rx, ry, l2, r2);
}

static void parse_switch_report(const uint8_t *data, uint16_t size) {
    if (size != 63) {
        SW2D_LOG("Ignoring input report with unexpected size %u", size);
        return;
    }

    uint32_t buttons = read_le32(data + 4);
    uint16_t lx = (uint16_t)data[10] | ((uint16_t)(data[11] & 0x0f) << 8);
    uint16_t ly = ((uint16_t)data[11] >> 4) | ((uint16_t)data[12] << 4);
    uint16_t rx = (uint16_t)data[13] | ((uint16_t)(data[14] & 0x0f) << 8);
    uint16_t ry = ((uint16_t)data[14] >> 4) | ((uint16_t)data[15] << 4);

    emit_uinput_placeholder(buttons, lx, ly, rx, ry, data[60], data[61]);
}

static bool provide_ltk(hci_con_handle_t con_handle, uint8_t addr_type,
                        bd_addr_t addr, uint8_t *ltk) {
    if (con_handle != connection_handle) {
        SW2D_SM_LOG("LTK request rejected for inactive handle 0x%04x",
                    con_handle);
        return false;
    }

    memcpy(ltk, donor_ltk1, sizeof(donor_ltk1));
    SW2D_SM_LOG("Injected donor LTK1 for %s type %u handle 0x%04x",
                bd_addr_to_str(addr), addr_type, con_handle);
    return true;
}

static void start_service_discovery(void) {
    uint8_t status;

    if (app_state != APP_W4_REENCRYPTION ||
        connection_handle == HCI_CON_HANDLE_INVALID) {
        return;
    }

    service_found = false;
    app_state = APP_DISCOVERING_SERVICE;
    SW2D_LOG("Link encrypted; discovering proprietary service");
    status = gatt_client_discover_primary_services_by_uuid128(
        gatt_client_handler, connection_handle, switch_service_uuid);
    if (status != ERROR_CODE_SUCCESS) {
        disconnect_with_error("service discovery start", status);
    }
}

static void request_next_init_write(void);

static void init_timer_handler(btstack_timer_source_t *timer) {
    UNUSED(timer);
    request_next_init_write();
}

static void init_can_write(void *context) {
    UNUSED(context);

    if (app_state != APP_INITIALIZING ||
        init_command_index >= (uint8_t)(sizeof(init_commands) / sizeof(init_commands[0]))) {
        return;
    }

    const init_command_t *command = &init_commands[init_command_index];
    uint8_t status = gatt_client_write_value_of_characteristic_without_response(
        connection_handle, command_write_characteristic.value_handle,
        command->size, (uint8_t *)command->data);
    if (status != ERROR_CODE_SUCCESS) {
        disconnect_with_error(command->name, status);
        return;
    }

    SW2D_LOG("Init command %u/%u sent: %s",
             (unsigned)(init_command_index + 1),
             (unsigned)(sizeof(init_commands) / sizeof(init_commands[0])),
             command->name);
    init_command_index++;

    if (init_command_index >= (uint8_t)(sizeof(init_commands) / sizeof(init_commands[0]))) {
        app_state = APP_READY;
        SW2D_LOG("READY: notifications active and initialization complete");
        return;
    }

    btstack_run_loop_set_timer(&init_timer, command->delay_after_ms);
    btstack_run_loop_add_timer(&init_timer);
}

static void request_next_init_write(void) {
    if (app_state != APP_INITIALIZING) {
        return;
    }

    init_write_registration.callback = init_can_write;
    init_write_registration.context = NULL;
    uint8_t status = gatt_client_request_to_write_without_response(
        &init_write_registration, connection_handle);
    if (status != ERROR_CODE_SUCCESS) {
        disconnect_with_error("request init write", status);
    }
}

static void handle_query_complete(uint8_t att_status) {
    uint8_t status;

    if (att_status != ATT_ERROR_SUCCESS) {
        disconnect_with_error("GATT query", att_status);
        return;
    }

    switch (app_state) {
        case APP_DISCOVERING_SERVICE:
            if (!service_found) {
                disconnect_with_error("proprietary service not found",
                                      ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
                return;
            }
            input_found = false;
            app_state = APP_DISCOVERING_INPUT;
            SW2D_LOG("Discovering input report characteristic");
            status = gatt_client_discover_characteristics_for_service_by_uuid128(
                gatt_client_handler, connection_handle, &switch_service,
                input_report_uuid);
            if (status != ERROR_CODE_SUCCESS) {
                disconnect_with_error("input characteristic discovery start", status);
            }
            break;

        case APP_DISCOVERING_INPUT:
            if (!input_found) {
                disconnect_with_error("input report characteristic not found",
                                      ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
                return;
            }
            command_found = false;
            app_state = APP_DISCOVERING_COMMAND;
            SW2D_LOG("Discovering command write characteristic");
            status = gatt_client_discover_characteristics_for_service_by_uuid128(
                gatt_client_handler, connection_handle, &switch_service,
                command_write_uuid);
            if (status != ERROR_CODE_SUCCESS) {
                disconnect_with_error("command characteristic discovery start", status);
            }
            break;

        case APP_DISCOVERING_COMMAND:
            if (!command_found) {
                disconnect_with_error("command write characteristic not found",
                                      ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
                return;
            }
            gatt_client_listen_for_characteristic_value_updates(
                &notification_listener, gatt_client_handler, connection_handle,
                &input_report_characteristic);
            app_state = APP_ENABLING_NOTIFICATIONS;
            SW2D_LOG("Enabling input notifications");
            status = gatt_client_write_client_characteristic_configuration(
                gatt_client_handler, connection_handle,
                &input_report_characteristic,
                GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
            if (status != ERROR_CODE_SUCCESS) {
                disconnect_with_error("notification enable start", status);
            }
            break;

        case APP_ENABLING_NOTIFICATIONS:
            app_state = APP_INITIALIZING;
            init_command_index = 0;
            SW2D_LOG("Notifications enabled; sending initialization commands");
            request_next_init_write();
            break;

        default:
            break;
    }
}

static void gatt_client_handler(uint8_t packet_type, uint16_t channel,
                                uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    switch (hci_event_packet_get_type(packet)) {
        case GATT_EVENT_SERVICE_QUERY_RESULT:
            if (app_state == APP_DISCOVERING_SERVICE) {
                gatt_event_service_query_result_get_service(packet, &switch_service);
                service_found = true;
                SW2D_LOG("Service found at 0x%04x-0x%04x",
                         switch_service.start_group_handle,
                         switch_service.end_group_handle);
            }
            break;

        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
            if (app_state == APP_DISCOVERING_INPUT) {
                gatt_event_characteristic_query_result_get_characteristic(
                    packet, &input_report_characteristic);
                input_found = true;
                SW2D_LOG("Input characteristic found at 0x%04x",
                         input_report_characteristic.value_handle);
            } else if (app_state == APP_DISCOVERING_COMMAND) {
                gatt_event_characteristic_query_result_get_characteristic(
                    packet, &command_write_characteristic);
                command_found = true;
                SW2D_LOG("Command characteristic found at 0x%04x",
                         command_write_characteristic.value_handle);
            }
            break;

        case GATT_EVENT_QUERY_COMPLETE:
            handle_query_complete(gatt_event_query_complete_get_att_status(packet));
            break;

        case GATT_EVENT_NOTIFICATION:
            if (app_state == APP_INITIALIZING || app_state == APP_READY) {
                parse_switch_report(gatt_event_notification_get_value(packet),
                                    gatt_event_notification_get_value_length(packet));
            }
            break;

        default:
            break;
    }
}

static void sm_event_handler(uint8_t packet_type, uint16_t channel,
                             uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_JUST_WORKS_REQUEST: {
            hci_con_handle_t handle =
                sm_event_just_works_request_get_handle(packet);
            SW2D_SM_LOG("Just Works requested (SC=%u); confirming",
                        sm_event_just_works_request_get_secure_connection(packet));
            sm_just_works_confirm(handle);
            break;
        }

        case SM_EVENT_PAIRING_STARTED:
            SW2D_SM_LOG("Pairing started");
            break;

        case SM_EVENT_PAIRING_COMPLETE: {
            uint8_t status = sm_event_pairing_complete_get_status(packet);
            if (status == ERROR_CODE_SUCCESS) {
                SW2D_SM_LOG("Pairing complete; waiting for re-encryption");
            } else {
                SW2D_SM_LOG("Pairing failed, status 0x%02x reason 0x%02x",
                            status, sm_event_pairing_complete_get_reason(packet));
                disconnect_with_error("pairing", status);
            }
            break;
        }

        case SM_EVENT_REENCRYPTION_STARTED:
            SW2D_SM_LOG("Re-encryption started for handle 0x%04x",
                        sm_event_reencryption_started_get_handle(packet));
            break;

        case SM_EVENT_REENCRYPTION_COMPLETE: {
            hci_con_handle_t handle =
                sm_event_reencryption_complete_get_handle(packet);
            uint8_t status =
                sm_event_reencryption_complete_get_status(packet);
            if (handle != connection_handle) {
                SW2D_SM_LOG("Ignoring re-encryption event for handle 0x%04x",
                            handle);
                break;
            }
            if (status != ERROR_CODE_SUCCESS) {
                SW2D_SM_LOG("Re-encryption failed, status 0x%02x", status);
                disconnect_with_error("re-encryption", status);
                break;
            }
            SW2D_SM_LOG("Re-encryption complete; link is encrypted");
            start_service_discovery();
            break;
        }

        default:
            break;
    }
}

static void hci_event_handler(uint8_t packet_type, uint16_t channel,
                              uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                start_scan();
            }
            break;

        case GAP_EVENT_ADVERTISING_REPORT: {
            if (app_state != APP_SCANNING ||
                !advertisement_is_switch_pro2(packet)) {
                break;
            }

            uint8_t advertising_type =
                gap_event_advertising_report_get_advertising_event_type(packet);
            if (advertising_type != 0) {
                break;
            }

            gap_event_advertising_report_get_address(packet, remote_addr);
            remote_addr_type =
                gap_event_advertising_report_get_address_type(packet);
            SW2D_LOG("Found Switch 2 Pro Controller %s (type %u)",
                     bd_addr_to_str(remote_addr), remote_addr_type);
            gap_stop_scan();
            app_state = APP_CONNECTING;
            uint8_t status = gap_connect(remote_addr, remote_addr_type);
            if (status != ERROR_CODE_SUCCESS) {
                disconnect_with_error("connect start", status);
            }
            break;
        }

        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet) ==
                GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
                uint8_t status =
                    gap_subevent_le_connection_complete_get_status(packet);
                if (status != ERROR_CODE_SUCCESS) {
                    SW2D_LOG("Connection failed, status 0x%02x", status);
                    start_scan();
                    break;
                }

                connection_handle =
                    gap_subevent_le_connection_complete_get_connection_handle(packet);
                app_state = APP_W4_REENCRYPTION;
                SW2D_LOG("Connected, handle 0x%04x; requesting SMP pairing",
                         connection_handle);
                sm_request_pairing(connection_handle);
            }
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE:
            SW2D_LOG("Disconnected, reason 0x%02x; restarting scan",
                     hci_event_disconnection_complete_get_reason(packet));
            start_scan();
            break;

        default:
            break;
    }
}

int btstack_main(int argc, const char *argv[]) {
    UNUSED(argc);
    UNUSED(argv);

    l2cap_init();
    sm_init();
    gatt_client_init();

    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(
        SM_AUTHREQ_NO_BONDING | SM_AUTHREQ_SECURE_CONNECTION);
    sm_set_secure_connections_only_mode(true);
    sm_register_ltk_callback(provide_ltk);

    hci_event_registration.callback = hci_event_handler;
    hci_add_event_handler(&hci_event_registration);
    sm_event_registration.callback = sm_event_handler;
    sm_add_event_handler(&sm_event_registration);

    btstack_run_loop_set_timer_handler(&init_timer, init_timer_handler);

    SW2D_LOG("Starting BTstack Switch 2 BLE bridge");
    SW2D_SM_LOG("Secure Connections Just Works enabled; donor LTK is placeholder");
    hci_power_control(HCI_POWER_ON);
    return 0;
}

int main(int argc, const char *argv[]) {
    return btstack_main(argc, argv);
}
