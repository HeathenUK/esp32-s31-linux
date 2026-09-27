# copy-in-place candidate, board run 5 (launcher). TEST-ONLY. /root/afp2/b5.txt (~6 min)
cat > /root/afp2/b5-inner.sh <<'IN'
cd /root/afp2
M=/root/afp2/music.mus; C=/root/afp2/oncpu; P=/root/afp2/libs31fp.so
K=/root/afp2/cache5; rm -rf $K
OPS="mul add sub div ge le eq unord fltsi fltun fixsi fixun ext trunc"
echo "== uname $(uname -v)  md5 $(md5sum $P | cut -c1-8)"
env LD_PRELOAD=$P S31FP_CACHE=$K S31FP_DEBUG=1 ./ptest-dyn dump mul 1 1 2>&1 >/dev/null | head -3
echo "== 1. two-process exactness: unpatched vs copy-in-place vs trampoline (md5 of 100k results+flags)"
i=0; for op in $OPS; do i=$((i+1))
  a=$(env S31FP=0 LD_PRELOAD=$P $C 1 ./ptest-dyn dump $op 100 $i | md5sum | cut -c1-12)
  b=$(env LD_PRELOAD=$P S31FP_CACHE=$K $C 1 ./ptest-dyn dump $op 100 $i | md5sum | cut -c1-12)
  c=$(env LD_PRELOAD=$P S31FP_CACHE=$K S31FP_COPY=0 $C 2 ./ptest-dyn dump $op 100 $i | md5sum | cut -c1-12)
  [ "$a" = "$b" ] && [ "$a" = "$c" ] && r=SAME || r=DIFFER; echo "$op $a $b $c $r"
done
echo "== 2. hammer: 280 fresh copy-in-place processes, CPU0/CPU1/either, scan and cache alternating"
j=0; for op in $OPS; do eval ref$j=$(./ptest-dyn dump $op 1 $((j+77)) | md5sum | cut -c1-12); j=$((j+1)); done
ok=0; bad=0
for i in $(seq 1 280); do
  j=$((i % 14)); op=$(echo $OPS | cut -d' ' -f$((j+1)))
  m=$((i % 3 + 1)); cd_=; [ $((i % 2)) = 0 ] && cd_=$K
  h=$(env LD_PRELOAD=$P S31FP_CACHE=$cd_ $C $m ./ptest-dyn dump $op 1 $((j+77)) | md5sum | cut -c1-12)
  eval r=\$ref$j; [ "$h" = "$r" ] && ok=$((ok+1)) || { bad=$((bad+1)); echo "BAD $i $op $h $r"; }
done
echo "hammer ok=$ok bad=$bad"
echo "== 3. title song 4 s x3 interleaved, CPU0: static v2 / preload copy / preload trampoline"
for r in 1 2 3; do
  printf "v2static r%s " $r; $C 1 ./oplbench-v2 $M 36 4 | tail -1
  printf "copy r%s " $r; env LD_PRELOAD=$P S31FP_CACHE=$K $C 1 ./oplbench-dyn $M 36 4 | tail -1
  printf "tramp r%s " $r; env LD_PRELOAD=$P S31FP_CACHE=$K S31FP_COPY=0 $C 1 ./oplbench-dyn $M 36 4 | tail -1
done
for s in 5 0 9; do printf "copy song%s " $s; env LD_PRELOAD=$P S31FP_CACHE=$K $C 1 ./oplbench-dyn $M $s 8 | tail -1; done
printf "copy cpu1 "; env LD_PRELOAD=$P S31FP_CACHE=$K $C 2 ./oplbench-dyn $M 36 4 | tail -1
echo "== 4. exec cost, 20 launches x2 rounds, ms/exec"
ex() { lab=$1; shift; s=$(cut -d' ' -f1 /proc/uptime)
  for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do "$@" >/dev/null 2>&1 </dev/null; done
  e=$(cut -d' ' -f1 /proc/uptime); echo "$s $e" | awk -v l="$lab" '{printf "%-40s %7.1f ms/exec\n", l, ($2-$1)*1000/20}'; }
for r in 1 2; do for A in "./oplbench-dyn" "./sdltone1" "/usr/bin/amixer -v" "/usr/bin/aplay --version"; do
  ex "r$r $A | plain" env $A
  ex "r$r $A | cache" env LD_PRELOAD=$P S31FP_CACHE=$K $A
done; done
echo B5_DONE
IN
setsid sh /root/afp2/b5-inner.sh </dev/null >/root/afp2/b5.txt 2>&1 &
echo B5_STARTED
