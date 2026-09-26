#!/bin/sh
# Host test of xlite's serial and reply-error handling (seqtest.c).
#
#   xlite/test/run-host.sh            # stock Xlib arm + xlite arm
#   XLITE_BASELINE=1 xlite/test/run-host.sh   # + HEAD's xlite (negative control)
#
# Runs in the s31-glref rig (re-execs itself there), against Xvfb depth 16.
# The stock-Xlib arm validates the expectations; the xlite arm is built for
# the host from /src/xlite exactly as tools/glref/xlite-load.sh builds it.
# The baseline arm builds xlite from `git HEAD` and is EXPECTED to fail - it
# is the proof that the test sees the bugs it is about. Arms run in parallel,
# one Xvfb each. Exit 0 iff every required arm passes.
set -u
if [ ! -x /usr/bin/Xvfb ] || [ ! -d /src/xlite ]; then
	here=$(cd "$(dirname "$0")/../.." && pwd)
	exec docker run --rm -v "$here":/src -w /src -e XLITE_BASELINE \
		s31-glref:latest sh /src/xlite/test/run-host.sh "$@"
fi

T=/tmp/xlite-seqtest
rm -rf $T; mkdir -p $T
# Several Xvfb starting at once race to create this; the loser serves only
# the abstract socket, which xcb finds and xlite (filesystem path) does not.
mkdir -p /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix

# xlite for the host from a source dir: $1 = sources, $2 = output dir
build_xlite() {
	mkdir -p "$2/src"
	cp "$1"/*.c "$1"/*.h "$1"/*.txt "$2/src/"
	python3 /src/tools/mkxlitestubs.py "$2/src/symbols.txt" "$2/src/stubs.c" \
		$(ls "$2"/src/xlite*.c) >/dev/null
	( cd "$2/src" && ls *.c | xargs -P"$(nproc)" -I{} \
		gcc -D_GNU_SOURCE -O2 -fPIC -w -I"$2/src" -c {} -o {}.o ) &&
	gcc -shared -Wl,-Bsymbolic-functions -Wl,-soname,libX11.so.6 \
		-o "$2/libX11.so.6" "$2"/src/*.o -lpthread -ldl
}

gcc -O2 -Wall -o $T/seqtest /src/xlite/test/seqtest.c -lX11 || exit 2
build_xlite /src/xlite $T/new || { echo "xlite failed to build" >&2; exit 2; }
if [ -n "${XLITE_BASELINE:-}" ]; then
	mkdir -p $T/head
	git -c safe.directory=/src -C /src archive HEAD xlite | tar -x -C $T/head &&
	build_xlite $T/head/xlite $T/base || { echo "baseline failed to build" >&2; exit 2; }
fi

# $1 = arm name, $2 = display, $3 = LD_LIBRARY_PATH ("" = stock Xlib)
arm() {
	Xvfb :$2 -screen 0 800x480x16 -nolisten tcp -noreset >$T/xvfb$2.log 2>&1 &
	xp=$!
	# ready = answering, not merely bound (xdpyinfo uses stock Xlib)
	for i in $(seq 150); do
		[ -S /tmp/.X11-unix/X$2 ] &&
			xdpyinfo -display :$2 >/dev/null 2>&1 && break
		sleep 0.1
	done
	[ -S /tmp/.X11-unix/X$2 ] && xdpyinfo -display :$2 >/dev/null 2>&1 ||
		echo "HARNESS ERROR: Xvfb :$2 not answering after 15 s" >&2
	DISPLAY=:$2 LD_LIBRARY_PATH=$3 timeout 90 $T/seqtest >$T/$1.out 2>&1
	echo "exit $?" >>$T/$1.out
	kill $xp 2>/dev/null
	wait $xp 2>/dev/null
}

arm stock 91 "" &
arm xlite 92 $T/new &
[ -n "${XLITE_BASELINE:-}" ] && arm baseline 93 $T/base &
wait

rc=0
for a in stock xlite baseline; do
	[ -f $T/$a.out ] || continue
	echo "=== $a"
	cat $T/$a.out
	case $a in baseline) continue ;; esac
	grep -q '^SEQTEST ALL PASS' $T/$a.out || rc=1
done
# the xlite arm must really have run on xlite
LD_LIBRARY_PATH=$T/new ldd $T/seqtest | grep -q "$T/new/libX11.so.6" ||
	{ echo "HARNESS ERROR: xlite arm did not bind xlite"; rc=1; }
echo "seqtest: $([ $rc = 0 ] && echo PASS || echo FAIL)"
exit $rc
