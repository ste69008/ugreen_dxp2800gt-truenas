obj-m += ug201x_full.o
ug201x_full-objs := ug_201x.o leds-sio-201x.o

EXTRA_CFLAGS += -Wno-error -Wno-missing-prototypes -Wno-unused-function

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	make -C $(KDIR) M=$(PWD) CONFIG_DEBUG_INFO_BTF= modules

clean:
	make -C $(KDIR) M=$(PWD) clean
