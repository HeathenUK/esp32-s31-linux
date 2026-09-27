#!/bin/bash
# arms.sh "label:ENV..." ...  - each arg one fresh-boot arm; ENV is passed to pre.sh
cd /Users/gadyke/esp32-s31-linux
A="-mixspeed 11025 -zone 384 -heapsize 12288 -width 320 -height 240 -fullscreen"
S=/private/tmp/claude-501/-Users-gadyke-esp32-s31-linux/c6beafe5-9619-4090-a435-84f45be7fe21/scratchpad
for spec in "$@"; do
  L=${spec%%:*}; E=${spec#*:}
  out=$(scripts/board/glquake-arm.sh $L 1 GQ_BASE=/root/quake/td "GQP=1; $E sh /root/gq/pre.sh $L" -- $A 2>&1)
  D=$(echo "$out" | tail -1)
  if ! grep -q FIRED $D/fired.txt; then echo "RETRY $L (no FIRED)"; rm -rf $D; out=$(scripts/board/glquake-arm.sh $L 1 GQ_BASE=/root/quake/td "GQP=1; $E sh /root/gq/pre.sh $L" -- $A 2>&1); D=$(echo "$out" | tail -1); fi
  printf 'cat /root/gq/%s.pre; echo SNAPBEGIN; cat /root/gq/%s.snap; echo SNAPEND\n' $L $L > $S/c-$L.sh
  python3 scripts/board/runsh.py $S/c-$L.sh 60 | tr -d '\r' | sed -n '6,400p' | grep -av 'RS_DONE\|Done\|^~' > $D/snap.txt
  echo "ARM $L [$E] $(echo "$out" | grep RESULT) | $(grep -a 'swappiness\|page-cluster\|min_free\|Priority' -A0 $D/snap.txt | tr '\n' ' ' | head -c 200) | $D"
done
echo ALLDONE
