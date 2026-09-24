#!/usr/bin/env python3
"""churn-cat.py - the host half of scripts/board/churn.sh.

Two sub-commands, both pure host-side arithmetic (no board access):

  churn-cat.py rates <snapA-dir> <snapB-dir>
      Per-second deltas between two board snapshots taken SECS apart
      (churn.sh writes them): every /proc/interrupts row per CPU column,
      ctxt/s, softirq/s per type per CPU, hrtimer nr_events/s per CPU from
      /proc/timer_list, per-CPU %user/%sys/%idle from /proc/stat, SD reads/s,
      swap-ins and major faults/s, and the game's context switches. The
      window length comes from the board's own /proc/uptime, not the host
      clock, because that is the clock the counters were incremented against.

  churn-cat.py cat <System.map> <samples.txt> [--top N] [--label L]
      Categorise a hart0 PC-sampler capture (rootfs/h1s output, one hex PC
      per line) by summing kernel symbols from System.map into named buckets:
      idle / irq-entry / softirq / timers / scheduler / locks / memory /
      storage / syscall-fs / drivers / M-mode / other. Each bucket is
      reported as % of ALL samples and % of KERNEL samples, split RAM
      (.text..fast) / flash (XIP), with the top symbols per bucket and the
      top 25 kernel symbols overall.

      SELF-CHECK (mandatory, aborts with exit 2 and MAP MISMATCH): the single
      most-frequent kernel PC must resolve inside arch_cpu_idle - that PC is
      the parked WFI, the one address the sampler can be certain of. The
      2026-09-24 X11 Quake profile failed this: its top PC 0xc032b1b4 (106
      samples) resolves to show_pwq against images/System.map #369, whose
      arch_cpu_idle is 0x888 higher, so every symbol name in that report is a
      neighbour of the real function, not the function. A category share
      taken from a mis-resolved capture is worthless, so this refuses to
      name symbols rather than print plausible junk.

Category regexes are matched in the order listed; first hit wins. `hrtick`
is deliberately in timers (it is the slice hrtimer priced by arm A) even
though it lives in kernel/sched. `rcu_` is checked (softirq) before
`sched` so rcu_sched_clock_irq does not land in scheduler.
"""
import bisect
import collections
import os
import re
import sys

