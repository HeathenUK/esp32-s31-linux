#!/bin/sh
# Build the plan's stage-5 corpus for the glref A/B harness, from the
# upstream sources, each with its OWN build system and no source change:
#   SDL 1.2.15 test/testgl    -> /src/gl/ref-apps/build/sdl12-test/testgl
#   SDL2 2.32.10 test/testgl2 -> /src/gl/ref-apps/build/sdl2-test/testgl2
#   rRootage 0.23a            -> /src/gl/ref-apps/build/rrootage/rr
# against the RIG's SDL (Debian: sdl12-compat 1.2.68 over SDL2 2.32.4, so
# the "SDL 1.2" arm exercises SDL2's GLX loader, not SDL 1.2.15's
# SDL_x11gl.c - see artifacts/gl/phase2/SUITE.md), with GLU from
# build-apps.sh. The tarballs are the ones the research scratchpad fetched
# (md5 in SUITE.md) and are expected in /src/gl/ref-apps/.
#
# Knobs used, all build-system inputs, none a source edit:
#  - --build=aarch64-unknown-linux-gnu: the 2012 config.guess cannot guess
#    aarch64 (Buildroot would refresh config.guess instead).
#  - SDL2 test: ac_cv_lib_OpenGL_glBegin=no, so it would link -lGL as on the
#    board (there is no glvnd libOpenGL there). testgl2 links no GL library
#    at all in the end: SDL2 dlopen()s libGL.so.1 and fetches every function
#    with SDL_GL_GetProcAddress.
#  - rRootage: make variables on the command line (its makefile.lin is the
#    MinGW one: -mwindows, no GL libraries), CXXFLAGS=-fpermissive and
#    -include cstring for 2003 C++ (bulletml), `make clean` first because the
#    tarball ships x86 objects, and -O0: see below.
#
# rRootage's screen.c loadGLTexture() strcpy()s "/usr/share/games/rRootage/"
# + "images/" + name into char name[32] - a stack overflow in the stock
# source that ASan reports at the first texture and that crashes the game
# (SIGBUS, jump to an odd PC) at -O1/-O2/-O3/-Os under Mesa and ours alike.
# At -O0 the overflow lands on dead stack and the game runs; both libGLs run
# the same binary, so the A/B stays fair. The board package needs Debian's
# patch (or an equivalent) - recorded in SUITE.md as a finding for stage 5a.
#
# GLtron 0.70 is NOT built: nebu/scripting/scripting.c defines
# scripting_Run*(char *) against nebu_scripting.h's (const char *) - a
# conflicting-types error no compiler flag downgrades. It needs a source
# patch, which the task rules out.
#
# Idempotent; --force rebuilds. From the host it re-execs itself in s31-glref.
set -eu

if [ ! -x /usr/bin/Xvfb ] || [ ! -d /src/tools/glref ]; then
    here=$(cd "$(dirname "$0")/../.." && pwd)
    exec docker run --rm -v "$here":/src -w /src s31-glref:latest sh /src/tools/glref/build-games.sh "$@"
fi
FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

R=/src/gl/ref-apps
P=$R/prefix
B=$R/build
J=$(nproc)
mkdir -p "$B"
sh /src/tools/glref/build-apps.sh >/dev/null   # GLU and freeglut first

unpack() { # dir tarball [tar args]
    d=$1 t=$2; shift 2
    [ -d "$R/$d" ] && return 0
    [ -f "$R/$t" ] || { echo "missing $R/$t" >&2; exit 2; }
    (cd "$R" && tar xzf "$t" "$@")
}
unpack SDL-1.2.15 SDL-1.2.15.tar.gz
# the Android project has entries virtiofs refuses; nothing here needs it
unpack SDL2-2.32.10 SDL2-2.32.10.tar.gz --exclude='SDL2-2.32.10/android-project*'
unpack rrootage-0.23a rrootage-0.23a.tar.gz

sdl12() {
    [ $FORCE = 0 ] && [ -x "$B/sdl12-test/testgl" ] && return 0
    rm -rf "$B/sdl12-test" && mkdir -p "$B/sdl12-test" && cd "$B/sdl12-test" &&
    "$R/SDL-1.2.15/test/configure" --build=aarch64-unknown-linux-gnu >configure.log 2>&1 &&
    make testgl >make.log 2>&1 && echo "sdl12 testgl: built"
}
sdl2() {
    [ $FORCE = 0 ] && [ -x "$B/sdl2-test/testgl2" ] && return 0
    rm -rf "$B/sdl2-test" && mkdir -p "$B/sdl2-test" && cd "$B/sdl2-test" &&
    "$R/SDL2-2.32.10/test/configure" --build=aarch64-unknown-linux-gnu \
        ac_cv_lib_OpenGL_glBegin=no >configure.log 2>&1 &&
    make testgl2 >make.log 2>&1 && echo "sdl2 testgl2: built"
}
rrootage() {
    [ $FORCE = 0 ] && [ -x "$B/rrootage/rr" ] && return 0
    rm -rf "$B/rrootage" && mkdir -p "$B/rrootage" &&
    cp -r "$R/rrootage-0.23a/src/." "$B/rrootage/" && cd "$B/rrootage/bulletml" &&
    make clean >/dev/null 2>&1 &&
    make -j"$J" CXXFLAGS="-O2 -fpermissive -include cstring" >../bulletml.log 2>&1 &&
    cd .. && make -f makefile.lin clean >/dev/null 2>&1 &&
    make -j"$J" -f makefile.lin MORE_CFLAGS="-DLINUX -O0 -Wall" CXXFLAGS="-fpermissive" \
        LDFLAGS="$(sdl-config --libs) -Lbulletml -L$P/lib -Wl,-rpath,$P/lib -lGLU -lGL -lbulletml -lSDL_mixer -lstdc++ -lm" \
        >rr.log 2>&1 && echo "rrootage: built"
}
# independent: side by side, each failure reported
sdl12 >"$B/sdl12.out" 2>&1 & p1=$!
sdl2 >"$B/sdl2.out" 2>&1 & p2=$!
rrootage >"$B/rrootage.out" 2>&1 & p3=$!
rc=0
wait $p1 || { echo "sdl12 testgl FAILED (see $B/sdl12-test/*.log)" >&2; rc=1; }
wait $p2 || { echo "sdl2 testgl2 FAILED (see $B/sdl2-test/*.log)" >&2; rc=1; }
wait $p3 || { echo "rrootage FAILED (see $B/rrootage/*.log)" >&2; rc=1; }
cat "$B/sdl12.out" "$B/sdl2.out" "$B/rrootage.out"
# the harness swaps implementations by LD_LIBRARY_PATH: nothing may bind glvnd
for f in "$B/sdl12-test/testgl" "$B/sdl2-test/testgl2" "$B/rrootage/rr"; do
    [ -x "$f" ] || continue
    if LD_LIBRARY_PATH=$P/lib ldd "$f" | grep -q 'libOpenGL.so'; then
        echo "$f links glvnd libOpenGL" >&2; rc=1
    fi
done
exit $rc
