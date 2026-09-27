# copy-in-place candidate, board run 5 (launcher). TEST-ONLY. /root/afp2/b5.txt (~6 min)
cat > /root/afp2/b5-inner.sh <<'IN'
cd /root/afp2
P=${1:-/root/afp2/libs31fp.so}; CHECK_ONLY=${2:-0}
M=/root/afp2/music.mus; C=/root/afp2/oncpu
K=/root/afp2/cache5; rm -rf $K
OPS="mul add sub div ge le eq unord fltsi fltun fixsi fixun ext trunc"
# Check exit status AND dump length before hashing. A failed producer piped
# into md5sum otherwise becomes a successful hash of empty/partial output.
digest() {
  expected=$1; shift
  "$@" > /root/afp2/b5.dump
  rc=$?
  if [ "$rc" != 0 ] || [ "$(wc -c < /root/afp2/b5.dump)" != "$expected" ]; then
    echo INVALID
  else
    md5sum /root/afp2/b5.dump | cut -c1-12
  fi
}
echo "== uname $(uname -v)  md5 $(md5sum $P | cut -c1-8)"
env S31FP_COPY=1 LD_PRELOAD=$P S31FP_CACHE=$K S31FP_DEBUG=1 ./ptest-dyn dump mul 1 1 2>&1 >/dev/null | head -3
echo "== 1. two-process exactness: unpatched vs copy-in-place vs trampoline (md5 of 100k results+flags)"
i=0; for op in $OPS; do i=$((i+1))
  a=$(digest 1200000 env S31FP=0 LD_PRELOAD=$P $C 1 ./ptest-dyn dump $op 100 $i)
  b=$(digest 1200000 env S31FP_COPY=1 LD_PRELOAD=$P S31FP_CACHE=$K $C 1 ./ptest-dyn dump $op 100 $i)
  c=$(digest 1200000 env LD_PRELOAD=$P S31FP_CACHE=$K S31FP_COPY=0 $C 2 ./ptest-dyn dump $op 100 $i)
  [ "$a" != INVALID ] && [ "$a" = "$b" ] && [ "$a" = "$c" ] && r=SAME || r=DIFFER; echo "$op $a $b $c $r"
done
echo "== 2. hammer: 280 fresh copy-in-place processes, CPU0/CPU1/either, scan and cache alternating"
j=0; for op in $OPS; do eval ref$j=$(digest 12000 env S31FP=0 ./ptest-dyn dump $op 1 $((j+77))); j=$((j+1)); done
ok=0; bad=0
for i in $(seq 1 280); do
  j=$((i % 14)); op=$(echo $OPS | cut -d' ' -f$((j+1)))
  m=$((i % 3 + 1)); cd_=; [ $((i % 2)) = 0 ] && cd_=$K
  h=$(digest 12000 env S31FP_COPY=1 LD_PRELOAD=$P S31FP_CACHE=$cd_ $C $m ./ptest-dyn dump $op 1 $((j+77)))
  eval r=\$ref$j; [ "$h" != INVALID ] && [ "$h" = "$r" ] && ok=$((ok+1)) || { bad=$((bad+1)); echo "BAD $i $op $h $r"; }
done
echo "hammer ok=$ok bad=$bad"
rm -f /root/afp2/b5.dump
[ "$CHECK_ONLY" = 1 ] && { echo B5_DONE; exit; }
echo "== 3. title song 4 s x3 interleaved, CPU0: static v2 / preload copy / preload trampoline"
for r in 1 2 3; do
  printf "v2static r%s " $r; $C 1 ./oplbench-v2 $M 36 4 | tail -1
  printf "copy r%s " $r; env S31FP_COPY=1 LD_PRELOAD=$P S31FP_CACHE=$K $C 1 ./oplbench-dyn $M 36 4 | tail -1
  printf "tramp r%s " $r; env S31FP_COPY=1 LD_PRELOAD=$P S31FP_CACHE=$K S31FP_COPY=0 $C 1 ./oplbench-dyn $M 36 4 | tail -1
done
for s in 5 0 9; do printf "copy song%s " $s; env S31FP_COPY=1 LD_PRELOAD=$P S31FP_CACHE=$K $C 1 ./oplbench-dyn $M $s 8 | tail -1; done
printf "copy cpu1 "; env S31FP_COPY=1 LD_PRELOAD=$P S31FP_CACHE=$K $C 2 ./oplbench-dyn $M 36 4 | tail -1
echo "== 4. exec cost, 20 launches x2 rounds, ms/exec"
ex() { lab=$1; shift; s=$(cut -d' ' -f1 /proc/uptime)
  for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do "$@" >/dev/null 2>&1 </dev/null; done
  e=$(cut -d' ' -f1 /proc/uptime); echo "$s $e" | awk -v l="$lab" '{printf "%-40s %7.1f ms/exec\n", l, ($2-$1)*1000/20}'; }
for r in 1 2; do for A in "./oplbench-dyn" "./sdltone1" "/usr/bin/amixer -v" "/usr/bin/aplay --version"; do
  ex "r$r $A | plain" env $A
  ex "r$r $A | cache" env S31FP_COPY=1 LD_PRELOAD=$P S31FP_CACHE=$K $A
done; done
echo B5_DONE
IN
setsid sh /root/afp2/b5-inner.sh "${B5_LIB:-/root/afp2/libs31fp.so}" "${B5_CHECK_ONLY:-0}" </dev/null >"${B5_OUT:-/root/afp2/b5.txt}" 2>&1 &
echo B5_STARTED
