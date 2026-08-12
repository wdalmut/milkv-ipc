################################################################################
#
# duos-ipc
#
################################################################################

DUOS_IPC_VERSION = 1.0
DUOS_IPC_SITE = $(BR2_EXTERNAL_DUOS_IPC_PATH)/src
DUOS_IPC_SITE_METHOD = local
DUOS_IPC_LICENSE = MIT
DUOS_IPC_LICENSE_FILES = ../LICENSE

define DUOS_IPC_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS) -Wall -Wextra"
endef

define DUOS_IPC_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/reader $(TARGET_DIR)/usr/bin/ipc-reader
	$(INSTALL) -D -m 0755 $(@D)/ipc_cmd $(TARGET_DIR)/usr/bin/ipc-cmd
endef

ifeq ($(BR2_PACKAGE_DUOS_IPC_KMOD),y)

# NON si usa l'infrastruttura kernel-module di Buildroot: quella presuppone che
# sia Buildroot a costruire il kernel (BR2_LINUX_KERNEL), mentre qui il kernel lo
# costruisce l'SDK da linux_5.10. Si invoca a mano il Makefile del kernel.
#
ifeq ($(BR2_aarch64),y)
DUOS_IPC_KARCH = arm64
else
DUOS_IPC_KARCH = riscv
endif

# Il Makefile stub dentro la output dir del kernel contiene un path assoluto
# fissato a build time (/sdk/...), quindi e' inservibile fuori da quel contesto:
# si entra dai sorgenti con O=, non con -C sulla output dir.
#
# La objdir NON si puo' dare per scontata. Buildroot ripulisce l'ambiente
# per-package: KERNEL_PATH sopravvive, KERNEL_OUTPUT_FOLDER no, e con quella
# vuota O= finisce a coincidere coi sorgenti e kbuild fallisce con
# "Kernel configuration is invalid". Quindi: si prova il valore dall'ambiente,
# poi si ripiega sull'unica build dir presente, e in entrambi i casi si valida
# cercando include/config/auto.conf, che e' il file che kbuild pretende
# davvero - non basta che la directory esista.
define DUOS_IPC_BUILD_KMOD
	@set -e; \
	test -n "$(TOP_DIR)" || { echo "!! TOP_DIR non nell'ambiente: lancia dall'SDK" >&2; exit 1; }; \
	test -n "$(KERNEL_PATH)" || { echo "!! KERNEL_PATH non nell'ambiente" >&2; exit 1; }; \
	KOBJ="$(KERNEL_PATH)/$(KERNEL_OUTPUT_FOLDER)"; \
	if [ ! -f "$$KOBJ/include/config/auto.conf" ]; then \
		KOBJ=$$(ls -d $(KERNEL_PATH)/build/*/ 2>/dev/null | sed 's:/$$::' | head -1); \
	fi; \
	if [ ! -f "$$KOBJ/include/config/auto.conf" ]; then \
		echo "!! kernel non configurato: nessun include/config/auto.conf" >&2; \
		echo "   KERNEL_PATH='$(KERNEL_PATH)'" >&2; \
		echo "   KERNEL_OUTPUT_FOLDER='$(KERNEL_OUTPUT_FOLDER)' (vuota = non propagata)" >&2; \
		echo "   candidate: $$(ls -d $(KERNEL_PATH)/build/*/ 2>/dev/null | tr '\n' ' ')" >&2; \
		exit 1; \
	fi; \
	echo ">> duos-ipc: modulo kernel contro $$KOBJ"; \
	$(TARGET_MAKE_ENV) $(MAKE) -C $(KERNEL_PATH) \
		O="$$KOBJ" \
		M=$(@D)/kmod \
		SDK_DIR=$(TOP_DIR) \
		KBUILD_EXTRA_SYMBOLS=$(TOP_DIR)/osdrv/interdrv/rtos_cmdqu/Module.symvers \
		ARCH=$(DUOS_IPC_KARCH) \
		CROSS_COMPILE=$(TARGET_CROSS) \
		modules
endef
DUOS_IPC_POST_BUILD_HOOKS += DUOS_IPC_BUILD_KMOD

# Installato in un path nostro, non in /mnt/system/ko: quella directory e'
# popolata da SYSTEM_OUT_DIR dell'SDK (non da Buildroot) e l'ordine di
# caricamento la' dentro e' fissato da loadsystemko.sh. Lo carica invece
# /etc/init.d/S99duos-ipc, che gira dopo, dal rootfs-overlay.
define DUOS_IPC_INSTALL_KMOD
	$(INSTALL) -D -m 0644 $(@D)/kmod/duos_ipc_irq.ko \
		$(TARGET_DIR)/usr/lib/duos-ipc/duos_ipc_irq.ko
endef
DUOS_IPC_POST_INSTALL_TARGET_HOOKS += DUOS_IPC_INSTALL_KMOD

endif # BR2_PACKAGE_DUOS_IPC_KMOD

$(eval $(generic-package))
