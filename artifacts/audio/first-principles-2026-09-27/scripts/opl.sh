# T1: OpenTyrian's synthesis loop (oplbench), libgcc vs libs31fp, CPU0 vs CPU1,
# plus the exec cost of the preload. Runs detached; results in
# /root/afp/opl.txt (~3 min). No audio device touched.
cat > /root/afp/opl-inner.sh <<'IN'
cd /root/afp
M=/root/oty/usr/share/opentyrian/data/music.mus
P=/root/afp/libs31fp.so
echo "== uname $(uname -v)"
echo "== routines libgcc CPU0"; ./oncpu 1 ./oplbench -r
echo "== routines s31fp CPU0"; LD_PRELOAD=$P S31FP_DEBUG=1 ./oncpu 1 ./oplbench -r
echo "== routines libgcc CPU1"; ./oncpu 2 ./oplbench -r
for s in 36 0 9 5; do
 echo "== song $s libgcc CPU0"; ./oncpu 1 ./oplbench $M $s 8
 echo "== song $s s31fp CPU0"; LD_PRELOAD=$P ./oncpu 1 ./oplbench $M $s 8
done
echo "== song 36 libgcc CPU1"; ./oncpu 2 ./oplbench $M 36 8
echo "== song 36 s31fp CPU1"; LD_PRELOAD=$P ./oncpu 2 ./oplbench $M 36 8
echo "== exec cost: 10 x sdltone1 usage-exit (libSDL+libc), plain then preload"
t0=$(cut -d' ' -f1 /proc/uptime)
for i in 1 2 3 4 5 6 7 8 9 10; do ./sdltone1 2>/dev/null; done
t1=$(cut -d' ' -f1 /proc/uptime)
for i in 1 2 3 4 5 6 7 8 9 10; do LD_PRELOAD=$P ./sdltone1 2>/dev/null; done
t2=$(cut -d' ' -f1 /proc/uptime)
echo "exec10 t0 $t0 plain_end $t1 preload_end $t2"
echo OPL_DONE
IN
setsid sh /root/afp/opl-inner.sh </dev/null >/root/afp/opl.txt 2>&1 &
echo OPL_STARTED
