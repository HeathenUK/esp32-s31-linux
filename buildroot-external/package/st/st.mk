################################################################################
#
# st
#
################################################################################

ST_VERSION = 0.9.2
ST_SITE = https://dl.suckless.org/st
ST_LICENSE = MIT
ST_LICENSE_FILES = LICENSE

ST_DEPENDENCIES = xlib_libX11 xlib_libXft fontconfig freetype host-pkgconf

# st has no configure; its config.mk hardcodes /usr/local, pkg-config and the
# host compiler. Override on the command line rather than patching, so a version
# bump does not need the patch refreshed.
ST_MAKE_OPTS = \
	CC="$(TARGET_CC)" \
	PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" \
	X11INC="$(STAGING_DIR)/usr/include/X11" \
	X11LIB="$(STAGING_DIR)/usr/lib"

# st hardcodes TERM=st-256color in config.h and expects `make install` to run
# tic and install its own terminfo. We install only the binary, so point it at
# an entry ncurses already ships - otherwise everything run inside it sees an
# unknown TERM and misbehaves in ways that look like st being broken.
define ST_USE_EXISTING_TERMINFO
	$(SED) 's/"st-256color"/"xterm-256color"/' $(@D)/config.def.h
	rm -f $(@D)/config.h
endef
ST_PRE_BUILD_HOOKS += ST_USE_EXISTING_TERMINFO

define ST_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) $(ST_MAKE_OPTS) st
endef

define ST_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/st $(TARGET_DIR)/usr/bin/st
endef

$(eval $(generic-package))
