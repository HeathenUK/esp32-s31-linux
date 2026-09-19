#!/usr/bin/env python3
"""Quiet, bounded stock-SDL benchmark runner. Deploy before timing; fetch after.

Build: ./docker/build.sh 'sh rootfs/build-sdlbench.sh'
Run:   python3 scripts/board/sdlbench.py --deploy --suite quick --repeats 3
No screenshots, polling commands, resets, or platform configuration changes.
"""
import argparse
import hashlib
import json
import pathlib
import shlex
import subprocess
import tempfile
import time
import runsh

ROOT = pathlib.Path(__file__).resolve().parents[2]

def script_run(script, timeout):
    with tempfile.NamedTemporaryFile('w', suffix='.sh') as f:
        f.write(script)
        f.flush()
        return runsh.run(f.name, timeout=timeout)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--deploy', action='store_true')
    p.add_argument('--isolate', action='store_true', help='restart the desktop before each run; use only on a dedicated test session')
    p.add_argument('--suite', choices=['quick','threads','profiles','extended','full','games','stability','controls'], default='quick')
    p.add_argument('--case', help='run one named case instead of a suite')
    p.add_argument('--sdl', type=int, choices=[1,2], default=1, help='library version for --case')
    p.add_argument('--timers', action='store_true', help='enable optional SDL timer subsystem for --case')
    p.add_argument('--require-futex', action='store_true', help='fail if either futex syscall probe reports missing support')
    p.add_argument('--repeats', type=int, default=3)
    p.add_argument('--frames', type=int, default=30)
    p.add_argument('--output', type=pathlib.Path)
    a = p.parse_args()
    if a.timers and not a.case:
        p.error('--timers requires --case; suites define their own subsystem flags')
    if not 1 <= a.frames <= 4096 or not 1 <= a.repeats <= 20:
        p.error('frames 1..4096, repeats 1..20')
    outdir=a.output or ROOT/'artifacts'/'sdlbench'/time.strftime('%Y%m%d-%H%M%S')
    outdir.mkdir(parents=True, exist_ok=True)
    if a.deploy:
        for v in (1,2):
            # Retain a deployment log and independently verify the actual
            # destination binaries before timing.
            r=subprocess.run(['python3',str(ROOT/'scripts/board/deploy.py'),str(ROOT/f'rootfs/sdlbench{v}.bin'),f'/root/sdlbench{v}'],text=True,capture_output=True)
            (outdir/f'deploy{v}.log').write_text(r.stdout+r.stderr)
        checks='\n'.join(f'md5sum /root/sdlbench{v}' for v in (1,2))
        result=script_run(checks,30)
        for v in (1,2):
            digest=hashlib.md5((ROOT/f'rootfs/sdlbench{v}.bin').read_bytes()).hexdigest()
            if f'{digest}  /root/sdlbench{v}' not in result:
                raise SystemExit(f'deployment checksum failed: SDL{v}')
    # Gate is uploaded once, outside timed intervals.
    gate=(ROOT/'scripts/board/sdlbench-settle.sh').read_text()
    installed=script_run("cat > /root/sdlbench-settle.sh <<'SDLBENCH_GATE_EOF'\n"+gate+"\nSDLBENCH_GATE_EOF\necho GATE_INSTALLED\n",35)
    if 'GATE_INSTALLED' not in installed: raise SystemExit('settle gate upload failed')
    inventory=script_run('uname -a\ncat /proc/cmdline /proc/meminfo\nps\nmd5sum /root/sdlbench1 /root/sdlbench2 /usr/bin/lvdesk /usr/lib/libSDL-1.2.so.0 /usr/lib/libSDL2-2.0.so.0\ncat /etc/lvdesk.env 2>/dev/null\n',35)
    (outdir/'inventory.log').write_text(inventory)
    jobs=[]
    if a.case:
        extra=['--case',a.case]+(['--timers'] if a.timers else [])
        jobs=[(f'sdl{a.sdl}-{a.case}',a.sdl,{},extra)]
    elif a.suite=='threads':
        jobs=[('sdl1-threads',1,{},['--case','threads']),('sdl2-threads',2,{},['--case','threads'])]
    elif a.suite=='controls':
        jobs=[('sdl1-indexed-timers',1,{},['--case','indexed_frame','--timers']),
              ('sdl1-indexed',1,{},['--case','indexed_frame']),
              ('sdl2-indexed-timers',2,{},['--case','indexed_frame','--timers']),
              ('sdl1-threads',1,{},['--case','threads']),
              ('sdl2-threads',2,{},['--case','threads'])]
    elif a.suite=='stability':
        jobs=[('sdl1-indexed-timers',1,{},['--case','indexed_frame','--timers']),
              ('sdl1-indexed',1,{},['--case','indexed_frame'])]
    elif a.suite=='games':
        jobs=[('doom-direct',1,{},['--case','doom_frame']),
              ('tyrian-d8',1,{},['--case','tyrian_frame']),
              ('tyrian-d32',1,{'SDLBENCH_DEPTH':'32'},['--case','tyrian_frame']),
              ('sdl2-indexed',2,{},['--case','indexed_frame']),
              ('sdl1-idle',1,{},['--case','idle_wait']),
              ('sdl1-idle-timers',1,{},['--case','idle_wait','--timers'])]
    elif a.suite=='profiles':
        for count in (0,16,64,128):
            for case in ('sprite_cpu','tyrian_frame'):
                jobs.append((f'sdl1-{case}-{count}',1,{'SDLBENCH_SPRITES':str(count)},['--case',case]))
    else:
        jobs=[('sdl1-d8',1,{'SDLBENCH_DEPTH':'8'},[]),
              ('sdl1-d16',1,{'SDLBENCH_DEPTH':'16'},[]),
              ('sdl1-d32',1,{'SDLBENCH_DEPTH':'32'},[]),
              ('sdl2-render',2,{},[]),
              ('sdl2-surface',2,{'SDLBENCH_SURFACE':'1'},[])]
        if a.suite in ('full','extended'):
            if a.suite=='extended': jobs=[]
            jobs += [('sdl1-fullscreen',1,{},['--fullscreen']),
                     ('sdl2-fullscreen',2,{},['--fullscreen']),
                     ('sdl2-scale640',2,{},['--width','640','--height','400']),
                     ('sdl1-audio',1,{},['--audio']),
                     ('sdl2-audio',2,{},['--audio']),
                     ('sdl1-no-mitshm',1,{'XLITE_NOMITSHM':'1'},[])]
    records=[]
    bad=False
    for rep in range(a.repeats):
        # Alternate order to expose order/warming effects without random noise.
        for label,version,env,extra in (jobs if rep%2==0 else list(reversed(jobs))):
            ident=f'{label}-r{rep+1}'
            command=['env','DISPLAY=:0']+[f'{k}={v}' for k,v in env.items()]+[f'/root/sdlbench{version}','--frames',str(a.frames),'--warmup','5']+extra
            cmd=shlex.join(command)
            isolate='/etc/init.d/S40lvdesk restart >/root/sdlbench-desktop-start.log 2>&1\nsleep 3\n' if a.isolate else ''
            script=isolate+f'''export SDLBENCH_LVDESK_PID=$(pidof lvdesk)
[ -n "$SDLBENCH_LVDESK_PID" ] || {{ echo NO_COMPOSITOR; exit 1; }}
sh /root/sdlbench-settle.sh 60 || {{ echo UNSETTLED; exit 1; }}
carrier_before=$(cat /sys/class/net/wlan0/carrier_changes 2>/dev/null || echo unknown)
{cmd} > /root/sdlbench-result.jsonl 2>&1
rc=$?
carrier_after=$(cat /sys/class/net/wlan0/carrier_changes 2>/dev/null || echo unknown)
[ "$carrier_before" = "$carrier_after" ] || rc=91
[ "$(wpa_cli -i wlan0 status 2>/dev/null | sed -n 's/^wpa_state=//p')" = COMPLETED ] || rc=92
echo NETWORK_AFTER carrier_before=$carrier_before carrier_after=$carrier_after
sleep 1
kill -0 "$SDLBENCH_LVDESK_PID" 2>/dev/null || rc=90
echo BENCH_RESULT_BEGIN
cat /root/sdlbench-result.jsonl
echo BENCH_RESULT_END_$rc
md5sum /root/sdlbench-result.jsonl
echo DESKTOP_LOG_BEGIN
cat /var/log/lvdesk.log
echo DESKTOP_LOG_END
'''
            print(f'{ident}: running quietly',flush=True)
            raw=script_run(script,280)
            (outdir/f'{ident}.log').write_text(raw)
            if 'UNSETTLED' in raw:
                raise SystemExit('Board did not settle: no benchmark result accepted')
            if 'NO_COMPOSITOR' in raw:
                raise SystemExit('No running compositor: refusing to benchmark stale scanout')
            body=raw.replace('\r','').split('BENCH_RESULT_BEGIN\n',1)[-1].split('BENCH_RESULT_END_',1)[0]
            run=[]
            for line in body.splitlines():
                if line.startswith('{'):
                    try: item=json.loads(line)
                    except json.JSONDecodeError:
                        bad=True
                        continue
                    item.update(run=ident,repeat=rep+1,variant=label)
                    records.append(item); run.append(item)
            transfer_ok=hashlib.md5(body.encode()).hexdigest()+'  /root/sdlbench-result.jsonl' in raw
            ok=(transfer_ok and 'BENCH_RESULT_END_0' in raw and any(x.get('type')=='done' and x.get('failures')==0 for x in run))
            if a.require_futex:
                probes=[x for x in run if x.get('type')=='futex']
                if len(probes)!=2 or not all(x.get('supported') for x in probes):
                    ok=False
            bad |= not ok
            records.append(dict(type='run_status',run=ident,repeat=rep+1,variant=label,ok=ok))
            print(f'{ident}: {"PASS" if ok else "FAIL (see raw log)"}, {sum(x.get("type")=="measure" for x in run)} measurements',flush=True)
            with (outdir/'results.jsonl').open('w') as f:
                for item in records: f.write(json.dumps(item)+'\n')
    print(outdir,flush=True)
    return int(bad)

if __name__=='__main__':
    raise SystemExit(main())
