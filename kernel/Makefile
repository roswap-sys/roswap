ifneq ($(KERNELRELEASE),)
obj-m := roswap.o
roswap-y := core.o map.o genl.o

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
	gcc -std=c11 -Wall -Wextra -Werror -pthread -DRS_MAP_TEST -I$(CURDIR) \
		$(CURDIR)/map_test.c -o /tmp/roswap-map-test
	/tmp/roswap-map-test
	gcc -std=c11 -Wall -Wextra -Werror -pthread -DRS_MAP_TEST -I$(CURDIR) \
		$(CURDIR)/hedge_race_test.c -o /tmp/roswap-hedge-race
	/tmp/roswap-hedge-race
	gcc -std=c11 -Wall -Wextra -Werror -pthread -DRS_MAP_TEST -I$(CURDIR) \
		$(CURDIR)/dead_race_test.c -o /tmp/roswap-dead-race
	/tmp/roswap-dead-race

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f /tmp/roswap-map-test /tmp/roswap-hedge-race /tmp/roswap-dead-race
endif
