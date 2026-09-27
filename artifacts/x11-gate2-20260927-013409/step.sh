for p in $(pidof opentyrian) $(pidof chocolate-doom) $(pidof uinject); do kill -9 $p; done
amixer -q sset 'DACL' 143 2>/dev/null; amixer -q sset 'DACR' 143 2>/dev/null
echo CLEAN
