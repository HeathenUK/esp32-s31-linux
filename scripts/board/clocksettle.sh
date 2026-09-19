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
# S30clock leaves /tmp/clock-stepped once ntpd has stepped this boot. That is
# the evidence to wait for: a restored stamp makes the year sane at boot while
# the real step is still to come, and a stopwatch started before it measures
# it. Without the marker (no network, or an older S30clock) fall back to the
# year test after 150 s so a board with no uplink can still be timed.
i=0
while [ $i -lt 48 ]; do
	Y=$(date +%Y)
	if [ -r /tmp/clock-stepped ] || { [ "$Y" -ge 2026 ] && [ $i -ge 30 ]; }; then
		A=$(date +%s); sleep 5; B=$(date +%s)
		D=$((B - A))
		if [ $D -ge 4 ] && [ $D -le 6 ]; then
			echo "clock settled: $(date '+%Y-%m-%d %H:%M:%S') (stepped at uptime $(cat /tmp/clock-stepped 2>/dev/null || echo '?')s, now $(cut -d. -f1 /proc/uptime)s)"
			exit 0
		fi
		echo "clock stepped mid-check (${D}s for 5s), retrying"
	fi
	i=$((i + 1))
	sleep 5
done
echo "clock NOT settled: year=$(date +%Y)"
exit 1
