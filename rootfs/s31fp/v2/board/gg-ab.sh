# glxgears A/B on ONE boot, interleaved plain / v2-preload x3, 12 s each,
# starting at uptime 140 (after the OpenTyrian arm). /root/afp2/ggab.txt
cat > /root/afp2/ggab-inner.sh <<'IN'
exec > /root/afp2/ggab.txt 2>&1
read u _ < /proc/uptime; u=${u%.*}; [ $u -lt 140 ] && sleep $((140 - u))
cd /root/gl2/bin
for r in 1 2 3; do for arm in plain v2; do
  PRE=; [ $arm = v2 ] && PRE="LD_PRELOAD=/root/afp2/libs31fp.so S31FP_CACHE=/root/afp2/cache"
  env $PRE DISPLAY=:0 ./glxgears > /tmp/gg.out 2>&1 &
  G=$!; sleep 12; kill $G; sleep 1; kill -9 $G 2>/dev/null
  echo "r$r $arm $(grep -a FPS /tmp/gg.out | tail -2 | awk '{print $(NF-1)}' | tr '\n' ' ')"
done; done
env LD_PRELOAD=/root/afp2/libs31fp.so S31FP_CACHE= S31FP_DEBUG=1 DISPLAY=:0 ./glxgears --help 2>&1 | grep s31fp | head -2 &
sleep 3; for p in $(pidof glxgears); do kill -9 $p; done
echo GGAB_DONE
IN
setsid sh /root/afp2/ggab-inner.sh </dev/null >/dev/null 2>&1 &
echo GGAB_STARTED
