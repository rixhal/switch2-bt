obj-m := switch2-bt-ff.o
KERNELDIR ?= /tmp/linux-6.18.32
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
