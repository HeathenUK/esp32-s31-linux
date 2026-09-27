#!/bin/bash
# ab.sh <seq e.g. "B A A B B A A B B A"> : GLQuake fullscreen timedemo, one run
# per fresh boot, A = #393 (images/ship-393-xipImage), B = #398 (0073 list,
# images/kf-398-xipImage), C = cold control if images/kf-cold-399-xipImage exists.
# Reflashes only on an arm change. Log: artifacts/gl/glquake/kfast/ab.log
cd "$(dirname "$0")/../../../.."
LOG=artifacts/gl/glquake/kfast/ab.log
cur=${CUR:-B}; n=${START:-1}
for x in $1; do
	case $x in A) img=images/ship-393-xipImage;; B) img=images/kf-398-xipImage;; C) img=images/kf-cold-399-xipImage;; esac
	if [ "$x" != "$cur" ]; then
		cp $img images/xipImage
		make flash-linux 2>&1 | grep -aE "Hash of data|ERROR|rror" >> $LOG
		cur=$x
	fi
	echo "=== arm $n $x $img $(date +%T)" >> $LOG
	scripts/board/glquake-arm.sh kf-$x$n 1 GQ_BASE=/root/quake/td GQ_CC=1 -- -mixspeed 11025 -zone 384 -heapsize 12288 -width 320 -height 240 -fullscreen >> $LOG 2>&1
	n=$((n+1))
done
echo ABDONE >> $LOG