# ---------------------------------------------------------------- categories
CATS = [
    ("idle",       r"arch_cpu_idle|default_idle_call|do_idle|cpu_idle_poll|cpu_startup_entry"),
    ("irq-entry",  r"handle_exception|ret_from_exception|do_irq|handle_riscv_irq|clic|^irqentry|"
                   r"handle_(percpu|level|simple|fasteoi)|generic_handle_domain|handle_irq_desc|"
                   r"note_interrupt|irq_exit|ipi_mux|handle_IPI|sbi_send_ipi|irq_enter|"
                   r"__handle_domain_irq|irq_find_mapping|smp_call_function|flush_smp_call|"
                   r"__smp_call_single_queue|generic_smp_call_function|ipi_"),
    ("softirq",    r"softirq|tasklet|rcu_|ksoftirqd"),
    ("timers",     r"hrtimer|timerqueue|clockevents|tick_|ktime|timekeeping|riscv_clock|"
                   r"riscv_timer|sbi_set_timer|__sbi_ecall|jiffies|update_wall|rdtime|"
                   r"hrtick|timer_|__run_timers|run_timer|expire_timers|posix_|"
                   r"clock_was_set|read_persistent|sched_clock|__sbi"),
    ("scheduler",  r"sched|schedule|pick_next|enqueue|dequeue|load_avg|pelt|update_curr|"
                   r"vruntime|calc_delta|ttwu|wake_up|try_to_wake|__switch_to|switch_mm|"
                   r"fstate|esp32s31_ext_switch|dl_server|update_curr_dl|task_rq_lock|"
                   r"set_next|put_prev|place_entity|update_rq_clock|cpuacct|__update_load|"
                   r"detach_entity|attach_entity|account_process_tick|account_.*_time|"
                   r"cputime|newidle|finish_task|context_switch|rt_mutex|task_blocks|"
                   r"activate_task|deactivate_task|resched_curr|check_preempt|"
                   r"nohz_|update_blocked|__balance|load_balance|select_task|"
                   r"wakeup_preempt|entity_|avg_vruntime|set_task_cpu|migrate|"
                   r"do_task_dead|kthread|smpboot|worker|workqueue|pwq|process_one_work|"
                   r"insert_work|__queue_work|kick_pool"),
    ("locks",      r"spin_lock|spin_unlock|mmiowb|mutex|rwsem|atomic64|_raw_|rwlock|"
                   r"seqcount|__lock|lock_acquire|osq|rt_spin|down_|up_read|up_write|"
                   r"queued_"),
    ("memory",     r"fault|page|swap|filemap|memset|memcpy|memmove|copy_|uaccess|alloc|"
                   r"free|kmem|vm_|cache_range|dma_|zone|lru|reclaim|shrink|mmap|"
                   r"pte|pmd|pgd|tlb|__flush|vma|anon|rmap|folio|slab|slub|__get_user|"
                   r"__put_user|clear_page|zram|zsmalloc|lzo|lz4|crypto|"
                   r"__memset|__memcpy|strn?len|strn?cmp|strn?cpy|memcmp|"
                   r"mempool|kmalloc|kfree|vmalloc|__arch_copy|sync_dma|"
                   r"arch_sync_dma|cache_"),
    ("storage",    r"dw_mci|mmc|blk_|bio|sbitmap|block|elevator|mq_|scsi|"
                   r"submit_bio|end_io|__bio|request_"),
    ("syscall-fs", r"ecall|syscall|poll|select|vfs|sys_|eventfd|read_write|futex|pipe|"
                   r"ksys|__x64|do_sys|fdget|fdput|__fget|fput|dentry|inode|ext4|"
                   r"cramfs|overlay|ovl_|iterate|getdents|epoll|signal|sigaction|"
                   r"do_notify|restore_sigcontext|setup_rt_frame|clock_gettime|"
                   r"gettimeofday|nanosleep|do_nanosleep|hrtimer_nanosleep|unix_|"
                   r"sock_|__sys_|do_writev|do_readv|new_sync|__vfs|path_|"
                   r"seq_|proc_|kernfs|sysfs|tty_|n_tty|write_iter|read_iter|"
                   r"__fdget|fd_install|do_dup|exit_|do_exit|do_wait|fork|clone|"
                   r"exec|load_elf|binfmt|mm_release|mm_init"),
    ("drivers",    r"lcd|ppa|gdma|dwc2|usb|hid|hosted|i2c|i2s|snd_|uart|serial|"
                   r"esp32s31|es8389|input_|evdev|gt1158|drm_|kms|fb_|cma|"
                   r"gpio|pinctrl|regmap|clk_|regulator|net_|netif|__napi|"
                   r"skb|__dev_queue|dev_hard|ip_|tcp_|udp_|eth_|wlan|bt_|hci_|"
                   r"l2cap|rfcomm|sco_|pcm|dai|soc_|dapm|codec|alsa|ioremap"),
]
CAT_ORDER = [c for c, _ in CATS] + ["M-mode", "other", "past-_etext"]
CAT_RE = [(c, re.compile(r)) for c, r in CATS]

PA_OFFSET = 0x70800000   # kernel VA (0xc0xxxxxx D symbols) -> PSRAM PA (0x50xxxxxx)


def categorise(name):
    for cat, rx in CAT_RE:
        if rx.search(name):
            return cat
    return "other"


