#!/bin/sh
# QEMU instruction counts per helper call (run in the s31fp-qemu container,
# cwd = the build dir). cost = insns per iteration minus the empty-call
# iteration, plus the empty function's one `ret`: i.e. instructions executed
# from the helper's entry to its return, tail calls into libgcc included.
B=${1:-./v2test1}
cnt() { qemu-riscv32 -cpu max -plugin /plugins/libinsn.so -d plugin $B bench "$@" 2>&1 | awk '/total insns/{print $3} /^insns:/{print $2}' | tail -1; }
per() {	# impl op class -> per-call instructions over 2 run lengths
	a=$(cnt $1 $2 $3 20000); b=$(cnt $1 $2 $3 60000)
	echo "$a $b" | awk '{printf "%.1f", ($2-$1)/40000}'
}
N0=$(per nop mul gen)
printf "%-6s %-7s %8s %8s %8s\n" op class libgcc old v2
while read op cl; do
	l=$(per lg $op $cl); o=$(per old $op $cl); v=$(per v2 $op $cl)
	echo "$l $o $v $N0" | awk -v op=$op -v cl=$cl '{printf "%-6s %-7s %8.1f %8.1f %8.1f\n", op, cl, $1-$4+1, $2-$4+1, $3-$4+1}'
done <<LIST
mul gen
mul low0
mul pow2
mul zero
add same
add opp
add cancel
sub opp
div gen
ge gen
le gen
eq gen
fltsi int
fixsi int
fixun int
ext int
trunc int
LIST
