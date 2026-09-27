# Copy-mode crash reproducer. Run through runsh.py; it launches detached.
# PB_LIB chooses a candidate (default shipped library); PB_TAG keeps evidence.
# PB_ARMS: at most three copycol/copynocol/tramp/plain runs, 180 s each.
# This is a correctness/stress run, NOT a performance comparison: runs share
# a boot and segvtrap adds memory. Use fresh quiet boots for performance.
# SEGVTRAP_HOLD preserves the reporter against prboom 2.5.0's signal().
# (-devparm only does this in prboom-plus, not the stock binary on the card.)
# The old 25 s test neither completed a demo nor kept the handler.
PB_LIB=${PB_LIB:-/usr/lib/libs31fp.so}
PB_TAG=${PB_TAG:-copy-check}
PB_ARMS=${PB_ARMS:-copycol copynocol copycol}
PB_TRAP=${PB_TRAP:-/root/afp2/segvtrap-copy.so}
PB_WARMUP=${PB_WARMUP:-0} # original failure followed OpenTyrian + glxgears
PB_WINDOW=${PB_WINDOW:-0}
PB_QUIET=${PB_QUIET:-0} # no reporter or hot-path debug counters; application acceptance only
case $PB_TAG in *[!a-zA-Z0-9_-]*|'') echo BAD_TAG; exit 1;; esac
set -- $PB_ARMS
[ $# -ge 1 ] && [ $# -le 3 ] || { echo BAD_ARMS; exit 1; }
[ "$PB_WARMUP" != 1 ] || [ $# -le 2 ] || { echo WARMUP_MAX_TWO_ARMS; exit 1; }
for arm do
  case $arm in copycol|copynocol|tramp|plain) ;; *) echo BAD_ARM; exit 1;; esac
done
[ -r "$PB_LIB" ] && [ -r /root/doom/wads/doom1.wad ] || exit 1
[ "$PB_QUIET" = 1 ] || [ -r "$PB_TRAP" ] || exit 1
if [ "$PB_WARMUP" = 1 ]; then
  [ -x /root/oty/usr/bin/opentyrian ] && [ -x /root/gl2/bin/glxgears ] &&
    [ -d /root/oty/usr/share/opentyrian/data ] || { echo WARMUP_MISSING; exit 1; }
fi
[ -z "$(pidof prboom)" ] || { echo PRBOOM_ALREADY_RUNNING; exit 1; }
D=/root/afp2/pb-$PB_TAG
mkdir "$D" || exit 1
cat > "$D/run.sh" <<'IN'
D=$1; P=$2; ST=$3; WARM=$4; WINDOW=$5; DONE=$6; QUIET=$7; shift 7
[ -z "$DONE" ] || trap 'printf "\n%s\n" "$DONE" > /dev/console' EXIT
exec > "$D/summary.txt" 2>&1
ulimit -c 0
unset LD_PRELOAD S31FP S31FP_COPY S31FP_COLOUR S31FP_DEBUG
printf 'BOARD '; uname -a
md5sum "$P" /root/doom/prboom
echo "QUIET=$QUIET"
amixer -q sset DACL 110; amixer -q sset DACR 110
if [ "$WARM" = 1 ]; then
  cd /root/oty/usr/share/opentyrian/data || exit 1
  env S31FP=1 S31FP_COPY=1 LD_PRELOAD="$P" DISPLAY=:0 HOME=/root \
    /root/oty/usr/bin/opentyrian --no-joystick > "$D/tyrian.log" 2>&1 &
  warm_pid=$!; sleep 60; kill -9 "$warm_pid" 2>/dev/null; wait "$warm_pid" 2>/dev/null
  warm_rc=$?; echo "WARMUP tyrian rc=$warm_rc expected=137"
  [ "$warm_rc" = 137 ] || { echo WARMUP_FAILED; exit 1; }
  env S31FP=1 S31FP_COPY=1 LD_PRELOAD="$P" DISPLAY=:0 \
    /root/gl2/bin/glxgears > "$D/gears.log" 2>&1 &
  warm_pid=$!; sleep 17; kill -9 "$warm_pid" 2>/dev/null; wait "$warm_pid" 2>/dev/null
  warm_rc=$?; echo "WARMUP gears rc=$warm_rc expected=137"
  [ "$warm_rc" = 137 ] || { echo WARMUP_FAILED; exit 1; }
  echo WARMUP_FINISHED
fi
cd /root/doom/wads || exit 1
height=240; mode=-fullscreen
[ "$WINDOW" = 1 ] && { height=200; mode=-window; }
n=0; bad=0
PRELOAD=$P
if [ "$QUIET" != 1 ]; then
  PRELOAD="$P $ST"
  export S31FP_DEBUG=1
fi
for arm do
  n=$((n+1)); log=$D/$n-$arm.log
  copy=0; colour=0; enable=1
  case $arm in copycol) copy=1; colour=1;; copynocol) copy=1;; plain) enable=0;; esac
  echo "START $n $arm"
  # Same sleep/reap watchdog pattern as runsh.py (no timeout applet on card).
  # No observer commands during the demo.
  env LD_PRELOAD="$PRELOAD" SEGVTRAP_HOLD=1 \
    S31FP=$enable S31FP_COPY=$copy S31FP_COLOUR=$colour S31FP_CACHE="$D/cache" \
    SDL_NOPARACHUTE=1 DISPLAY=:0 /root/doom/prboom \
    -iwad /root/doom/wads/doom1.wad -width 320 -height "$height" \
    "$mode" -timedemo demo1 > "$log" 2>&1 &
  game=$!
  ( trap 'kill "$sleeper" 2>/dev/null; wait "$sleeper" 2>/dev/null; exit' TERM INT
    sleep 180 & sleeper=$!; wait "$sleeper"
    echo "WATCHDOG $n $arm"; kill -9 "$game" 2>/dev/null
  ) & guard=$!
  wait "$game"
  rc=$?
  kill "$guard" 2>/dev/null; wait "$guard" 2>/dev/null
  grep -aE 's31fp:|SEGVTRAP|Timed .*gametics|signal 11' "$log"
  ok=1
  # Stock prboom 2.5.0 ends a timedemo via I_Error -> I_SafeExit(-1).
  # 255 is expected ONLY with the full completion line and no fault report.
  [ "$rc" = 255 ] && grep -aq 'Timed 5026 gametics.*frames per second' "$log" || ok=0
  if grep -aqE 'SEGVTRAP sig |signal 11' "$log"; then ok=0; fi
  [ "$QUIET" = 1 ] || grep -aq 'SEGVTRAP holding fault handlers' "$log" || ok=0
  if [ "$copy" = 1 ] && [ "$QUIET" != 1 ]; then
    grep -aq 'of which copied in place 0x0000000[1-9a-f]' "$log" || ok=0
  fi
  if [ "$colour" = 1 ] && [ "$QUIET" != 1 ]; then
    grep -aq 'pages given their original frame colour 0x0000000[1-9a-f]' "$log" || ok=0
  fi
  [ "$ok" = 1 ] || bad=$((bad+1))
  echo "END $n $arm rc=$rc pass=$ok"
  sleep 2
 done
amixer -q sset DACL 143; amixer -q sset DACR 143
echo "PB_DONE runs=$n failures=$bad"
IN
setsid sh "$D/run.sh" "$D" "$PB_LIB" "$PB_TRAP" "$PB_WARMUP" "$PB_WINDOW" "${PB_DONE_TOKEN:-}" "$PB_QUIET" "$@" </dev/null >/dev/null 2>&1 &
echo "PB_STARTED $D pid=$!"
