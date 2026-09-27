# copy-in-place: frame colour of the COW'd pages vs title-song time, 20 runs. /root/afp2/b7.txt (~3 min)
cat > /root/afp2/b7-inner.sh <<'IN'
cd /root/afp2; M=/root/afp2/music.mus; C=/root/afp2/oncpu; P=/root/afp2/libs31fp-colour.so; K=/root/afp2/cache5
for r in $(seq 1 20); do
  o=$(env LD_PRELOAD=$P S31FP_CACHE=$K S31FP_DEBUG=1 $C 1 ./oplbench-dyn $M 36 3 2>&1)
  echo "run $r $(echo "$o" | grep 'colours after' | cut -d' ' -f4-) us $(echo "$o" | tail -1 | awk '{print $5}')"
done
echo "before: $(env LD_PRELOAD=$P S31FP_CACHE=$K S31FP_DEBUG=1 ./oplbench-dyn 2>&1 | grep 'colours before')"
echo B7_DONE
IN
setsid sh /root/afp2/b7-inner.sh </dev/null >/root/afp2/b7.txt 2>&1 &
echo B7_STARTED
