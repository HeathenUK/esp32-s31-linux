#!/bin/sh
# Cross-compile xlite - our libX11 replacement - for the board.
#
# The output is named libX11.so.6.4.0 with soname libX11.so.6 on purpose:
# nothing above it is rebuilt. libXt, libXaw and the clients reference these
# symbols by name, so putting this first on the library path is the whole
# integration.
set -e
SYSROOT=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
# Bind internal calls at link time - see xstubs/build.sh for the reasoning.
# These libraries are built outside Buildroot, so BR2_TARGET_LDFLAGS misses them.
LDHARD="-Wl,-Bsymbolic-functions"
OUT=/src/images/libX11.so.6.4.0
rm -f "$OUT"	# a failed build must leave nothing to ship

echo "--- generating stubs ---"
IMPLS=$(ls /src/xlite/xlite*.c)
python3 /src/tools/mkxlitestubs.py /src/xlite/symbols.txt /src/xlite/stubs.c $IMPLS

echo "--- compiling ---"
INSTR=""
[ -n "$XLITE_INSTRUMENT" ] && INSTR="-DXLITE_INSTRUMENT -finstrument-functions"
CFLAGS="-O2 -fno-omit-frame-pointer -fPIC -Wall -Wno-unused-parameter $INSTR"
# One object per source, compiled in parallel, then linked in the glob's
# order: the same compiler and link as the one-command build this replaced,
# and the stripped libraries are byte-identical to its output (checked).
OBJ=$(mktemp -d)
trap 'rm -rf "$OBJ"' EXIT
export CC CFLAGS SYSROOT OBJ
ls /src/xlite/*.c | xargs -P"$(nproc)" -I{} sh -c \
	'$CC $CFLAGS -isystem "$SYSROOT/usr/include" -I/src/xlite -c "$1" -o "$OBJ/$(basename "$1").o"' _ {}
OBJS=""
for f in /src/xlite/*.c; do OBJS="$OBJS $OBJ/$(basename "$f").o"; done
$CC -O2 -fPIC -shared $INSTR $LDHARD -Wl,-soname,libX11.so.6 -o "$OUT" $OBJS
${CC%gcc}strip "$OUT"
ls -l "$OUT"

# The two extension clients below link against the libX11 just built and not
# against each other, so they build side by side.
build_rr() {
# libXrandr.so.2: the RANDR client SDL2 dlopens by that name. Built from its
# own directory so the glob above cannot pull it into libX11, and linked
# against the libX11 just built for xlite_req()/xlite_reply(). See
# xlite/randr/xrandr.c for why this exists (SDL 2.32 has no XVidMode).
RR=/src/images/libXrandr.so.2.2.0
rm -f "$RR"
$CC -O2 -fPIC -shared -Wall -Wno-unused-parameter \
	-isystem "$SYSROOT/usr/include" -I/src/xlite \
	$LDHARD \
	-Wl,-soname,libXrandr.so.2 \
	-o "$RR" /src/xlite/randr/xrandr.c "$OUT" &&
${CC%gcc}strip "$RR" &&
ls -l "$RR"
}

build_vm() {
# libXxf86vm.so.1: the XFree86-VidMode client a stock X11 application links
# (TyrQuake 0.71's default Linux target). Same shape as libXrandr above; see
# xlite/vidmode/xf86vm.c for why the real library cannot be used.
VM=/src/images/libXxf86vm.so.1.0.0
rm -f "$VM"
$CC -O2 -fPIC -shared -Wall -Wno-unused-parameter \
	-isystem "$SYSROOT/usr/include" -I/src/xlite \
	$LDHARD \
	-Wl,-soname,libXxf86vm.so.1 \
	-o "$VM" /src/xlite/vidmode/xf86vm.c "$OUT" &&
${CC%gcc}strip "$VM" &&
ls -l "$VM"
}

build_rr > "$OBJ/rr.log" 2>&1 & rr=$!
build_vm > "$OBJ/vm.log" 2>&1 & vm=$!
rc=0
wait $rr || rc=1
cat "$OBJ/rr.log"
wait $vm || rc=1
cat "$OBJ/vm.log"
exit $rc
