#
# Makefile for the linux duet framework
#

ifneq ($(KERNELRELEASE),)
# kbuild part of the Makefile
obj-$(CONFIG_DUET) := duet.o
duet-y += init.o ioctl.o task.o bmap.o hook.o

else
# normal Makefile
CONFIG_DUET ?= m
KDIR ?= /lib/modules/`uname -r`/build

default:
	$(MAKE) -C $(KDIR) M=$(PWD)

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
endif
