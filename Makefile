CC ?= aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall
PKG_CONFIG ?= pkg-config
KERNELDIR ?= /tmp/linux-6.18.32

all: switch2-bt switch2-bt-ff.ko

switch2-bt: switch2-bt.c hid_report_descriptor.h
	$(CC) $(CFLAGS) -o $@ switch2-bt.c $$($(PKG_CONFIG) --cflags --libs libsystemd)

switch2-bt-ff.ko: switch2-bt-ff.c Kbuild
	$(MAKE) -C $(KERNELDIR) M=$(PWD) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KBUILD_MODPOST_WARN=1 modules

clean:
	rm -f switch2-bt *.o *.ko *.mod *.mod.c *.mod.o Module.symvers modules.order .*.cmd
