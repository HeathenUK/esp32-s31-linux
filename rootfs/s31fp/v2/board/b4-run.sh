# s31fp v2 FINAL candidate (fcsr read once, NX written only if unset), board run 4
# (launcher). TEST-ONLY. Results /root/afp2/b4.txt (~6 min).
cat > /root/afp2/b4-inner.sh <<'IN'
cd /root/afp2
M=/root/afp2/music.mus; C=/root/afp2/oncpu; P=/root/afp2/libs31fp.so; P1=/root/afp2/libs31fp1.so
K=/root/afp2/cache; rm -rf $K
T=; command -v timeout >/dev/null && T="timeout 10"
echo "== uname $(uname -v)  md5 $(md5sum $P | cut -c1-8)"
echo "== 1. helpers on silicon vs libgcc's own (bits+fflags, random initial flags, directed modes)"
i=0; for op in mul add sub div ge le eq unord fltsi fltun fixsi fixun ext trunc; do i=$((i+1))
  $C 1 ./v2check-board check $op 300 $((i+1500)) | grep RESULT | cut -c1-120; done
echo "== 2. the real preload: unpatched process vs patched process, same operands (md5 of results+flags)"
env LD_PRELOAD=$P S31FP_CACHE=$K S31FP_DEBUG=1 ./ptest-dyn dump mul 1 1 2>&1 >/dev/null | head -2
i=0; for op in mul add sub div ge le eq unord fltsi fltun fixsi fixun ext trunc; do i=$((i+1))
  a=$(env S31FP=0 LD_PRELOAD=$P $C 1 ./ptest-dyn dump $op 100 $i | md5sum | cut -c1-12)
  b=$(env LD_PRELOAD=$P S31FP_CACHE=$K $C 1 ./ptest-dyn dump $op 100 $i | md5sum | cut -c1-12)
  [ "$a" = "$b" ] && r=SAME || r=DIFFER; echo "$op $a $b $r"
done
echo "== 3. hammer: 280 fresh patched processes on CPU0/CPU1/either, scan and cache alternating"
for j in $(seq 0 13); do eval ref$j=$(./ptest-dyn dump $(echo mul add sub div ge le eq unord fltsi fltun fixsi fixun ext trunc | cut -d' ' -f$((j+1))) 1 $((j+77)) | md5sum | cut -c1-12); done
ok=0; bad=0
for i in $(seq 1 280); do
  j=$((i % 14)); op=$(echo mul add sub div ge le eq unord fltsi fltun fixsi fixun ext trunc | cut -d' ' -f$((j+1)))
  m=$((i % 3 + 1)); cd_=; [ $((i % 2)) = 0 ] && cd_=$K
  h=$(env LD_PRELOAD=$P S31FP_CACHE=$cd_ $C $m ./ptest-dyn dump $op 1 $((j+77)) | md5sum | cut -c1-12)
  eval r=\$ref$j; [ "$h" = "$r" ] && ok=$((ok+1)) || { bad=$((bad+1)); echo "BAD $i $op $h $r"; }
done
echo "hammer ok=$ok bad=$bad"
echo "== 4. title song, 4 s, x3 interleaved, CPU0 (static builds; dyn = shipped shape, +preload = the real path)"
for r in 1 2 3; do
  for v in old v2 v2f v2n; do printf "%s r%s " $v $r; $C 1 ./oplbench-$v $M 36 4 | tail -1; done
  printf "dyn r%s " $r; $C 1 ./oplbench-dyn $M 36 4 | tail -1
  printf "dyn+preload r%s " $r; env LD_PRELOAD=$P S31FP_CACHE=$K $C 1 ./oplbench-dyn $M 36 4 | tail -1
done
printf "dyn+preload song5 "; env LD_PRELOAD=$P S31FP_CACHE=$K $C 1 ./oplbench-dyn $M 5 8 | tail -1
printf "v2 song5 "; $C 1 ./oplbench-v2 $M 5 8 | tail -1
printf "v2 song0 "; $C 1 ./oplbench-v2 $M 0 8 | tail -1
printf "v2 song9 "; $C 1 ./oplbench-v2 $M 9 8 | tail -1
echo "== 5. helper bench, final v2 (ns/iter; nop from run 1)"
while read op cl; do $C 1 ./v2bench-board bench v2 $op $cl 200000; done <<L
mul gen
mul low0
mul pow2
mul zero
add same
add opp
div gen
ge gen
fltsi int
fixsi int
trunc int
L
echo "== 6. exec cost: 20 launches, ms/exec (env in every arm; S31FP=0 = library loaded, constructor returns at once)"
ex() { lab=$1; shift; s=$(cut -d' ' -f1 /proc/uptime)
  for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do $T "$@" >/dev/null 2>&1 </dev/null; done
  e=$(cut -d' ' -f1 /proc/uptime); echo "$s $e" | awk -v l="$lab" '{printf "%-40s %7.1f ms/exec\n", l, ($2-$1)*1000/20}'; }
set -- 
for r in 1 2; do
  for A in "./oplbench-dyn" "./sdltone1" "/usr/bin/amixer -v" "/usr/bin/aplay --version" "/usr/bin/bluetoothctl --version"; do
    B=${A%% *}; [ -x $B ] || continue
    ex "r$r $A | plain"        env $A
    ex "r$r $A | v1 (old)"     env LD_PRELOAD=$P1 $A
    ex "r$r $A | loaded, off"  env S31FP=0 LD_PRELOAD=$P $A
    ex "r$r $A | scan"         env LD_PRELOAD=$P S31FP_CACHE= $A
    ex "r$r $A | cache"        env LD_PRELOAD=$P S31FP_CACHE=$K $A
  done
done
for A in ./oplbench-dyn ./sdltone1 /usr/bin/amixer /usr/bin/aplay /usr/bin/bluetoothctl; do [ -x $A ] && { printf "%s: " $A; env LD_PRELOAD=$P S31FP_CACHE= S31FP_DEBUG=1 $A --version 2>&1 </dev/null | grep "s31fp:"; }; done
echo "== 7. scan cost of big binaries' text (file read, not run)"
for f in /usr/bin/opentyrian /root/quake/sdlquake /root/doom/prboom; do [ -e $f ] && ./scanbench $f; done
ls $K | wc -l
echo B4_DONE
IN
setsid sh /root/afp2/b4-inner.sh </dev/null >/root/afp2/b4.txt 2>&1 &
echo B4_STARTED