# ---------------------------------------------------------------- System.map
def load_map(path):
    syms, marks = [], {}
    for line in open(path):
        p = line.split()
        if len(p) < 3:
            continue
        addr, typ, name = int(p[0], 16), p[1], p[2]
        if name in ("__text_fast_start", "__text_fast_end", "_exiprom", "_etext",
                    "_stext", "arch_cpu_idle", "default_idle_call",
                    "sysctl_sched_features", "_sdata"):
            marks[name] = addr
        # Section markers (__softirqentry_text_start, __irqentry_text_end, ...)
        # share an address with the first real function after them; keep the
        # function, not the marker, so __do_softirq reads as __do_softirq.
        if typ in "tTwW" and not re.match(r"^__.*_text_(start|end)$", name):
            syms.append((addr, name))
    syms.sort()
    return syms, marks


def resolve(syms, addrs, pc):
    i = bisect.bisect_right(addrs, pc) - 1
    if i < 0:
        return None
    return syms[i][1]


# ---------------------------------------------------------------- cat
def cmd_cat(argv):
    top, label = 25, ""
    pos = []
    i = 0
    while i < len(argv):
        if argv[i] == "--top":
            top = int(argv[i + 1]); i += 2
        elif argv[i] == "--label":
            label = argv[i + 1]; i += 2
        else:
            pos.append(argv[i]); i += 1
    if len(pos) != 2:
        sys.exit("usage: churn-cat.py cat <System.map> <samples.txt> [--top N] [--label L]")
    syms, marks = load_map(pos[0])
    addrs = [a for a, _ in syms]
    fast_lo, fast_hi = marks.get("__text_fast_start", 0), marks.get("__text_fast_end", 0)
    etext = marks.get("_etext", 0)
    rom_hi = marks.get("_exiprom", 0xC0800000)
    idle_lo = marks.get("arch_cpu_idle")
    if idle_lo is None:
        sys.exit("cat: no arch_cpu_idle in %s - not a kernel System.map" % pos[0])

    pcs = [int(l, 16) for l in open(pos[1]) if l.strip()]
    n = len(pcs)
    if n < 100:
        sys.exit("cat: only %d samples in %s - refusing to categorise" % (n, pos[1]))

    # Buckets: where the PC is, before any symbol is named.
    where = collections.Counter()
    kpc = collections.Counter()            # kernel PCs, exact
    for pc in pcs:
        if fast_lo <= pc < fast_hi:
            where["kernel RAM (.text..fast)"] += 1; kpc[pc] += 1
        elif 0xC0000000 <= pc < rom_hi:
            where["kernel flash (XIP)"] += 1; kpc[pc] += 1
        elif pc >= 0xC0000000:
            where["kernel other (data/bss?)"] += 1; kpc[pc] += 1
        elif 0x40000000 <= pc < 0x50000000 or 0x2F000000 <= pc < 0x30000000:
            where["M-mode (OpenSBI / hart0 monitor)"] += 1
        elif pc == 0:
            where["no sample (0)"] += 1
        elif pc >= 0x80000000:
            where["user: shared libraries"] += 1
        else:
            where["user: the program"] += 1
    nk = sum(kpc.values())
    nm_mode = where["M-mode (OpenSBI / hart0 monitor)"]

    hdr = "churn-cat %s: %d samples, %d kernel (%.1f%%), %d M-mode (%.1f%%)" % (
        label, n, nk, 100.0 * nk / n, nm_mode, 100.0 * nm_mode / n)
    print(hdr)
    for cat, c in where.most_common():
        print("  %5.1f%%  %5d  %s" % (100.0 * c / n, c, cat))

    # ---- SELF-CHECK: the most-hit kernel PC must be the parked WFI in arch_cpu_idle.
    if not kpc:
        sys.exit("cat: no kernel samples at all - wrong sampler addresses?")
    top_pc, top_n = kpc.most_common(1)[0]
    top_sym = resolve(syms, addrs, top_pc)
    idle_syms = ("arch_cpu_idle", "default_idle_call")
    print("\nself-check: most-hit kernel PC 0x%08x x%d resolves to %s (arch_cpu_idle @ 0x%08x)"
          % (top_pc, top_n, top_sym, idle_lo))
    if top_sym not in idle_syms:
        print("MAP MISMATCH: the parked-WFI PC does not land in arch_cpu_idle. This")
        print("  System.map is not the kernel the capture ran on (or the sampler")
        print("  addresses are wrong). REFUSING to name kernel symbols. Confirm the")
        print("  board's `uname -a` #N against `grep -a -m1 -o 'Linux version 7[^#]*#[0-9]*' images/xipImage`")
        print("  and `nm hello_world.elf | grep s31_h1s_` for the sampler pair.")
        sys.exit(2)

    # ---- categorise
    cat_n = collections.Counter()
    cat_ram = collections.Counter()
    cat_syms = collections.defaultdict(collections.Counter)
    sym_n = collections.Counter()
    sym_where = {}
    for pc, c in kpc.items():
        in_ram = fast_lo <= pc < fast_hi
        if pc >= etext and pc < fast_lo and etext:
            name = resolve(syms, addrs, pc) or "?"
            cat = "past-_etext"
            name = "%s(+0x%x past _etext)" % (name, pc - etext)
        else:
            name = resolve(syms, addrs, pc) or ("0x%08x" % pc)
            cat = categorise(name)
        cat_n[cat] += c
        if in_ram:
            cat_ram[cat] += c
        cat_syms[cat][name] += c
        sym_n[name] += c
        sym_where[name] = "RAM" if in_ram else "flash"
    cat_n["M-mode"] = nm_mode

    total_cat = sum(cat_n.values())
    nonidle = nk - cat_n["idle"]
    print("\ncategories (%% of ALL %d samples | %% of kernel+M-mode %d | RAM/flash split)" % (n, nk + nm_mode))
    print("  %-12s %6s %6s %6s %6s %6s" % ("category", "n", "%all", "%kern", "RAM", "flash"))
    for cat in CAT_ORDER:
        c = cat_n.get(cat, 0)
        if not c:
            continue
        r = cat_ram.get(cat, 0)
        print("  %-12s %6d %5.1f%% %5.1f%% %6d %6d" % (cat, c, 100.0 * c / n,
              100.0 * c / max(1, nk + nm_mode), r, c - r))
    print("  %-12s %6d %5.1f%%" % ("SUM", total_cat, 100.0 * total_cat / n))
    print("  non-idle kernel (excluding M-mode): %d = %.1f%% of all samples; idle %.1f%%"
          % (nonidle, 100.0 * nonidle / n, 100.0 * cat_n["idle"] / n))
    core = cat_n["irq-entry"] + cat_n["timers"] + cat_n["scheduler"] + cat_n["softirq"]
    print("  irq-entry+timers+scheduler+softirq = %d = %.1f%% of non-idle kernel samples; drivers %.1f%%"
          % (core, 100.0 * core / max(1, nonidle), 100.0 * cat_n["drivers"] / max(1, nonidle)))
    if 100.0 * nonidle / n < 20.0:
        print("  VERDICT: non-idle kernel < 20% of CPU0 samples -> the '36% kernel' framing is KILLED for this capture")
    else:
        print("  VERDICT: non-idle kernel >= 20% of CPU0 samples -> the kernel share is real for this capture")

    print("\ntop symbols per category (share of ALL samples)")
    for cat in CAT_ORDER:
        if cat in ("M-mode",) or not cat_syms.get(cat):
            continue
        print("  [%s]" % cat)
        for name, c in cat_syms[cat].most_common(10):
            print("    %5.1f%%  %5d  %-5s %s" % (100.0 * c / n, c, sym_where[name], name))

    print("\ntop %d kernel symbols overall (share of ALL samples)" % top)
    for name, c in sym_n.most_common(top):
        print("  %5.1f%%  %5d  %-5s %-10s %s" % (100.0 * c / n, c, sym_where[name],
              categorise(name) if "past _etext" not in name else "past-_etext", name))

    # Flash residue on the per-event path, for arm E (the FASTFN pick).
    residue = [s for s in sym_n if sym_where[s] == "flash" and categorise(s) in
               ("irq-entry", "timers", "scheduler", "locks", "softirq")]
    rn = sum(sym_n[s] for s in residue)
    print("\nflash residue on the interrupt/switch/timer path: %d samples = %.2f%% of all"
          % (rn, 100.0 * rn / n))
    for s in sorted(residue, key=lambda s: -sym_n[s])[:15]:
        print("    %5.2f%%  %5d  %s" % (100.0 * sym_n[s] / n, sym_n[s], s))
    if 100.0 * rn / n < 2.0:
        print("  arm E rule: residue < 2% combined -> do NOT spend RAM text on it")
    return 0


