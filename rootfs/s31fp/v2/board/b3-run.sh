# s31fp v2 preload CANDIDATE on the board (launcher). TEST-ONLY: LD_PRELOAD on
# our own test programs and a few ordinary apps' usage/no-display exits, in a
# subshell; nothing installed. Cache in /root/afp2/cache (not the default).
# Results /root/afp2/b3.txt (~5 min).
cat > /root/afp2/b3-inner.sh <<'IN'
cd /root/afp2
M=/root/afp2/music.mus; C=/root/afp2/oncpu; P=/root/afp2/libs31fp.so; P1=/root/afp2/libs31fp1.so
K=/root/afp2/cache; rm -rf $K
echo "== uname $(uname -v)"
echo "== 1. patch path: first launch (scan), second (cache hit), debug"
env LD_PRELOAD=$P S31FP_CACHE=$K S31FP_DEBUG=1 ./ptest-dyn check mul 1 2>&1 | head -2
env LD_PRELOAD=$P S31FP_CACHE=$K S31FP_DEBUG=1 ./ptest-dyn check mul 1 2>&1 | head -2
ls -l $K
echo "== 2. hammer: 300 fresh processes, each patches 14 entries then checks 1000 sets through them"
bad=0; ok=0
for i in $(seq 1 300); do
  case $((i % 3)) in 0) m=1;; 1) m=2;; 2) m=3;; esac
  case $((i % 2)) in 0) cd_=$K;; 1) cd_=;; esac
  o=$(env LD_PRELOAD=$P S31FP_CACHE=$cd_ $C $m ./ptest-dyn check $(echo mul add sub div ge le eq unord fltsi fltun fixsi fixun ext trunc | cut -d' ' -f$((i % 14 + 1))) 1 $i 2>&1 | grep RESULT)
  case "$o" in *" 0 v2 mismatches"*) ok=$((ok+1));; *) bad=$((bad+1)); echo "BAD $i: $o";; esac
done
echo "hammer ok=$ok bad=$bad"
echo "== 3. patched entry points on silicon: 200k sets per helper"
i=0; for op in mul add sub div ge le eq unord fltsi fltun fixsi fixun ext trunc; do i=$((i+1))
  env LD_PRELOAD=$P S31FP_CACHE=$K $C 1 ./ptest-dyn check $op 200 $((i+1200)) | grep RESULT | cut -c1-140; done
echo "== 4. audio through the trampoline (hash must equal oplbench-lg's)"
$C 1 ./oplbench-dyn $M 36 4 | tail -1
env LD_PRELOAD=$P S31FP_CACHE=$K $C 1 ./oplbench-dyn $M 36 4 | tail -1
env LD_PRELOAD=$P S31FP_CACHE=$K $C 1 ./oplbench-dyn $M 5 8 | tail -1
echo "== 5. missing library (does musl refuse to start the program?)"
env LD_PRELOAD=/nonexistent/libs31fp.so ./oplbench-dyn >/dev/null 2>/tmp/np.err; echo "exit $?"; head -c 200 /tmp/np.err; echo
echo "== 6. exec cost, 20 launches, ms/exec (busybox env included in every arm)"
ex() { lab=$1; shift; s=$(cut -d' ' -f1 /proc/uptime)
  for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do "$@" >/dev/null 2>&1; done
  e=$(cut -d' ' -f1 /proc/uptime); echo "$s $e" | awk -v l="$lab" '{printf "%-34s %7.1f ms/exec\n", l, ($2-$1)*1000/20}'; }
APPS="./oplbench-dyn ./sdltone1"
for a in /usr/bin/xcalc /usr/bin/xdpyinfo /usr/bin/glxgears /usr/bin/xclock; do [ -x $a ] && APPS="$APPS $a"; done
echo "apps: $APPS"
for r in 1 2; do for B in $APPS; do
  ex "r$r $B plain"     env -u DISPLAY $B
  ex "r$r $B v1"        env -u DISPLAY LD_PRELOAD=$P1 $B
  ex "r$r $B v2-scan"   env -u DISPLAY LD_PRELOAD=$P S31FP_CACHE= $B
  ex "r$r $B v2-cache"  env -u DISPLAY LD_PRELOAD=$P S31FP_CACHE=$K $B
done; done
for B in $APPS; do printf "%s: " $B; env -u DISPLAY LD_PRELOAD=$P S31FP_CACHE= S31FP_DEBUG=1 $B 2>&1 | grep s31fp: ; done
echo "== 7. scan cost of the stock app's text (file read only, not run)"
for f in /usr/bin/opentyrian /root/quake/sdlquake /root/doom/prboom; do [ -e $f ] && ./scanbench $f; done
ls -l $K | head
echo B3_DONE
IN
setsid sh /root/afp2/b3-inner.sh </dev/null >/root/afp2/b3.txt 2>&1 &
echo B3_STARTED
