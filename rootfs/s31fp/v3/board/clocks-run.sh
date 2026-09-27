# Diagnostic clock evaluation only. Existing candidate is NOT approved for apps.
# Launch with runsh --done S31_CLOCK_REVIEW_FINISHED --done-timeout 240.
D=/root/afp2/mul-review
P=/root/afp3/libs31fp.so
[ -x "$D/clktest" ] && [ -r "$P" ] || exit 1
cat > "$D/clocks-inner.sh" <<'IN'
D=/root/afp2/mul-review; P=/root/afp3/libs31fp.so; C=/root/afp2/oncpu
trap 'printf "\nS31_CLOCK_REVIEW_FINISHED\n" >/dev/console' EXIT
exec > "$D/clocks.txt" 2>&1
unset LD_PRELOAD S31FP_DEBUG
ulimit -c 0
uname -a
md5sum "$D/clktest" "$P"
run() {
  echo "CLOCK_CASE $*"
  "$@" & child=$!
  ( trap 'kill "$sleeper" 2>/dev/null; wait "$sleeper" 2>/dev/null; exit' TERM INT
    sleep 45 & sleeper=$!; wait "$sleeper"
    echo CLOCK_WATCHDOG; kill -9 "$child" 2>/dev/null
  ) & guard=$!
  wait "$child"; rc=$?
  kill "$guard" 2>/dev/null; wait "$guard" 2>/dev/null
  echo "CLOCK_EXIT=$rc"
}
for mask in 1 2; do
  run "$C" "$mask" "$D/clktest" rdtime
  run "$C" "$mask" "$D/clktest" hz 500
  for mode in 0 1; do
    run env LD_PRELOAD="$P" S31FP=0 S31STR=0 S31CLK=$mode "$C" "$mask" "$D/clktest" cost 10000
  done
done
for mode in 0 1; do
  run env LD_PRELOAD="$P" S31FP=0 S31STR=0 S31CLK=$mode "$D/clktest" mono 10 3
  run env LD_PRELOAD="$P" S31FP=0 S31STR=0 S31CLK=$mode "$D/clktest" drift 5
done
echo CLOCK_REVIEW_DONE
IN
setsid sh "$D/clocks-inner.sh" </dev/null >/dev/null 2>&1 &
echo CLOCK_REVIEW_SUBMITTED
