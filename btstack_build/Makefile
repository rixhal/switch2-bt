# Minimal Makefile for sw2d_btstack using BTstack posix-h4
BTSTACK_ROOT = $(HOME)/btstack

CORE = \
	btstack_chipset_bcm.c \
	btstack_link_key_db_tlv.c \
	btstack_run_loop_posix.c \
	btstack_tlv_posix.c \
	btstack_uart_posix.c \
	hci_dump_posix_fs.c \
	hci_transport_h4.c \
	le_device_db_tlv.c \
	rijndael.c \
	btstack_stdin_posix.c \
	btstack_memory.c \
	btstack_linked_list.c \
	btstack_memory_pool.c \
	btstack_run_loop.c \
	btstack_util.c \
	ad_parser.c \
	hci.c \
	hci_cmd.c \
	hci_dump.c \
	hci_event.c \
	hci_event_builder.c \
	l2cap.c \
	l2cap_signaling.c \
	btstack_tlv.c \
	btstack_crypto.c \
	uECC.c \
	sm.c \
	btstack_ring_buffer.c \
	sw2d_btstack.c

SRC_DIRS = \
	-I$(BTSTACK_ROOT)/src/ble \
	-I$(BTSTACK_ROOT)/src/classic \
	-I$(BTSTACK_ROOT)/src \
	-I$(BTSTACK_ROOT)/platform/embedded \
	-I$(BTSTACK_ROOT)/platform/posix \
	-I$(BTSTACK_ROOT)/chipset/bcm \
	-I$(BTSTACK_ROOT)/3rd-party/rijndael \
	-I$(BTSTACK_ROOT)/3rd-party/micro-ecc \
	-I$(BTSTACK_ROOT)/3rd-party/tinydir \
	-I.

OBJS = $(CORE:.c=.o)

CFLAGS = -g -Wall $(SRC_DIRS)
LDFLAGS = -lm -lpthread

all: sw2d_btstack

%.o: $(BTSTACK_ROOT)/src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: $(BTSTACK_ROOT)/src/ble/%.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: $(BTSTACK_ROOT)/src/classic/%.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: $(BTSTACK_ROOT)/platform/posix/%.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: $(BTSTACK_ROOT)/chipset/bcm/%.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: $(BTSTACK_ROOT)/3rd-party/rijndael/%.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: $(BTSTACK_ROOT)/3rd-party/micro-ecc/%.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: $(BTSTACK_ROOT)/3rd-party/tinydir/%.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

sw2d_btstack: $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

clean:
	rm -f *.o sw2d_btstack

deploy: sw2d_btstack
	scp sw2d_btstack root@10.10.10.140:/storage/switch2-bt/sw2d_btstack
