#!/usr/bin/env python3
"""Measure observer effects on a dedicated desktop, with unchanged SDL clients.

All arms launch the same background benchmark after the same delay. The
intervention is board-side and begins three seconds after SDL reports ready. This measures
command/output and capture workloads, not the cost of merely reading an idle
serial port. No hardware capture is advertised as cost-free.
"""
import argparse
import hashlib
import json
from pathlib import Path
import time
from sdlbench import script_run, ROOT

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--frames',type=int,default=360)
p.add_argument('--repeats',type=int,default=2)
p.add_argument('--output',type=Path)
a=p.parse_args()
if not 60<=a.frames<=1000 or not 1<=a.repeats<=10: p.error('frames 60..1000; repeats 1..10')
out=a.output or ROOT/'artifacts'/'sdlbench'/('observer-'+time.strftime('%Y%m%d-%H%M%S'))
out.mkdir(parents=True,exist_ok=True)
gate=(ROOT/'scripts/board/sdlbench-settle.sh').read_text()
installed=script_run("cat > /root/sdlbench-settle.sh <<'SDLBENCH_GATE_EOF'\n"+gate+"\nSDLBENCH_GATE_EOF\necho GATE_INSTALLED\n",35)
if 'GATE_INSTALLED' not in installed: raise SystemExit('settle gate upload failed')
records=[]
arms={
    'quiet': ':',
    'console_commands': 'i=0; while [ "$i" -lt 16 ]; do uname -a; cat /proc/meminfo; ps; i=$((i+1)); done',
    'jpeg_capture': '$REC /root/sdlbench-observer.mjpeg 2 2 85 512 >/root/sdlbench-capture.log 2>&1',
    'jpeg_capture_serial': '$REC /root/sdlbench-observer.mjpeg 2 2 85 512 >/root/sdlbench-capture.log 2>&1 && base64 /root/sdlbench-observer.mjpeg',
}
failed=False
for rep in range(a.repeats):
    for arm in (list(arms) if rep%2==0 else list(reversed(arms))):
        ident=f'{arm}-r{rep+1}'
        print(f'{ident}: measuring controlled observer workload',flush=True)
        script=f'''/etc/init.d/S40lvdesk restart >/root/sdlbench-desktop-start.log 2>&1
sleep 3
export SDLBENCH_LVDESK_PID=$(pidof lvdesk)
[ -n "$SDLBENCH_LVDESK_PID" ] || {{ echo NO_COMPOSITOR; exit 1; }}
REC=/root/mjpegrec
[ -x "$REC" ] || REC=/usr/bin/mjpegrec
[ -x "$REC" ] || {{ echo NO_RECORDER; exit 1; }}
# STOP retains the ring. Prime every arm equally before timing so a late
# quiet arm does not inherit a different allocation from an earlier capture.
$REC /root/sdlbench-observer-prime.mjpeg 1 1 85 512 >/root/sdlbench-prime.log 2>&1
sh /root/sdlbench-settle.sh 60 || {{ echo UNSETTLED; exit 1; }}
carrier_before=$(cat /sys/class/net/wlan0/carrier_changes 2>/dev/null || echo unknown)
env DISPLAY=:0 SDLBENCH_START_DELAY_MS=2000 /root/sdlbench1 --case indexed_frame --frames {a.frames} --warmup 5 > /root/sdlbench-observer.jsonl 2>&1 &
bench_pid=$!
(attempt=0
 # Poll only during initialization / the benchmark's start delay. Stop polling
 # before timing, then delay until the primed loop is in flight.
 until grep -q '"type":"subsystems"' /root/sdlbench-observer.jsonl; do
   kill -0 "$bench_pid" 2>/dev/null || exit 1
   attempt=$((attempt+1))
   [ "$attempt" -lt 20 ] || exit 1
   sleep 1
 done
 sleep 3
 echo OBSERVER_BEGIN
 cat /proc/uptime
 {arms[arm]}
 observer_rc=$?
 cat /proc/uptime
 echo OBSERVER_END_$observer_rc
) &
observer_pid=$!
wait "$bench_pid"
rc=$?
wait "$observer_pid"
carrier_after=$(cat /sys/class/net/wlan0/carrier_changes 2>/dev/null || echo unknown)
[ "$carrier_before" = "$carrier_after" ] || rc=91
[ "$(wpa_cli -i wlan0 status 2>/dev/null | sed -n 's/^wpa_state=//p')" = COMPLETED ] || rc=92
echo NETWORK_AFTER carrier_before=$carrier_before carrier_after=$carrier_after
sleep 1
kill -0 "$SDLBENCH_LVDESK_PID" 2>/dev/null || rc=90
echo BENCH_RESULT_BEGIN
cat /root/sdlbench-observer.jsonl
echo BENCH_RESULT_END_$rc
md5sum /root/sdlbench-observer.jsonl
'''
        raw=script_run(script,220)
        (out/f'{ident}.log').write_text(raw)
        body=raw.replace('\r','').split('BENCH_RESULT_BEGIN\n',1)[-1].split('BENCH_RESULT_END_',1)[0]
        run=[]
        for line in body.splitlines():
            if not line.startswith('{'): continue
            item=json.loads(line)
            item.update(run=ident,variant=arm,repeat=rep+1)
            run.append(item); records.append(item)
        transfer_ok=hashlib.md5(body.encode()).hexdigest()+'  /root/sdlbench-observer.jsonl' in raw
        ok=transfer_ok and 'BENCH_RESULT_END_0' in raw and 'OBSERVER_END_0' in raw and any(r.get('type')=='done' and r.get('failures')==0 for r in run)
        records.append(dict(type='run_status',run=ident,variant=arm,repeat=rep+1,ok=ok))
        failed |= not ok
        with (out/'results.jsonl').open('w') as f:
            for item in records: f.write(json.dumps(item)+'\n')
        print(f'{ident}: {"PASS" if ok else "FAIL"}',flush=True)
        if not ok: raise SystemExit('Observer trial failed; inspect raw log before further trials')
print(out)
raise SystemExit(int(failed))
