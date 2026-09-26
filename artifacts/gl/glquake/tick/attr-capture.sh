# attribution: GLQuake fullscreen timedemo on the current boot; snapshots at t=60 and t=90 into tmpfs
mkdir -p /root/gq; D=/tmp/attr; rm -rf $D; mkdir -p $D
cat > /root/gq/attr-bg.sh <<'Y'
D=/tmp/attr
snap() {
  cat /proc/interrupts > $D/irq$1; grep -E '^(cpu|ctxt|intr|softirq)' /proc/stat > $D/stat$1; cat /proc/softirqs > $D/sirq$1
  awk 'FNR==1{split(FILENAME,a,"/");p=a[3];t=a[5]} /^Name:/{n=$2} /^voluntary_ctxt/{v=$2} /^nonvoluntary_ctxt/{print p,t,n,v,$2}' /proc/[0-9]*/task/[0-9]*/status > $D/ctx$1 2>/dev/null
  grep -E 'nr_events|^cpu:|^ #[0-9]+:' /proc/timer_list > $D/tl$1
  grep -E 'pswpin|pswpout|pgmajfault' /proc/vmstat > $D/vm$1
  cut -d' ' -f1 /proc/uptime > $D/up$1
}
echo "SF $(devmem 0x5008a114 32) SLICE $(devmem 0x5008a124 32) NSLICE $(devmem 0x5008a120 32)" > $D/sf
setsid sh /root/glquake-run.sh attr 420 -mixspeed 11025 -zone 384 -heapsize 12288 -width 320 -height 240 -fullscreen </dev/null >/dev/null 2>&1 &
sleep 60; snap A; sleep 10; grep -E '^ #[0-9]+:|^cpu:' /proc/timer_list > $D/tlA2; sleep 10; grep -E '^ #[0-9]+:|^cpu:' /proc/timer_list > $D/tlA3; sleep 10; snap B
P=$(pidof quakespasm); L=$(pidof lvdesk lvdesk.new)
for p in $P $L $(pidof s31-bt wpa_supplicant bluetoothd); do echo "$p $(cat /proc/$p/comm) slack=$(cat /proc/$p/timerslack_ns 2>/dev/null) aff=$(grep Cpus_allowed_list /proc/$p/status)"; done > $D/slack
echo done > $D/done
Y
setsid sh /root/gq/attr-bg.sh </dev/null >/dev/null 2>&1 &
echo FIRED $(uname -v) up $(cut -d' ' -f1 /proc/uptime)
