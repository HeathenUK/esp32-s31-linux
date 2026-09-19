#!/usr/bin/env python3
"""Summarize quiet JSONL runs without claiming submitted frames reached scanout."""
import argparse
import collections
import json
import statistics
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('results',type=Path,nargs='+')
a=p.parse_args()
for path in a.results:
    rows=[json.loads(x) for x in path.read_text().splitlines() if x.strip()]
    groups=collections.defaultdict(list)
    invalid={r['run'] for r in rows if r['type']=='run_status' and not r['ok']}
    print(f'\n{path}')
    print('| Variant / case | Runs | Mean ms (median; range) | p95 ms (median) | Worst frame ms | Client CPU ms/op | lvdesk CPU ms/op* |')
    print('|---|---:|---:|---:|---:|---:|---:|')
    for r in rows:
        if r['type']=='measure' and r['run'] not in invalid: groups[(r['variant'],r['case'])].append(r)
    for (variant,case),rs in groups.items():
        wall=[r['mean_ns']/1e6 for r in rs]
        cpu=[(r['user_us']+r['sys_us'])/r['n']/1000 for r in rs]
        comp=[r['lvdesk_ticks']/r['clk_tck']*1000/r['n'] for r in rs]
        p95=statistics.median(r['p95_ns']/1e6 for r in rs)
        worst=max(r['max_ns']/1e6 for r in rs)
        print(f'| {variant} / {case} | {len(rs)} | {statistics.median(wall):.3f}; {min(wall):.3f}–{max(wall):.3f} | {p95:.3f} | {worst:.3f} | {statistics.median(cpu):.3f} | {statistics.median(comp):.3f} |')
    print('*Compositor accounting is quantized by CLK_TCK and excludes work after the interval.')
    for r in rows:
        if r['type']=='threads' and r['run'] not in invalid:
            print(f"{r['run']}: wait wall {r['wall_ns']/1e6:.2f} ms, process CPU {r['cpu_us']/1000:.2f} ms")
        if r['type'] in ('error','audio') or (r['type']=='check' and not r['pass']): print(r)
