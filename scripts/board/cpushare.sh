#!/bin/sh
# cpushare.sh <seconds> <out.txt> - board-side, fork-free CPU attribution.
#
# WHY. GrieferPig runs Linux SMP with the radio blobs as kthreads. Whether that
# is worth porting here hinges on ONE number: how much CPU everything that is
# NOT the game burns while the game runs. Below ~15% a second hart buys the
# game nothing it can feel (and shares the same 13.6 MB/s PSRAM); above ~30%
# it is the next big project. `top` cannot answer it - it samples an instant
# and forks per screen. This reads every task's utime+stime and /proc/stat
# twice, <seconds> apart, with shell builtins only, and prints the delta per
# task plus the system totals. Run it under setsid with output to a file while
# the workload runs (verify-sdl.sh fires prboom that way), then read the file.
#
#   setsid sh /root/cpushare.sh 60 /root/cpushare.txt </dev/null >/dev/null 2>&1 &
#
# Output: one line per task that used any CPU: "ticks comm pid", then totals
# from /proc/stat (user nice system idle iowait irq softirq) as deltas, then
# the idle share. USER_HZ ticks; only ratios matter.
secs=${1:-60}; out=${2:-/root/cpushare.txt}
snap() {
	# $1 = file. Builtins only: read each /proc/<pid>/stat, take fields 14+15.
	: > "$1"
	for d in /proc/[0-9]*; do
		[ -r "$d/stat" ] || continue
		read -r line < "$d/stat" || continue
		# comm may contain spaces; it is bracketed, so strip up to ") "
		rest=${line#*) }
		set -- $rest
		# after ')' the fields are state(3) ppid ... utime is field 14
		# overall, i.e. the 12th of $rest; stime the 13th
		ut=${12}; st=${13}
		comm=${line#* (}; comm=${comm%%)*}
		echo "${d#/proc/} $comm $((ut + st))" >> "$1"
	done
	read -r c u n s i w q sq rest < /proc/stat
	echo "STAT $u $n $s $i $w $q $sq" >> "$1"
}
snap /tmp/cs_a
sleep "$secs"
snap /tmp/cs_b
{
	echo "# cpushare over ${secs}s, USER_HZ ticks"
	# join by pid: tasks alive at both ends
	while read -r pid comm t1; do
		[ "$pid" = STAT ] && continue
		t0=$(awk -v p="$pid" '$1==p {print $3; exit}' /tmp/cs_a)
		[ -n "$t0" ] || t0=0
		d=$((t1 - t0))
		[ "$d" -gt 0 ] && echo "$d $comm $pid"
	done < /tmp/cs_b | sort -rn
	a=$(grep ^STAT /tmp/cs_a); b=$(grep ^STAT /tmp/cs_b)
	set -- $a; au=$2; an=$3; as=$4; ai=$5; aw=$6; aq=$7; asq=$8
	set -- $b; bu=$2; bn=$3; bs=$4; bi=$5; bw=$6; bq=$7; bsq=$8
	du=$((bu-au)); dn=$((bn-an)); ds=$((bs-as)); di=$((bi-ai)); dw=$((bw-aw)); dq=$((bq-aq)); dsq=$((bsq-asq))
	tot=$((du+dn+ds+di+dw+dq+dsq))
	echo "TOTAL user=$du nice=$dn system=$ds idle=$di iowait=$dw irq=$dq softirq=$dsq all=$tot"
	[ "$tot" -gt 0 ] && echo "IDLE_PERCENT $((di * 100 / tot))  BUSY_PERCENT $(((tot - di) * 100 / tot))"
} > "$out"
rm -f /tmp/cs_a /tmp/cs_b
