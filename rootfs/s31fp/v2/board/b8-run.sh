# colour-placed copy-in-place: exactness, hammer, and time distribution. /root/afp2/b8.txt (~6 min)
cat > /root/afp2/b8-inner.sh <<'IN'
cd /root/afp2; M=/root/afp2/music.mus; C=/root/afp2/oncpu; P=/root/afp2/libs31fp.so; PD=/root/afp2/libs31fp-colour.so
K=/root/afp2/cache8; rm -rf $K
OPS="mul add sub div ge le eq unord fltsi fltun fixsi fixun ext trunc"
echo "== uname $(uname -v)  md5 $(md5sum $P | cut -c1-8)"
env LD_PRELOAD=$P S31FP_CACHE=$K S31FP_DEBUG=1 ./ptest-dyn dump mul 1 1 2>&1 >/dev/null | head -4
echo "== exactness: unpatched vs coloured copy"
i=0; for op in $OPS; do i=$((i+1))
  a=$(env S31FP=0 LD_PRELOAD=$P $C 1 ./ptest-dyn dump $op 100 $i | md5sum | cut -c1-12)
  b=$(env LD_PRELOAD=$P S31FP_CACHE=$K $C 1 ./ptest-dyn dump $op 100 $i | md5sum | cut -c1-12)
  [ "$a" = "$b" ] && r=SAME || r=DIFFER; echo "$op $a $b $r"; done
j=0; for op in $OPS; do eval ref$j=$(./ptest-dyn dump $op 1 $((j+77)) | md5sum | cut -c1-12); j=$((j+1)); done
ok=0; bad=0
for i in $(seq 1 280); do
  j=$((i % 14)); op=$(echo $OPS | cut -d' ' -f$((j+1))); m=$((i % 3 + 1)); cd_=; [ $((i % 2)) = 0 ] && cd_=$K
  h=$(env LD_PRELOAD=$P S31FP_CACHE=$cd_ $C $m ./ptest-dyn dump $op 1 $((j+77)) | md5sum | cut -c1-12)
  eval r=\$ref$j; [ "$h" = "$r" ] && ok=$((ok+1)) || { bad=$((bad+1)); echo "BAD $i $op"; }
done
echo "hammer ok=$ok bad=$bad"
echo "== time: coloured (colour-debug build) x20, uncoloured x6, static x4"
for r in $(seq 1 20); do
  o=$(env LD_PRELOAD=$PD S31FP_CACHE=$K S31FP_DEBUG=1 $C 1 ./oplbench-dyn $M 36 3 2>&1)
  echo "col $r $(echo "$o" | grep 'colours before' | cut -d' ' -f4-) -> $(echo "$o" | grep 'colours after' | cut -d' ' -f4-) us $(echo "$o" | tail -1 | awk '{print $5}')"
  if [ $((r % 5)) = 0 ]; then
    printf "static %s " $r; $C 1 ./oplbench-v2 $M 36 3 | tail -1 | awk '{print $5}'
  fi
  if [ $((r % 3)) = 0 ]; then
    o=$(env LD_PRELOAD=$PD S31FP_CACHE=$K S31FP_DEBUG=1 S31FP_COLOUR=0 $C 1 ./oplbench-dyn $M 36 3 2>&1)
    echo "nocol $r $(echo "$o" | grep 'colours after' | cut -d' ' -f4-) us $(echo "$o" | tail -1 | awk '{print $5}')"
  fi
done
printf "shipped-lib cpu1 "; env LD_PRELOAD=$P S31FP_CACHE=$K $C 2 ./oplbench-dyn $M 36 4 | tail -1 | awk '{print $5}'
printf "shipped-lib song5 "; env LD_PRELOAD=$P S31FP_CACHE=$K $C 1 ./oplbench-dyn $M 5 8 | tail -1 | awk '{print $5, $NF}'
echo "== exec cost (20 launches x2), ms/exec"
ex() { lab=$1; shift; s=$(cut -d' ' -f1 /proc/uptime)
  for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do "$@" >/dev/null 2>&1 </dev/null; done
  e=$(cut -d' ' -f1 /proc/uptime); echo "$s $e" | awk -v l="$lab" '{printf "%-40s %7.1f ms/exec\n", l, ($2-$1)*1000/20}'; }
for r in 1 2; do for A in "./oplbench-dyn" "./sdltone1" "/usr/bin/amixer -v"; do
  ex "r$r $A | plain" env $A
  ex "r$r $A | cache" env LD_PRELOAD=$P S31FP_CACHE=$K $A
  ex "r$r $A | cache nocolour" env LD_PRELOAD=$P S31FP_CACHE=$K S31FP_COLOUR=0 $A
done; done
echo B8_DONE
IN
setsid sh /root/afp2/b8-inner.sh </dev/null >/root/afp2/b8.txt 2>&1 &
echo B8_STARTED
