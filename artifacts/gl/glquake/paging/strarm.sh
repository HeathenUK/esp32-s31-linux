#!/bin/bash
# strarm.sh <label> [MINFREE] : fresh boot, stream.sh, collect
cd /Users/gadyke/esp32-s31-linux; S=/private/tmp/claude-501/-Users-gadyke-esp32-s31-linux/c6beafe5-9619-4090-a435-84f45be7fe21/scratchpad
L=$1; MF=${2:-}
O=artifacts/gl/glquake/arms/stream-$L-$(date +%m%d-%H%M%S); mkdir -p $O
python3 scripts/board/reset.py > $O/reset.log 2>&1
printf 'rm -f /root/gq/stream-%s.txt; MINFREE=%s setsid sh /root/gq/stream.sh %s </dev/null >/dev/null 2>&1 &\necho FIRED $(uname -v) up $(cut -d" " -f1 /proc/uptime)\n' $L "$MF" $L > $S/sf.sh
python3 scripts/board/runsh.py $S/sf.sh 60 60 | grep -a FIRED > $O/fired.txt; cat $O/fired.txt
printf 'i=0; while [ $i -lt 30 ]; do grep -q STREAMDONE /root/gq/stream-%s.txt 2>/dev/null && break; sleep 5; i=$((i+1)); done; cat /root/gq/stream-%s.txt\n' $L $L > $S/sw.sh
python3 scripts/board/runsh.py $S/sw.sh 200 | tr -d '\r' | sed -n '/^STREAM /,/^STREAMDONE/p' > $O/stream.txt
cat $O/stream.txt; echo $O
