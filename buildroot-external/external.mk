include $(sort $(wildcard $(BR2_EXTERNAL_ESP32_S31_PATH)/package/*/*.mk))

# alsa-lib on musl/rv32 falls back to a SYNC_PTR ioctl for every PCM position
# read (345/s under Doom, ~4% of the core) because the kernel refuses the
# status/control mmap: sound/core/pcm_native.c allows it only on x86, PPC and
# Alpha ("coherent mmap"). Not fixable from userspace - defining
# __USE_TIME_BITS64 here made alsa-lib ask for the 64-bit-time offsets and
# the kernel refused those identically (ENXIO, measured 2026-09-10). Left
# stock. The reducible part is the COUNT, which s31route's period
# negotiation controls.

################################################################################
# OpenGL: SDL stays GL-OFF unless explicitly switched on (docs/gl-packaging.md)
#
# WHY HERE. SDL 1.2's configure enables GL on its own whenever GL/gl.h,
# GL/glx.h and GL/glu.h compile from staging (SDL-1.2.15 configure.in:1650-1672,
# default enable_video_opengl=yes), and buildroot's sdl.mk passes no
# --disable-video-opengl (package/sdl/sdl.mk:31-33, 76-82). Today it is GL-off
# only because the headers are absent: the shipped config.log fails its probe
# with "GL/gl.h: No such file or directory". Once s31-libgl stages mesa3d-headers
# and libglu stages glu.h, the answer would depend on whether SDL happened to be
# (re)configured before or after them - build ORDER deciding every SDL 1.2
# app's video path. So the switch is stated, both ways.
#
# WHY IT WORKS FROM external.mk. This file is included AFTER every
# package/*/*.mk (buildroot/Makefile:550 then :564), i.e. after sdl.mk's
# $(eval $(autotools-package)). That is still in time for CONF_OPTS and
# hooks, because the generated configure recipe refers to them with a deferred
# $$($$(PKG)_CONF_OPTS) (package/pkg-autotools.mk:193, pkg-meson.mk:171) and
# the hooks are expanded in the stamp recipe (package/pkg-generic.mk:262-264),
# all at recipe time. SDL_CONF_OPTS was first set with += on an undefined
# variable (sdl.mk:31), so it is recursive and += appends. Autoconf takes the
# last of repeated --enable/--disable switches, so this also wins over any
# earlier one.
#
# It is NOT in time for <PKG>_DEPENDENCIES: the ordering rule
# "$(SDL_TARGET_CONFIGURE): | $(SDL_FINAL_DEPENDENCIES)" is expanded when the
# eval parses it (pkg-generic.mk:940). So the GL-on arm adds its own order-only
# prerequisite to the same stamp instead of appending to SDL_DEPENDENCIES.
################################################################################

ifeq ($(BR2_PACKAGE_SDL),y)
ifeq ($(BR2_PACKAGE_S31_LIBGL_SDL_OPENGL),y)
SDL_CONF_OPTS += --enable-video-opengl
$(SDL_TARGET_CONFIGURE): | libgl libglu
else
SDL_CONF_OPTS += --disable-video-opengl
endif
endif

# SDL2 already passes --disable-video-opengl unless BR2_PACKAGE_SDL2_OPENGL
# (package/sdl2/sdl2.mk:133-138), so it needs no guard while GL is off.
#
# PLAN 4.2 OPTION B, WRITTEN BUT INERT: it only exists once SDL2_OPENGL is
# enabled (stage 5b). SDL2's configure defines SDL_VIDEO_RENDER_OGL together
# with SDL_VIDEO_OPENGL whenever GL/gl.h+glext.h compile (SDL2-2.32.10
# configure.ac:2566-2582) and has no switch for the render driver alone.
# Deleting the define from the generated header lets SDL_internal.h:142-144
# default it to 0, so "opengl" never enters SDL_CreateRenderer's list
# (SDL_render.c:113) while GLX contexts (configure.ac:2528) stay. The checks
# make a changed header format fail the build instead of silently shipping
# SDL's GL renderer.
ifeq ($(BR2_PACKAGE_S31_LIBGL_SDL2_NO_GL_RENDERER),y)
define S31_SDL2_DROP_GL_RENDERER
	$(SED) '/^#define SDL_VIDEO_RENDER_OGL 1$$/d' $(@D)/include/SDL_config.h
	! grep -q '^#define SDL_VIDEO_RENDER_OGL ' $(@D)/include/SDL_config.h
	grep -q '^#define SDL_VIDEO_OPENGL_GLX 1' $(@D)/include/SDL_config.h
endef
SDL2_POST_CONFIGURE_HOOKS += S31_SDL2_DROP_GL_RENDERER
endif

# mesa3d-demos compiles its data path in as the RELATIVE "../data/" unless
# -Dwith-system-data-files=true (mesa-demos-9.0.0 meson.build:31-36), while
# installing the files to /usr/share/mesa-demos (src/data/meson.build). Stock
# buildroot passes neither (package/mesa3d-demos/mesa3d-demos.mk:13-15), so
# every textured demo (texcyl, isosurf, terrain, geartrain, ...) fails to find
# its data unless started from a directory that happens to have ../data. This is
# the package's own build option, not an app change.
ifeq ($(BR2_PACKAGE_MESA3D_DEMOS),y)
MESA3D_DEMOS_CONF_OPTS += -Dwith-system-data-files=true
endif

# libGLU without the C++ runtime on the board.
#
# GLU 9.0.3 is C plus a C++ NURBS tessellator (glu-9.0.3 src/meson.build:4-111,
# the libnurbs/*.cc sources; bufpool.h:130 overloads operator new), so meson
# links libGLU.so.1 with g++ and records NEEDED libstdc++.so.6. This board has
# no C++ runtime: board/esp32-s31/post-build.sh:12 deletes lib/libstdc++.so*
# unconditionally, so every GLU app (rRootage, the GLUT demos, prboom-plus GL)
# would die at load. Keeping it instead costs a 1.69 MB library (532 kB text,
# 39 kB data, measured from the toolchain's libstdc++.so.6.0.34) loaded and
# relocated per GLU process from the card.
#
# -static-libstdc++ links only the members GLU uses (operator new/delete, the
# pure-virtual and EH support) into libGLU itself. It is a link flag, not a
# source change. It is possible because the toolchain's libstdc++.a is PIC:
# its 191 objects carry GOT/PCREL relocations and no R_RISCV_HI20/LO12
# (counted 2026-09-25). LIBGLU_LDFLAGS is read by the meson cross-file sed at
# configure time (package/pkg-meson.mk:143 and :154), so appending here works.
# The hook turns a regression into a build failure rather than a board that
# cannot start a GLU app.
ifeq ($(BR2_PACKAGE_LIBGLU),y)
LIBGLU_LDFLAGS += -static-libstdc++
define S31_LIBGLU_CHECK_NO_LIBSTDCXX
	if $(TARGET_READELF) -d $(TARGET_DIR)/usr/lib/libGLU.so.1 | \
		grep -q 'NEEDED.*libstdc++'; then \
		echo "libglu: libGLU.so.1 still NEEDs libstdc++.so.6, which" \
			"post-build.sh deletes - see external.mk" >&2; \
		exit 1; \
	fi
endef
LIBGLU_POST_INSTALL_TARGET_HOOKS += S31_LIBGLU_CHECK_NO_LIBSTDCXX
endif
