obj-m := hantrovgem.o
hantrovgem-objs = vgem_drv.o vgem_fence.o
KDIR := /lib/modules/$(shell uname -r)/build
#KDIR := /home/cdthor2vsi/nan/open_source/kernel_code/linuxfromthor2/linux/
PWD := $(shell pwd)
default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean::
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	-rm -f .version
