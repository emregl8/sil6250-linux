obj-m += irq_startup_test.o
SIL6250_KERNEL_DIR ?= $(src)/../../kernel
ccflags-y += -I$(SIL6250_KERNEL_DIR)

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) W=1 KCFLAGS=-Werror modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
