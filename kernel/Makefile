# Out-of-tree module. Build inside the lab VM, against that VM's headers:
#   make -C kernel
#   make -C /lib/modules/$(uname -r)/build M=$PWD/kernel modules
ifneq ($(KERNELRELEASE),)
obj-m := roswap.o
else

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
endif
