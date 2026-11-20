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

MAP_CFLAGS := -std=c11 -Wall -Wextra -Werror -pthread -DRS_MAP_TEST -I$(CURDIR)
SAN_ASAN := -fsanitize=address,undefined -fno-omit-frame-pointer
SAN_TSAN := -fsanitize=thread

map-test:
	$(MAKE) -C $(CURDIR) map-test-run SUFFIX= EXTRA=

map-test-asan:
	$(MAKE) -C $(CURDIR) map-test-run SUFFIX=-asan EXTRA="$(SAN_ASAN)"

map-test-tsan:
	$(MAKE) -C $(CURDIR) map-test-run SUFFIX=-tsan EXTRA="$(SAN_TSAN)"

map-test-run:
	gcc $(MAP_CFLAGS) $(EXTRA) $(CURDIR)/map_test.c -o /tmp/roswap-map-test$(SUFFIX)
	/tmp/roswap-map-test$(SUFFIX)
	gcc $(MAP_CFLAGS) $(EXTRA) $(CURDIR)/hedge_race_test.c -o /tmp/roswap-hedge-race$(SUFFIX)
	/tmp/roswap-hedge-race$(SUFFIX)
	gcc $(MAP_CFLAGS) $(EXTRA) $(CURDIR)/dead_race_test.c -o /tmp/roswap-dead-race$(SUFFIX)
	/tmp/roswap-dead-race$(SUFFIX)

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f /tmp/roswap-map-test /tmp/roswap-hedge-race /tmp/roswap-dead-race \
		/tmp/roswap-map-test-asan /tmp/roswap-hedge-race-asan /tmp/roswap-dead-race-asan \
		/tmp/roswap-map-test-tsan /tmp/roswap-hedge-race-tsan /tmp/roswap-dead-race-tsan
endif
