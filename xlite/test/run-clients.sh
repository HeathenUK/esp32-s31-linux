#!/bin/sh
# Stock X clients and stock GL demos over xlite on the host rig: a
# regression check for xlite changes.
#
#   xlite/test/run-clients.sh       # new xlite vs git HEAD's xlite
#
# Each client runs under the xlite built from /src/xlite and under HEAD's,
# on its own Xvfb (depth 16), in parallel. The core-X clients (xlogo, xeyes,
# xcalc) are deterministic, so their root-window captures must be
# byte-identical between the two builds, and the client must meet the same
# fate (alive / exit code) - or HEAD's must have drawn nothing at all (it
# hangs: xcalc on the host rig waits for ever on a QueryFont answered with
# BadFont under HEAD). Any other difference is a regression.
# The GL demos (mesa-demos glxgears, GLUT gears) are checked for loading,
# mapping and animating (two captures 0.5 s apart differ) - under HEAD they
# are expected to die at load, which is what the new symbols fix. libGL is
# ours from $GLREF_OURS (default /src/gl/out-host); libXext is xstubs', and
# libXrandr/libXxf86vm are xlite's own, as on the board.
set -u
if [ ! -x /usr/bin/Xvfb ] || [ ! -d /src/xlite ]; then
	here=$(cd "$(dirname "$0")/../.." && pwd)
	exec docker run --rm -v "$here":/src -w /src -e GLREF_OURS \
		s31-glref:latest sh /src/xlite/test/run-clients.sh "$@"
fi
OURS=${GLREF_OURS:-/src/gl/out-host}
T=/tmp/xlite-clients
R=/src/gl/ref-apps
XD=$R/build/mesa-demos/src/xdemos
GD=$R/build/mesa-demos/src/demos
rm -rf $T; mkdir -p $T
# Several Xvfb starting at once race to create this; the loser serves only
# the abstract socket, which xcb finds and xlite (filesystem path) does not.
mkdir -p /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix

