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
endef

$(eval $(generic-package))
