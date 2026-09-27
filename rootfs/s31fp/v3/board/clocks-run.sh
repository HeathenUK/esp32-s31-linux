# Kernel time64 vDSO diagnostic. Run through runsh.py with a 100s deadline.
# Stop applications first; no other board observer may run concurrently.
set -eu
D=${V3_DIR:-/root/s31vdso}
C=${V3_ONCPU:-/root/afp2/oncpu}
[ -x "$D/clktest" ] && [ -r "$D/libs31fp.so" ] && [ -x "$C" ]
uname -a
md5sum $D/libs31fp.so $D/clktest
# Isolated diagnostic: no application workload running alongside it.
killall lvdesk 2>/dev/null || true
export LD_PRELOAD=$D/libs31fp.so S31FP=0 S31STR=0 S31CLK=1
S31FP_DEBUG=1 $D/clktest abi
for mask in 1 2; do
    echo "CLOCK_CPU_MASK=$mask"
    "$C" "$mask" $D/clktest abi
    "$C" "$mask" $D/clktest cost 200000
done
$D/clktest mono 35 3
$D/clktest drift 5
$D/clktest step
S31CLK=0 $D/clktest abi
# Also exercise new clock with the shipping copy and string policy.
S31FP=1 S31STR=1 S31FP_COPY=1 $D/clktest abi
echo CLOCK_BOARD_PASS
