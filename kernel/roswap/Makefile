# Kbuild 需要这份 Makefile。顶层入口是 CMake。

OFED_DIR ?= /usr/src/ofa_kernel/default
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) OFED_DIR=$(OFED_DIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
