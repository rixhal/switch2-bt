# Makefile for sw2d_btstack — BTstack port/linux + Switch 2 app
BTSTACK_ROOT ?= $(HOME)/btstack
PROJECT_DIR  ?= $(HOME)/switch2-raw
TARGET       = sw2d_btstack

# Framework
CORE_SRC = \
	$(BTSTACK_ROOT)/port/linux/main.c \
	$(PROJECT_DIR)/sw2d_btstack.c \
	$(BTSTACK_ROOT)/platform/posix/btstack_run_loop_posix.c \
	$(BTSTACK_ROOT)/platform/posix/btstack_tlv_posix.c \
	$(BTSTACK_ROOT)/platform/posix/hci_dump_posix_fs.c \
	$(BTSTACK_ROOT)/platform/posix/btstack_stdin_posix.c \
	$(BTSTACK_ROOT)/platform/posix/btstack_signal.c \
	$(BTSTACK_ROOT)/platform/linux/hci_transport_linux.c \
	$(BTSTACK_ROOT)/src/ble/le_device_db_tlv.c \
	$(BTSTACK_ROOT)/3rd-party/rijndael/rijndael.c \
	$(BTSTACK_ROOT)/src/btstack_memory.c \
	$(BTSTACK_ROOT)/src/btstack_linked_list.c \
	$(BTSTACK_ROOT)/src/btstack_memory_pool.c \
	$(BTSTACK_ROOT)/src/btstack_run_loop.c \
	$(BTSTACK_ROOT)/src/btstack_util.c \
	$(BTSTACK_ROOT)/src/ad_parser.c \
	$(BTSTACK_ROOT)/src/hci.c \
	$(BTSTACK_ROOT)/src/hci_cmd.c \
	$(BTSTACK_ROOT)/src/hci_dump.c \
	$(BTSTACK_ROOT)/src/hci_event.c \
	$(BTSTACK_ROOT)/src/hci_event_builder.c \
	$(BTSTACK_ROOT)/src/l2cap.c \
	$(BTSTACK_ROOT)/src/l2cap_signaling.c \
	$(BTSTACK_ROOT)/src/btstack_tlv.c \
	$(BTSTACK_ROOT)/src/btstack_crypto.c \
	$(BTSTACK_ROOT)/3rd-party/micro-ecc/uECC.c \
	$(BTSTACK_ROOT)/src/sm.c \
	$(BTSTACK_ROOT)/src/btstack_ring_buffer.c \
	$(BTSTACK_ROOT)/src/btstack_audio.c \
	$(BTSTACK_ROOT)/src/btstack_linked_queue.c \
	$(BTSTACK_ROOT)/src/btstack_ltv_builder.c \
	$(BTSTACK_ROOT)/src/ble/gatt_client.c \
	$(BTSTACK_ROOT)/src/ble/att_dispatch.c \
	$(BTSTACK_ROOT)/port/linux/btstack_main_config.c

INC_DIRS = \
	-I. \
	-I$(BTSTACK_ROOT)/port/linux \
	-I$(BTSTACK_ROOT)/src/ble \
	-I$(BTSTACK_ROOT)/src/classic \
	-I$(BTSTACK_ROOT)/src \
	-I$(BTSTACK_ROOT)/platform/embedded \
	-I$(BTSTACK_ROOT)/platform/posix \
	-I$(BTSTACK_ROOT)/platform/linux \
	-I$(BTSTACK_ROOT)/3rd-party/rijndael \
	-I$(BTSTACK_ROOT)/3rd-party/micro-ecc \
	-I$(BTSTACK_ROOT)/3rd-party/tinydir \
	-I$(BTSTACK_ROOT)/3rd-party/bluedroid/encoder/include \
	-I$(BTSTACK_ROOT)/3rd-party/bluedroid/decoder/include \
	-I$(BTSTACK_ROOT)/3rd-party/yxml \
	-I$(BTSTACK_ROOT)/3rd-party/hxcmod-player

OBJS = $(notdir $(CORE_SRC:.c=.o))
DEPS = $(OBJS:.o=.d)

CC      ?= gcc
CFLAGS  += -g -Wall $(INC_DIRS)
LDFLAGS += -lm -lpthread -lbluetooth

.PHONY: all clean deploy

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $^ $(LDFLAGS) -o $@

%.o: $(BTSTACK_ROOT)/port/linux/%.c
	$(CC) $(CFLAGS) -MMD -c $< -o $@

%.o: $(BTSTACK_ROOT)/src/%.c
	$(CC) $(CFLAGS) -MMD -c $< -o $@

%.o: $(BTSTACK_ROOT)/src/ble/%.c
	$(CC) $(CFLAGS) -MMD -c $< -o $@

%.o: $(BTSTACK_ROOT)/src/classic/%.c
	$(CC) $(CFLAGS) -MMD -c $< -o $@

%.o: $(BTSTACK_ROOT)/platform/posix/%.c
	$(CC) $(CFLAGS) -MMD -c $< -o $@

%.o: $(BTSTACK_ROOT)/platform/linux/%.c
	$(CC) $(CFLAGS) -MMD -c $< -o $@

%.o: $(BTSTACK_ROOT)/3rd-party/rijndael/%.c
	$(CC) $(CFLAGS) -MMD -c $< -o $@

%.o: $(BTSTACK_ROOT)/3rd-party/micro-ecc/%.c
	$(CC) $(CFLAGS) -MMD -c $< -o $@

%.o: $(BTSTACK_ROOT)/3rd-party/bluedroid/encoder/srce/%.c
	$(CC) $(CFLAGS) -MMD -c $< -o $@

%.o: $(PROJECT_DIR)/%.c
	$(CC) $(CFLAGS) -MMD -c $< -o $@

CFLAGS += -include $(BTSTACK_ROOT)/port/linux/btstack_config.h

clean:
	rm -f *.o *.d $(TARGET)

deploy: $(TARGET)
	scp $(TARGET) root@10.10.10.140:/storage/switch2-bt/$(TARGET)

-include $(DEPS)
