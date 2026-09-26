#!/bin/sh
# build.sh - cross-compile libGL.so.1 (TinyGL core + GL ABI + gl/glx) for the
# board, and the headless test. Run on the Mac (it re-runs itself inside the
# build container through docker/build.sh) or inside the container:
#     gl/build.sh
# Output: /src/images/libGL.so.1 (stripped, soname libGL.so.1)
#         /src/gl/out-rv32/libGL.so.1.unstripped (same, with symbols)
#         /src/gl/out-rv32/headless_gears (static; runs from anywhere)
#         /src/gl/out-rv32/core_test (static)
# Both tests also run on the Mac under qemu-user (image s31-glref-qemu):
#   docker run --rm -v $REPO:/src s31-glref-qemu qemu-riscv32 \
#     -cpu rv32,zba=true,zbb=true,zbc=true,zbs=true /src/gl/out-rv32/core_test.qemu
# (the *.qemu builds link gl/tests/qemu_libc.c: qemu has no ESP PIE)
# Builds nothing else and touches nothing under /src/build (read-only use of
# the Buildroot sysroot's X11 headers and libraries).
#
# Buildroot mode (buildroot-external/package/s31-libgl, docs/gl-packaging.md
# section 2): when S31GL_OUT is set, build.sh builds the library alone from
# the tree it lives in, with
#   S31GL_CC        the compiler (TARGET_CC; its sysroot supplies X11)
#   S31GL_CFLAGS    base flags, put FIRST so our -O2/-Os/-fPIC win
#   S31GL_LDFLAGS   base link flags (TARGET_LDFLAGS)
#   S31GL_OUT       writes $S31GL_OUT/libGL.so.1.2.0 (SONAME libGL.so.1) and
#                   its objects under $S31GL_OUT/obj, nothing else
#   S31GL_STRIP=0   leaves it unstripped
# and without -Wl,-z,defs (the contract: it links the staging libX11/libXext
# and resolves against xlite/xstubs on the board).
set -e
if [ -n "$S31GL_OUT" ]; then
	GL=$(cd "$(dirname "$0")" && pwd)
	CC=${S31GL_CC:?S31GL_CC must be set with S31GL_OUT}
	NM=${S31GL_NM:-${CC%gcc}nm}
	STRIP=${S31GL_STRIPPROG:-${CC%gcc}strip}
	ARCHFLAGS="$S31GL_CFLAGS"
	LDFLAGS="$S31GL_LDFLAGS"
	ZDEFS=""
	XINC=""
	XLIBS="-lXext -lX11"
	OBJ=$S31GL_OUT/obj
	OUT=$S31GL_OUT/libGL.so.1.2.0
	mkdir -p "$S31GL_OUT"
	unset S31GL_NO_GLX
	export GL CC NM ARCHFLAGS LDFLAGS ZDEFS XINC XLIBS OBJ OUT
	sh "$GL/api/build-lib.sh"
	[ "${S31GL_STRIP:-1}" = 0 ] || "$STRIP" "$OUT"
	ls -l "$OUT"
	exit 0
fi
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
if [ ! -x "$CC" ]; then
	REPO=$(cd "$(dirname "$0")/.." && pwd)
	exec "$REPO/docker/build.sh" "S31GL_NO_GLX=$S31GL_NO_GLX S31GL_TGL_FRAMEPTR=$S31GL_TGL_FRAMEPTR sh /src/gl/build.sh"
fi
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
GL=/src/gl
NM=${CC%gcc}nm
STRIP=${CC%gcc}strip
ARCHFLAGS="-march=rv32imafc_zicsr_zifencei_zba_zbb_zbc_zbs -mabi=ilp32"
XINC="-isystem $SYSROOT/usr/include"
XLIBS="-L$SYSROOT/usr/lib -Wl,-rpath-link,$SYSROOT/usr/lib -lXext -lX11"
OBJ=/tmp/s31gl-rv32
OUT=/src/images/libGL.so.1
TEST=$GL/out-rv32/headless_gears
mkdir -p $GL/out-rv32
rm -f $TEST $GL/out-rv32/core_test $GL/out-rv32/*.qemu $GL/out-rv32/libGL.so.1.unstripped
[ -n "$S31GL_NO_GLX" ] || unset S31GL_NO_GLX
export GL CC NM ARCHFLAGS XINC XLIBS OBJ OUT
sh $GL/api/build-lib.sh
cp $OUT $GL/out-rv32/libGL.so.1.unstripped   # symbols for profiles / addr2line
$STRIP $OUT
ls -l $OUT

echo "--- headless_gears, core_test (static: the core objects of libGL.so.1, no X)"
echo "    and their qemu-user variants (scalar mem*/str*: qemu has no ESP PIE), in parallel"
# every step a background job, each waited on, so a failure fails the build
waitall() { for p in $pids; do wait $p; done; pids=""; }
pids=""
$CC -O2 -Wall $ARCHFLAGS -I$GL/include -I$GL/api -c $GL/tests/headless_gears.c \
	-o $OBJ/headless_gears.o & pids="$pids $!"
$CC -O2 -Wall $ARCHFLAGS -I$GL/include -I$GL/api -c $GL/tests/core_test.c \
	-o $OBJ/core_test.o & pids="$pids $!"
$CC -O2 $ARCHFLAGS -c $GL/tests/qemu_libc.c -o $OBJ/qemu_libc.o & pids="$pids $!"
$CC -O2 -Wall $ARCHFLAGS -I$GL/include -I$GL/api -c $GL/tests/raster_gate.c \
	-o $OBJ/raster_gate.o & pids="$pids $!"
$CC -O2 $ARCHFLAGS -I$GL/tinygl/source -c $GL/tests/d2f_test.c -o $OBJ/d2f_test.o & pids="$pids $!"
waitall
( $CC -static $ARCHFLAGS -o $TEST $OBJ/headless_gears.o $(cat $OBJ/core.list) -lm && $STRIP $TEST ) &
pids="$pids $!"
( $CC -static $ARCHFLAGS -o $GL/out-rv32/core_test $OBJ/core_test.o $(cat $OBJ/core.list) -lm &&
	$STRIP $GL/out-rv32/core_test ) & pids="$pids $!"
for t in headless_gears core_test d2f_test raster_gate; do
	$CC -static $ARCHFLAGS -o $GL/out-rv32/$t.qemu $OBJ/$t.o $OBJ/qemu_libc.o \
		$(cat $OBJ/core.list) -lm & pids="$pids $!"
done
waitall
ls -l $TEST $GL/out-rv32/core_test $GL/out-rv32/*.qemu
