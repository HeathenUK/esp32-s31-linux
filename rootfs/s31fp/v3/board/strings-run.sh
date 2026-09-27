# V3 string correctness and CPU routing. Launch through scripts/board/runsh.py.
# No observations during workloads. Four bounded correctness cases plus short routing/microbenchmark cases;
# use passive completion notification with a 600 s backstop. Uses the existing pb-run/runsh sleep/reap watchdog.
V3_DIR=${V3_DIR:-/root/afp3}
V3_OUT=${V3_OUT:-/root/afp3/strings-check.txt}
[ ! -e "$V3_OUT" ] || { echo OUTPUT_EXISTS; exit 1; }
[ -x "$V3_DIR/strtest" ] && [ -r "$V3_DIR/libs31fp.so" ] || exit 1
[ -z "$(pidof prboom opentyrian strtest)" ] || { echo WORKLOAD_BUSY; exit 1; }
cat > "$V3_DIR/strings-inner.sh" <<'IN'
D=$1; OUT=$2; DONE=$3; DIAG=$4; CMP=$5
[ -z "$DONE" ] || trap 'printf "\n%s\n" "$DONE" > /dev/console' EXIT
exec > "$OUT" 2>&1
unset LD_PRELOAD S31FP_DEBUG
ulimit -c 0
uname -a
md5sum "$D/libs31fp.so" "$D/strtest"
bad=0
run() {
  label=$1; shift
  echo "START $label"
  "$@" > "$D/$label.log" 2>&1 & child=$!
  ( trap 'kill "$sleeper" 2>/dev/null; wait "$sleeper" 2>/dev/null; exit' TERM INT
    sleep 90 & sleeper=$!; wait "$sleeper"
    echo "WATCHDOG $label"; kill -9 "$child" 2>/dev/null
  ) & guard=$!
  wait "$child"; rc=$?
  kill "$guard" 2>/dev/null; wait "$guard" 2>/dev/null
  cat "$D/$label.log"
  [ "$rc" = 0 ] && grep -q '0 mismatches' "$D/$label.log" || bad=$((bad+1))
  echo "END $label rc=$rc"
}
P=$D/libs31fp.so; C=/root/afp2/oncpu
if [ "$CMP" = 1 ]; then
  for r in 1 2 3; do
    modes="0 scalar"; [ "$r" != 2 ] || modes="scalar 0"
    for size in 8 64 1024 4096; do
      for where in -1 0 $((size-1)); do
        for mode in $modes; do
          echo "CMP round=$r mode=$mode size=$size where=$where"
          env S31FP=0 S31STR=$mode S31CLK=0 LD_PRELOAD="$P" "$C" 1 "$D/strtest" cmpbench "$size" 10000 "$where"
          rc=$?; echo "CMP_EXIT=$rc"; [ "$rc" = 0 ] || bad=$((bad+1))
        done
      done
    done
  done
  echo "V3_CMP_DONE failures=$bad"
  exit
fi
if [ "$DIAG" = 1 ]; then
  P="$P /root/afp2/segvtrap-copy.so"
  run threads env S31FP=0 S31STR=1 S31CLK=0 S31FP_DEBUG=1 LD_PRELOAD="$P" "$D/strtest" check 3000 4 0
  run scalar-migration env S31FP=0 S31STR=scalar S31CLK=0 S31FP_DEBUG=1 LD_PRELOAD="$P" "$D/strtest" check 3000 4 1
  run dispatch-migration env S31FP=0 S31STR=1 S31CLK=0 S31FP_DEBUG=1 LD_PRELOAD="$P" "$D/strtest" check 3000 4 1
  echo "V3_DIAG_DONE failures=$bad"
  exit
fi
run libc-cpu0 env S31FP=0 S31STR=0 S31CLK=0 LD_PRELOAD="$P" "$C" 1 "$D/strtest" libc 5000
run dispatch-cpu0 env S31FP=0 S31STR=1 S31CLK=0 S31FP_DEBUG=1 LD_PRELOAD="$P" "$C" 1 "$D/strtest" check 5000
run dispatch-cpu1 env S31FP=0 S31STR=1 S31CLK=0 S31FP_DEBUG=1 LD_PRELOAD="$P" "$C" 2 "$D/strtest" check 5000
run migration env S31FP=0 S31STR=1 S31CLK=0 S31FP_DEBUG=1 LD_PRELOAD="$P" "$D/strtest" check 3000 4 1
if [ "$bad" = 0 ]; then
  for mode in 0 1; do
    echo "ROUTING mode=$mode start_mask=2"
    env S31FP=0 S31STR=$mode S31CLK=0 LD_PRELOAD="$P" "$C" 2 "$D/v3work" 1000
    rc=$?; echo "ROUTING_EXIT=$rc"; [ "$rc" = 0 ] || bad=$((bad+1))
  done
  # No debug counters, affinity flipper or external observer in timed arms.
  for size in 64 1024 4096; do
    for arm in libc-cpu0 dispatch-cpu0 dispatch-cpu1 scalar-cpu0; do
      mode=1; mask=1
      case $arm in libc-*) mode=0;; scalar-*) mode=scalar;; esac
      [ "$arm" != dispatch-cpu1 ] || mask=2
      echo "BENCH_ARM $arm size=$size"
      env S31FP=0 S31STR=$mode S31CLK=0 LD_PRELOAD="$P" "$C" "$mask" "$D/strtest" bench "$size" 10000
      rc=$?; echo "BENCH_EXIT=$rc"; [ "$rc" = 0 ] || bad=$((bad+1))
    done
  done
fi
echo "V3_DONE failures=$bad"
IN
setsid sh "$V3_DIR/strings-inner.sh" "$V3_DIR" "$V3_OUT" "${V3_DONE_TOKEN:-}" "${V3_DIAG:-0}" "${V3_CMP:-0}" </dev/null >/dev/null 2>&1 &
echo "V3_LAUNCH pid=$! output=$V3_OUT"
