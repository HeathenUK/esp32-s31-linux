# prboom SIGSEGV under copy-in-place: which mechanism? 25 s per arm. /root/afp2/pb.txt
cat > /root/afp2/pb-inner.sh <<'IN'
exec > /root/afp2/pb.txt 2>&1
P=/root/afp2/libs31fp.so; K=/root/afp2/cachepb; ls /root/segvtrap.so
cd /root/doom
for arm in copycol copynocol tramp copycol; do
  case $arm in copycol) E="";; copynocol) E="S31FP_COLOUR=0";; tramp) E="S31FP_COPY=0";; esac
  rm -f /tmp/pb.log
  env LD_PRELOAD="$P /root/segvtrap.so" S31FP_CACHE=$K S31FP_DEBUG=1 $E DISPLAY=:0 ./prboom -width 320 -height 200 -window -timedemo demo1 > /tmp/pb.log 2>&1 &
  G=$!; sleep 25; kill -9 $G 2>/dev/null; sleep 1; for p in $(pidof prboom); do kill -9 $p; done
  echo "== $arm: $(grep -a -c 'signal 11' /tmp/pb.log) segv lines"; grep -a -E "s31fp|segvtrap|pc|PC" /tmp/pb.log | head -12
done
echo PB_DONE
IN
setsid sh /root/afp2/pb-inner.sh </dev/null >/dev/null 2>&1 &
echo PB_STARTED
