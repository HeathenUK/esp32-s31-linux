# leaner colouring: CPU0 and CPU1 distributions, exec cost, exactness. /root/afp2/b10.txt (~5 min)
cat > /root/afp2/b10-inner.sh <<'IN'
cd /root/afp2
DONE=$2
[ -z "$DONE" ] || trap 'printf "\n%s\n" "$DONE" > /dev/console' EXIT
if [ "$1" = arithmetic ] || [ "$1" = arithmetic-exact ]; then
  unset LD_PRELOAD S31FP_DEBUG
  C=/root/afp2/oncpu; M=/root/afp2/music.mus
  OLD=/root/afp2/candidate/libs31fp.so
  NEW=/root/afp2/mul-review/libs31fp.so
  md5sum "$OLD" "$NEW"
  # Compare separate disabled/enabled processes: body matching can also
  # patch the checker's renamed libgcc reference, so an in-process oracle
  # alone is insufficient for preload/copy validation.
  for mask in 1 2; do
    for enabled in 0 1; do
      env S31FP=$enabled S31FP_COPY=1 S31FP_DEBUG=1 LD_PRELOAD="$NEW" \
        "$C" "$mask" ./ptest-dyn dump mul 100 173 > "/root/afp2/mul-review/dump-$enabled" || exit 1
    done
    cmp /root/afp2/mul-review/dump-0 /root/afp2/mul-review/dump-1 || exit 1
    echo "ARITHMETIC_EXACT mask=$mask"
  done
  rm /root/afp2/mul-review/dump-0 /root/afp2/mul-review/dump-1
  [ "$1" != arithmetic-exact ] || { echo ARITHMETIC_EXACT_DONE; exit; }
  for r in 0 1 2 3 4 5; do
    arms="old new"; [ $((r % 2)) = 0 ] || arms="new old"
    for song in 36 5; do
      for arm in $arms; do
        P=$OLD; [ "$arm" != new ] || P=$NEW
        echo "ARITHMETIC round=$r song=$song arm=$arm"
        env S31FP=1 S31FP_COPY=1 S31FP_COLOUR=1 \
          S31FP_CACHE=/root/afp2/arithmetic-cache LD_PRELOAD="$P" \
          "$C" 1 ./oplbench-dyn "$M" "$song" 2
        rc=$?; echo "EXIT=$rc"; [ "$rc" = 0 ] || exit 1
      done
    done
  done
  echo ARITHMETIC_DONE
  exit
fi
if [ "$1" = placement ]; then
  unset LD_PRELOAD S31FP_DEBUG
  X=/usr/lib/libs31fp.so; S=/root/afp2/placement-shipped.so
  N=/root/afp2/candidate/libs31fp.so
  cp "$X" "$S" || exit 1
  echo PLACEMENT_IDENTICAL_INPUTS; md5sum "$X" "$S" "$N"
  cmp "$X" "$S" || exit 1
  M=/root/afp2/music.mus; C=/root/afp2/oncpu
  for r in 1 2 3; do
    # Reverse paired placement order on alternate rounds.
    arms="xip-tramp sd-tramp xip-copy sd-copy xip-nocol sd-nocol candidate-copy"
    [ "$r" = 2 ] && arms="sd-tramp xip-tramp sd-copy xip-copy sd-nocol xip-nocol candidate-copy"
    for arm in $arms; do
      P=$X; copy=1; colour=1
      case $arm in sd-*) P=$S;; candidate-*) P=$N;; esac
      case $arm in *tramp) copy=0;; *nocol) colour=0;; esac
      echo "PLACEMENT round=$r arm=$arm"
      env S31FP=1 S31FP_COPY=$copy S31FP_COLOUR=$colour \
        S31FP_CACHE=/root/afp2/placement-cache LD_PRELOAD="$P" \
        "$C" 1 ./oplbench-dyn "$M" 36 2
      rc=$?; echo "EXIT=$rc"; [ "$rc" = 0 ] || exit 1
    done
  done
  echo PLACEMENT_DONE
  exit
fi
M=/root/afp2/music.mus; C=/root/afp2/oncpu; P=/root/afp2/libs31fp.so; PD=/root/afp2/libs31fp-colour.so
K=/root/afp2/cache10; rm -rf $K
OPS="mul add sub div ge le eq unord fltsi fltun fixsi fixun ext trunc"
echo "== uname $(uname -v)  md5 $(md5sum $P | cut -c1-8)"
env LD_PRELOAD=$P S31FP_CACHE=$K S31FP_DEBUG=1 ./ptest-dyn dump mul 1 1 2>&1 >/dev/null | head -4
i=0; for op in $OPS; do i=$((i+1))
  a=$(env S31FP=0 LD_PRELOAD=$P ./ptest-dyn dump $op 50 $i | md5sum | cut -c1-12)
  b=$(env LD_PRELOAD=$P S31FP_CACHE=$K ./ptest-dyn dump $op 50 $i | md5sum | cut -c1-12)
  [ "$a" = "$b" ] && r=SAME || r=DIFFER; printf "%s %s  " $op $r; done; echo
for cpu in 1; do
  for r in 1 2 3 4 5 6; do
    o=$(env LD_PRELOAD=$PD S31FP_CACHE=$K S31FP_DEBUG=1 $C $cpu ./oplbench-dyn $M 36 3 2>&1)
    echo "cpu$cpu col $r $(echo "$o" | grep 'colours after' | cut -d' ' -f4-) us $(echo "$o" | tail -1 | awk '{print $5}')"
    if [ $((r % 2)) = 0 ]; then
      printf "cpu$cpu tramp %s " $r; env LD_PRELOAD=$P S31FP_CACHE=$K S31FP_COPY=0 $C $cpu ./oplbench-dyn $M 36 3 | tail -1 | awk '{print $5}'
      printf "cpu$cpu static %s " $r; $C $cpu ./oplbench-v2 $M 36 3 | tail -1 | awk '{print $5}'
      printf "cpu$cpu plain %s " $r; $C $cpu ./oplbench-dyn $M 36 3 | tail -1 | awk '{print $5}'
    fi
  done
done
echo "== exec cost (20 launches x2), ms/exec"
ex() { lab=$1; shift; s=$(cut -d' ' -f1 /proc/uptime)
  for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do "$@" >/dev/null 2>&1 </dev/null; done
  e=$(cut -d' ' -f1 /proc/uptime); echo "$s $e" | awk -v l="$lab" '{printf "%-40s %7.1f ms/exec\n", l, ($2-$1)*1000/20}'; }
for r in 1 2; do for A in "./oplbench-dyn" "./sdltone1" "/usr/bin/amixer -v"; do
  ex "r$r $A | plain" env $A
  ex "r$r $A | cache" env LD_PRELOAD=$P S31FP_CACHE=$K $A
  ex "r$r $A | cache nocolour" env LD_PRELOAD=$P S31FP_CACHE=$K S31FP_COLOUR=0 $A
done; done
echo B10_DONE
IN
setsid sh /root/afp2/b10-inner.sh "${B10_MODE:-legacy}" "${B10_DONE_TOKEN:-}" </dev/null >"${B10_OUT:-/root/afp2/b10.txt}" 2>&1 &
echo B10_STARTED
