#!/bin/sh
# Would the board's stock GL stack LOAD against the RV32 xlite? musl binds
# every symbol at load, so one missing import aborts a program before main.
#
#   ./docker/build.sh 'sh /src/xlite/test/rv32-linkcheck.sh'
#   ./docker/build.sh 'XLITE_BASELINE=1 sh /src/xlite/test/rv32-linkcheck.sh'
#
# Links an empty program against each library and program with
# --no-allow-shlib-undefined, so the linker reports every import nothing
# provides. The X libraries are the ones the board ships: /src/images
# (xlite/build.sh's libX11, libXrandr, libXxf86vm; xstubs' libXext; our
# libGL) and the Buildroot target's libXi, libXfixes, libglut and mesa-demos.
# XLITE_BASELINE=1 adds the same check against git HEAD's xlite, built to
# /tmp - a negative control. Read-only on the build volume. Exit 0 iff every
# target resolves against /src/images.
set -u
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
RE=${CC%gcc}readelf
NM=${CC%gcc}nm
SYS=/src/build/buildroot/host/riscv32-buildroot-linux-musl/sysroot
T=/src/build/buildroot/target
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
LIBS="libglut.so.3 libXi.so.6 libXfixes.so.3"
APPS="glxgears glxgears_fbconfig glxheads manywin multictx gears glxinfo"
echo "int main(void){return 0;}" >$W/m.c

# $1 = dir to populate, $2 = libX11, $3 = libXrandr
stack() {
	mkdir -p "$1"
	ln -s "$2" "$1/libX11.so.6"
	ln -s "$3" "$1/libXrandr.so.2"
	ln -s /src/images/libXext.so.6.4.0 "$1/libXext.so.6"
	ln -s /src/images/libXxf86vm.so.1.0.0 "$1/libXxf86vm.so.1"
	ln -s /src/images/libGL.so.1 "$1/libGL.so.1"
	for n in $LIBS; do ln -s $T/usr/lib/$n "$1/$n"; done
}

# $1 = arm, $2 = lib dir; prints one line per target, returns 1 on a gap
check() {
	bad=0
	for t in $LIBS $APPS; do
		case $t in
		*.so.*) objs="$2/$t" ;;
		*)	[ -f $T/usr/bin/$t ] || { echo "$1 $t: not on the target"; continue; }
			objs=$($RE -d $T/usr/bin/$t |
			       sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' |
			       while read n; do
				case $n in libc.so) ;;
				*) if [ -e "$2/$n" ]; then echo "$2/$n"
				   else echo "$T/usr/lib/$n"; fi ;;
				esac
			       done) ;;
		esac
		# the libraries' imports: the linker checks them
		u=$($CC --sysroot=$SYS -o $W/m $W/m.c -L"$2" -Wl,-rpath-link,"$2" \
			-Wl,-rpath-link,$T/usr/lib -Wl,--no-allow-shlib-undefined \
			$objs 2>&1 | grep -o "undefined reference to \`[^']*" |
			sed 's/.*`//' | sort -u | tr '\n' ' ')
		# a program's own imports: against what its libraries export
		case $t in *.so.*) ;; *)
			for o in $objs $SYS/usr/lib/libc.so; do
				$NM -D --defined-only "$o" 2>/dev/null
			done | awk '{print $3}' | sort -u >$W/def
			u="$u$($NM -D --undefined-only $T/usr/bin/$t |
				awk '$1 == "U" {print $2}' | sort -u |
				comm -23 - $W/def | tr '\n' ' ')" ;;
		esac
		if [ -n "$u" ]; then echo "$1 $t: MISSING $u"; bad=1
		else echo "$1 $t: resolves"; fi
	done
	return $bad
}

stack $W/new /src/images/libX11.so.6.4.0 /src/images/libXrandr.so.2.2.0
check new $W/new; rc=$?
if [ -n "${XLITE_BASELINE:-}" ]; then
	mkdir -p $W/h && cd $W/h &&
	git -c safe.directory=/src -C /src archive HEAD xlite | tar -x &&
	python3 /src/tools/mkxlitestubs.py xlite/symbols.txt xlite/stubs.c \
		$(ls xlite/xlite*.c) >/dev/null &&
	$CC -O2 -fPIC -shared -w -I$SYS/usr/include -Ixlite \
		-Wl,-soname,libX11.so.6 -o $W/h/libX11.so.6 xlite/*.c &&
	$CC -O2 -fPIC -shared -w -I$SYS/usr/include -Ixlite \
		-Wl,-soname,libXrandr.so.2 -o $W/h/libXrandr.so.2 \
		xlite/randr/xrandr.c $W/h/libX11.so.6 &&
	stack $W/head $W/h/libX11.so.6 $W/h/libXrandr.so.2 &&
	check HEAD $W/head
fi
echo "rv32-linkcheck: $([ $rc = 0 ] && echo PASS || echo FAIL)"
exit $rc
