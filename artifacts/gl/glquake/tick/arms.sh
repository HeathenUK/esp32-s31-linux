cd /Users/gadyke/esp32-s31-linux
for arm in base:k-base hrtoff:k-hrtick ka0:k-ka0 base:k-base hrtoff:k-hrtick ka0:k-ka0; do
  L=tick-${arm%%:*}; W=/root/gq/${arm##*:}.sh
  echo "=== $L $W $(date +%H:%M:%S)"
  scripts/board/glquake-arm.sh $L 1 GQ_BASE=/root/quake/td GQ_WRAP=$W -- -mixspeed 11025 -zone 384 -heapsize 12288 -width 320 -height 240 -fullscreen 2>&1 | tail -4
  printf 'cat /root/gq/knob.txt\n' > /private/tmp/claude-501/-Users-gadyke-esp32-s31-linux/c6beafe5-9619-4090-a435-84f45be7fe21/scratchpad/knob.sh; python3 scripts/board/runsh.py /private/tmp/claude-501/-Users-gadyke-esp32-s31-linux/c6beafe5-9619-4090-a435-84f45be7fe21/scratchpad/knob.sh 30 | grep -a "^KNOB"
done
echo ALLDONE $(date +%H:%M:%S)
