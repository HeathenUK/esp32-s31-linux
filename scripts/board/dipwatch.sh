#!/bin/sh
# dipwatch.sh <fps-log> <seconds> <out.txt> - board-side, fork-free dip attributor.
#
# One line per second while the game runs, every column a counter the kernel
# already keeps, read with shell builtins so the sampler costs nothing the
# game can feel (the same rule as pagewatch.sh: busybox applets fork, and a
# fork per second is a measurable tax here):
#
#   up  fps  busy%  idle%  game_majflt  lvdesk_majflt  sd_sectors_rd  swapfree_kB  hosted_irq
#
# fps comes from fpsonly.so's log (<fps-log>: "frames timestamp" per 10
# frames), so a dip shows as a low fps second AND, on the same line, what the
# machine was doing: idle% high = the game was WAITING (I/O, audio, a lock);
# idle 0 with majflt/sectors climbing = paging off the card; idle 0 with
# nothing else moving = CPU contention (cpushare.sh says with whom).
#
#   setsid sh /root/dipwatch.sh /root/doom/fps-x.txt 120 /root/dip.txt </dev/null >/dev/null 2>&1 &
FL=$1; SECS=${2:-120}; OUT=${3:-/root/dip.txt}
G=""; L=""
for p in /proc/[0-9]*; do read c < $p/comm 2>/dev/null; [ "$c" = prboom ] && G=${p#/proc/}; [ "$c" = lvdesk ] && L=${p#/proc/}; done
: > "$OUT"
echo "# up fps busy idle game_majflt lvdesk_majflt sd_rd_sectors swapfree_kB hosted_irq  (game pid ${G:-?}, lvdesk ${L:-?})" >> "$OUT"
read -r _ pu pn ps pi pw pq psq rest < /proc/stat
pf=0; [ -r "$FL" ] && { while read -r n t; do pf=$n; done < "$FL"; }
i=0
while [ $i -lt "$SECS" ]; do
	sleep 1; i=$((i+1))
	read -r _ u n s idle w q sq rest < /proc/stat
	tot=$(( (u-pu)+(n-pn)+(s-ps)+(idle-pi)+(w-pw)+(q-pq)+(sq-psq) ))
	busy=0; idp=0
	[ "$tot" -gt 0 ] && { idp=$(( (idle-pi)*100/tot )); busy=$((100-idp)); }
	pu=$u; pn=$n; ps=$s; pi=$idle; pw=$w; pq=$q; psq=$sq
	f=0; [ -r "$FL" ] && { while read -r nn tt; do f=$nn; done < "$FL"; }
	fps=$((f-pf)); pf=$f
	gm=0; [ -n "$G" ] && [ -r /proc/$G/stat ] && { read -r line < /proc/$G/stat; set -- ${line#*) }; gm=${10}; }
	lm=0; [ -n "$L" ] && [ -r /proc/$L/stat ] && { read -r line < /proc/$L/stat; set -- ${line#*) }; lm=${10}; }
	sd=0; [ -r /sys/block/mmcblk0/stat ] && { read -r r1 r2 sect rest < /sys/block/mmcblk0/stat; sd=$sect; }
	sf=0; while read -r k v u2; do [ "$k" = "SwapFree:" ] && sf=$v; done < /proc/meminfo
	hi=0; while read -r a b rest; do case "$a" in *:) case "$rest" in *hosted*|*wireless*) hi=$b;; esac;; esac; done < /proc/interrupts
	up=0; read -r up rest < /proc/uptime
	echo "${up%.*} $fps $busy $idp $gm $lm $sd $sf $hi" >> "$OUT"
done
