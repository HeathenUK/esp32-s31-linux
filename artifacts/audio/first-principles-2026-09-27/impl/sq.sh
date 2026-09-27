# sdlquake timedemo demo1 exactly as regress-quick.sh invokes it (fullscreen
# 320x240, sound on at DAC 110, restored to 143) - QUIET: no polling at all
# while the game runs. One `sleep` process for the whole demo, then read the
# result and kill the game. (A 1-2 s grep/sleep loop during the demo is a
# workload of its own on this board - artifacts/gl/dips/README.md.)
# The s31route debug line is printed by the plugin itself at open.
#   setsid sh /root/afp/sq.sh <label> <secs> [ENV=VAL] </dev/null >/dev/null 2>&1 &
L=$1; SECS=$2; shift 2; O=/root/afp/sq-$L.txt; exec > $O 2>&1
echo "SQ $L $(uname -v) up $(cut -d' ' -f1 /proc/uptime) env $*"
amixer -q sset DACL 110 2>/dev/null; amixer -q sset DACR 110 2>/dev/null
# label o*: the pre-2026-09-27 plugin (6b781685) through a temporary .asoundrc
case $L in o*) printf 'pcm_type.s31route { lib "/root/afp/s31route-old.so" }\n' > /root/.asoundrc; echo "old plugin via /root/.asoundrc";; esac
cd /root/quake
env DISPLAY=:0 S31ROUTE_DEBUG=1 "$@" ./sdlquake -basedir /root/quake -winsize 320 240 -fullscreen +timedemo demo1 > /tmp/sq.out 2>&1 &
Q=$!
sleep $SECS
echo "SDLQUAKE $(grep -a 'frames' /tmp/sq.out | tail -1) (read at ${SECS}s, alive $([ -d /proc/$Q ] && echo 1 || echo 0))"
grep -a s31route /tmp/sq.out | head -3
kill $Q 2>/dev/null; sleep 1; kill -9 $Q 2>/dev/null
amixer -q sset DACL 143 2>/dev/null; amixer -q sset DACR 143 2>/dev/null
rm -f /root/.asoundrc
echo SQDONE
