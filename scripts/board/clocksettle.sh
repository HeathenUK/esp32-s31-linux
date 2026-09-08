# Wait until the wall clock has stopped moving under us.
#
# prboom measures a timedemo in realtics off the wall clock. The board boots at
# the epoch and NTP steps it seconds-to-minutes later, once Wi-Fi associates -
# so a demo fired at the login prompt gets the step charged to its own run time
# and reports nonsense. It is not subtle when it happens (5026 gametics in
# 10,678,186 realtics = 0.0 fps) but it looks exactly like a catastrophic
# regression, which is worse than looking like a bug.
#
# Settled means: the year is sane (NTP has stepped at least once) AND two reads
# five seconds apart differ by five seconds, so no further step is in flight.
i=0
while [ $i -lt 24 ]; do
	Y=$(date +%Y)
	if [ "$Y" -ge 2026 ]; then
		A=$(date +%s); sleep 5; B=$(date +%s)
		D=$((B - A))
		if [ $D -ge 4 ] && [ $D -le 6 ]; then
			echo "clock settled: $(date '+%Y-%m-%d %H:%M:%S')"
			exit 0
		fi
		echo "clock stepped mid-check (${D}s for 5s), retrying"
	fi
	i=$((i + 1))
	sleep 5
done
echo "clock NOT settled: year=$(date +%Y)"
exit 1
