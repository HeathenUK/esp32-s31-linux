#!/bin/sh
# Prove the cloud environment works, including that it fails where it must.
#   sh tools/cloud/check.sh        # or: make cloud-check
set -u
R=$(cd "$(dirname "$0")/../.." && pwd)
. "$R/tools/cloud/env.sh"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
fail=0
ok() { printf 'PASS  %s\n' "$*"; }
no() { printf 'FAIL  %s\n' "$*"; fail=1; }

cat >"$W/t.c" <<'EOF'
#include <fenv.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
	volatile float f = 1.5f * argc;           /* hardware F */
	volatile double d = 1.0 / 3.0 * argc;     /* libgcc soft-double */
	feclearexcept(FE_ALL_EXCEPT);
	volatile double e = d * 3.0;
	char buf[64];
	memcpy(buf, "abcdefghijklmnopqrstuvwxyz0123456789", 37);
	printf("f=%g d=%.17g e=%.17g nx=%d ld=%Lg %s\n", f, d, e,
	       fetestexcept(FE_INEXACT) != 0, (long double)d, buf + 20);
	return 0;
}
EOF
want='f=1.5 d=0.33333333333333331 e=1 nx=1 ld=0.333333 uvwxyz0123456789'

# ABI: soft-float calling convention, single-precision FPU, no D
"$S31_CC" -mabi=ilp32 -dM -E - </dev/null >"$W/defs"
grep -q '__riscv_float_abi_soft 1' "$W/defs" && grep -q '__riscv_flen 32' "$W/defs" &&
	! grep -q '__riscv_d ' "$W/defs" && ok "ABI ilp32, FLEN 32, no D" || no "ABI macros"

for mode in -static ""; do
	s31-cc $mode -O2 -o "$W/t" "$W/t.c" || { no "s31-cc $mode build"; continue; }
	got=$(s31-qemu "$W/t" 2>&1)
	[ "$got" = "$want" ] && ok "s31-cc ${mode:-dynamic}: $got" || no "s31-cc ${mode:--dynamic}: got '$got'"
done

# no esp.* in what s31-cc produced
if riscv32-esp-linux-musl-objdump -d "$W/t" | grep -q 'esp\.'; then no "s31-cc output contains esp.* instructions"; else ok "s31-cc output has no esp.* instructions"; fi

# negative control 1: the SHIPPING toolchain's default output must SIGILL
"$S31_CC" -O2 -static -o "$W/ship" "$W/t.c"
s31-qemu "$W/ship" >/dev/null 2>&1; rc=$?
[ $rc = 132 ] && ok "control: shipping-toolchain binary SIGILLs under QEMU (xesp), as expected" ||
	no "control: shipping-toolchain binary exited $rc (expected 132 SIGILL)"

# negative control 2: the board CPU model has no D
printf 'double g(double a,double b){return a*b;}\nint main(int c,char**v){volatile double x=g(c,1.5);return x>0?0:1;}\n' >"$W/d.c"
s31-cc -march=rv32imafdc_zicsr -mabi=ilp32 -O2 -static -o "$W/d" "$W/d.c"
s31-qemu "$W/d" >/dev/null 2>&1; rc=$?
[ $rc = 132 ] && ok "control: D instruction SIGILLs on the board CPU model" ||
	no "control: D instruction exited $rc under the board CPU model (expected 132)"
S31_QEMU_CPU=max s31-qemu "$W/d" && ok "S31_QEMU_CPU=max runs the D oracle" || no "S31_QEMU_CPU=max"

# instruction counting plugin (icount.sh / oplcount.sh use /plugins/libinsn.so)
n=$(s31-qemu -plugin "$R/build/cloud/qemu/plugins/libinsn.so" -d plugin "$W/t" 2>&1 | awk '/total insns/{print $3}')
[ -n "$n" ] && [ "$n" -gt 1000 ] && ok "libinsn plugin: $n instructions" || no "libinsn plugin output '$n'"

# the Docker-contract path the s31fp build scripts use
[ -x /src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc ] &&
	ok "/src resolves to this checkout" || no "/src toolchain path"

[ $fail = 0 ] && echo "CLOUD ENV OK" || echo "CLOUD ENV BROKEN"
exit $fail
