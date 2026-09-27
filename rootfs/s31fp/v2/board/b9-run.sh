# leaner colouring: CPU0 and CPU1 distributions, exec cost, exactness. /root/afp2/b9.txt (~5 min)
cat > /root/afp2/b9-inner.sh <<'IN'
cd /root/afp2; M=/root/afp2/music.mus; C=/root/afp2/oncpu; P=/root/afp2/libs31fp.so; PD=/root/afp2/libs31fp-colour.so
K=/root/afp2/cache9; rm -rf $K
OPS="mul add sub div ge le eq unord fltsi fltun fixsi fixun ext trunc"
echo "== uname $(uname -v)  md5 $(md5sum $P | cut -c1-8)"
env LD_PRELOAD=$P S31FP_CACHE=$K S31FP_DEBUG=1 ./ptest-dyn dump mul 1 1 2>&1 >/dev/null | head -4
i=0; for op in $OPS; do i=$((i+1))
  a=$(env S31FP=0 LD_PRELOAD=$P ./ptest-dyn dump $op 50 $i | md5sum | cut -c1-12)
  b=$(env LD_PRELOAD=$P S31FP_CACHE=$K ./ptest-dyn dump $op 50 $i | md5sum | cut -c1-12)
  [ "$a" = "$b" ] && r=SAME || r=DIFFER; printf "%s %s  " $op $r; done; echo
for cpu in 1 2; do
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
echo B9_DONE
IN
setsid sh /root/afp2/b9-inner.sh </dev/null >/root/afp2/b9.txt 2>&1 &
echo B9_STARTED
