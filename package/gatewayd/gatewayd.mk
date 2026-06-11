GATEWAYD_VERSION = 1.0
GATEWAYD_SITE_METHOD = local
GATEWAYD_SITE = $(GATEWAYD_PKGDIR)

define GATEWAYD_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Os -Wall -Wextra \
		-o $(@D)/gatewayd $(GATEWAYD_PKGDIR)/gatewayd.c $(TARGET_LDFLAGS)
	$(TARGET_STRIP) $(@D)/gatewayd
endef

define GATEWAYD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/gatewayd $(TARGET_DIR)/usr/sbin/gatewayd
	$(INSTALL) -D -m 0755 $(GATEWAYD_PKGDIR)/S99gatewayd \
		$(TARGET_DIR)/etc/init.d/S99gatewayd
endef

$(eval $(generic-package))
