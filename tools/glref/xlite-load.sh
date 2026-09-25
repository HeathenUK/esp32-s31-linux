#!/bin/sh
# glref xlite load arm: would each suite app LOAD on the board?
#
#   tools/glref/xlite-load.sh [OUT.md]
#
# The suite runs apps against the host's stock libX11, so a PASS there says
# nothing about the X calls xlite lacks - and musl binds eagerly, so one
# missing import aborts the app at load on the board (review finding
# R7-harness-scope). This arm resolves every app, and every library it loads
# (libglut, libGLU), against the BOARD's X stack built for the host:
#   libGL.so.1   ours ($GLREF_OURS, default /src/gl/out-host)
#   libX11.so.6  xlite, /src/xlite unmodified (xlite/build.sh's recipe)
#   libXext.so.6 xstubs' STUB_XEXT, /src/xstubs unmodified
#   libXrandr.so.2, libXxf86vm.so.1  xlite's own (xlite/randr, xlite/vidmode)
# with `ldd -r`, which performs every data and function relocation the way
# LD_BIND_NOW / musl do, and lists each undefined symbol. No app is run and
# no display is needed. Libraries the board does not replace (libXi,
# libXrender ...) resolve to the host's Debian builds, standing in for the
# board's Buildroot ones, so their imports against xlite show up too.
#
# Writes OUT.md (default artifacts/gl/xlite-load.md): one row per app,
# LOADS or MISSING-SYMBOL with the names. Exit 0 always (it is a report).
set -u

if [ ! -x /usr/bin/Xvfb ] || [ ! -d /src/tools/glref ]; then
    here=$(cd "$(dirname "$0")/../.." && pwd)
    exec docker run --rm -v "$here":/src -w /src -e GLREF_OURS \
        s31-glref:latest sh /src/tools/glref/xlite-load.sh "$@"
fi

OUTMD=${1:-/src/artifacts/gl/xlite-load.md}
OURS=${GLREF_OURS:-/src/gl/out-host}
R=/src/gl/ref-apps
P=$R/prefix
MD=$R/build/mesa-demos
XD=$MD/src/xdemos
GD=$MD/src/demos
XL=$R/build/xlite-host
G=/src/tools/glref

sh $G/build-apps.sh >/dev/null || { echo "reference apps failed to build" >&2; exit 2; }

# --- the board's X stack, for the host (rebuilt every run: it is seconds) ---
rm -rf "$XL" /tmp/xl-src
mkdir -p "$XL" /tmp/xl-src
cp /src/xlite/*.c /src/xlite/*.h /src/xlite/*.txt /tmp/xl-src/
python3 /src/tools/mkxlitestubs.py /tmp/xl-src/symbols.txt /tmp/xl-src/stubs.c \
    $(ls /tmp/xl-src/xlite*.c) >/dev/null
gcc -D_GNU_SOURCE -O2 -fPIC -shared -w -I/tmp/xl-src -Wl,-Bsymbolic-functions \
    -Wl,-soname,libX11.so.6 -o "$XL/libX11.so.6" /tmp/xl-src/*.c -lpthread -ldl ||
    { echo "xlite failed to build for the host" >&2; exit 2; }
gcc -O2 -fPIC -shared -w -DSTUB_XEXT -Wl,-Bsymbolic-functions \
    -Wl,-soname,libXext.so.6 -o "$XL/libXext.so.6" /src/xstubs/xstubs.c ||
    { echo "xstubs libXext failed to build for the host" >&2; exit 2; }
# xlite's own libXrandr.so.2 and libXxf86vm.so.1 (xlite/build.sh), which the
# board ships in place of the real ones
gcc -O2 -fPIC -shared -w -I/tmp/xl-src -Wl,-Bsymbolic-functions \
    -Wl,-soname,libXrandr.so.2 -o "$XL/libXrandr.so.2" /src/xlite/randr/xrandr.c "$XL/libX11.so.6" ||
    { echo "xlite libXrandr failed to build for the host" >&2; exit 2; }
gcc -O2 -fPIC -shared -w -I/tmp/xl-src -Wl,-Bsymbolic-functions \
    -Wl,-soname,libXxf86vm.so.1 -o "$XL/libXxf86vm.so.1" /src/xlite/vidmode/xf86vm.c "$XL/libX11.so.6" ||
    { echo "xlite libXxf86vm failed to build for the host" >&2; exit 2; }

LIBPATH=$OURS:$XL:$P/lib
{
    echo "# xlite load arm"
    echo
    echo "Each app and the GL libraries it loads, resolved with \`ldd -r\` (every"
    echo "relocation, as musl binds) against our libGL + xlite libX11 + xstubs"
    echo "libXext (+ xlite's libXrandr/libXxf86vm), built for the host from the"
    echo "repo sources; libXi/libXrender are the host's, standing in for the"
    echo "board's. LOADS means no undefined symbol; it says nothing about what"
    echo "the calls then do."
    echo
    echo "| app | result | undefined symbols (library) |"
    echo "|---|---|---|"
} >"$OUTMD"
nload=0 nmiss=0
while IFS='|' read -r name kind cap frames cmd; do
    name=$(echo "$name" | tr -d ' ')
    case $name in ''|'#'*) continue ;; esac
    bin=$(echo "$cmd" | sed 's/^ *//' | cut -d' ' -f1 | sed "s|\$XD|$XD|g; s|\$GD|$GD|g")
    if [ ! -x "$bin" ]; then
        echo "| $name | NOT-BUILT | $bin |" >>"$OUTMD"
        continue
    fi
    # sanity: the app must really bind xlite and our libGL here
    who=$(LD_LIBRARY_PATH=$LIBPATH ldd "$bin" 2>/dev/null | grep -E 'libX11.so.6|libGL.so.1 ' | awk '{print $3}' | tr '\n' ' ')
    case "$who" in *"$XL/libX11.so.6"*) ;; *) echo "| $name | HARNESS-ERROR | libX11 not xlite: $who |" >>"$OUTMD"; continue ;; esac
    und=$(LD_LIBRARY_PATH=$LIBPATH ldd -r "$bin" 2>&1 |
        sed -n 's/^undefined symbol: \([^ \t]*\)[ \t]*(\(.*\))$/\1 (\2)/p' |
        sed "s|$R/||g; s|/usr/lib/[^ ]*/||g; s|$OURS/||g; s|$XL/||g" | sort -u)
    if [ -z "$und" ]; then
        echo "| $name | LOADS | |" >>"$OUTMD"
        nload=$((nload + 1))
    else
        list=$(echo "$und" | tr '\n' ';' | sed 's/;$//; s/;/, /g')
        echo "| $name | MISSING-SYMBOL | $list |" >>"$OUTMD"
        nmiss=$((nmiss + 1))
    fi
done <$G/apps.txt
{
    echo
    echo "$nload load, $nmiss would abort at load on the board."
    echo
    echo "These are gaps in xlite / the stub libraries (not libGL: every gl*/glX*"
    echo "import resolves). Owners: xlite for libX11 names, xstubs for libXext."
} >>"$OUTMD"
echo "xlite load arm: $nload load, $nmiss missing -> $OUTMD"
exit 0
