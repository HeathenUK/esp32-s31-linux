#!/bin/bash
# qsreplay.sh - the QuakeSpasm GL workload proxy on the qemu instruction
# counter: replay a gltrace (tools/glref/gltrace/capture-qs.sh) on RV32 bare
# metal with OUR library objects, board flags, exact minstret.
#
#   gl/bench/qsreplay.sh [GLDIR] [OUT] [TRACE]
#     GLDIR  the gl/ tree to measure (default this repo's gl/; a snapshot
#            such as gl/bench/base5 works, as for bench.sh)
#     OUT    output directory (default gl/bench/out-qsr)
#     TRACE  default gl/bench/qstrace/work/qs/qs.gltr
# Knobs: QSR_NOBUILD=1 reuses OUT's library objects (build_q.sh already ran);
#   QSR_ENV="S31GL_TEXFILTER=0 ..." runtime toggles for the library (setenv
#   before the first context); QSR_LABEL names the run; QSR_NULL=0 skips the
#   harness-floor image; S31_BENCH_LIBM=musl as build_q.sh.
# Two images, run in parallel:
#   qsr.elf       the replay through the library
#   qsr_null.elf  the same replay calling empty functions: what the replayer
#                 itself costs per frame (decode + call), so
#                 library = qsr - qsr_null
# Output: OUT/qsr/<label>.txt (every line), and on stdout the per-window
# lines: M instructions per counted frame (mean, min, max), soft-double
# calls per frame, frame hashes against the live capture, and the texture /
# other heap the library holds after the load phase and at the end.
# s31, MIT.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$HERE/../.." && pwd)
GLDIR=$(cd "${1:-$HERE/..}" && pwd)
OUT=${2:-$HERE/out-qsr}
TRACE=${3:-$HERE/qstrace/work/qs/qs.gltr}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
TRACE=$(cd "$(dirname "$TRACE")" && pwd)/$(basename "$TRACE")
[ -f "$TRACE" ] || { echo "qsreplay: no trace $TRACE (tools/glref/gltrace/capture-qs.sh)"; exit 2; }
LABEL=${QSR_LABEL:-$(basename "$GLDIR")}
TC=${S31_BENCH_TC:-$HOME/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf}
B=$TC/bin; L=$TC/riscv32-esp-elf/lib; MULTI=rv32imac_zicsr_zifencei_zaamo_zalrsc/ilp32
BOARD="-Os -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs -mabi=ilp32 -mtune=esp-base"
G=$R/tools/glref/gltrace
if [ -z "$QSR_NOBUILD" ] || [ ! -f "$OUT/obj/core.list" ]; then
	"$HERE/build_q.sh" "$GLDIR" "$OUT" > "$OUT/build_q.log" 2>&1 || { cat "$OUT/build_q.log"; exit 1; }
fi
Q=$OUT/qsr
mkdir -p "$Q"
echo "$TRACE" > "$Q/trace.path"   # qsprof.sh replays the same trace
[ -f "$TRACE.names" ] || python3 "$G/trace.py" names "$TRACE" > "$TRACE.names"
python3 "$G/gen.py" --inc "$GLDIR/include/GL" dispatch "$TRACE.names" "$Q/dispatch.c" --mode direct
python3 "$G/gen.py" --inc "$GLDIR/include/GL" dispatch "$TRACE.names" "$Q/dispatch_null.c" --mode null
CF="$BOARD -O2 -w -I$G -I$GLDIR/include -I$GLDIR/api"
cc() { $B/riscv32-esp-elf-gcc $CF "$@"; }
cc -DQR_LABEL="\"$LABEL\"" -c "$HERE/q_replay.c" -o "$Q/q_replay.o" &
cc -DQR_LABEL="\"$LABEL-null\"" -c "$HERE/q_replay.c" -o "$Q/q_replay_null.o" &
cc -c "$G/replay.c" -o "$Q/replay.o" &
cc -c "$Q/dispatch.c" -o "$Q/dispatch.o" &
cc -c "$Q/dispatch_null.c" -o "$Q/dispatch_null.o" &
wait
WR=$(tr ' ' '\n' < "$HERE/wraps.txt" | grep . | sed 's/^/-Wl,--wrap=/' | tr '\n' ' ')
[ -n "$S31_BENCH_NOWRAP" ] && WR=
MW="-Wl,--wrap=malloc -Wl,--wrap=free -Wl,--wrap=calloc -Wl,--wrap=realloc"
LM="-lm"; [ -f "$OUT/libmuslm.a" ] && LM="$OUT/libmuslm.a -lm"
link() {
	$B/riscv32-esp-elf-gcc -march=rv32imac_zicsr_zifencei_zaamo_zalrsc -mabi=ilp32 \
		-specs=semihost.specs -nostartfiles -T "$HERE/qemu/link.ld" \
		"$HERE/qemu/crt.S" "$L/$MULTI/crt0.o" "$@" "$Q/replay.o" "$OUT/demo/dwrap.o" \
		$(cat "$OUT/obj/core.list") $WR $MW -Wl,--gc-sections $LM 2>&1 | grep -v "RWX\|LOAD segment" || true
}
link "$Q/q_replay.o" "$Q/dispatch.o" -o "$Q/qsr.elf" &
link "$Q/q_replay_null.o" "$Q/dispatch_null.o" -o "$Q/qsr_null.elf" &
wait
ls "$Q/qsr.elf" "$Q/qsr_null.elf" >/dev/null
run() {
	local d=$Q/run-$LABEL-$1
	rm -rf "$d"; mkdir -p "$d"
	ln -s "$TRACE" "$d/qs.gltr"
	for kv in $QSR_ENV; do echo "$kv"; done > "$d/qr.env"
	( cd "$d" && "$HERE/run_q.sh" "$Q/$2" "${QSR_TIMEOUT:-3000}" > out.txt 2>&1 )
}
run lib qsr.elf &
[ "${QSR_NULL:-1}" = 0 ] || run null qsr_null.elf &
wait
{
	echo "# qsreplay $LABEL: GLDIR=$GLDIR TRACE=$TRACE QSR_ENV=$QSR_ENV"
	cat "$Q/run-$LABEL-lib/out.txt"
	[ -f "$Q/run-$LABEL-null/out.txt" ] && grep "^qsr " "$Q/run-$LABEL-null/out.txt"
} > "$Q/$LABEL.txt"
grep "^qsr " "$Q/$LABEL.txt"
grep -q "differ" "$Q/run-$LABEL-lib/out.txt" || { echo "qsreplay: the replay did not finish (see $Q/run-$LABEL-lib/out.txt)"; exit 1; }
