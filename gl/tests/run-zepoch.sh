#!/bin/bash
# run-zepoch.sh [OUTDIR] - the depth epochs (phase 3a G03) and the dirty
# boxes of the clears are invisible: gl/tests/zepoch_test with both off
# (S31GL_ZTRICK=0 S31GL_DIRTYBOX=0: every clear whole, the reference), each
# alone, and both on (the default), and the x-extent boxes
# (S31GL_DIRTYBOX=2, alone and with the epochs), on the host rig (gl/out-host,
# gl/host-build.sh) and on RV32 under qemu-user (gl/out-rv32/zepoch_test.qemu,
# gl/build.sh), at 96x72 and 320x240. Every per-frame line (colour, depth as
# GL_FLOAT and GL_UNSIGNED_SHORT) must match the reference; the last line
# says how many full clears left the depth memory untouched (0 with the
# epochs off). Nothing touches the board. s31, MIT.
set -u
R=$(cd "$(dirname "$0")/../.." && pwd)
O=${1:-$R/artifacts/gl/phase3a/zepoch}
mkdir -p "$O"
rc=0
V="ref:0:0 z:1:0 d:0:1 zd:1:1 x:0:2 zx:1:2"
for sz in "96 72" "320 240"; do
	set -- $sz
	for v in $V; do
		n=${v%%:*}; zt=$(echo $v | cut -d: -f2); db=$(echo $v | cut -d: -f3)
		docker run --rm -v "$R":/src -e S31GL_ZTRICK=$zt -e S31GL_DIRTYBOX=$db s31-glref:latest \
			/src/gl/out-host/zepoch_test $1 $2 > "$O/host-$1-$n.txt" 2>&1 &
		docker run --rm -v "$R":/src -e S31GL_ZTRICK=$zt -e S31GL_DIRTYBOX=$db s31-glref-qemu \
			qemu-riscv32 -cpu rv32,zba=true,zbb=true,zbc=true,zbs=true \
			/src/gl/out-rv32/zepoch_test.qemu $1 $2 > "$O/rv32-$1-$n.txt" 2>&1 &
	done
done
wait
for sz in 96 320; do
	for a in host rv32; do
		ref="$O/$a-$sz-ref.txt"
		for n in z d zd x zx; do
			f="$O/$a-$sz-$n.txt"
			n0=$(grep -c " f[0-9]" "$ref"); n1=$(grep -c " f[0-9]" "$f")
			if [ "$n0" -gt 0 ] && diff <(grep " f[0-9]" "$ref") <(grep " f[0-9]" "$f") > "$O/$a-$sz-$n.diff"; then
				echo "zepoch $a $sz $n: IDENTICAL to ref, $n1 frames; $(tail -1 "$f" | sed 's/zepoch: //')"
			else
				echo "zepoch $a $sz $n: DIFFER ($n0 / $n1 frames; $(grep -c '^[<>]' "$O/$a-$sz-$n.diff") lines, $O/$a-$sz-$n.diff)"; rc=1
			fi
		done
	done
done
# host against RV32 (informational: the pixels scenario's raster positions
# differ between the two builds with everything off too)
for n in ref zd; do
	cmp -s <(grep " f[0-9]" "$O/host-320-$n.txt") <(grep " f[0-9]" "$O/rv32-320-$n.txt") &&
		echo "zepoch host = rv32 (320, $n)" ||
		echo "zepoch host != rv32 (320, $n): $(diff <(grep ' f[0-9]' "$O/host-320-$n.txt") <(grep ' f[0-9]' "$O/rv32-320-$n.txt") | grep '^<' | awk '{print $2}' | sort | uniq -c | tr '\n' ' ') - informational"
done
exit $rc