# the board's X stack for the host, from xlite sources $1 into $2
stack() {
	mkdir -p "$2/src"
	cp "$1"/*.c "$1"/*.h "$1"/*.txt "$2/src/"
	python3 /src/tools/mkxlitestubs.py "$2/src/symbols.txt" "$2/src/stubs.c" \
		$(ls "$2"/src/xlite*.c) >/dev/null
	( cd "$2/src" && ls *.c | xargs -P"$(nproc)" -I{} \
		gcc -D_GNU_SOURCE -O2 -fPIC -w -I"$2/src" -c {} -o {}.o ) &&
	gcc -shared -Wl,-Bsymbolic-functions -Wl,-soname,libX11.so.6 \
		-o "$2/libX11.so.6" "$2"/src/*.o -lpthread -ldl &&
	gcc -O2 -fPIC -shared -w -DSTUB_XEXT -Wl,-soname,libXext.so.6 \
		-o "$2/libXext.so.6" /src/xstubs/xstubs.c &&
	gcc -O2 -fPIC -shared -w -I"$2/src" -Wl,-soname,libXrandr.so.2 \
		-o "$2/libXrandr.so.2" "$1/randr/xrandr.c" "$2/libX11.so.6" &&
	gcc -O2 -fPIC -shared -w -I"$2/src" -Wl,-soname,libXxf86vm.so.1 \
		-o "$2/libXxf86vm.so.1" "$1/vidmode/xf86vm.c" "$2/libX11.so.6"
}
mkdir -p $T/head
git -c safe.directory=/src -C /src archive HEAD xlite | tar -x -C $T/head
stack /src/xlite $T/new & stack $T/head/xlite $T/base & wait
[ -f $T/new/libXxf86vm.so.1 ] && [ -f $T/base/libXxf86vm.so.1 ] ||
	{ echo "a build failed" >&2; exit 2; }

# $1 tag, $2 display, $3 libdir, $4... command
run1() {
	tag=$1 dn=$2 lib=$3; shift 3
	Xvfb :$dn -screen 0 800x480x16 -nolisten tcp -noreset >/dev/null 2>&1 &
	xp=$!
	for i in $(seq 150); do
		[ -S /tmp/.X11-unix/X$dn ] &&
			xdpyinfo -display :$dn >/dev/null 2>&1 && break
		sleep 0.1
	done
	[ -S /tmp/.X11-unix/X$dn ] && xdpyinfo -display :$dn >/dev/null 2>&1 ||
		echo "HARNESS ERROR: Xvfb :$dn not answering after 15 s" >&2
	xwd -root -silent -display :$dn >$T/$tag.empty.xwd 2>/dev/null
	DISPLAY=:$dn XLITE_TRACE=1 LD_LIBRARY_PATH=$OURS:$lib:$R/prefix/lib "$@" \
		>$T/$tag.log 2>&1 &
	cp=$!
	sleep 2.5
	xwd -root -silent -display :$dn >$T/$tag.a.xwd 2>/dev/null
	sleep 0.5
	xwd -root -silent -display :$dn >$T/$tag.b.xwd 2>/dev/null
	if kill -0 $cp 2>/dev/null; then echo alive >$T/$tag.state
	else wait $cp; echo "exited $?" >$T/$tag.state; fi
	kill $cp 2>/dev/null; wait $cp 2>/dev/null
	kill $xp 2>/dev/null; wait $xp 2>/dev/null
}

dn=60
for c in "xlogo" "xeyes" "xcalc" "glxgears:$XD/glxgears -geometry 320x240+0+0" \
	 "gears:$GD/gears -geometry 320x240+0+0"; do
	name=${c%%:*}; cmd=${c#*:}; [ "$name" = "$c" ] && cmd=$c
	for b in new base; do
		run1 $name.$b $dn $T/$b $cmd &
		dn=$((dn + 1))
	done
done
wait

md5() { md5sum "$1" 2>/dev/null | cut -c1-32; }
rc=0
for name in xlogo xeyes xcalc; do
	a=$(md5 $T/$name.new.a.xwd); b=$(md5 $T/$name.base.a.xwd)
	st="$(cat $T/$name.new.state) / HEAD $(cat $T/$name.base.state)"
	e=$(md5 $T/$name.base.empty.xwd)
	if [ -n "$a" ] && [ "$a" = "$b" ] &&
	   [ "$(cat $T/$name.new.state)" = "$(cat $T/$name.base.state)" ]; then
		echo "PASS $name: capture and fate identical to HEAD's xlite ($st)"
	elif [ -n "$a" ] && [ "$b" = "$e" ] && [ "$a" != "$e" ] &&
	     grep -q alive $T/$name.new.state; then
		echo "PASS $name: draws; HEAD's xlite drew nothing ($st)"
	else
		echo "FAIL $name: new $a, HEAD $b ($st)"; rc=1
		sed 's/^/    new log: /' $T/$name.new.log | head -8
	fi
done
for name in glxgears gears; do
	a=$(md5 $T/$name.new.a.xwd); b=$(md5 $T/$name.new.b.xwd)
	st="$(cat $T/$name.new.state); HEAD's xlite: $(cat $T/$name.base.state)"
	if grep -q alive $T/$name.new.state && [ -n "$a" ] && [ "$a" != "$b" ]; then
		echo "PASS $name: loads, maps and animates over xlite ($st)"
	else
		echo "FAIL $name: $st, captures $a $b"; rc=1
	fi
	grep -h "symbol lookup\|undefined symbol\|unimplemented\|xlite: " $T/$name.base.log |
		sort -u | head -3 | sed 's/^/    HEAD: /'
done
for f in $T/*.new.log; do
	grep -h "unimplemented\|xlite: X error\|lookup error" $f | sort -u | head -5 |
		sed "s|^|    $(basename $f .new.log): |"
done
echo "clients: $([ $rc = 0 ] && echo PASS || echo FAIL)"
exit $rc
