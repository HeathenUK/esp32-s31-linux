# distribution of the copy-in-place title-song time (bimodal in run 5?). /root/afp2/b6.txt (~3 min)
cat > /root/afp2/b6-inner.sh <<'IN'
cd /root/afp2; M=/root/afp2/music.mus; C=/root/afp2/oncpu; P=/root/afp2/libs31fp.so; K=/root/afp2/cache5
for r in 1 2 3 4 5 6 7 8 9 10 11 12; do
  printf "copy %s " $r; env LD_PRELOAD=$P S31FP_CACHE=$K $C 1 ./oplbench-dyn $M 36 3 | tail -1 | awk '{print $5}'
  if [ $((r % 2)) = 0 ]; then
    printf "tramp %s " $r; env LD_PRELOAD=$P S31FP_CACHE=$K S31FP_COPY=0 $C 1 ./oplbench-dyn $M 36 3 | tail -1 | awk '{print $5}'
    printf "static %s " $r; $C 1 ./oplbench-v2 $M 36 3 | tail -1 | awk '{print $5}'
  fi
done
echo B6_DONE
IN
setsid sh /root/afp2/b6-inner.sh </dev/null >/root/afp2/b6.txt 2>&1 &
echo B6_STARTED
