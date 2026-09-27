set -eu
D=/root/afp2/pb-vdso-release-quiet
cat "$D/summary.txt"
printf 'TYRIAN_UNDERRUN_REPORTS '
grep -ac 'underrun' "$D/tyrian.log" || true
printf 'ACTIVE_DESKTOP '
pidof lvdesk
md5sum /opt/s31/libs31fp.so
cat /etc/s31fp.env
grep -q 'PB_DONE runs=1 failures=0' "$D/summary.txt"
! grep -q 'WATCHDOG\|WARMUP_FAILED' "$D/summary.txt"
echo S31_VDSO_RELEASE_PASS
