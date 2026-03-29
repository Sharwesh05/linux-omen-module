obj-m += hpomen.o

KVERSION ?= $(KERNELRELEASE)

ifeq ($(KVERSION),)
KVERSION := $(shell uname -r)
endif

KDIR := /lib/modules/$(KVERSION)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