# ---------------------------------------------------------------- rates
def read(path):
    try:
        return open(path).read()
    except OSError:
        return ""


def parse_interrupts(txt):
    """{row: [cpu0, cpu1, ...]} plus the row's description."""
    rows, desc = {}, {}
    lines = txt.splitlines()
    if not lines:
        return rows, desc, 0
    ncpu = len(lines[0].split())
    for line in lines[1:]:
        p = line.split()
        if not p or not p[0].endswith(":"):
            continue
        key = p[0][:-1]
        vals = []
        for tok in p[1:1 + ncpu]:
            if tok.isdigit():
                vals.append(int(tok))
            else:
                break
        rest = " ".join(p[1 + len(vals):])
        rows[key] = vals + [0] * (ncpu - len(vals))
        desc[key] = rest
    return rows, desc, ncpu


def parse_stat(txt):
    d = {"cpu": {}}
    for line in txt.splitlines():
        p = line.split()
        if not p:
            continue
        if p[0].startswith("cpu") and p[0] != "cpu":
            d["cpu"][p[0]] = [int(x) for x in p[1:]]
        elif p[0] in ("ctxt", "intr", "softirq", "processes"):
            d[p[0]] = int(p[1])
    return d


def parse_softirqs(txt):
    d = {}
    lines = txt.splitlines()
    for line in lines[1:]:
        p = line.split()
        if p and p[0].endswith(":"):
            d[p[0][:-1]] = [int(x) for x in p[1:] if x.isdigit()]
    return d


