# 0064 step 1: the sdlat bytes form works and O_DIRECT sticks.
#   python3 scripts/board/runsh.py artifacts/tracks/polled/board/00-sdlat-check.sh 60
# Pass: both lines print "min ... p50 ...". Kill: "read: Invalid argument"
# (the 'b' suffix was not honoured or alignment is wrong) or "O_DIRECT did
# not stick" - fix the tool before any pricing.
ls -la /root/sdlat
/root/sdlat /dev/mmcblk0 512b 50 rand
/root/sdlat /dev/mmcblk0 1024b 50 rand
/root/sdlat /dev/mmcblk0 4 50 rand
/root/sdlat /dev/mmcblk0 4k 50 rand
/root/sdlat /dev/mmcblk0 100b 5 rand 2>&1 | head -2   # must refuse (not a 512 multiple)
echo SDLAT_CHECK_DONE
