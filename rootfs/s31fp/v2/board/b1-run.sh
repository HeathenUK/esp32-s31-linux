# s31fp v2, board run 1 (launcher; returns at once). Detached; results in
# /root/afp2/b1.txt (~6 min). TEST binaries only: oplbench builds, the helper
# checker/bench. Nothing installed, no app run, no audio device opened.
cat > /root/afp2/b1-inner.sh <<'IN'
cd /root/afp2
M=/root/afp2/music.mus
C=/root/afp2/oncpu
echo "== uname $(uname -v)"
echo "== check on silicon: v2 vs libgcc's own routines, bits + fflags, RNE + directed modes"
i=0; for op in mul add sub div ge le eq unord fltsi fltun fixsi fixun ext trunc; do
  i=$((i+1)); $C 1 ./v2check-board check $op 300 $((i+900)) | grep RESULT
done
for v in lg old v2 v2n; do echo "== routines $v CPU0"; $C 1 ./oplbench-$v -r; done
echo "== helper bench CPU0 (ns per loop iteration; subtract nop)"
for impl in nop lg old v2; do
  while read op cl; do $C 1 ./v2bench-board bench $impl $op $cl 200000; done <<L
mul gen
mul low0
mul pow2
mul zero
add same
add opp
add cancel
div gen
ge gen
eq gen
fltsi int
fixsi int
fixun int
ext int
trunc int
L
done
echo "== title song 36, 4 s, x5 interleaved, CPU0"
for r in 1 2 3 4 5; do for v in lg old v2 v2n; do
  printf "%s r%s " $v $r; $C 1 ./oplbench-$v $M 36 4 | tail -1
done; done
for s in 0 9 5; do for v in lg old v2 v2n; do
  printf "%s song%s " $v $s; $C 1 ./oplbench-$v $M $s 8 | tail -1
done; done
echo "== title on CPU1 (lent hart)"
for v in lg v2; do printf "%s cpu1 " $v; $C 2 ./oplbench-$v $M 36 4 | tail -1; done
echo B1_DONE
IN
setsid sh /root/afp2/b1-inner.sh </dev/null >/root/afp2/b1.txt 2>&1 &
echo B1_STARTED
