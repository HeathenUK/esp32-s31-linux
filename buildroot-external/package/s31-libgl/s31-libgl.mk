################################################################################
#
# s31-libgl - the board's own libGL.so.1 (TinyGL-based GL 1.x + client GLX)
#
# A Buildroot "libgl" PROVIDER, on the nvidia-driver pattern
# (package/nvidia-driver/Config.in:24-34, nvidia-driver.mk:23-24 and 74-82):
# it selects BR2_PACKAGE_HAS_LIBGL, sets BR2_PACKAGE_PROVIDES_LIBGL, takes its
# GL/ and KHR/ headers from mesa3d-headers, and installs gl.pc and glx.pc. That
# is what lets the STOCK consumers - libglu (-Dgl_provider=gl, i.e. gl.pc),
# libfreeglut (CMake FindOpenGL, i.e. libGL.so + GL/gl.h), mesa3d-demos (gl.pc
# and glx.pc) - build against it unmodified. An x11-stage drop-in cannot do
# that: it only exists after Buildroot has finished.
#
# THE SOURCE IS gl/ IN THIS REPO, and the compile lives in gl/build.sh, not
# here, so the host iteration build and this one cannot drift apart. The
# contract this file relies on (docs/gl-packaging.md section 2):
#
#   S31GL_CC        compiler          (here: Buildroot's TARGET_CC wrapper)
#   S31GL_CFLAGS    base CFLAGS       (here: TARGET_CFLAGS - the board -march,
#                                      -Os and whatever the defconfig adds;
#                                      build.sh appends its own -O level,
#                                      -fPIC and library flags AFTER these,
#                                      so its choices win where they clash)
#   S31GL_LDFLAGS   base LDFLAGS      (here: TARGET_LDFLAGS, which carries the
#                                      defconfig's -Wl,--as-needed
#                                      -Wl,-Bsymbolic-functions - the flags
#                                      memory s31-our-libs-miss-buildroot-flags
#                                      says our out-of-tree libs kept missing)
#   S31GL_OUT       output directory  libGL.so.1.2.0, SONAME libGL.so.1
#   S31GL_STRIP=0   do not strip      (Buildroot strips the target copy
#                                      itself; staging keeps symbols)
#
# build.sh must write nothing outside S31GL_OUT and its own tree, and must not
# link with -Wl,-z,defs: it links against the STOCK libX11/libXext in staging
# and resolves against xlite/xstubs on the board, like every X client here.
#
################################################################################

S31_LIBGL_VERSION = 1.0
S31_LIBGL_SITE = $(BR2_EXTERNAL_ESP32_S31_PATH)/../gl
S31_LIBGL_SITE_METHOD = local
S31_LIBGL_LICENSE = MIT
S31_LIBGL_LICENSE_FILES = $(if $(wildcard $(S31_LIBGL_SITE)/LICENSE),LICENSE)
S31_LIBGL_INSTALL_STAGING = YES
S31_LIBGL_PROVIDES = libgl

# mesa3d-headers is not a build dependency of ours alone: packages that depend
# on "libgl" only reach the headers through us, exactly as nvidia-driver.mk
# says of itself. X11 and Xext for the GLX surface and the MIT-SHM present.
S31_LIBGL_DEPENDENCIES = mesa3d-headers xlib_libX11 xlib_libXext

# SITE_METHOD=local is implemented as an override srcdir and rsynced into
# $(@D) (package/pkg-generic.mk:645-647 and :228). gl/ref-apps is ~91 MB of
# host reference builds and tarballs that must not be copied on every rsync.
S31_LIBGL_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = \
	--exclude=/ref-apps --exclude=/out --exclude=/build

S31_LIBGL_OUT = $(@D)/_br_out
S31_LIBGL_SO = libGL.so.1.2.0

define S31_LIBGL_BUILD_CMDS
	test -f $(@D)/build.sh || \
		{ echo "s31-libgl: gl/build.sh is missing" >&2; exit 1; }
	rm -rf $(S31_LIBGL_OUT)
	mkdir -p $(S31_LIBGL_OUT)
	cd $(@D) && $(TARGET_MAKE_ENV) \
		S31GL_CC="$(TARGET_CC)" \
		S31GL_CFLAGS="$(TARGET_CFLAGS)" \
		S31GL_LDFLAGS="$(TARGET_LDFLAGS)" \
		S31GL_OUT="$(S31_LIBGL_OUT)" \
		S31GL_STRIP=0 \
		$(SHELL) ./build.sh
	# Fail here, loudly, rather than install something the loader will
	# not find: the soname is the whole integration (SDL 1.2
	# SDL_x11gl.c:38 and SDL2 SDL_x11opengl.c:47 dlopen "libGL.so.1").
	test -f $(S31_LIBGL_OUT)/$(S31_LIBGL_SO) || \
		{ echo "s31-libgl: build.sh did not produce $(S31_LIBGL_SO)" >&2; exit 1; }
	$(TARGET_READELF) -d $(S31_LIBGL_OUT)/$(S31_LIBGL_SO) | \
		grep -q 'SONAME.*\[libGL\.so\.1\]' || \
		{ echo "s31-libgl: $(S31_LIBGL_SO) has no SONAME libGL.so.1" >&2; exit 1; }
endef

define S31_LIBGL_INSTALL_LIB
	$(INSTALL) -D -m 0755 $(S31_LIBGL_OUT)/$(S31_LIBGL_SO) \
		$(1)/usr/lib/$(S31_LIBGL_SO)
	ln -sf $(S31_LIBGL_SO) $(1)/usr/lib/libGL.so.1
	ln -sf libGL.so.1 $(1)/usr/lib/libGL.so
endef

# Not using $(SED) because it works in place (-i).
define S31_LIBGL_INSTALL_STAGING_CMDS
	$(call S31_LIBGL_INSTALL_LIB,$(STAGING_DIR))
	mkdir -p $(STAGING_DIR)/usr/lib/pkgconfig
	sed -e 's:@VERSION@:$(S31_LIBGL_VERSION):' \
		$(S31_LIBGL_PKGDIR)/gl.pc.in \
		> $(STAGING_DIR)/usr/lib/pkgconfig/gl.pc
	sed -e 's:@VERSION@:$(S31_LIBGL_VERSION):' \
		$(S31_LIBGL_PKGDIR)/glx.pc.in \
		> $(STAGING_DIR)/usr/lib/pkgconfig/glx.pc
endef

# libGL.so (the -lGL link name) goes to the target as well: it costs a
# directory entry, and old code that dlopen()s the bare name then works.
define S31_LIBGL_INSTALL_TARGET_CMDS
	$(call S31_LIBGL_INSTALL_LIB,$(TARGET_DIR))
endef

$(eval $(generic-package))
