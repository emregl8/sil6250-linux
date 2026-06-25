# sil6250 - out-of-tree build
#
#   make            build sil6250.ko against the running kernel
#   make clean      remove build artifacts
#   sudo make install / modules_install
#
obj-m += sil6250.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# This kernel (cachyos) is clang-built; out-of-tree modules must match.
# Override on the command line (make LLVM=) if your kernel was built with gcc.
LLVM ?= 1
export LLVM

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

modules_install install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

.PHONY: all clean modules_install install
