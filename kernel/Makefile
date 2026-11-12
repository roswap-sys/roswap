# Out-of-tree module. Build inside the lab VM, against that VM's headers:
#   make -C kernel
#   make -C /lib/modules/$(uname -r)/build M=$PWD/kernel modules
ifneq ($(KERNELRELEASE),)
obj-m := roswap.o
roswap-y := core.o map.o genl.o

# 没有 OFED 树就用内核自带的 rdma/ 头。这条路径没有在 MLNX_OFED 上编过。
OFA_KERNEL := /usr/src/ofa_kernel/default
ifneq ($(wildcard $(OFA_KERNEL)/include/rdma/rdma_cm.h),)
NOSTDINC_FLAGS += -I$(OFA_KERNEL)/include
ifneq ($(wildcard $(OFA_KERNEL)/include/linux/compat-2.6.h),)
ccflags-y += -include $(OFA_KERNEL)/include/linux/compat-2.6.h
endif
ifneq ($(wildcard $(OFA_KERNEL)/Module.symvers),)
KBUILD_EXTRA_SYMBOLS += $(OFA_KERNEL)/Module.symvers
endif
endif
else

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

map-test:
	gcc -std=c11 -Wall -Wextra -Werror -DRS_MAP_TEST -I$(CURDIR) \
		$(CURDIR)/map_test.c -o /tmp/roswap-map-test
	/tmp/roswap-map-test

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f /tmp/roswap-map-test
endif
