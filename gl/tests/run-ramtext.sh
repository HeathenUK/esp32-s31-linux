#!/bin/sh
# run-ramtext.sh OUTDIR [SUITEBASE] - lever L1 (tinygl/source/s31_ramtext.c):
# the hot range copied to RAM gives the same output as the XIP copy, and
# really runs. From the Mac, host only (nothing touches the board). Writes
# OUTDIR/ (a path under the repo):
#   rv32-build.log  gl/build.sh (library to gl/out-rv32/, NOT /src/images):
#                   ramtext.py's line for libGL.so.1 and every static test
#   rv32.txt        qemu-user RV32, each static test with S31GL_RAMTEXT=0 and
#                   =1: headless_gears' frame md5, and every other test's
#                   whole output compared (the per-context log lines aside)
#   rv32-objdump.txt tests/ramtext_objdump.py: the table against objdump's
#                   disassembly of the range (library, headless_gears.qemu)
#   rv32-trace.txt  tests/ramtext_trace.py: blocks run in the XIP range and
#                   in the RAM copy, and what still enters the XIP range
#   rv32-dyn.txt    the real thing: libGL.so.1 (core, no GLX) loaded by
#                   musl's ld.so under qemu-user, a dynamic headless_gears,
#                   on and off, md5s and the blocks run in the copy
#   host.txt        the host rig (AArch64) libGL from gl/host-build.sh (run
#                   it first; run-3a.sh does): the same tests, on and off
#   suite-rt1/      tools/glref/suite.sh with S31GL_RAMTEXT=1, and
#   suite.txt       its frames against SUITEBASE (a suite run with the
#                   default, e.g. artifacts/gl/phase3a/p4-l1/suite): every
#                   PNG compared byte for byte, and suitecmp.py
# s31, MIT.
set -u
A=${1:?outdir}
R=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$A"; A=$(cd "$A" && pwd)
AC=/src/${A#$R/}
cd "$R"
QEMU='qemu-riscv32 -cpu rv32,zba=true,zbb=true,zbc=true,zbs=true'
# the per-context log line carries the pid; ramtext's own line is the arm
NOLOG='s/libGL: context [0-9]* created (pid [0-9]*, comm [^)]*)//; /libGL: ramtext/d'

S31GL_IMAGE=/src/gl/out-rv32/libGL.so.1 gl/build.sh > "$A/rv32-build.log" 2>&1 ||
	{ tail -20 "$A/rv32-build.log"; exit 1; }
grep "^ramtext:" "$A/rv32-build.log"
# the table against the disassembly (tests/ramtext_objdump.py), for the
# library and a static test
./docker/build.sh 'OD=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-objdump
for f in libGL.so.1.unstripped headless_gears.qemu; do
	python3 /src/gl/tests/ramtext_objdump.py /src/gl/out-rv32/$f $OD | sed "s#^#$f: #"
done' > "$A/rv32-objdump.txt" 2>&1
cat "$A/rv32-objdump.txt"

docker run --rm -v "$R":/src -w /tmp s31-glref-qemu sh -c "
D=/src/gl/out-rv32
for v in 0 1; do
	S31GL_RAMTEXT=\$v $QEMU \$D/headless_gears.qemu 320 240 100 /tmp/hg\$v.ppm > /tmp/hg\$v.txt 2>&1
	echo \"headless_gears 320x240x100 S31GL_RAMTEXT=\$v: rc \$? \$(md5sum < /tmp/hg\$v.ppm | cut -c1-32) \$(grep ramtext /tmp/hg\$v.txt)\"
	for t in core_test raster_gate d2f_test filt_test zepoch_test; do
		S31GL_RAMTEXT=\$v $QEMU \$D/\$t.qemu > /tmp/\$t.\$v.raw 2>&1; echo \$? >> /tmp/\$t.\$v.raw
		sed '$NOLOG' /tmp/\$t.\$v.raw > /tmp/\$t.\$v
	done
done
cmp -s /tmp/hg0.ppm /tmp/hg1.ppm && echo 'headless_gears: frames identical on/off' || echo 'headless_gears: frames DIFFER'
for t in core_test raster_gate d2f_test filt_test zepoch_test; do
	cmp -s /tmp/\$t.0 /tmp/\$t.1 && echo \"\$t: output identical on/off (\$(tail -2 /tmp/\$t.0 | head -1))\" ||
		{ echo \"\$t: output DIFFERS\"; diff /tmp/\$t.0 /tmp/\$t.1 | head -5; }
	[ \$t = d2f_test ] && continue      # conversions only: it makes no context
	grep -q 'libGL: ramtext on' /tmp/\$t.1.raw || echo \"\$t: the RAM copy did NOT engage\"
done
" > "$A/rv32.txt" 2>&1
cat "$A/rv32.txt"

docker run --rm -v "$R":/src -w /tmp s31-glref-qemu \
	python3 /src/gl/tests/ramtext_trace.py /src/gl/out-rv32/headless_gears.qemu 64 48 3 /tmp/h.ppm \
	> "$A/rv32-trace.txt" 2>&1
cat "$A/rv32-trace.txt"

# the shared library through musl's loader: a core-only libGL (the GLX
# layer needs libX11, which qemu cannot run here) and headless_gears linked
# against it, with the scalar mem*/str* exported so libGL binds to them
./docker/build.sh 'set -e
CC=/src/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
A="-march=rv32imafc_zicsr_zifencei_zba_zbb_zbc_zbs -mabi=ilp32"
G=/src/gl; O=$G/out-rv32/dyn; mkdir -p $O
GL=$G CC=$CC NM=${CC%gcc}nm ARCHFLAGS="$A" XINC= XLIBS= OBJ=/tmp/rtdyn OUT=$O/libGL.so.1 \
	S31GL_NO_GLX=1 sh $G/api/build-lib.sh | grep "^ramtext: libGL"
$CC -O2 $A -I$G/include -I$G/api $G/tests/headless_gears.c $G/tests/qemu_libc.c -o $O/hg_dyn \
	-L$O -l:libGL.so.1 -lm -Wl,--export-dynamic -Wl,-rpath,$O' > "$A/rv32-dyn.txt" 2>&1
docker run --rm -v "$R":/src -w /tmp s31-glref-qemu sh -c "
S=/src/toolchain/riscv32-esp-linux-musl/riscv32-esp-linux-musl/sysroot
O=/src/gl/out-rv32/dyn
for v in 0 1; do
	S31GL_RAMTEXT=\$v $QEMU -L \$S -strace -d exec,nochain -D /tmp/dyn\$v.log \$O/hg_dyn 320 240 30 /tmp/d\$v.ppm > /tmp/dyn\$v.txt 2>&1
	echo \"hg_dyn 320x240x30 S31GL_RAMTEXT=\$v: rc \$? \$(md5sum < /tmp/d\$v.ppm | cut -c1-32) \$(grep ramtext /tmp/dyn\$v.txt)\"
	python3 - /tmp/dyn\$v.log \$O/libGL.so.1 <<'EOF'
import re, sys
sys.dont_write_bytecode = True
sys.path.insert(0, '/src/gl/api')
from ramtext import Elf
e = Elf(sys.argv[2])
start, end = e.sym('__s31hot_start')['value'], e.sym('__s31hot_end')['value']
slot = e.sym('s31_ramtext_slot')['value']
st = open(sys.argv[1], errors='replace').read()
i = st.index('libGL.so.1\",O_RDONLY')
base = int(re.search(r'mmap2?\(NULL,[^\n]*\) = (0x[0-9a-f]+)', st[i:]).group(1), 16)
page = (base + slot + 4095) & ~4095
dst = page + (start & 63)
mp = [l for l in st.splitlines() if 'mprotect(0x%08x' % page in l]
xip = ram = 0
for m in re.finditer(r'\[[0-9a-f]+/([0-9a-f]+)/', st):
    pc = int(m.group(1), 16)
    if base + start <= pc < base + end: xip += 1
    elif dst <= pc < dst + end - start: ram += 1
print('    libGL at 0x%x: hot-range blocks run from XIP %d, from the RAM copy %d; %s'
      % (base, xip, ram, mp[0].strip() if mp else 'no mprotect of the slot'))
EOF
done
cmp -s /tmp/d0.ppm /tmp/d1.ppm && echo 'hg_dyn: frames identical on/off' || echo 'hg_dyn: frames DIFFER'
" >> "$A/rv32-dyn.txt" 2>&1
cat "$A/rv32-dyn.txt"

docker run --rm -v "$R":/src -w /tmp s31-glref:latest sh -c "
H=/src/gl/out-host
for v in 0 1; do
	S31GL_RAMTEXT=\$v \$H/headless_gears 320 240 100 /tmp/hg\$v.ppm > /tmp/hg\$v.txt 2>&1
	echo \"headless_gears 320x240x100 S31GL_RAMTEXT=\$v: rc \$? \$(md5sum < /tmp/hg\$v.ppm | cut -c1-32) \$(grep ramtext /tmp/hg\$v.txt)\"
	for t in core_test raster_gate filt_test zepoch_test; do
		S31GL_RAMTEXT=\$v \$H/\$t > /tmp/\$t.\$v.raw 2>&1; echo \$? >> /tmp/\$t.\$v.raw
		sed '$NOLOG' /tmp/\$t.\$v.raw > /tmp/\$t.\$v
	done
done
cmp -s /tmp/hg0.ppm /tmp/hg1.ppm && echo 'headless_gears: frames identical on/off' || echo 'headless_gears: frames DIFFER'
for t in core_test raster_gate filt_test zepoch_test; do
	cmp -s /tmp/\$t.0 /tmp/\$t.1 && echo \"\$t: output identical on/off (\$(tail -2 /tmp/\$t.0 | head -1))\" ||
		{ echo \"\$t: output DIFFERS\"; diff /tmp/\$t.0 /tmp/\$t.1 | head -5; }
	grep -q 'libGL: ramtext on' /tmp/\$t.1.raw || echo \"\$t: the RAM copy did NOT engage\"
done
" > "$A/host.txt" 2>&1
cat "$A/host.txt"

if [ -n "${2:-}" ]; then
	B=$(cd "$2" && pwd)
	docker run --rm -v "$R":/src -w /src -e S31GL_RAMTEXT=1 s31-glref:latest \
		sh /src/tools/glref/suite.sh --run "${AC#/src/artifacts/gl/}/suite-rt1" --jobs 10 \
		> "$A/suite-rt1.log" 2>&1
	{
		n=0; same=0
		for f in "$B"/ours/*.png; do
			n=$((n + 1))
			g="$A/suite-rt1/ours/$(basename "$f")"
			if cmp -s "$f" "$g"; then same=$((same + 1)); else echo "DIFFERS: $(basename "$f")"; fi
		done
		echo "suite frames: $same of $n identical to $B with S31GL_RAMTEXT=1"
		grep -l "libGL: ramtext on" "$A"/suite-rt1/ours/*.log 2>/dev/null | wc -l |
			sed 's/^ */suite logs with "libGL: ramtext on": /'
		python3 gl/bench/suitecmp.py "$B/report.json" "$A/suite-rt1/report.json" | tail -1
	} > "$A/suite.txt" 2>&1
	cat "$A/suite.txt"
fi
