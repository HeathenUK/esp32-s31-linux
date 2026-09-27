set -eu
[ "$(md5sum /root/s31vdso/libs31fp.so | cut -d ' ' -f 1)" = 9767181c1c32d35d8eabc09108f87eff ]
cp /opt/s31/libs31fp.so /root/s31fp-release-backup/libs31fp-before-clock.so
cp /etc/s31fp.env /root/s31fp-release-backup/s31fp-before-clock.env
cp /root/s31vdso/libs31fp.so /opt/s31/libs31fp.so.new
chmod 755 /opt/s31/libs31fp.so.new
mv /opt/s31/libs31fp.so.new /opt/s31/libs31fp.so
cat > /etc/s31fp.env.new <<'IN'
# SD-backed s31fp; copied arithmetic executes in application RAM.
# Common clocks use the kernel time64 vDSO; S31CLK=0 restores libc.
if [ -r /opt/s31/libs31fp.so ]; then
    export LD_PRELOAD=/opt/s31/libs31fp.so
    export S31FP_COPY=${S31FP_COPY:-1}
    export S31STR=${S31STR:-1}
    export S31CLK=${S31CLK:-1}
fi
IN
mv /etc/s31fp.env.new /etc/s31fp.env
sync
unset S31FP S31STR S31CLK LD_PRELOAD
. /etc/s31fp.env
S31FP_DEBUG=1 /root/s31vdso/clktest abi
md5sum /opt/s31/libs31fp.so
cat /etc/s31fp.env
echo FAST_CLOCK_INSTALLED
