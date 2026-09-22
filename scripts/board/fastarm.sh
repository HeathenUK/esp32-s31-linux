#!/bin/bash
# fastarm.sh <label> "<objects>" - one .text..fast candidate arm, end to end.
#
# Rewrites S31_FAST_PROFILED in arch/riscv/kernel/vmlinux-xip.lds.S, builds the
# SMP kernel, syncs it out of the Docker volume, flashes it, and BOOT-CHECKS it
# with alive.py --reset. An empty object list is the control arm.
#
# Why a script: moving kernel text to RAM is a guess-and-measure loop, and each
# arm is six steps where skipping one is silent - a failed build leaves the
# previous image in images/ and `make flash-linux` happily writes THAT (it
# prints "Hash of data verified" either way). 2026-09-21 that cost an arm: a
# broken linker-script edit failed the build and the stale non-booting kernel
# was flashed and "measured". So this aborts the moment a build fails.
#
# Placement is not believed until System.map says so (TEXT_MAIN globs .text.*;
# see the header of the .lds.S), so the symbols are checked here too.
set -u
cd "$(dirname "$0")/../.."
L=${1:?label}; OBJS=${2-}
LDS=linux-71-port/arch/riscv/kernel/vmlinux-xip.lds.S
OUT=artifacts/smp-finish

python3 - "$LDS" "$OBJS" <<'PY'
import sys
path, objs = sys.argv[1], sys.argv[2]
lines = open(path).read().split('\n')
out, i = [], 0
while i < len(lines):
    if lines[i].startswith('#define S31_FAST_PROFILED'):
        while lines[i].endswith('\\'):		# eat the continuation lines
            i += 1
        out.append('#define S31_FAST_PROFILED\t' + objs)
    else:
        out.append(lines[i])
    i += 1
open(path, 'w').write('\n'.join(out))
PY
echo "--- arm $L: S31_FAST_PROFILED = ${OBJS:-<empty>}"

./docker/build.sh "cd /src && \$S31_MAKE linux SMP=1 > /src/$OUT/build-fast-$L.log 2>&1; echo BUILD_EXIT=\$?" | tail -1 | tee /tmp/fastarm.exit
grep -q "BUILD_EXIT=0" /tmp/fastarm.exit || {
	echo "BUILD FAILED - not flashing (images/ still holds the previous kernel):"
	grep -aiE "error|syntax" "$OUT/build-fast-$L.log" | head -5
	exit 1
}
make sync-images >/dev/null 2>&1
echo "    image: $(strings images/xipImage | grep -ao '#[0-9][0-9]* SMP' | head -1)"
make flash-linux 2>&1 | grep -aiE "hash of data|error|abort" | head -1 | sed 's/^/    /'
python3 scripts/board/alive.py --reset --timeout 60 --log "$OUT/stall-305/fast-$L.raw" > "$OUT/stall-305/fast-$L.txt" 2>&1
echo "    boot: $(grep -a '^STAGE' "$OUT/stall-305/fast-$L.txt" | tail -1)"
