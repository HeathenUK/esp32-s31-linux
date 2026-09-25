#!/bin/sh
# Build the stock reference GL apps for the glref A/B harness, unmodified:
#   GLU 9.0.3 (Mesa GLU)      -> /src/gl/ref-apps/prefix/lib/libGLU.so.1
#   freeglut 3.8.0            -> /src/gl/ref-apps/prefix/lib/libglut.so.3
#   mesa-demos 9.0.0          -> /src/gl/ref-apps/build/mesa-demos/src/{xdemos,demos}/*
#
# Everything links libGL.so.1 and nothing else from GL (no glvnd libOpenGL /
# libGLX), exactly as on the board against s31-libgl, so the same binary runs
# on Mesa or on ours just by LD_LIBRARY_PATH. Debian's own libGLU links
# libOpenGL.so.0 and its glx.pc says -lGLX, which is why GLU is rebuilt here
# and tools/glref/pkgconfig overrides gl.pc/glx.pc.
#
# Run from the host (re-execs itself in the s31-glref container) or inside it.
# Idempotent: each component is skipped when its output already exists; pass
# --force to rebuild all three.
set -eu

if [ ! -x /usr/bin/Xvfb ] || [ ! -d /src/tools/glref ]; then
    here=$(cd "$(dirname "$0")/../.." && pwd)
    exec docker run --rm -v "$here":/src -w /src s31-glref:latest sh /src/tools/glref/build-apps.sh "$@"
fi

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

R=/src/gl/ref-apps
P=$R/prefix
B=$R/build
mkdir -p "$B" "$P"
export PKG_CONFIG_PATH=/src/tools/glref/pkgconfig:$P/lib/pkgconfig:$P/lib/$(gcc -dumpmachine)/pkgconfig
export LD_LIBRARY_PATH=$P/lib

fetch() { # name tarball url
    [ -d "$R/$1" ] && return 0
    [ -f "$R/$2" ] || wget -q -O "$R/$2" "$3"
    (cd "$R" && tar xf "$2")
}

fetch glu-9.0.3 glu-9.0.3.tar.xz https://archive.mesa3d.org/glu/glu-9.0.3.tar.xz
fetch freeglut-3.8.0 freeglut-3.8.0.tar.gz https://github.com/freeglut/freeglut/releases/download/v3.8.0/freeglut-3.8.0.tar.gz
fetch mesa-demos-9.0.0 mesa-demos-9.0.0.tar.xz https://archive.mesa3d.org/demos/mesa-demos-9.0.0.tar.xz

# --- GLU ------------------------------------------------------------------
if [ $FORCE = 1 ] || [ ! -e "$P/lib/libGLU.so.1" ]; then
    rm -rf "$B/glu"
    meson setup "$B/glu" "$R/glu-9.0.3" --prefix="$P" --libdir=lib \
        -Dgl_provider=gl -Ddefault_library=shared -Dbuildtype=release >"$B/glu.log" 2>&1
    ninja -C "$B/glu" install >>"$B/glu.log" 2>&1
    echo "glu: built"
fi

# --- freeglut ---------------------------------------------------------------
if [ $FORCE = 1 ] || [ ! -e "$P/lib/libglut.so.3" ]; then
    rm -rf "$B/freeglut"
    cmake -S "$R/freeglut-3.8.0" -B "$B/freeglut" -G Ninja \
        -DCMAKE_INSTALL_PREFIX="$P" -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_BUILD_TYPE=Release -DOpenGL_GL_PREFERENCE=LEGACY \
        -DFREEGLUT_BUILD_DEMOS=OFF -DFREEGLUT_BUILD_STATIC_LIBS=OFF \
        -DFREEGLUT_GLES=OFF -DFREEGLUT_WAYLAND=OFF >"$B/freeglut.log" 2>&1
    ninja -C "$B/freeglut" install >>"$B/freeglut.log" 2>&1
    echo "freeglut: built"
fi

# --- mesa-demos -------------------------------------------------------------
XDEMOS="glxgears glxinfo glxheads manywin multictx offset glxgears_fbconfig"
GLUTDEMOS="gears morph3d bounce spectex geartrain ipers terrain tunnel fire teapot texcyl isosurf"
MD=$B/mesa-demos
if [ $FORCE = 1 ] || [ ! -e "$MD/build.ninja" ]; then
    rm -rf "$MD"
    meson setup "$MD" "$R/mesa-demos-9.0.0" -Dbuildtype=release \
        -Dglut=enabled -Dx11=enabled -Degl=disabled -Dgles1=disabled \
        -Dgles2=disabled -Dosmesa=disabled -Dlibdrm=disabled \
        -Dvulkan=disabled -Dwayland=disabled >"$B/mesa-demos.log" 2>&1
fi
targets=""
for t in $XDEMOS; do targets="$targets src/xdemos/$t"; done
for t in $GLUTDEMOS; do targets="$targets src/demos/$t"; done
# shellcheck disable=SC2086
ninja -C "$MD" $targets >>"$B/mesa-demos.log" 2>&1
echo "mesa-demos: built $(echo $targets | wc -w) targets:$(echo $targets | sed "s|src/[a-z]*/||g")"

# --- the GL-linkage invariant ------------------------------------------------
bad=0
for f in "$P/lib/libGLU.so.1" "$P/lib/libglut.so.3" $(for t in $targets; do echo "$MD/$t"; done); do
    n=$(readelf -d "$f" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p' | tr '\n' ' ')
    case "$n" in
        *libOpenGL*|*libGLX*|*libEGL*) echo "BAD LINKAGE: $f needs $n"; bad=1 ;;
    esac
done
[ $bad = 0 ] && echo "linkage: every binary needs libGL.so.1 only (no libOpenGL/libGLX)"
exit $bad
