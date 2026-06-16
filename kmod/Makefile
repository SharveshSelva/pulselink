# Out-of-tree kernel module build, plus host-side userspace + ring unit test.
#
#   make            -> build the kernel module (.ko)  [needs kernel headers]
#   make test       -> build+run the host ring unit test (gcc, no kernel)
#   make telemetry_cat -> build the userspace reader (gcc)
#   make clean

ifneq ($(KERNELRELEASE),)
# --- kbuild context (invoked by the kernel build system) ---
obj-m += pulselink_telemetry.o
pulselink_telemetry-objs := telemetry_drv.o tring.o

else
# --- normal shell context ---
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)
CC   ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -std=c11

.PHONY: all modules test clean
all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

telemetry_cat: telemetry_cat.c telemetry_uapi.h
	$(CC) -Wall -Wextra -O2 telemetry_cat.c -o telemetry_cat

telemetry_stat: telemetry_stat.c telemetry_uapi.h
	$(CC) -Wall -Wextra -O2 telemetry_stat.c -o telemetry_stat

tools: telemetry_cat telemetry_stat

test: tring_test
	./tring_test

tring_test: tring_test.c tring.c tring.h telemetry_uapi.h
	$(CC) $(CFLAGS) tring_test.c tring.c -o tring_test

clean:
	-$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || true
	rm -f telemetry_cat telemetry_stat tring_test
	rm -f *.o *.ko *.mod *.mod.c *.symvers *.order .*.cmd

endif
