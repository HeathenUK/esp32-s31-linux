################################################################################
#
# xfiles
#
################################################################################

XFILES_VERSION = c47a0d9
XFILES_SITE = $(BR2_EXTERNAL_ESP32_S31_PATH)/../buildroot/dl/xfiles
XFILES_SITE_METHOD = file
XFILES_LICENSE = MIT
XFILES_LICENSE_FILES = LICENSE

XFILES_DEPENDENCIES = \
	xlib_libX11 xlib_libXft xlib_libXpm xlib_libXcursor \
	xlib_libXrender xlib_libXext fontconfig host-pkgconf

# The Makefile hardcodes host include and library paths - /usr/local,
# /usr/X11R6 and /usr/include/freetype2 - which Buildroot's toolchain wrapper
# rejects outright as host-header poisoning. They sit inside the variables the
# build uses, so they cannot be overridden from the command line; replace the
# whole line with pkg-config output, which is correct for a cross build and
# survives a version bump better than deleting each path individually.
define XFILES_DROP_HOST_PATHS
	$(SED) 's|-I. -I/usr/local/include -I/usr/X11R6/include|-I.|' $(@D)/Makefile
	$(SED) 's|-I/usr/include/freetype2 -I/usr/X11R6/include/freetype2|`$(PKG_CONFIG_HOST_BINARY) --cflags freetype2 fontconfig xft`|' $(@D)/Makefile
	$(SED) 's|-L/usr/local/lib -L/usr/X11R6/lib||' $(@D)/Makefile
endef
XFILES_PRE_BUILD_HOOKS += XFILES_DROP_HOST_PATHS

define XFILES_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)" xfiles
endef

# xfilesctl is not optional decoration: it is how copy, move and delete are
# performed. Without it the file manager browses but cannot act.
define XFILES_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/xfiles $(TARGET_DIR)/usr/bin/xfiles
	$(INSTALL) -D -m 0755 $(@D)/examples/xfilesctl $(TARGET_DIR)/usr/bin/xfilesctl
	$(INSTALL) -D -m 0755 $(@D)/examples/xfilesthumb $(TARGET_DIR)/usr/bin/xfilesthumb
endef

$(eval $(generic-package))
