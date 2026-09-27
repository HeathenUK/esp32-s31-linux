#!/bin/sh
# The s31fp suites that can run without a board, under QEMU:
#   v2  bit-exactness vs libgcc, bits + fflags, every helper   (D oracle: -cpu max)
#   v3  string routines vs reference, through the preload and through libc
#   v3  clock ABI: brackets, resolutions, errno, fork, signals
# N (thousands of operand sets per helper) defaults to 50; V2-REPORT used 20000.
#   sh tools/cloud/test.sh [N]        # or: make cloud-test
set -u
R=$(cd "$(dirname "$0")/../.." && pwd)
. "$R/tools/cloud/env.sh"
N=${1:-50}
cd "$R" || exit 1
make --no-print-directory s31fp-v3 >build/cloud/s31fp-build.log 2>&1 ||
	{ echo "FAIL  make s31fp-v3 (build/cloud/s31fp-build.log)"; tail -5 build/cloud/s31fp-build.log; exit 1; }
echo "PASS  make s31fp-v3: $(md5sum build/s31fp-v3/libs31fp.so | cut -c1-32) libs31fp.so"
fail=0
for op in mul add sub div ge le eq unord fltsi fltun fixsi fixun ext trunc; do
	out=$(S31_QEMU_CPU=max s31-qemu build/s31fp-v2/v2check1 check $op "$N" 2>&1); rc=$?
	res=$(echo "$out" | grep '^RESULT')
	if [ $rc = 0 ] && echo "$res" | grep -q ' 0 v2 mismatches'; then
		echo "PASS  v2 $op: $(echo "$res" | cut -d' ' -f3-4) sets, 0 mismatches"
	else
		echo "FAIL  v2 $op rc=$rc: $res"; fail=1
	fi
done
V=build/s31fp-v3
for arm in preload libc; do
	if [ $arm = preload ]; then
		out=$(timeout 600 s31-qemu -E LD_PRELOAD="$R/$V/libs31fp.so" $V/strtest 2>&1); rc=$?
	else
		out=$(timeout 600 s31-qemu $V/strtest libc 2>&1); rc=$?
	fi
	res=$(echo "$out" | grep '^RESULT' | tail -1)
	[ $rc = 0 ] && echo "$res" | grep -q ' 0 mismatches' && echo "PASS  v3 strings ($arm): $res" ||
		{ echo "FAIL  v3 strings ($arm) rc=$rc: $res"; fail=1; }
done
out=$(timeout 300 s31-qemu -E LD_PRELOAD="$R/$V/libs31fp.so" $V/clktest abi 2>&1); rc=$?
res=$(echo "$out" | grep '^RESULT' | tail -1)
[ $rc = 0 ] && echo "$res" | grep -q 'failures 0' && echo "PASS  v3 clock: $res" ||
	{ echo "FAIL  v3 clock rc=$rc: $res"; fail=1; }
[ $fail = 0 ] && echo "CLOUD TESTS OK" || echo "CLOUD TESTS FAILED"
exit $fail
