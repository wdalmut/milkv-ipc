# Modulo kernel out-of-tree, compilato contro il kernel dell'SDK.
#
# NON usa l'infrastruttura kernel-module di Buildroot: quella presuppone che sia
# Buildroot a costruire il kernel, mentre qui lo costruisce l'SDK da linux_5.10.
#
# A mano:
#   make -C src/kmod \
#        KDIR=$SDK/linux_5.10 \
#        SDK_DIR=$SDK \
#        ARCH=riscv \
#        CROSS_COMPILE=riscv64-unknown-linux-musl-
#
# Da Buildroot ci pensa package/duos-ipc/duos-ipc.mk.

obj-m += duos_ipc_irq.o

# rtos_cmdqu.h dell'SDK: da' IP_SYSTEM e i prototipi di request_rtos_irq() e
# free_rtos_irq(). E' l'unica dipendenza del modulo dai sorgenti Sophgo.
ccflags-y += -I$(SDK_DIR)/osdrv/interdrv/rtos_cmdqu
# shared_msg.h: unica fonte di verita' del contratto, condivisa coi due lati
# userspace e RTOS.
ccflags-y += -I$(src)/../shared

KDIR ?= /lib/modules/$(shell uname -r)/build

# request_rtos_irq()/free_rtos_irq() sono esportate da cv181x_rtos_cmdqu, che e'
# anch'esso un modulo esterno: i suoi simboli non stanno nel Module.symvers del
# kernel. Senza questo, modpost li dichiara undefined e la build si ferma.
KBUILD_EXTRA_SYMBOLS := $(SDK_DIR)/osdrv/interdrv/rtos_cmdqu/Module.symvers
export KBUILD_EXTRA_SYMBOLS

all:
	@test -n "$(SDK_DIR)" || { echo "!! SDK_DIR non impostata: serve per rtos_cmdqu.h" >&2; exit 1; }
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
