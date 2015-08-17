obj-m		:= hv_mmls_cdev.o
hv_mmls_cdev-objs := hv_cmd.o hv_cdev.o
KERN_SRC	:= /lib/modules/$(shell uname -r)/build/
PWD			:= $(shell pwd)

modules:
	make -C $(KERN_SRC) M=$(PWD) modules

install:
	make -C $(KERN_SRC) M=$(PWD) modules_install
	depmod -a

clean:
	make -C $(KERN_SRC) M=$(PWD) clean