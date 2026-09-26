################################################################################
#
# rrootage - rRootage 0.23a, SDL 1.2 + OpenGL (GL plan stage 5a)
#
# STOCK SOURCE, BUILD-SYSTEM INPUTS ONLY (docs/gl-plan-2026-09-25.md rule 1):
#  - makefile.lin is the MinGW one (-mwindows, -lglut, no GL libraries), so its
#    variables are set on the make command line, as tools/glref/build-games.sh
#    does on the host;
#  - -fpermissive and -include cstring for its 2003 C++ (bulletml), and
#    -std=gnu17 for its 2003 C (GCC 15 defaults to C23, where f() is f(void));
#  - the tarball ships x86 objects: `make clean` first;
#  - libstdc++ linked statically (-Wl,-Bstatic: gcc drives the link, so
#    -static-libstdc++ is inert), and BEFORE -lGLU: libGLU carries its own
#    static libstdc++ and exports it, so linked after it the game bound 81 C++
#    symbols to libGLU; post-build.sh deletes the shared C++ runtime;
#  - -O0, deliberately. screen.c loadGLTexture() strcpy()s
#    "/usr/share/games/rRootage/images/" + a file name into char name[32]: a
#    stack overflow in the stock source that crashes the game at -O1..-O3/-Os
#    (under Mesa and our libGL alike, host rig, ASan-confirmed). At -O0 the
#    overflow lands on dead stack. Debian carries a source patch for it; the
#    plan forbids patching the app, so this is the price, and it only slows
#    the game's own logic - the GL work is in our optimised libGL.
#
#
# RESULT 2026-09-26: builds, but the board crashes (SIGSEGV) in loadGLTexture -
# the overflow is unconditional (the path is 42 bytes into 32; SHARE_LOC is a
# plain #define in the .c files, so no build flag can shorten it) and x86 at
# -O0 only survived by stack layout. Not enabled in the defconfig. Enable it
# only with Debian's source patch, if the owner accepts that as off-the-shelf.
#
################################################################################

RROOTAGE_VERSION = 0.23a
RROOTAGE_SOURCE = rrootage_$(RROOTAGE_VERSION).orig.tar.gz
RROOTAGE_SITE = http://deb.debian.org/debian/pool/main/r/rrootage
RROOTAGE_LICENSE = BSD-2-Clause
RROOTAGE_LICENSE_FILES = readme_e.txt
RROOTAGE_DEPENDENCIES = sdl sdl_mixer libglu libgl

RROOTAGE_CXX = $(TARGET_CXX) $(TARGET_CXXFLAGS) -O0 -fpermissive -include cstring

define RROOTAGE_BUILD_CMDS
	$(MAKE) -C $(@D)/src/bulletml clean
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/src/bulletml \
		CC="$(RROOTAGE_CXX)" CXX="$(RROOTAGE_CXX)" LD="$(RROOTAGE_CXX)" AR="$(TARGET_AR)" \
		CXXFLAGS="" CFLAGS=""
	$(MAKE) -C $(@D)/src -f makefile.lin clean
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/src -f makefile.lin \
		CC="$(TARGET_CC)" CXX="$(TARGET_CXX)" \
		DEFAULT_CFLAGS="`$(STAGING_DIR)/usr/bin/sdl-config --cflags`" \
		MORE_CFLAGS="$(TARGET_CFLAGS) -std=gnu17 -DLINUX -O0 -Wall" CXXFLAGS="-fpermissive" \
		LDFLAGS="$(TARGET_LDFLAGS) `$(STAGING_DIR)/usr/bin/sdl-config --libs` -Lbulletml -lbulletml -Wl,-Bstatic -lstdc++ -lsupc++ -Wl,-Bdynamic -lGLU -lGL -lSDL_mixer -lm"
endef

# The game reads /usr/share/games/rRootage (compiled in) and runs from the SD.
define RROOTAGE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/src/rr $(TARGET_DIR)/usr/games/rrootage
	mkdir -p $(TARGET_DIR)/usr/share/games/rRootage
	cp -r $(@D)/rr_share/. $(TARGET_DIR)/usr/share/games/rRootage/
endef

$(eval $(generic-package))
