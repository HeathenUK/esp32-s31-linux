################################################################################
#
# prboomplus - PrBoom+ 2.5.1.4
#
# WHY A SECOND DOOM. The buildroot package pins prboom 2.5.0, released in 2008.
# PrBoom+ is the maintained fork and 2.5.1.4 is the LAST release on the SDL 1.2
# line, which matters here: SDL 1.2 reaches our shim through the zero-copy
# xlite-SHM path, and every SDL2 client so far has been markedly slower. The
# later PrBoom+ tags (2.5.1.5um onward, and 2.6.x) require SDL2, so 2.5.1.4 is
# the newest engine that can be compared against prboom 2.5.0 with the library
# held constant. Anything else conflates the engine with the graphics path.
#
# Built through buildroot's autotools infrastructure rather than by hand: the
# sysroot, the SDL prefix, autoreconf and the endianness fixups are all
# problems that package solved already.
#
################################################################################

PRBOOMPLUS_VERSION = prboom-plus_2.5.1.4
PRBOOMPLUS_SITE = $(call github,coelckers,prboom-plus,$(PRBOOMPLUS_VERSION))
PRBOOMPLUS_SUBDIR = prboom2
PRBOOMPLUS_CONF_ENV = ac_cv_type_gid_t=yes ac_cv_type_uid_t=yes
PRBOOMPLUS_DEPENDENCIES = sdl sdl_net sdl_mixer
PRBOOMPLUS_LICENSE = GPL-2.0+
PRBOOMPLUS_LICENSE_FILES = prboom2/COPYING
PRBOOMPLUS_AUTORECONF = YES
# The tree bundles its own PCRE with its own ltmain.sh, which buildroot's
# libtool patch does not match: "7 out of 7 hunks ignored". Nothing here links
# libtool libraries for the target, so skip the patch rather than fight it.
PRBOOMPLUS_LIBTOOL_PATCH = NO

PRBOOMPLUS_CFLAGS = $(TARGET_CFLAGS)

ifeq ($(BR2_TOOLCHAIN_GCC_AT_LEAST_15),y)
# Same reason as the prboom package: this is 2013 C and gcc 15 defaults to C23,
# where implicit declarations and old-style definitions are errors.
PRBOOMPLUS_CFLAGS += -std=gnu18
endif

# -fcommon: this is 2013 C and it relies on tentative definitions being merged
# into common symbols. GCC 10 changed the default to -fno-common, so the link
# fails with "multiple definition of `demover'" and a long tail behind it.
# Compiling with -fcommon restores the behaviour the code was written for; the
# alternative is patching every duplicate definition in an upstream tree we do
# not own.
PRBOOMPLUS_CFLAGS += -fcommon

PRBOOMPLUS_CONF_ENV += CFLAGS="$(PRBOOMPLUS_CFLAGS)"

ifeq ($(BR2_PACKAGE_LIBPNG),y)
PRBOOMPLUS_DEPENDENCIES += libpng
endif

PRBOOMPLUS_CONF_OPTS = \
	--oldincludedir=$(STAGING_DIR)/usr/include \
	--with-sdl-prefix=$(STAGING_DIR)/usr \
	--with-sdl-exec-prefix=$(STAGING_DIR)/usr \
	--disable-cpu-opt \
	--disable-sdltest \
	--disable-gl \
	--without-net

# THE WAD GENERATOR IS A HOST TOOL THAT AUTOTOOLS BUILDS FOR THE TARGET.
#
# data/Makefile.am declares rdatawad as noinst_PROGRAMS and then RUNS it to
# generate prboom-plus.wad, so a cross build compiles it for riscv32 and the
# build host cannot execute it: "./rdatawad: cannot execute binary file: Exec
# format error". prboom 2.5.0 does not hit this because its release tarball
# ships the wad prebuilt; a git tag does not.
#
# So build the generator separately with HOSTCC, in a scratch copy of data/ so
# no host object files end up where the target link will find them, run it
# there, and drop the result in. Then stop the target build from trying: strip
# the rule's prerequisites, and since the file is now present and has no
# prerequisites, make leaves it alone. rdatawad is still built for the target
# by `all`, which is harmless because nothing runs it.
define PRBOOMPLUS_HOST_WAD
	rm -rf $(@D)/.hostwad
	cp -a $(@D)/prboom2/data $(@D)/.hostwad
	# ASK make FOR THE LUMP LIST, then run the generator ourselves.
	#
	# WAD_CMDLINE expands eight other variables, so reconstructing it by
	# hand is how the first attempt produced a 12-byte wad - a header and
	# nothing else. But invoking make's own `prboom-plus.wad` rule drags in
	# automake's maintainer chain (the wad needs rdatawad needs Makefile
	# needs Makefile.in needs ../aclocal.m4) which reaches above the
	# scratch copy and fails. A standalone phony target has no such
	# prerequisites, so it expands the variable without waking any of that.
	# -o Makefile is still needed: make remakes its own makefiles BEFORE
	# considering any target, phony or not, and that is the step that
	# reaches for ../aclocal.m4.
	printf 's31-print-wadcmd:\n\t@echo $$(WAD_CMDLINE)\n' \
		>> $(@D)/.hostwad/Makefile
	# -I the configured build root: rd_*.c include config.h, which
	# configure wrote one level above data/. It is the TARGET's config.h,
	# safe here only because host and target are both little-endian and
	# this tool uses nothing else from it.
	cd $(@D)/.hostwad && $(HOSTCC) $(HOST_CFLAGS) -I. -I$(@D)/prboom2 \
		-o rdatawad rd_main.c rd_util.c rd_output.c rd_sound.c \
		rd_palette.c rd_graphic.c $(HOST_LDFLAGS) -lm
	cd $(@D)/.hostwad && ./rdatawad -I . \
		$$($(MAKE1) -s -o Makefile -o ../aclocal.m4 \
			s31-print-wadcmd) -o prboom-plus.wad
	# A header-only wad is 12 bytes and the game starts and then cannot
	# find its menu graphics, which is a confusing way to fail.
	test $$(stat -c%s $(@D)/.hostwad/prboom-plus.wad) -gt 100000
	cp $(@D)/.hostwad/prboom-plus.wad $(@D)/prboom2/data/prboom-plus.wad
	$(SED) 's|^prboom-plus.wad *: *rdatawad.*|prboom-plus.wad :|' \
		$(@D)/prboom2/data/Makefile
	touch $(@D)/prboom2/data/prboom-plus.wad
endef
PRBOOMPLUS_PRE_BUILD_HOOKS += PRBOOMPLUS_HOST_WAD

define PRBOOMPLUS_INSTALL_TARGET_CMDS
	$(INSTALL) -D $(@D)/prboom2/src/prboom-plus \
		$(TARGET_DIR)/usr/games/prboom-plus
	$(INSTALL) -D $(@D)/prboom2/data/prboom-plus.wad \
		$(TARGET_DIR)/usr/share/games/doom/prboom-plus.wad
endef

$(eval $(autotools-package))
