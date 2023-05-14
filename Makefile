obj-m 		:= valsa.o
KERNELVER 	?= $(shell uname -r)
KERNELDIR	?= /lib/modules/$(KERNELVER)/build

all:
	make -C $(KERNELDIR) M=$(PWD) modules
clean:
	make -C $(KERNELDIR) M=$(PWD) clean
