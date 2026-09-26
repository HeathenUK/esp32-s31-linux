# gl-arm.sh - one measurement arm of stock glxgears on the board, ON the board:
#   sh gl-arm.sh <name> <render_scale 0|1> <shmbufs 1|2> <fs|win> [runs] [lvdesk]
# Starts the desktop (default /root/lvdesk.new, else the given binary) with
# the boot environment, then per run: glxgears from SD (/root/gl2/bin, stock
# libs in /root/gl2/lib, libGL from $GLLIB or /usr/lib), 4 s warm-up, a 10 s
# window. Per run it prints the presents/s (xshim's ShmPutImage count, one per
# glxgears frame), the CPU% of glxgears and of the desktop over the window
# (/proc stat ticks), and for fullscreen the desktop's present-gap histogram
# delta (lvdesk fsg: <25 <50 <100 <200 <400 >=400 ms). Output goes to
# /root/glarm-<name>.txt; run it setsid and collect the file (~16 s a run).
# Fresh boot per arm is the caller's job. Discard run 1 (warm-up).
N=$1; RS=$2; NB=$3; MODE=$4; RUNS=${5:-6}; BIN=${6:-/root/lvdesk.new}
GL=${GLLIB:-}
exec > /root/glarm-$N.txt 2>&1
echo "ARM $N glpre=${GLPRE:-} render_scale=$RS shmbufs=$NB mode=$MODE runs=$RUNS bin=$BIN lib=${GL:-/usr/lib} lvenv=${LVENV:-} glenv=${GLENV:-} $(uname -v) up $(cut -d' ' -f1 /proc/uptime)"
# GLPRE="cmd": a board-side command run first (e.g. a module parameter for
# the arm, echo 0 > /sys/module/esp32s31_lcd/parameters/ppa_explore_max)
[ -n "${GLPRE:-}" ] && eval "$GLPRE"
killall lvdesk lvdesk.new lvdesk.ceil 2>/dev/null; for p in $(pidof glxgears); do kill -9 $p; done; sleep 1
[ -r /etc/lvdesk.env ] && . /etc/lvdesk.env
# LVENV="A=1 B=2": extra desktop environment for an arm (a runtime toggle);
# GLENV the same for the client (e.g. S31GL_HOLD=0)
[ -n "${LVENV:-}" ] && export $LVENV
LVDESK_CTL=1 setsid $BIN >/var/log/lvdesk.log 2>&1 </dev/null &
sleep 8
L=$(pidof $(basename $BIN))
FL=""; [ "$MODE" = fs ] && FL=-fullscreen
LP=/root/gl2/lib; [ -n "$GL" ] && LP=$GL:/root/gl2/lib
t() { awk '{print $14+$15}' /proc/$1/stat; }
# the put count at the moment of the signal, and that moment
puts() { pu=$(cut -d' ' -f1 /proc/uptime); kill -USR1 $L; sleep 1; echo "$(grep -a 'MIT-SHM ShmPutImage' /var/log/lvdesk.log | tail -n 1 | awk '{print $2}') $pu"; }
gaps() { grep -a 'lvdesk: frames' /var/log/lvdesk.log | tail -n 1 | sed 's/.*gaps //'; }
r=1
while [ $r -le $RUNS ]; do
	cd /root/gl2/bin
	setsid sh -c "exec env ${GLENV:-} S31GL_RENDER_SCALE=$RS S31GL_SHMBUFS=$NB LD_LIBRARY_PATH=$LP ./glxgears $FL >/tmp/glarm.out 2>&1" </dev/null >/dev/null 2>&1 &
	sleep 4
	G=$(pidof glxgears)
	p0=$(puts); g0a=$(gaps)
	g0=$(t $G); l0=$(t $L); u0=$(cut -d' ' -f1 /proc/uptime)
	sleep 10
	g1=$(t $G); l1=$(t $L); u1=$(cut -d' ' -f1 /proc/uptime)
	p1=$(puts); g1a=$(gaps)
	echo "RUN $r $(echo "$p1 $p0 $u1 $u0 $g1 $g0 $l1 $l0" | awk '{d=$5-$6; printf "fps %.1f gears %.0f%% lvdesk %.0f%%", ($1-$3)/($2-$4), ($7-$8)/d, ($9-$10)/d}') | gaps0 $g0a | gaps1 $g1a"
	grep -a "buffers\|FPS" /tmp/glarm.out | tail -n 3 | tr '\n' ' '; echo
	for p in $(pidof glxgears); do kill -9 $p; done
	sleep 2
	r=$((r+1))
done
grep -a "render scale\|fullscreen" /var/log/lvdesk.log | tail -n 6
free | head -2
echo DONE
