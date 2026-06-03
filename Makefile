CC ?= gcc
CFLAGS ?= -O2 -s -Wall
LDLIBS = -lbluetooth

.PHONY: all clean aarch64

all: sw2d_final

sw2d: sw2d.c
	$(CC) $(CFLAGS) -o $@ $<

sw2d_final: sw2d_final.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

sw2d_lib: sw2d_lib.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

hciletest: hciletest.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

gattdump: gattdump.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# Cross-compile for aarch64 (RPi5 / LibreELEC)
aarch64:
	aarch64-linux-gnu-gcc -O2 -s -Wall -o sw2d_final sw2d_final.c -lbluetooth

clean:
	rm -f sw2d sw2d_final sw2d_lib hciletest gattdump