def parse_timer_list(txt):
    """From the grep'd /proc/timer_list: nr_events per cpu and the armed
    hrtimer function names (a count of how many are pending per snapshot)."""
    ev, armed = {}, collections.Counter()
    cpu = None
    for line in txt.splitlines():
        s = line.strip()
        m = re.match(r"cpu:\s*(\d+)", s)
        if m:
            cpu = int(m.group(1)); continue
        m = re.match(r"\.nr_(events|retries|hangs)\s*:\s*(\d+)", s)
        if m and cpu is not None:
            ev.setdefault(cpu, {})[m.group(1)] = int(m.group(2)); continue
        m = re.search(r"#\d+:\s*<[0-9a-fx]+>,\s*([A-Za-z_0-9.]+)", s)
        if m:
            armed[m.group(1)] += 1
    return ev, armed


def parse_kv(txt):
    d = {}
    for line in txt.splitlines():
        p = line.replace(":", " ").split()
        if len(p) >= 2 and p[1].lstrip("-").isdigit():
            d[p[0]] = int(p[1])
    return d


def cmd_rates(argv):
    if len(argv) != 2:
        sys.exit("usage: churn-cat.py rates <snapA-dir> <snapB-dir>")
    A, B = argv
    ua = read(os.path.join(A, "uptime")).split()
    ub = read(os.path.join(B, "uptime")).split()
    if not ua or not ub:
        sys.exit("rates: missing uptime in a snapshot (%s / %s)" % (A, B))
    secs = float(ub[0]) - float(ua[0])
    if secs <= 1.0:
        sys.exit("rates: window is %.2f s - snapshots are not A then B" % secs)
    print("window %.1f s (board /proc/uptime)" % secs)

    # /proc/stat: per-CPU busy split and the global counters
    sa, sb = parse_stat(read(os.path.join(A, "stat"))), parse_stat(read(os.path.join(B, "stat")))
    for k in ("ctxt", "intr", "softirq", "processes"):
        if k in sa and k in sb:
            print("  %-10s %8.1f /s" % (k, (sb[k] - sa[k]) / secs))
    for cpu in sorted(sa["cpu"]):
        if cpu not in sb["cpu"]:
            continue
        d = [b - a for a, b in zip(sa["cpu"][cpu], sb["cpu"][cpu])]
        tot = sum(d) or 1
        # user nice system idle iowait irq softirq steal
        print("  %-5s user %4.1f%%  sys %4.1f%%  idle %4.1f%%  iowait %4.1f%%  irq %4.1f%%  softirq %4.1f%%  (tick-based; undercounts kernel here)"
              % (cpu, 100.0 * (d[0] + d[1]) / tot, 100.0 * d[2] / tot, 100.0 * d[3] / tot,
                 100.0 * d[4] / tot, 100.0 * d[5] / tot, 100.0 * d[6] / tot))

    # /proc/interrupts per row per CPU
    ia, da, ncpu = parse_interrupts(read(os.path.join(A, "interrupts")))
    ib, db, _ = parse_interrupts(read(os.path.join(B, "interrupts")))
    print("\ninterrupts/s per CPU (row: %s  description)" % "  ".join("CPU%d" % i for i in range(ncpu)))
    dev_total = [0.0] * ncpu
    ipi_total = [0.0] * ncpu
    timer_total = [0.0] * ncpu
    rows = []
    for key in ib:
        if key not in ia:
            continue
        d = [(b - a) / secs for a, b in zip(ia[key], ib[key])]
        if sum(d) == 0:
            continue
        rows.append((sum(d), key, d, db.get(key, "")))
    for tot, key, d, desc in sorted(rows, reverse=True):
        print("  %-6s %s  %s" % (key + ":", "  ".join("%8.1f" % x for x in d), desc))
        is_ipi = key.startswith("IPI")
        is_timer = (not is_ipi) and ("riscv-timer" in desc or "timer" in desc.lower())
        for i, x in enumerate(d):
            if is_ipi:
                ipi_total[i] += x
            elif is_timer:
                timer_total[i] += x
            else:
                dev_total[i] += x
    print("  %-10s %s  (all non-timer, non-IPI rows)" % ("DEV-IRQ:", "  ".join("%8.1f" % x for x in dev_total)))
    print("  %-10s %s" % ("TIMER-IRQ:", "  ".join("%8.1f" % x for x in timer_total)))
    print("  %-10s %s" % ("IPI-IRQ:", "  ".join("%8.1f" % x for x in ipi_total)))
    if sum(dev_total) < 400:
        print("  VERDICT: device IRQ rows sum to %.0f/s (< 400/s) -> 'device IRQ count' is NOT a lever (they were ~1%% of a core on 09-22)" % sum(dev_total))
    else:
        print("  VERDICT: device IRQ rows sum to %.0f/s (>= 400/s) -> worth pricing per source" % sum(dev_total))
    if ncpu > 1:
        fc = [k for k, v in db.items() if "Function call" in v]
        if fc:
            k = fc[0]
            x = (ib[k][1] - ia[k][1]) / secs
            print("  Function-call IPIs INTO CPU1: %.1f/s = %.2f%% of a hart at 165 us each%s"
                  % (x, 100.0 * x * 165e-6, "" if x >= 30 else "  -> arm B (TTWU_QUEUE) rule: < 30/s, wake path < 0.5%, CLOSED"))

    # /proc/softirqs
    fa, fb = parse_softirqs(read(os.path.join(A, "softirqs"))), parse_softirqs(read(os.path.join(B, "softirqs")))
    print("\nsoftirqs/s per CPU")
    for key in fb:
        if key in fa:
            d = [(b - a) / secs for a, b in zip(fa[key], fb[key])]
            if sum(d) > 0:
                print("  %-9s %s" % (key + ":", "  ".join("%8.1f" % x for x in d)))
    if "RCU" in fa and "RCU" in fb:
        r0 = (fb["RCU"][0] - fa["RCU"][0]) / secs
        print("  arm F (NOCB=0) rule: RCU softirq on CPU0 = %.0f/s -> %s" % (r0, "worth an arm (> 100/s)" if r0 > 100 else "KILL, not worth a kthread hop"))

    # hrtimer events per CPU from timer_list
    ta, arm_a = parse_timer_list(read(os.path.join(A, "timer_list")))
    tb, arm_b = parse_timer_list(read(os.path.join(B, "timer_list")))
    print("\nhrtimer events/s per CPU (/proc/timer_list .nr_events)")
    for cpu in sorted(tb):
        if cpu in ta and "events" in ta[cpu] and "events" in tb[cpu]:
            print("  cpu%d  nr_events %8.1f /s   retries %+d  hangs %+d" % (
                cpu, (tb[cpu]["events"] - ta[cpu]["events"]) / secs,
                tb[cpu].get("retries", 0) - ta[cpu].get("retries", 0),
                tb[cpu].get("hangs", 0) - ta[cpu].get("hangs", 0)))
    print("  armed hrtimers seen in snapshot A: %s" % (", ".join("%s x%d" % kv for kv in arm_a.most_common()) or "-"))
    print("  armed hrtimers seen in snapshot B: %s" % (", ".join("%s x%d" % kv for kv in arm_b.most_common()) or "-"))
    print("  arm A (HRTICK) check: hrtick armed A=%d B=%d (expect 0 after bits 12/13 are cleared)"
          % (arm_a.get("hrtick", 0), arm_b.get("hrtick", 0)))

    # vmstat, SD, the game
    va, vb = parse_kv(read(os.path.join(A, "vmstat"))), parse_kv(read(os.path.join(B, "vmstat")))
    print("\npaging")
    for k in ("pswpin", "pswpout", "pgmajfault", "pgfault"):
        if k in va and k in vb:
            print("  %-11s %8.1f /s" % (k, (vb[k] - va[k]) / secs))
    sda, sdb = read(os.path.join(A, "sdstat")).split(), read(os.path.join(B, "sdstat")).split()
    if len(sda) >= 7 and len(sdb) >= 7:
        print("  SD reads %8.1f /s  %8.1f KiB/s   writes %6.1f /s  %6.1f KiB/s" % (
            (int(sdb[0]) - int(sda[0])) / secs, (int(sdb[2]) - int(sda[2])) / 2.0 / secs,
            (int(sdb[4]) - int(sda[4])) / secs, (int(sdb[6]) - int(sda[6])) / 2.0 / secs))
    qa, qb = parse_kv(read(os.path.join(A, "qstatus"))), parse_kv(read(os.path.join(B, "qstatus")))
    if "voluntary_ctxt_switches" in qa and "voluntary_ctxt_switches" in qb:
        print("  game ctxt switches: voluntary %.1f /s  nonvoluntary %.1f /s" % (
            (qb["voluntary_ctxt_switches"] - qa["voluntary_ctxt_switches"]) / secs,
            (qb["nonvoluntary_ctxt_switches"] - qa["nonvoluntary_ctxt_switches"]) / secs))
    sqa, sqb = read(os.path.join(A, "qstat")).split(), read(os.path.join(B, "qstat")).split()
    if len(sqa) >= 2 and len(sqb) >= 2:
        ut = (int(sqb[0]) - int(sqa[0])) / secs; st = (int(sqb[1]) - int(sqa[1])) / secs
        print("  game CPU (tick-based): user %.0f%%  sys %.0f%%  (main thread majflt delta %s)" % (
            ut, st, (int(sqb[2]) - int(sqa[2])) if len(sqa) >= 3 and len(sqb) >= 3 else "?"))
    return 0


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("cat", "rates"):
        sys.exit(__doc__)
    sys.exit(cmd_cat(sys.argv[2:]) if sys.argv[1] == "cat" else cmd_rates(sys.argv[2:]))


if __name__ == "__main__":
    main()
