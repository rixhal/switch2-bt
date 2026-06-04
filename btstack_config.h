// btstack_config.h for sw2d_btstack — Switch 2 BLE HID Host on Linux
#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

// POSIX / Linux port features
#define HAVE_MALLOC
#define HAVE_POSIX_FILE_IO
#define HAVE_BTSTACK_STDIN
#define HAVE_POSIX_TIME

// BTstack features
#define ENABLE_BLE
#define ENABLE_LE_CENTRAL
#define ENABLE_GATT_CLIENT
#define ENABLE_GATT_CLIENT_PAIRING
#define ENABLE_LOG_INFO
#define ENABLE_LOG_ERROR
#define ENABLE_PRINTF_HEXDUMP

// Memory config
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_INCOMING_PRE_BUFFER_SIZE 14

#define MAX_NR_HCI_CONNECTIONS     1
#define MAX_NR_L2CAP_SERVICES      3
#define MAX_NR_L2CAP_CHANNELS      3
#define MAX_NR_RFCOMM_MULTIPLEXERS 0
#define MAX_NR_RFCOMM_SERVICES     0
#define MAX_NR_RFCOMM_CHANNELS     0
#define MAX_NR_BTSTACK_LINK_KEY_DB_MEMORY_ENTRIES 2
#define MAX_NR_BNEP_SERVICES       0
#define MAX_NR_BNEP_CHANNELS       0
#define MAX_NR_HID_HOST_CONNECTIONS 1
#define MAX_NR_GATT_CLIENTS        1
#define MAX_NR_SM_LOOKUP_ENTRIES   3
#define MAX_NR_SERVICE_RECORD_ITEMS 1
#define MAX_NR_WHITELIST_ENTRIES   1
#define MAX_NR_LE_DEVICE_DB_ENTRIES 1

// NVM (no persistent storage)
#define NVM_NONE
#define NVM_NUM_LINK_KEYS 0
#define NVM_NUM_DEVICE_DB_ENTRIES 1

// Mesh (required stubs)
#define MAX_NR_MESH_VIRTUAL_ADDRESSES 0
#define MAX_NR_MESH_SUBNETS            0
#define MAX_NR_MESH_APPKEYS            0
#define MAX_NR_MESH_MODELS             0

// ATT DB
#define ATT_DB_UTIL_NUM_PERSISTENT_CLIENTS 1
#define ATT_DB_UTIL_CAPACITY 512

#endif
