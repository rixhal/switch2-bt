CC ?= gcc
CFLAGS ?= -O2 -s -Wall
LDLIBS = -lbluetooth

.PHONY: all clean aarch64 tools test

all: sw2d_final

sw2d: sw2d.c
	$(CC) $(CFLAGS) -o $@ $<

sw2d_final: sw2d_final.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

sw2d_lib: sw2d_lib.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

hciletest: hciletest.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

two_socket_test: two_socket_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

tools: sw2d_final sw2d_lib hciletest two_socket_test

gattdump: gattdump.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# Tools: sw2d_lib + hciletest
tools: sw2d_lib hciletest

# Tests
test_args: tests/test_args.c sw2d_final.c
	$(CC) $(CFLAGS) -DTESTING -o $@ tests/test_args.c $(LDLIBS)

test_bdaddr: tests/test_bdaddr.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

test_adv_parse: tests/test_adv_parse.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

test: test_args test_bdaddr test_adv_parse
	@echo "=== test_args ==="
	./test_args
	@echo ""
	@echo "=== test_bdaddr ==="
	./test_bdaddr
	@echo ""
	@echo "=== test_adv_parse ==="
	./test_adv_parse

# Cross-compile for aarch64 (RPi5 / LibreELEC)
aarch64:
	aarch64-linux-gnu-gcc -O2 -s -Wall -o sw2d_final sw2d_final.c -lbluetooth -lcrypto
	aarch64-linux-gnu-gcc -O2 -s -Wall -o hciletest hciletest.c -lbluetooth
	aarch64-linux-gnu-gcc -O2 -s -Wall -o sw2d_lib sw2d_lib.c -lbluetooth

clean:
	rm -f sw2d sw2d_final sw2d_lib hciletest gattdump test_args test_bdaddr test_adv_parse
