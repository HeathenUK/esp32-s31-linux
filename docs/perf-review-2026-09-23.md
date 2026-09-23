# Performance review, 2026-09-23

## 1. Bottom line

183 proposals were judged — the 95 candidates carried in from the standing list and 88 discovered in this pass. **All 183 carry three independent lens verdicts** (history and rules, physics and feasibility, mechanism read against the source). Of the 95, **zero were rated a large gain**: 54 small, 41 none-or-negative, with physics returning 7 plausible / 33 weak / 55 refuted; of the 88 discovered items, exactly one reached medium (X3-1, a CPU PLL overclock, which the other two lenses call weak) and every other one is small or none-or-negative. That is the review's central finding and it should be read literally: the obvious levers on this platform are spent, and what remains is a programme of 1-4% items, most of which are individually smaller than the ±10% per-boot Quake lottery and therefore cannot be validated one at a time. The three independent rankings agreed on a small core — **C39** (function-granular hot text), **C37/X2-5** (frame pointers off by accident), **C32** (4 MB megapages) and **C04/X1-20** (the L1 cache counters as the missing PMU) appear in all three — and disagreed mainly on framing: two ranked against fullscreen Quake/Doom fps and put X3-1 at or near the top, while the third rejected that framing entirely on the grounds that `fs_render_set(0)` (lvdesk.c:4084-4113) makes the compositor provably absent from every headline measurement, and ranked instead for desktop responsiveness, where C51, C77, X2-11 and C70 become the main event and X3-1 does not appear. The items that could still be structural rather than incremental are X3-1 (the only lever that reaches the ~51% of CPU0 that is app code the rules forbid touching), C39+C18 (which converts a stalled, un-aimable RAM-text programme worth ~10% of CPU0 into a measurable one), and C04 (which is the only instrument that can see D-cache displacement, the unmeasured half of every text-placement result to date). For Quake at 320x240 with sound, the honest landing point is **17.5-18.2 fps median without the overclock** and **18.8-19.7 fps median only if the 400 MHz memory domain ships stable and essentially everything else lands at the optimistic end** — these are section 4's own numbers, and both fall short of the goal. The project's own waits ceiling is 17.3/0.84 = **20.6 fps on the best boot** (docs/smp-finish-plan.md:138-141), and ~10% of CPU0 is PSRAM stall inside five pixel loops in a binary the hard rules forbid rebuilding, so **20 fps is not reliably reachable by anything in this review and should not be planned for**.

**Baseline convention.** Four different medians appear in the source material — 16.2, 16.4, 16.6 and 17.1 fps. This report quotes deltas against a **16.4 fps median** unless a section names the boot its measurement came from; T1.7's and T1.9's lens numbers were taken on 16.6 fps boots and section 6's arithmetic on a 16.2 fps run, and both are labelled where they appear.

**Independently re-verified during this pass** (so that the corrections below are not read as a general loss of confidence): `artifacts/quake/h1s-062518/pcs.txt` re-resolved gives user 59.23%, kernel flash non-idle 19.82%, `.text..fast` 11.83%, `arch_cpu_idle` 8.61%; D_DrawSpans8 11.90%, D_DrawZSpans 4.62%, R_DrawSurfaceBlock8 mip0+mip1 3.91%, top-5 pixel loops 22.50% (PIE base 0x6909e000, 5,916/5,950 = 99.4% coverage). `images/System.map` confirms every T1.8 address and `__text_fast_start c083bc50 / __text_fast_end c0881990` = 286,016 B with `__of_table_esp32s31_systimer` at c084102c inside it. `vmlinux-xip.lds.S:462` really does bind the section list to the last glob only. `arch/riscv/Kconfig:102`, `esp32s31_defconfig:266` and `Makefile:242/251` confirm the FRAME_POINTER trap. `pagemap.h:460-474` confirms X3-18 is a literal no-op. `esp32s31-lcd.c:1770-1776` confirms the vblank-hrtimer exclusion. `defer_copy`, `ppa_async`, `ppa_spin_us` and `idle_poll_ms` are all `0644` module params and `CONFIG_STRICT_DEVMEM is not set`, so the devmem A/B routes work. C32's SECRETMEM mitigation is sufficient: `images/System.map` has `__riscv_sys_memfd_secret` but no module loader and only a weak `bpf_int_jit_compile`, so secretmem is the only live caller of the rv32 leaf-rewrite path.

---

## 2. Tier 1 — the best available levers

Nine items. Each has at least two lenses not refuted and a history status of novel or retry-justified — **with one declared exception: X1-5, carried into T1.8 as "arm 1", has `history.status = already-rejected`**, and is admitted only because its arm-1 subset is disjoint from the rejected part. Gains quoted are the lenses' recomputations, not the finders' claims.

### T1.1 — X3-1: CPU PLL 320 → 400 MHz (CPLL `fb_div` 8→10), gated on the in-spec 240 MHz mirror arm

**Mechanism.** `clk_ll_cpll_set_freq_mhz()` writes `LP_CLKRST.cpll_div.cpll_fb_div = 8`, `ref_div = 1` → CPLL 320 MHz, and `clk_ll_cpll_set_config()` is a documented no-op on this target ("CPLL frequency controlled by digital only"). `fb_div` is an ordinary writable field, so 10 gives 400 MHz with no analog reconfiguration. Everything downstream is a divider: at CPU 320 today `MEM_CLK` 160, `SYS_CLK` 106.67 and `APB_CLK` 53.33 are all already pinned at their vendor maxima (`rtc_clk_cpu_freq_to_cpll_mhz()`), so the PLL is the only headroom. At 400 with cpu_div 1 / mem_div 2 / sys_div 4 / apb_div 2: CPU 400 (+25%), MEM 200, SYS 100 (−6%, legal), APB 50 (−6%, legal).

**Evidence.** History novel: the dead-end register's only two clock entries are the external flash interface (#27) and the cpufreq governor (#55 — hart0 holds 320 MHz always; 4,388 transitions changed nothing). Flash sits on BBPLL, PSRAM on MPLL and SDMMC on the 80 MHz SYS clock (`clk-esp32s31.c:24-42`), so the calibrated MSPI timing point and the 42 Hz panel do not move with it. It is the only proposal in 183 that scales TyrQuake's own rasteriser — D_DrawSpans8 11.90%, D_DrawZSpans 4.62%, top-5 pixel loops 22.50% of CPU0 (offline resolution of `artifacts/quake/h1s-062518/pcs.txt` against `rootfs/tiopex-quake.dbg`, PIE base 0x6909e000, 99.4% symbol coverage).

**Realistic gain.** Physics **+6 to +9%** (16.4 → 17.4-17.9 fps); mechanism **+10 to +13%** (→ 18.0-18.5). Both verdicts weak. The finder's +15-25% is arithmetically impossible, and the physics ceiling is arithmetic rather than opinion: the main thread is 86% running and a +25% clock shortens only 20% of that, so **0.86 × 0.20 = 17.2% is the absolute maximum even if every running cycle were pure issue**. The mechanism's +10-13% sits inside that ceiling but assumes a large issue-bound fraction, which is precisely what the mirror arm exists to test — and, as originally written, could not. **Until the mirror arm is built correctly and validated, quote the physics number.**

**Effort.** week+ including a soak. **Risk: high, and of the silent kind.** MEM_CLK 200 MHz is 25% over the stated 160 ceiling, and `cache_ctrl0` (`hp_sys_clkrst_struct.h:400-433`) clocks dcache, icache0 and icache1 from that domain with no divider of its own — a marginal cache domain corrupts rather than hangs, with 15.4 MB of PSRAM behind it.

**First experiment — the 240 MHz mirror arm, and it is a build, not a one-liner.** Two defects in the cheap version must be fixed before it can measure anything.

1. **The timebase must move with the clock, or the arm returns a false null.** `linux-71-port/arch/riscv/boot/dts/espressif/esp32s31.dtsi:196` has `timebase-frequency = <320000000>` and `opensbi-esp32-s31/platform/generic/espressif/esp32s31.c:41` has `#define S31_TIMEBASE_HZ 320000000UL`. The RISC-V `time` CSR runs at the CPU clock (X3-1's own evidence list carries this caveat). At 240 MHz with both constants left at 320e6, Linux's clock runs 1.333x fast; `scripts/board/quake-timedemo.sh:38` scrapes TyrQuake's own `N frames S seconds F fps` line, which comes from `gettimeofday`, so **reported fps is inflated by exactly 320/240 and a true −25% slowdown reads as zero change** — which is the exact output the step-1 decision rule would convert into "drop X3-1 entirely, and with it X3-13, X3-17, X3-3, X3-5 and X2-16".
2. **The one-line edit is probably a no-op.** `bootloader/sdkconfig.defaults` contains no `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_*` line at all; `bootloader/sdkconfig:2811` has `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_320=y`, and an existing sdkconfig wins over defaults, so the board would still run at 320.

**Correct arm:** DTS `timebase-frequency` **and** `S31_TIMEBASE_HZ` to 240000000, `make linux` **and** `make opensbi` (a DTS edit needs both — the kernel uses a builtin DTB), the bootloader frequency change in `sdkconfig`, then flash all of it. **Validate before believing:** confirm the actual clock from the loader's `cpu_start: cpu freq:` line, and cross-check the demo's wall time against a host stopwatch so a timebase error cannot hide inside the fps number. Budget ~40-60 minutes, not 7. It is still the exact mirror (−25% CPU, −25% MEM) and still fits the fps-vs-clock slope in the safe direction; a shallow, *validated* slope kills the arm for one build.

**Lens disagreement.** The 4-point spread between the two estimates is entirely "how much of the frame is PSRAM-latency-bound". The 250 MHz octal pin clock does not scale with the core, so in CPU cycles that stall gets ~25% *worse*. If the validated mirror arm's slope is shallow, take the physics number.

### T1.2 — C39 (with C18 as enabler; supersedes X1-11): function-granular, profile-ordered kernel text

**Mechanism.** Placement is by whole object today. Build with `-ffunction-sections`, merge the h1s profiles into a hot-first ordered list, emit it (a) at the head of `.text.fast` aligned to a way and (b) at the head of flash `.text` for what stays in flash. Ship a same-size cold layout control, which is the test nobody has run for the unverified "RAM text evicts D-cache" explanation of #320.

**Evidence.** `.text..fast` = c083bc50..c0881990 = **286,016 B**. Re-resolved against the 12,000-sample capture: **260 symbols / 65,166 B took at least one sample; 2,925 symbols / 220,898 B took none.** (The proposal's own unverified count of 296 symbols / 66,992 B is within resolution error of this pair; quote one pair, not a mix of both.) Flash kernel code that is not idle is **19.82% of CPU0** spread over hundreds of symbols. The concentration, recomputed: top flash symbol `esp32s31_clic_handle_irq` = 93/12,000 = **0.78%**; **top 10 flash symbols = 474 samples = 3.95%**; the specific IRQ-dispatch chain X1-11 names (eleven functions) = 368 samples = **3.07%**. The tightest verified statement of the case is the mechanism lens's: **the top 50 non-idle flash functions are 13,556 B of code carrying 10.07% of CPU0.** (The 4.62% "top 10" figure that circulated earlier does not reproduce; it was borrowed from a different subset.) Sampled kernel PCs pile into the last 8 bytes of each 64 B line (flash 31%, RAM 33%, against 12.5% uniform and 15% for Quake's own cached code), which is the fetch-stall signature. `CONFIG_LD_DEAD_CODE_DATA_ELIMINATION` is available but unset and `-ffunction-sections` appears nowhere in the kernel build.

**Realistic gain.** Physics **+2-5%, central 4%**; mechanism **+3-5%, central 4%** — both roughly half the finder's +5-10%. Plus 150-220 kB of PSRAM returned. **This is the cap for the whole flash-text family (T1.4, T1.8), not an addend to it.**

**Effort.** days. **Risk:** medium — function-level lists will contain early-boot and self-referencing symbols (spinlock.o and sched_clock.o already fail to boot), each costing a build/flash/bisect cycle; and layout perturbation is the same uncontrolled variable that produced #320's −5%.

**Known limit.** `-ffunction-sections` only splits `.text`. `__sched`, `__lockfunc`, `noinstr`, `.entry.text`/`.irqentry.text` and all of entry.S stay whole — so `__schedule`, `irqentry_exit`, `handle_exception` and `do_raw_spin_lock` are unreachable this way. That is exactly what T1.8 exists for.

**C18, the enabler.** In `vmlinux-xip.lds.S:462` the section list binds only to the last pattern, so every other bare `*x.o` claims *all* that object's unplaced sections — `__of_table_esp32s31_systimer` sits at c084102c, inside `.text.fast`, and the glob also drags 22.6 kB of never-executed text into RAM. The early XIP mapping of `[_sdata,_end)` is PAGE_KERNEL (non-exec), so RAM text cannot execute before `setup_vm_final`. Fixing both is worth ~+1% on its own (range 0 to +2.4%) and raises the ceiling on T1.4 and T1.8. **Carry the caveat:** the glob fix resurrects the rating-450 systimer clocksource (MMIO poll read, not SMP-safe) — keep it disabled or lower-rated.

**First experiment (≤10 min).** Measure in-app *k* directly inside one boot: `scripts/board/fastarm.sh pelt "*kernel/softirq.o *kernel/sched/build_policy.o"` (part of #320, known to boot; build off-board, flash + boot check ~2 min), then `artifacts/quake/h1s-062518/run.sh` unchanged (~2.5 min) and resolve with `h1s-report.py`. If in-app *k* is 3 or more the upper end is live; if it is near 1 the whole family collapses to the low end.

### T1.3 — C37 / X2-5: turn off `CONFIG_FRAME_POINTER`, which is on only by accident

**Mechanism.** `arch/riscv/Kconfig:102` selects FRAME_POINTER under PERF_EVENTS. The Makefile enables PROFILING in the base tweak list and later clears PROFILING and PERF_EVENTS (`Makefile:251`) but not FRAME_POINTER; because it is also `default y` under `ARCH_WANT_FRAME_POINTERS`, `olddefconfig` keeps it forever. The kernel Makefile:962-963 then adds `-fno-omit-frame-pointer -fno-optimize-sibling-calls` to every object. `esp32s31_defconfig:266` says it should be off. This is the identical selector/selectee trap the Makefile already documents for PERF_EVENTS, one level deeper.

**Realistic gain.** Mechanism measured it directly rather than estimating: **175 of 12,000 CPU0 PC samples land on an instruction this change deletes** = 1.5-2.5% of CPU0 → **+0.25-0.45 fps**. Physics agrees at ~2% of CPU0 → +1-2%. Resolvable only on pingpong (+2-4% on a same-hart round trip), never on fps. Also returns **~187 kB of linux-partition text** — slack 119,803 B → ~307 kB, which more than doubles the headroom that has repeatedly forced feature-vs-diagnostics trades — and ~11 kB of `.text..fast`.

**Effort.** hours, one build. **Risk:** low. The `!FRAME_POINTER` stack walker already exists (`stacktrace.c:105-135`) and degrades oops backtraces to a stack scan, which is what `pm-resolve.py` already does; KALLSYMS is off regardless. Keep FRAME_POINTER for `PROF=1`/`DIAG=1` builds.

**First experiment (≤10 min, no board).** Add `--disable FRAME_POINTER` to `PROF_TWEAKS`, run one `make linux` (~200 s), confirm `.config` shows it unset, and read the line the Makefile already prints at :667. If free space does not move from 119,803 to ~300,000 B, the static half is wrong and it dies with zero board time.

### T1.4 — C23 / X1-6 / X1-22: expose the existing I-cache prelock as a module parameter

**Mechanism.** `drivers/cache/esp32s31_cache.c:229-312` already implements `esp32s31_icache_prelock()` — two sections, ≤16,383 B each, then preload with a wait on the self-clearing ENA bit. It is reachable only through debugfs, and `DIAG ?= 0` (`Makefile:466-468`) compiles debugfs out, so the shipping kernel cannot reach it at all. It has never been measured; commit bd916b1 ("esp32s31: pick up I-cache prelock") is a bare submodule bump with no result.

**Why it is structurally different.** It costs **zero RAM and zero D-cache lines**. It is the only I-cache lever that does not compete with the 15.4 MB constraint or with the mechanism that sank the 43 kB `.text..fast` batch, it sidesteps every RAM-text boot failure, and a module parameter gives a **within-one-boot A/B**. That is the only practical way to chase a sub-2% effect through the ±10% lottery — but it is not free of its own bias: dead-end register **#19** records that same-boot arms are position-biased (ON 211 → 614 ms, OFF 285 → 781 ms in the same boot), and CLAUDE.md requires a fresh boot per arm. Run it as **A/B/A and require the two A arms to agree within the effect being claimed**; the A/A gap is the error bar, and if it exceeds the effect the result is unusable regardless of the B arm.

**Realistic gain.** Mechanism, measured against the shipped layout with no relink: the two best non-aliasing 2 kB sections hold **2.38% of CPU0 samples**; at the project's 5.98x flash penalty the refetch share of a locked line is 83%, so *gross* recovery computes to ~2.0% of CPU0. **Discount that**: the 5.98x figure was measured on a 72 kB self-evicting loop where *every* fetch missed (the same caveat section 4 applies to the whole flash-text family), so real recovery is well under 2%. Physics: 1-5% of CPU0, possibly negative. Call it **+0 to +2%, central ~1%**. Whether it rises once C39 packs the spine contiguously in flash is a hypothesis about a build that does not exist; neither lens supports it.

**Effort.** hours to a first number, days to ship. **Risk:** the cache is **16 kB per hart**, not 32 (IDF `Kconfig.cache` `CACHE_L1_ICACHE_SIZE=0x4000`; the TRM's 32 kB is both private caches together), so 2 ways over 128 sets is 8 kB per way — locking 2-8 kB is 12-50% of it. Sweep 2/4/8 kB; the top of that sweep can go negative.

**First experiment (≤10 min, no build) — with a live hazard.** On a booted idle board through `runsh.py`: read 0x2C000018 (size flags), 0x2C00004C (lock enables), 0x2C000058 (section sizes) with busybox devmem; run `/root/pingpong 1 1 8000` three times as arm A; lock a window over the spinlocks/trap entry/`__switch_to` flash addresses; repeat. ~3 minutes. **Before running it:** dead-end register #18 and the memory note `s31-icache-autoload-wedges` record that writing the *neighbouring* autoload ENA in this same register block (`DR_REG_CACHE_BASE` 0x2C000000) **killed the hart instantly, twice, from S-mode**. Prelock is a different register and the mechanism lens verified the offsets against the container's `cache_reg.h`, so this is probably fine — but record the register state first, expect a possible hard death, and have `reset.py` ready. `DIAG=0` means devmem is the only route, so there is no driver-side guard on the write.

### T1.5 — C04 / X1-20: the L1 cache access counters as the missing PMU

**Mechanism.** `CACHE_L1_CACHE_ACS_CNT_CTRL_REG` at 0x2C000180 (DR_REG_CACHE_BASE 0x2C000000) enables per-bus HIT/MISS/CONFLICT/NXTLVL_RD/WR counters: enable bits 0 (IBUS0 = hart0), 1 (IBUS1 = hart1), 4/5 (DBUS0/1 on the shared D-cache); clear bits 16-21; counters at 0x184-0x190, 0x194-0x1a0, 0x1c4-0x1d4, 0x1d8-0x1e8. Verified in the container's `cache_reg.h`. Nothing in the repo or in IDF has ever touched them; the overflow interrupt enables at 0x15c default to 0.

**Why it is in Tier 1 despite 0% direct gain.** Every `.text..fast` decision to date has been made on PC samples, and a PC sample cannot distinguish "stalled on a flash line fill" from "executing", cannot see D-cache displacement at all, and cannot rank candidates by refills. That is how the last four text batches were chosen, and all four measured zero or negative (#58). This is the only instrument that changes that, and it multiplies the expected value of T1.2, T1.4 and T1.8.

**Effort.** hours. **Risk:** low for the board, with the same register-block caveat as T1.4 — this is `DR_REG_CACHE_BASE`, where a neighbouring write killed the hart twice. The real risk is misreading: `IBUS1` mixes hart1's user PSRAM instruction fills with its kernel flash fills, so treat the **D-side (DBUS0 vs DBUS1) as the genuinely new number** and the I-side as contaminated.

**First experiment (≤10 min).** One `runsh.py` script: `devmem 0x2C000180 32 0x00330000` then `... 0x33` to clear and enable; dump the 18 counters; validate against a known-cold loop (`pingpong`) versus a known-hot one; the counters wrap in ~14 s so bracket short windows. ~8 minutes.

**Lens disagreement, and it matters.** C04's physics and mechanism lenses both say plausible; X1-20's physics lens rates the same registers none/negative, on the grounds that the I-side is contaminated and its bytes-to-seconds conversion uses a bandwidth model the project measured 2.7x off. **Do not buy this for the per-boot lottery** — that chase was explicitly called off (worklog-2026-09-19.md:1769-1771) and PSRAM chase latency was eliminated as its cause over 8 fresh boots (261.2→384.3 ns, 1.47x spread, no correlation).

### T1.6 — C32 (fold in X3-8): 4 MB megapages for the RV32 linear map

**Mechanism.** `best_map_size()` returns `PMD_SIZE` only when `IS_ENABLED(CONFIG_64BIT)`, so slab, page tables, page cache and stacks all run on 4 KB PTEs while the flash text already gets 4 MB leaves. PA 0x50000000 ↔ VA 0xC0800000, both 4 MB aligned; the first 12 MB is three Sv32 leaves with the top 4 MB left at 4 KB around the carve-outs. The Sv32 walker does not snoop the D-cache (`pgtable.h:644`), so every TLB miss is two uncached PSRAM reads at ~0.4-0.5 us each. **X3-8** is a strict subset with a confirmed diagnosis — `setup_vm_final`'s ordering leaves the kernel RAM image and all 281 kB of `.text..fast` on 4 KB entries — and should be folded in rather than run separately.

**Realistic gain.** Physics ~2% (range 0.3-6%); mechanism 1-3% on Quake, 3-6% on pingpong/ctxbench. The whole spread hangs on an undocumented TLB size.

**Effort.** hours. **Risk, and it is a correctness break, not a performance one:** RV32 has no `__split_linear_mapping` (pageattr.c is 64BIT-only), so a leaf can never be split afterwards; and `CONFIG_SECRETMEM` is `default y` with `__ARCH_WANT_MEMFD_SECRET` wired on rv32, which turns `memfd_secret()` against a 4 MB leaf into an unprivileged kernel-memory kill. **Disable SECRETMEM in the same build.** That mitigation is sufficient here: `images/System.map` has `__riscv_sys_memfd_secret` but no module loader and only a weak `bpf_int_jit_compile`, so secretmem is the only live caller of the rv32 leaf-rewrite path.

**First experiment (≤10 min, no kernel build).** A userspace stride pointer-chase for the TLB knee. `rootfs/membench.c:64-75` already does a dependent-load chase but with a hardcoded 64 B stride — sweep the stride past the page size instead and find the reach. Decide on that number before building anything.

### T1.7 — C86 (merged with X1-17): soft double and libm out of `libc.so`

**Mechanism.** libc's math is software double end to end: `floor()` makes 10 soft-double calls (~2 us), and even the float API evaluates its kernels in `double_t` (`__cosdf` 9 soft calls and 0 FPU ops; `sincosf` 14 and 3). Three arch override files: (1) the floor/ceil/trunc/round/modf family by integer bit manipulation, bit-exact; (2) float trig in single precision with `fmadd.s` and a 3-part Cody-Waite reduction; (3) link `rootfs/s31fp`'s `muldf3`/`adddf3`/conv into `libc.so` ahead of libgcc.

**Evidence.** Re-resolving the 12,000 samples against `images/libc.nm` at base 0x95720000: `__adddf3` 201, `__muldf3` 94, `__subdf3` 60, `__truncdfsf2` 25 = **380 samples = 3.17% of CPU0**; libm bodies 109 = 0.91%; absolute ceiling if all libc math were free = 500/12,000 = **4.17%**. The record names this as the explicit next step: worklog-2026-09-19.md:540 — "Untried and generic: libm (sqrt via fsqrt.s + integer correction, floor, etc.) by ordinary symbol interposition". s31fp is already built, bit-exact over 4M operand pairs, mul 723 → 296 ns, and has been sitting disabled since 2026-09-20.

**Realistic gain.** The two lenses do not agree and both endpoints belong in the record: physics +1.2-2.0% of wall → **16.6 → 16.8-16.95** (+0.2-0.35 fps), and it explicitly calls the candidate's figure inflated ~1.5x because `__adddf3`+`__subdf3` are 67% of the block and s31fp improves neither; mechanism 2.5-3.5% of CPU0 → 16.6 → 17.0-17.2. Honest range: **16.6 → 16.8-17.2** (both lenses quoted on a 16.6 fps boot; on this report's 16.4 baseline that is 16.6-17.0). **The three parts are not additive** — (1) and (2) delete the very calls (3) speeds up, so after them s31fp's remaining libc scope is ~0.3%. Doom is 0 (prboom is fixed point); desktop ~0.

**Effort.** days. **Risk:** the float trig is ≤1 ulp, not bit-identical — flag it. **Keep C87's route out:** redirecting the app's *own* statically linked libgcc copies from `ld.so` is the redline question, not a performance question, and it is worth only ~0.2-0.3% of CPU0 more.

**First experiment (≤10 min).** A ~40-line `LD_PRELOAD` counting shim over floor/ceil/trunc/round/modf/sin/cos/sincos/sinf/cosf/sincosf/atan/atan2/pow/sqrt (increment, tail-call `dlsym(RTLD_NEXT)`, dump from a destructor), pushed with `deploy.py` and added to the existing `setsid` line in the timedemo's run.sh. That gives caller attribution, which the PC capture cannot, and decides the whole proposal.

### T1.8 — C20 + C19 (+ X1-5 arm 1): finish the interrupt and entry spine

**Mechanism and evidence.** `images/System.map` (2026-09-22) shows the entire generic IRQ chain still in flash: `handle_irq_desc` c0054a3c, `generic_handle_domain_irq` c0054ac4, `handle_irq_event(_percpu)` c0055228/c0055264, `note_interrupt` c0057876, chip.o c005889e-c0058d16, `__irq_resolve_mapping` c0059972, `tick_program_event` c0070272, `sbi_set_timer` c0027e48, `riscv_intc_irq` c01e7520, `esp32s31_clic_handle_irq` c01e9dae, `riscv_timer_interrupt` c02d4fb4, `_raw_spin_*` c0452032-c0452352. C19: entry.S opens with `.section .irqentry.text`, so the `*entry.o(.text .text.*)` line matches an empty `.text` and the whole irqentry block links in flash — `__irqentry_text_start` c04524d8 … `__irqentry_text_end` c045277c = **676 B**, of which `handle_exception` c0452500 → end is **636 B**; the correct anchor for the relocation is the section start, not `handle_exception`. It runs twice per syscall/IRQ/fault/switch.

**Realistic gain.** Candidate claimed 4.5-5.5% of CPU0; mechanism recomputes the bootable set at **425/12,000 = 3.54%** and the gain at **+2.3-2.8% of CPU0 gross** = +0.3-0.5 fps. C19 adds +0.5-1% of CPU0 for under 1 kB of RAM. Two independent checks say it is fetch, not MMIO: per-IRQ cost doubles from ~52 us idle to ~106 us under Quake, and samples cluster at the `jalr` into flash callees.

**Why it is live again.** The OpenSBI 2-mod-4 CSR fix (commit acaa102) was landed specifically to unblock `irq/chip.o`; kernel #337 boots with it in RAM. It was not shipped because chip.o's symbols are ~0.8% of CPU0 and RAM text costs shared D-cache lines (#56, #58).

**Effort.** days, and **after** C18. **Risk:** bare globs — adding `*timer-riscv.o` to `S31_FAST_OBJS` steals `TIMER_OF_TABLES()` out of `INIT_DATA` and hangs the boot; the CLIC driver in RAM still floods "cause: 7" (#336); and the 43 kB batch measured −5% on the sdl1 canary.

**X1-5 arm 1 is the free rider, and is the one admission-rule exception in Tier 1** (its history status is already-rejected). ~2.3 kB of the flash-resident spine into the 6,360 B already reserved and unused inside the OpenSBI SRAM window — zero PSRAM, zero D-cache lines, and SRAM is in front of no cache at all. Do not extend it: the larger version is an explicit measured rejection ("REJECTED: the interrupt dispatch path in RAM (it trades the switch away)"). **Attribute the number precisely:** the 107.8 → 171.3 us context-switch regression belongs to dead-end #56's "10 kB dispatch pre-fix" arm, not to an SRAM arm.

**First experiment (≤10 min).** `scripts/board/fastarm.sh irqspine "*kernel/softirq.o *kernel/irq/chip.o *kernel/irq/handle.o *kernel/irq/irqdesc.o *kernel/irq/irqdomain.o *kernel/irq/spurious.o *drivers/clocksource/timer-riscv.o *kernel/time/tick-oneshot.o"` (leaving out the blocked objects), then h1s inside one Quake run so the fps lottery does not matter. ~7-8 min.

### T1.9 — C62 then C60: the SD request path

**C62 (hours, one line — but an untested upstream path).** Add `.common_caps = MMC_CAP_DONE_COMPLETE` to `esp32s31_drv_data` (`dw_mmc-pltfm.c:74-76`); `mmc_blk` then completes from dw_mmc's BH through `blk_mq_complete_request` instead of waking the `mmc_complete` kworker (`block.c:2244`). Physics **+0.5-1.75%, central +0.8%**; mechanism 1.95 → 1.1-1.3 ms per 4 KiB request.

Three things must be said plainly. **(1) Nothing in the tree sets this cap.** `grep -rn MMC_CAP_DONE_COMPLETE drivers/mmc include/linux/mmc` returns only the `#define` (`include/linux/mmc/host.h:423`), the accessor (`drivers/mmc/core/host.h:49`) and three consumers in `core/block.c` (2181, 2220, 2333) — **no host driver, including `drivers/mmc/host/mmc_hsq.c`**. So "the same hop HSQ removed" is not what HSQ did here, and the code path has no in-tree exerciser; the physics lens's first risk is exactly this ("dead code upstream"). **(2) The hidden cost is real:** with the cap set, `mmc_blk_mq_complete_prev_req()` returns at `block.c:2178-2181`, so `mmc_blk_card_busy()`/CMD13 never runs for *any* request — silent write-error swallowing. **(3) Neither offered mitigation works.** It is a *host* capability tested per-request with no direction check (`block.c:2181`), so it cannot be "scoped to reads"; and "keep the busy poll" means editing `drivers/mmc/core/block.c`, i.e. mature upstream MMC core, which the rules forbid. The only honest mitigation is to **prove CMD13 is redundant for this host** — that the controller's own error reporting covers what the poll would have caught — before shipping it.

**C60 (week+).** A polled host driver of our own, completing reads ≤32 KiB synchronously in `->request` — no IRQ, no BH, no kworker, no sleep. Of a ~1.95 ms 4 KiB read only 561 us is inside dw_mmc, which itself profiles at 0.0%; workqueue 18.9% + scheduler 15.4% of per-request CPU is exactly what a synchronous path deletes. Physics **+1.2%**; mechanism −0.65 to −0.85 ms per request. It must **replace** dw_mmc, not sit beside it: a coexisting fork adds ~35-40 kB to a partition with 119,803 B of slack.

**Ceiling for the family.** All storage work on Quake is **+6.3%** — on this report's 16.4 baseline that is **→ 17.4 fps** (the finder quoted 17.6 against a 16.6 fps boot; section 6's independent arithmetic ceiling of 7.1% was computed against a 16.2 fps run and lands at the same 17.4). Doom +0.4-0.7%. The desktop refault burst is where it shows: −0.2-0.3 s off ~1.6 s.

**First experiment (≤10 min, no build).** One existing fresh-boot `quake-timedemo.sh` with `PRE='cat /sys/block/mmcblk0/stat > /tmp/s0'` and the same read at the tail. Diff field 1 (reads completed) and field 3 (sectors) — **not** field 4, which sums in-flight time across requests. Sectors ÷ majflt gives the realised request size, which sizes C60 before it is written.

---

## 3. Tier 2 — worthwhile

Grouped so that items sharing an experiment are run together. Nothing here is individually resolvable on fps.

### T2.1 — One build, one A/B: kernel entry hygiene
**C34 + X1-13 + X1-15 + X1-36** (mask LCD VSYNC/TRANS_DONE unless `hw_vblank`; the vblank hrtimer under a refcount), **C42 + X1-12 + X1-16 + X1-33** (clear HRTICK/HRTICK_DL, default-on in 7.1 against upstream's off), **C22 + X3-21 arm 4** (cache the timer/IPI `irq_desc` to skip riscv-intc's radix walk), **C33 + X1-23 arm A** (compile out the always-on ktime/atomic64 stats in `esp32s31_cache_range`, which run under a raw spinlock with IRQs off on every `set_pte` and every damage row).

Sizes, measured not estimated: LCD 52 IRQ/s carrying zero work = 0.3-0.7% of CPU0; vblank hrtimer 42/s = 0.5-0.9%; HRTICK 0.2-1.0%; cache stats 0.5-1.5% of CPU0. **Components sum to 1.5-4.1% of CPU0; quote +1.0 to +1.8% after the interference haircut that every previous bundle here has paid.** One day, one build, judged on `/proc/interrupts` rates and h1s sample share.

**C42's ship path must be named, not just its A/B route.** The A/B is a devmem write to `sysctl_sched_features` at c086fad0 / PA 0x5006FAD0 (0x1BE1FBDF → 0x1BE1CBDF) and that is fine as a measurement. The *shipped* form must not be a patch to `kernel/sched/features.h` — that is upstream scheduler source, even though it only restores the ≤7.0 default. Ship it from our own arch code or an `early_initcall` that clears the two bits.

**Two arms must be excluded, and two more dropped outright.**
- Do **not** remove the vblank hrtimer outright — the driver's own comment at esp32s31-lcd.c:1770-1776 records that relying on the refcount left armed flip events undelivered and weston's commits failing with "Resource busy" until it stopped repainting; gate it on `.enable_vblank` instead.
- Do **not** mask the PPA or GDMA ch24 interrupts (X3-21 arm 3): the PPA ISR is the sole completion source, the 800x480 op this workload runs waits ~3,991 us against a 300 us spin cap, so masking it costs **~6-7% of CPU0** in spinning, and "dma ch24" is the LCD's m2m copy channel (20343000), not the PPA (20345000).
- **Dropped: X1-18's entropy arm.** Disabling `add_interrupt_randomness`+`fast_mix` (47/12,000 = 0.39%) weakens the entropy pool on a board running Wi-Fi and Bluetooth. CLAUDE.md: "Do not disable Bluetooth (or other features) to buy performance." Its own history lens flagged it as feature-removal-for-speed. The rule-compliant remainder of X1-18 is 0.18% = +0.03 fps.
- **Demoted: `noirqdebug`.** It turns off `note_interrupt`, the kernel's stuck-interrupt guard — the one message that says "irq N: nobody cared" and disables a screaming line, on a board whose whole diagnostic story is "dmesg is useless by construction". For 0.175% of CPU0 that is a bad trade as a shipped `CMDLINE_ADD` default. Use it as a **measurement knob** to price the guard, never as a default.

### T2.2 — Touch poll scoped to fullscreen
**C35 + X1-35**, with **C12** as the larger form. The GT1158 has no INT line, so the driver polls: 3 i2c messages and 4 wake/sleep cycles per poll, 43.9 i2c IRQ/s measured, and 100 of 154 idle context switches/s. The knob exists (`idle_poll_ms`, 0644): 60 → 500 took ctxt 154 → 54/s, i2c 43 → 5/s, cpubench +3.8%. It was withdrawn for first-touch latency — but a fullscreen client holding the grabs needs no touch responsiveness at all, which is the scoping the withdrawal never considered. Physics ~2% on fullscreen (range 1.3-3.4%); mechanism ~1%.
**Experiment:** A/B/A with `oncpu 0 cpubench` at `idle_poll_ms` 60/500/60, ~5 min, no build. **Same-boot caveat (dead-end #19):** same-boot arms are position-biased (ON 211 → 614, OFF 285 → 781 ms in one boot). The A/B/A shape partly answers that; state it explicitly and use the A-to-A gap as the error bar — if it is larger than the claimed effect, the run proves nothing and the arms must go to fresh boots.

### T2.3 — Swap-in request shape, two knobs, no build
**C49 + X1-9** (+ X1-27, X2-10, X2-19 as variants). `echo 0 > /sys/kernel/mm/swap/vma_ra_enabled` plus `vm.page-cluster=2`. Every recorded page-cluster sweep ran in VMA mode, where the window is centred on the faulting *address* and each neighbour is its own 2.25 ms request; physical-slot mode reads adjacent *slots*, which `swap_read_folio_fs()` merges into one `->read_iter` of up to 32 pages. `vma_ra_enabled` appears in zero of 1,065 commits. Physics +0 to +2.6%; mechanism +0.5-1.5%, ceiling ~2%, with a 30-40% chance of a small regression at page-cluster 3 and a real risk at 260-788 kB MemAvailable.
**Experiment:** two fresh-boot `quake-timedemo.sh` runs with the knob in `PRE` and `/sys/block/mmcblk0/stat` before and after; judge on majflt and sectors read, never on fps. ~7 min.

### T2.4 — The last X round trip
**C94 + X3-11 route (a)** (+ **X2-11** for the windowed case). MIT-SHM carries `ShmCompletion` precisely so the client need not block; our xshim advertises event base 0 because it never sends one, and our xlite hardcodes `sendEvent=0`.

**Disclosure, because the record says otherwise.** "Both ends are ours" is false as stated. Sending `ShmCompletion` is only half the change: **SDL 1.2 has no ShmCompletion handling anywhere**, so route (a) as originally written also needs `X11_NormalUpdate` to pass `send_event=True` plus brand-new SDL code to consume the event and hold the wait until the next `SDL_Flip`/`UpdateRects`/`LockSurface`. libSDL1.2 is a Buildroot package, and X3-11's history lens recorded both routes as **violates-rules** on exactly that basis. The harness rules do name SDL1.2 as platform-side, so this is arguable rather than settled — but it must be argued, not resolved silently. **The clean version lives entirely inside xlite:** defer the wait to the next `XShmPutImage` rather than sending an event the client cannot consume. Do that one if it holds.

Physics +0.2-1.5% Quake / +0.4-3.1% Doom; mechanism +0.5-1.0% / +1-2%. The ceiling is the measured `ppoll` share, 1.8% of the main thread = **+2.5% absolute**; the finder's 5.5-6.3 ms/frame is a waitsamp artefact class (`sys=0 pc=0` is `task_current_syscall()` returning -EAGAIN, not a wait class). Route (b) (MAP_FIXED buffer swapping under a cached `vid.buffer`) is net-negative and visually broken. X2-11 (drop the pre-reply 64 kB copy) is ~0 in fullscreen because late present makes the copy load-bearing there, but is real in the windowed path where late present measured neutral (13.859 vs 13.892 ms, 5 fresh boots per arm).

**Experiment.** `XLITE_NOSYNCWAIT` **does not exist** — `grep` over `xlite/` and `lvdesk/` returns nothing; the shipped knobs are XLITE_RING, XLITE_NOSHM, XLITE_NOMITSHM, XLITE_SHMFLUSH, XLITE_VISUAL, XLITE_IBUF, XLITE_QSTART, XLITE_TRACE/_INPUT, XLITE_INSTRUMENT, XLITE_IMG, XLITE_BACKTRACE. **Write it:** a ~5-line `XLITE_NOSYNCWAIT=1` bypass in xlite's XSync (`xlite/xlite.c:963` — flush, set `x->shm_seq`, return *without* `xlite_reply`), push it with `deploy.py`, and measure on the **sdl1-indexed canary, not Quake**. It deletes the wait outright and strictly dominates both routes, so it bounds the whole family in one boot.

### T2.5 — Instrument upgrades that are free
**X2-14** — the capability already exists: `rootfs/pcsample.c` and two siblings already symbolise user PCs per process. The work is to **port that into `scripts/board/h1s-report.py`** so it resolves user PCs against the unstripped `.dbg` twins already in `rootfs/`, not to teach the project something new. Two lenses did exactly this offline during this review with no board time and produced the first real decomposition of the 59% (D_DrawSpans8 11.90%, D_DrawZSpans 4.62%, `__muldf3` 2.57%, R_DrawSurfaceBlock8 mip0+mip1 3.91%). ~30 lines, retro-fits every capture in `artifacts/`. **X3-16's free half** — read `voluntary_ctxt_switches` from `/proc/<tid>/status` during a timedemo; combined with waitsamp's 9.9% duty that gives the mean block duration and settles "one wait repeated 30 times a frame" versus "noise spread thin" with no kernel change. **C45** — re-price the four structural milliseconds (7-8 ms kworker hand-off, 0.7-1.1 ms PPA wake, 1.3-5.8 ms socket syscalls, 13 ms fork) now that the CLIC-level bug, OpenSBI-in-SRAM and softirq.o-in-RAM have all landed; `defer_copy`, `ppa_async`, `ppa_spin_us` are all 0644 params, so it is an A/B/A inside one boot with `dirtybench` — **subject to the same dead-end #19 caveat as T1.4 and T2.2: report the A-to-A gap and treat it as the error bar.** **X3-10/X2-18** — tag existing captures by frame and process; no new code needed for the first cut.

### T2.6 — Internal SRAM tier riders
**C21** and **C15** (finish OpenSBI's SRAM fast window with the per-trap callees). The evidence is the strongest single data point on the board: OpenSBI's trap dispatch into SRAM took the context switch 280.7 → 57.2 us and SD 4k p50 3.79 → 2.21 ms, with the first 4,976 bytes alone worth 157.5 → 80.0 us. But the budget is ~6,144 B plus a 3,968 B gap and nothing more (hart0's heap has 4-12 kB free), so this is worth +0.3-1% of CPU0 and only as a rider on T1.8.

### T2.7 — Desktop-side items (0% on the fullscreen metric, by construction)
These matter only under the desktop-responsiveness framing; all three rankers agree they are zero on Quake and Doom because `fs_render_set(0)` makes LVGL paint nothing in fullscreen.
- **C51** — return ~240 kB of daemon RAM (udevd's 160 kB anon + 80 kB of /run/udev tmpfs; the other ~200 kB of the headline 440 is clean, evictable page cache). Nothing consumes its rules now that devtmpfs makes the nodes, lvdesk watches /dev/input itself, HID is on hart0 and Weston is gone. Physics rated it refuted purely on Quake fps; against ~1.3 MB free at an idle desktop it is 18-34% more headroom. **Two recorded risks the summary must carry: remove, do not replace, and verify hotplug and `/dev/uhid` first.** Substituting busybox mdev as the `/sbin/hotplug` handler forks per uevent, and a process launch is 0.16 s on this board — 269 devices at coldplug is ~43 s of forks, strictly worse than udevd's already-backgrounded nice-19. The history lens additionally cautions on removing a feature at all. The anchoring evidence runs the other way for a reason: mlockall on lvdesk moved 1.25 MB from clients to the compositor and took average input lag 170 → 367 ms, worst 610 → 1163.
- **C77** — make direct expansion and fast present z-order aware instead of declining on any overlap: +5-6% of a windowed frame whenever another window overlaps, 0 on the single-window canary.
- **C70** — save-under hardware cursor under direct scanout. A regression repair: direct scanout took the cursor plane away as a side effect. Measure the LVGL software cursor's real cost first (`uinject park` vs `uinject stress`, two 10 s windows on `/proc/<lvdesk>/stat`) — `scanout-direct-plan.md:330` lists that as an open question and the 19 ms figure in the driver comment is X's, not ours.
- **C93** — xlite GC value caching: ~174 of 550 ChangeGCs removable, ~12-16 ms of a 505 ms maximise (−2.4 to −3.2%) once the profiler's own `clock_gettime` overhead is subtracted. The xtlite half is inert.
- **C55 (MADV_WILLNEED half only)** — lvdesk knows intent (hover, press, focus, fullscreen exit) and can `process_madvise(MADV_WILLNEED)` a background client's anon VMAs before the click lands: 845 ms/MB → ~310 ms/MB. **Drop the PAGEOUT half** — `mm/madvise.c` skips any folio with `mapcount != nr_pages`, so every xshim client surface is untouchable, and PAGEOUT queues SD writes rather than freeing pages.
- **C68** — the diagnosis is confirmed and belongs in the record even though the gain is ~0: on rv32 `_PAGE_NOCACHE`/`_PAGE_IO`/`_PAGE_MTMASK` are literally 0, so `pgprot_writecombine()` maps ordinary cached memory and the "write-combine" story in two docs and a memory note is impossible. `CONFIG_CMA_ALIGNMENT=8` starts every large CMA buffer at colour 0 of the 32 kB-per-way D-cache. First arm is a `blendbench` size sweep, ~4 min, no build.
- **C76's unresolved sub-question** (see section 5): ~3 ms/frame of lvdesk unattributed in `xshim_poll_ready` outside `handle()`.

---

## 4. Compounding

**What shares one pool and must not be added.** T1.2 (C39), T1.4 (C23), T1.8 (C20/C19/X1-5) and T2.6 (C21/C15) all draw on the same quantity: **kernel code executing from 80 MHz flash that is not idle = 19.82% of CPU0**. The 5.98x flash-vs-RAM figure was measured on a 72 kB self-evicting loop where *every* fetch missed, so real code recovers far less — and that discount applies inside T1.4's arithmetic too, not only here. Their joint realistic cap is **+4 to +6% of CPU0**, not the sum of their individual estimates (~9%). C39 is the cap; C20/C19 and C23 are instalments inside it.

**What genuinely compounds, multiplicatively.**
- **C37 → C39, twice.** Fewer instructions means fewer flash bytes fetched *and* ~187 kB of returned linux-partition slack, which is precisely what bounds how much C39 may move into RAM.
- **C18 → C39/C20/C23.** Until the glob and early-NX defects are fixed, spinlock.o, sched_clock.o, memcpy and the CLIC driver cannot be relocated at all; C18 raises the ceiling on all three rather than adding to them.
- **C04 → everything in the text family.** It converts placement from argument into refill counts. Four consecutive batches chosen by argument measured zero or negative.
- **C32 (TLB) is disjoint** from all of the above and covers the 11.83% of CPU0 already running from `.text..fast`, which the text family does not touch.
- **C86 is disjoint** — it sits in the 7.78% library bucket, orthogonal to every kernel item, and applies to Quake only.
- **T2.3 + T2.4 + T1.9 are sub-additive** inside the ~6.3% total storage ceiling: physical-slot readahead merges the requests, WILLNEED issues them, DONE_COMPLETE and C60 cheapen each one.

**Realistic combined figure, and the double-counting to avoid.** Model: the main thread is 86% busy and 14% waiting, so if platform work makes CPU0 work *k* times faster with waits unchanged, fps scales by 1/(0.86/k + 0.14). Central estimates: text family +4-6% of CPU0, frame pointers +1.5-2.5%, TLB +1-3%, libc soft-double +1-2.5% (Quake only), hygiene bundle +1.0-1.8%, touch +1%, storage+swap +1.5-2%, last round trip +0.5-1.5%. Compounded, that is *k* ≈ 1.10-1.13, and the formula gives **16.4 → 17.8-18.2 fps median** (k=1.10 → 17.79, k=1.13 → 18.20). **Discount to ~17.5-18.0** because interference between these arms is the norm here, not the exception. Add a working 400 MHz clock and it becomes **18.8-19.7**; at the safer 360 MHz point, 18.2-18.7. Naively summing the individual claims gives ~+25%, which is wrong by roughly a factor of two and is exactly the error the review is guarding against.

**Measurement discipline this forces.** Only X3-1 is individually resolvable on a Quake timedemo. Everything else must ship as a bundle and be judged on `pingpong` medians, `/proc/interrupts` rates and h1s sample shares, with fps used only as a multi-boot median at the end. Two changes in this project's history looked like 12-18% wins and were both inside a ±22-40% band. Where a same-boot A/B/A is used instead of fresh boots per arm, dead-end #19 applies: report the A-to-A gap and treat it as the floor on what can be claimed.

---

## 5. Improving the three past wins

### SMP (hart0 lent as CPU1)
The win is banked and the obvious extensions are closed by measurement. CPU1 is **63.7% idle** during a timedemo (`artifacts/quake/placement-061521/run.log`: 573 busy ticks against 1,021 idle over 1,603 ticks = 16.03 s), lvdesk owns 480 of CPU1's 573, and **forced placement of any kind loses**: co-located 16.43/15.56 ms, split 16.13/16.09, free 13.83; a pinned lvdesk is +15.3% worse; `capacity-dmips-mhz` measured worse and was reverted. Dual pinned CoreMark (1048 + 1064 vs 1059 single) says two busy harts do not contend measurably.

What is still worth doing: **C45** — re-price the 7-8 ms kworker hand-off, whose mechanism was never found and which fits the CLIC-level signature (a task switched in from an IRQ stayed deaf until some `sret`) that `patches/0057` fixed. That single number gates whether lvdesk's 3.6 ms expand plus present can move to the idle hart at all, which is step 5 of the SMP plan and is currently forbidden by a constant that may no longer be true. Cost: ~5 min with `dirtybench` and existing 0644 params. Everything else in this family is refuted: X3-7 (SCHED_MC/`sd_llc`, 0.71% of one CPU and it lands on the idle hart), C30/X2-8 (tick duty to CPU1, +0.2-0.5% with a systematic 10 ms timer-wheel penalty), C13/X1-19 (device IRQs to CPU1, ~1% of a core total), C46 (hosted RX on CPU1, ~1 IRQ/s when the network is idle), X2-9 (PIE bounces measured at ~3/s board-wide), C07 (the lent CPU is at most 0.82-0.84 of a core and CPU1 is idle anyway).

### RAM text (`.text..fast`)
The mechanism is proven — softirq.o alone took a same-hart futex round trip 675 → 542 us (−19.7%), and 113 kB of timer+storage took the timer IRQ 860 → 465 us and SD 12.07 → 8.05 ms per request. But **object granularity is spent**: #320's profiled 43 kB batch measured −5% on the sdl1 canary, rcu/tree.o −1.7%/+4%, the DRM commit batch <1% of wall, a cold xattr.o −7.1%. Of 286,016 B of PSRAM, **220,898 B across 2,925 symbols took zero samples in 12,000**.

The three extensions worth building are exactly T1.2, T1.4 and T1.8, in that order, with T1.5 to aim them and C18 to unblock them. The important structural point: the region's growth is a *trade*, not a free win — the linker script itself records that newly cached text evicts data both harts use — so the goal is **coverage per byte**, not more bytes. And note what function granularity cannot reach: `.sched.text`, `.spinlock.text`, `.irqentry.text` and entry.S assembly, which is why C19 and the SRAM tier still have a job.

### Cross-layer flattening (lvdesk + xshim + xlite)
This is the well that is closest to dry, and it is closed by measurement rather than by argument: MIT-SHM adoption gave 31.7 fps but flickered (weapon sprite) and is illegal; scanning out the client's buffer is closed; PPA CLUT expansion measured −7.5% windowed; a windowed 32-bit SDL2 visual lost three times; the af_unix socket the ring replaced is already gone; the XLITE-SHM answer cache measured nothing; shared window buffers gave +5.6% against a predicted +20-25%.

What is left: **C94/X3-11 route (a)**, in the xlite-only form described in T2.4 — ceiling +2.5% on Quake and +1-2% on Doom; **X2-11** in the *windowed* path only, where late present measured neutral so the pre-reply copy is pure waste; **C77** z-order clipping, which is what keeps fast present eligible once a second window is open and therefore compounds with X2-11.

**C76's unresolved sub-question is larger than any of those, and it is not a new discovery.** ~3 ms/frame of lvdesk sits in `xshim_poll_ready` outside `handle()`, unattributed since 2026-09-20 (`LVDESK_PROF` input 8.8 ms/frame against xsp handle 5.7) — that line is verbatim in C76's own evidence array, and C76's mechanism text names `xshim_poll_ready` as a HOTTEXT target. C76 is rated [rejected/refuted/weak] and sits in section 7 at +0 to +1% Quake, so carry its discounts with it: the mechanism lens recomputed lvdesk at **17.3 ms per client frame** (not the 18.5 the proposal used) from `artifacts/quake/placement-061521` over 1,603 ticks = 16.03 s, and **CPU1 is 63.7% idle**, so time freed inside lvdesk returns to a hart that already has spare time. That is the same reason T2.7 says desktop items are 0% on the fullscreen metric by construction. It is still worth one `XSHIM_PROF`/`LVDESK_PROF` session on a warm board before any compositor code is written — as an attribution question, not as a claimed win.

---

## 6. Paging

Apps stay on SD and page from it; that is a hard rule, and every proposal below respects it.

**What the measurements actually say.** A Quake demo takes **1,227-1,556 major faults** with **VmSwap 6.1-6.6 MB** and **MemAvailable 260-788 kB** (`artifacts/quake/td-*/run.log`). Three independent instruments bound the cost: the majflt accounting puts all paging at ~0.5 s of a 60 s demo (0.83%); `waitsamp` puts the main thread's D-state page faults at ~2% of its time (4.59% in the stricter run); the PC sampler puts mm/faults at **1.7% of CPU0** (`do_swap_page` 0.9 + `handle_mm_fault` 0.8). The arithmetic ceiling — every fault free — is 1,282 × 3.3 ms = 4.23 s of 59.8 s = **7.1%**, i.e. 16.2 → 17.4 fps on that run's own baseline (on this report's 16.4 baseline, 16.4 → 17.6; T1.9 quotes the same family at +6.3%, 16.4 → 17.4).

**Two maximal controls already returned zero.** TyrQuake run from XIP flash removes 100% of the app's file-backed text paging: **10.7 vs 10.9 fps, 18% fewer major faults** — "flash placement was never the source of the gain". `rootfs/locktext` mlocked every r-xp mapping (1,456 kB, nothing failed): **863 vs 825 faults, 15.4 vs 15.1 fps**. An 18% fault reduction bought nothing, and eliminating file-backed paging entirely bought nothing. The residue is anonymous heap swap, which neither control touches.

**What is still worth trying, in order.**
1. **T2.3 — physical-slot swap readahead** (`vma_ra_enabled=0`, page-cluster 2-3). Two sysfs/sysctl writes, no build, reversible in a second, and genuinely never run on this board. +0.5-1.5%, ceiling ~2%, with a real regression path at page-cluster 3.
2. **T1.9 — the request path** (`MMC_CAP_DONE_COMPLETE`, with the CMD13 question settled first; then the polled host driver). This is where the desktop refault burst lives, not Quake.
3. **T2.7 — C55's WILLNEED half.** Prefetch on intent from the compositor; compounds with (1) rather than adding to it.
4. **Memory returned is the honest lever, not fault-path tuning**: C51 ~240 kB of true anon+tmpfs (the rest is evictable page cache), X1-11(a) 180-220 kB of PSRAM. Against ~1.3 MB free at an idle desktop that is meaningful for interactivity; against Quake's 6.2 MB swap deficit it is ~3-7% of the gap and worth ~0 fps.

**What is closed, with the reason.** Large page-cache folios (X3-18) are a **literal no-op on rv32**: `mapping_max_folio_order()` and `mapping_min_folio_order()` both open with `if (!IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE)) return 0;` (`pagemap.h:460-474`, independently confirmed) and THP is unselectable (`select HAVE_ARCH_TRANSPARENT_HUGEPAGE if 64BIT && MMU`) — dead-end #90's conclusion is right, its stated reason should be amended to cite the accessor gate. The `mmap_miss` ratchet (X3-19) is real code but the latch is demonstrably not dominating: read_ahead_kb still moves prboom's faults 314 → 97 across the sweep, so read-around is firing. Backing the heap with a file (X3-20) puts 10 MB of continuously re-dirtied memory into `balance_dirty_pages` accounting on a board whose dirtyable memory during the demo is on the order of 100 pages. zswap/zram (C48, X1-10) — capped zram cannot fall through; at the limit it returns -ENOMEM and the folio is re-dirtied. Readahead width (X1-32, C50, C53, X1-28) — every arm that read more did worse: 512 kB read 98 MB against 16 kB's 5.4 MB per demo, with 586 vs 386 faults and 18.8 vs 19.9 fps. KSM, hugetlbfs, a CPU1 prefetch daemon and MGLRU are all unavailable or priced out (#91, #40).

**One standing constraint for anything in this section:** at 260-788 kB MemAvailable, every speculatively fetched page must evict a resident one, and evicting a dirty anon page is a ~2.5 ms SD write. Break-even on a 4-page window needs roughly a 75% hit rate. That is the arithmetic behind every negative result above.

---

## 7. Rejected in this review

Every id not in Tier 1 or Tier 2, grouped by theme — and every id in both input lists is accounted for: all 95 candidate ids (C01-C95) and all 88 discovered ids (X1-1..X1-38, X2-1..X2-26, X3-1..X3-24) appear somewhere in this report. Lens verdicts in brackets are history / physics / mechanism.

### Clocks, caches, bus and hart0 firmware
- **C01** [retry/weak/weak] Cache autoload prefetchers from the loader — ceiling +4.6% of CPU0 at 100% accuracy; ENA from S-mode killed the hart twice, from the loader it read back a no-op (register still 0x2).
- **C02** [novel/weak/weak] Critical-word-first + early restart — +0.5-1.5% central, ceiling ~3-4%; as proposed 0% or negative (see X1-21).
- **C03** [retry/weak/weak] Flash above 80 MHz by a hand port of MSPI timing tuning — physics +4-5% on Quake (28% of CPU0 executes from flash) but **unreachable**: the S31 LL `HAL_ASSERT(false)`s on any flash core clock other than 80, `MSPI_TIMING_FLASH_NEEDS_TUNING` is `(MODULE_CLOCK > 80)` and Kconfig tops out at 80. *Record correction: the recorded reason ("there is no mspi_timing_tuning/port/esp32s31/") is stale — the port exists in the container IDF and PSRAM DQS tuning runs every boot; the conclusion still holds for a different reason.*
- **C05 / X1-7 / X1-24** [novel,retry,novel / weak,refuted,refuted / weak,plausible,weak] AXI bus performance monitor at 0x20511000 — 0% direct; the `pclk_khz` sweep already answers the headline bus question (45.3 → 26.9 MB/s of scanout moved Doom inside its own spread).
- **C06 / X1-4** [novel,retry/refuted,weak/weak] AXI QoS for the cache-refill master — 0-2%, most likely under 1%; bounded by the same scanout null test.
- **C07** [novel/weak/weak] Account for and cut hart0's own load — the lent CPU is at most 0.82-0.84 of a core and CPU1 is 64% idle; Quake sees it only through a 1.04 ms/frame ppoll.
- **C08** [done/refuted/refuted] Pin 320 MHz / real performance governor — exactly 0%; the clock never leaves 320 MHz.
- **C10** [novel/refuted/refuted] Hart0 doorbell coalescing — the doorbell ISR is 0.66 us of a 165 us crossing.
- **C11** [novel/refuted/refuted] Coalesce HID reports on hart0 — exactly 0 on a timedemo; no hosted line appears in the Quake IRQ census.
- **C13** [rejected/refuted/refuted] Forward compositor IRQs to CPU1 — device-IRQ-only symbols are 191/12,000 = 1.6% of CPU0 total.
- **C14** [novel/refuted/refuted] Reclaim hart0 loader flash for platform text — the loader is 1,857,376 B in a 1,900,544 B partition; the whole fault subsystem is 1.7% of CPU0.
- **X1-1** [novel/refuted/**strong**] MHCR branch predictor/BTB/RAS — **the boot ROM already does `MHCR |= 0x1030` on both harts before the mhartid split** (0x2f8046a4). Keep the one-line readback in `esp32s31_early_init()` to turn the argument into a fact; expect 0%.
- **X1-2** [retry/weak/weak] I-cache autoload prefetcher over the flash window — the flash port is serial, so a prefetcher cannot add bandwidth; bear case −5% to a starved hart.
- **X1-3** [rejected/refuted/refuted] Flash at 120 MHz — duplicate of C03; 22-32 MB/s measured against a 34.6 MB/s ideal at 80.
- **X1-8** [rejected/refuted/refuted] Pin the MSPI/PSRAM timing point across boots — the tuned point is two IO-pad registers inside a latency fixed at 18 dummy cycles; killed on 8 boots with no correlation.
- **X1-21** [done/refuted/refuted] Critical-word-first / early restart — **bit 4 = 1 *disables* early restart, which is already on at reset**; writing it is a regression. The WRAP half is capped at ~1.2% of CPU0.
- **X2-1** [rejected/refuted/weak] PMA per-region cache-policy bits — 0% in fullscreen; the target region receives no CPU accesses at all.
- **X2-2** [novel/refuted/weak] Posted writes on the CPU/SDMMC bridges — 0.05-0.3% of a core.
- **X2-3** [rejected/refuted/refuted] 2D-DMA M2M block descriptors — Quake and Doom fullscreen are a *scale*; an M2M pair cannot interpolate.
- **X2-4** [rejected/refuted/weak] Thirteen diagnostic stores per lent-CPU trap — a complete guest-trap round trip measures 696 cycles, which bounds the whole tax.
- **X3-13** [rejected/refuted/plausible] PSRAM/flash clock differential as a universal cache-idea pricer — instrument, 0% direct. History: the flash arm is **already done in the field**, because DIO → QIO at a constant 80 MHz is exactly the same-clock 2x fetch-bandwidth control it asks for, with the slope on three real workloads recorded at `docs/current-state.md:1346-1364`. Physics refuted on magnitude: in-cache memcpy 177 MB/s against 102 MB/s to PSRAM (`docs/accel-plan.md:42-50`) puts a **hard 42% ceiling** on the PSRAM stall fraction of any code on this board, reached only by pure streaming copy, and the realistic addressable recovery for Quake computes to +2.5-3.9%. Mechanism plausible and plumbed — `esp_psram_impl_ap_oct.c:418-444` is the only place the PSRAM clock is set and both knobs act — but the plan is under-powered: 2 boots per arm cannot resolve anything under ~20% against a ±10% lottery and a ~17% between-boot spread, so it needs 5+ boots per arm. Its "free rider" (lottery = timing calibration) was killed on 8 boots on 2026-09-21. If the memory-latency share is still wanted, run 200 vs 100 MHz (both on MPLL 400) and take the x-axis from measured `membw` ratios, not nominal clock.
- **X2-16** [novel/refuted/weak] Dump the D-cache tag array — hart0's share is already answered at ~0 from source; week+ for no lever.
- **X2-17** [done/refuted/weak] Memory-level parallelism — the instrument exists (`rootfs/membw.c`) and the three-way coincidence dissolves once units are fixed (102 MB/s is traffic, 192 is the PPA's fill).
- **X3-3** [novel/refuted/weak] D-cache preload engine as a prefetch primitive — the 2x arithmetic rests on `membw`'s chase being 64 B-strided; on ilp32 it is 32 B.
- **X3-5** [rejected/refuted/weak] Page colouring for conflict misses — Quake's screen (76.8 kB), z-buffer (153.6 kB) and surface cache (~650 kB) each individually exceed the whole 64 kB D-cache, so these are capacity misses and colouring has nothing to remove.

### Kernel text placement and codegen
- **C24** [retry/weak/weak] Hosted input/RX path in RAM — 0 on every headline workload; no input device activity in a timedemo.
- **C25** [rejected/weak/weak] Hosted transport copy/checksum — 3.5-4% of CPU0 but only while Wi-Fi moves bulk data.
- **C26** [novel/refuted/refuted] Wait/wake spine in RAM — futex+eventfd is 0.095% of CPU0.
- **C27** [rejected/refuted/refuted] `sg_next` and slab fast paths on the SD path — ceiling 0.367% of CPU0.
- **C28** [novel/refuted/refuted] Make `/proc/profile` count `.text..fast` — 0%; the shipping kernel does not contain the code.
- **C38** [retry/weak/refuted] Zcmp+Zcb — 0.85% denser code × 19.6% of CPU0 = 0.157% = +0.03 fps.
- **C40** [novel/refuted/weak] `-O2` for the RAM-resident objects — 0.4-0.9% of CPU0 gross, material chance of net negative.
- **C41** [novel/refuted/weak] Static Zbb + `INIT_STACK_ALL_ZERO` → NONE — +0.03 to +0.09 fps.
- **X1-11** [rejected/weak/plausible] Function-granular `.text..fast` — half (b) is dead-end #55 post-fix; half (a) (220,898 B at zero samples) is real RAM but ~0 fps, and is folded into C39. Its "IRQ-dispatch subset = 4.6%" figure does not reproduce; the eleven functions it names sum to 3.07%.
- **X1-5** (beyond arm 1) [rejected/weak/plausible] `.text..sram` for the dispatch path — the named target is an explicit measured rejection; the 107.8 → 171.3 us context-switch regression belongs to dead-end #56's 10 kB dispatch pre-fix arm.
- **X2-7** [rejected/refuted/weak] Inline the lock/IRQ helpers — 0.1-0.5% of CPU0; ceiling +0.31 fps.
- **X2-15** [novel/refuted/weak] `minstret` / CPI — **`minstret` is absent from the S31 core's sleep-retention CSR inventory** while the esp32h4 control in the same IDF lists it; the instrument cannot be built.
- **X3-2** [rejected/refuted/refuted] Shrink `.text..fast` to ~16 kB — **net negative, ~−1 to −2 fps**; it evicts the SD/blk/mmc path back to flash to recover 270 kB of a 15.4 MB machine.
- **X3-14** [done/refuted/plausible] Pad-sweep the kernel text — the geometry is already written down (`docs/s31_hardware/s31_cache.txt:5-6`); ≤1.5% of CPU0 and not reproducible.
- **X3-15** [rejected/refuted/weak] Footprint-swept pingpong + victim-side refill tax — the 2-D sweep was run and the mechanism formally retracted.

### Interrupts, timers, scheduler, IPC
- **C16 / X1-38** [novel,rejected/refuted/refuted] Own clockevent driver / program the timer from S-mode — 0.06-0.3% of CPU0; the store to 0x10004000 from S-mode faults, so it cannot be built as written.
- **C17** [retry/refuted/weak] Lazy PIE/HWLoop by trap-on-first-use — 0.3-0.6% of CPU0; the coproc save itself is <100 cycles.
- **C29** [retry/refuted/refuted] **/ X3-24** [rejected/refuted/refuted] Re-test tickless idle — 0% to slightly negative (X3-24's mechanism lens computes −0.5% to +0.3%, centred on 0: CPU1 takes 185 timer IRQ/s and at HZ=100 at most 100 of them are the tick), and it reopens a silent total hang (≈1 death per 2 runs against 7/8 + 5/5 on the periodic tick) that takes hart0's hosted link with it and therefore erases its own evidence. The fix offered as the reason to retry (`hrtimer_rearm_deferred`) is `#ifdef CONFIG_SMP` + `clic_on_lent_cpu()`-gated (`irq-esp32s31-clic.c:930-931`) and cannot have touched the UP-era hart1 death (`docs/current-state.md:7498-7505`).
- **C30 / X2-8** [novel,rejected/refuted/weak] Hand the tick duty to CPU1 — +0.2-0.5%, with every timer-wheel timer on CPU0 firing one jiffy late.
- **C31** [rejected/refuted/refuted] rdtime clocksource / SMP-safe systimer / rv32 vDSO — 0.03% of CPU0; the systimer half costs time once C18 lands.
- **C36 / C95** [rejected,novel/refuted/refuted,weak] Per-boot lottery via the vblank hrtimer's CPU / page-colour fingerprinting — 0% and 0 to −0.5%; total success on the lottery is +6.5% on the mean and +0.0% on the best boot.
- **C43** [novel/weak/weak] Poll-before-wfi (`TIF_POLLING_NRFLAG`) — +0.1-0.4% Quake, plausibly −10 to −25% on cross-hart pingpong.
- **C44** [novel/weak/weak] Spin-then-sleep on the XLITE-RING — can only remove the reply-direction hand-off; calibrated against late present it is a fraction of a percent.
- **C46** [rejected/refuted/weak] Hosted RX on the lent CPU — the hosted doorbell runs at ~1 IRQ/s with the network idle.
- **C47** [retry/refuted/weak] Wake-to-run latency tuning under a CPU-bound competitor — 0 to +1%, outer bound +2.4%.
- **X1-14** [rejected/refuted/weak] Three CLIC MMIO writes + three spinlock/mmiowb pairs per IRQ — 0.2-0.65% of CPU0, and `handle_fasteoi_irq` is unsafe on this CLIC.
- **X1-18** (entropy half) [rejected on rules/refuted/weak] Disable `add_interrupt_randomness`+`fast_mix` — **rule break** (feature removal for speed on a board running Wi-Fi and Bluetooth); 0.39% of CPU0. The rule-compliant remainder is 0.18% = +0.03 fps.
- **X1-19** [rejected/refuted/weak] Device IRQs off CPU0 on a corrected cost model — +0.2-0.5% for the safely movable subset, negative if completion IRQs are included.
- **X2-23** [rejected/refuted/weak] `WF_SYNC` on the ring doorbell — ceiling +0.29%; the outcome it produces (co-locating client and compositor) is already measured as losing.
- **X2-25** [done/refuted/weak] An instrument for runqueue wait — the 0.84 in "17.3/0.84 = 20.6" is tick accounting, not waitsamp's R-state; runqueue wait is ~2 pp of wall.
- **X3-7** [novel/refuted/plausible] `SCHED_MC` / `sd_llc` — total addressable budget is 0.71% of one CPU and it lands on the 63.7%-idle hart.
- **X3-16** [done/weak/plausible] Wake ledger in `__schedule`/`try_to_wake_up` — history already-done: no scheduler patch exists in the tree, but three of its four questions are already answerable today and one is contradicted by the very line it cites as evidence (`docs/current-state.md:5539`, where "cut idle wakeups" was **withdrawn** on exactly that listing); the in-tree `SCHED_STATS`/`SCHED_INFO`/`TASK_DELAY_ACCT` path already keeps per-task `nr_wakeups`, locality and sleep sums. Physics weak/small: zero fps directly — it is an instrument — against a pool of ~9.9% of the main thread's wall clock (`artifacts/quake/wait-063645/out.txt`: 7.3% 'S' + 2.6% 'D' in sleeps shorter than the proc-read gap), i.e. a ~+0.6 fps ceiling. Mechanism plausible: the ledger itself is genuinely cheap (~20-40 instructions of RAM text against a measured 107.8 us per switch, under 0.2%, and one shared D-cache means no coherence traffic), but it is dominated by the free read, and its waker-identity hook records `current`, which is the interrupted task and not the waker for the IRQ/softirq wakes that dominate on this board. ~150-250 lines in the hottest path. Its free half is in T2.5.
- **X3-21** [rejected/refuted/weak] Delete 136 consumerless IRQ/s at ~170 us each — the physics premise holds (the whole entry chain really is in flash: every chain symbol from `handle_exception` c0452500 to `handle_percpu_devid_irq` c0058d16 sits below `_exiprom` c052ab74, against `.text..fast` from ~c083c000), but **only 48 of the 136/s survive source reading**: the per-entry cost is ~97-121 us not 172; 42/s produce no interrupt at all, 21.5/s are the load-bearing GDMA m2m completion, and masking the PPA costs ~6-7% of CPU0. Arm 1 is then 48.4 IRQ/s × ~121 us = **0.59% of CPU0** and it breaks the documented `hw_vblank` commissioning runbook at `esp32s31-lcd.c:2433-2441`. Arm 4's `irq_desc` caching is in T2.1; arm 4's `noirqdebug` is demoted to a measurement knob there.
- **X3-22** [rejected/refuted/weak] Scanout timebase from the GDMA descriptor pointer — the register read itself is sound and nearly free (`ESP32S31_AXI_TX_DSCR`; `esp32s31_axi_build_linear` emits 191 descriptors of 4032 B at 800x480, contiguous in the gen_pool, so address → index is arithmetic; one uncached MMIO read and ~764 B of kmalloc, no new `.text..fast`), but the rest of the chain is not: `wait_vblank` already defaults false, so there is 0 ms to recover. Independent gain **+0.1 to +0.3 fps (0.6-1.5%)**, below the measurement floor — the direct vblank chain is 35 of 4,438 CPU0 samples = 0.79%. The residual is the T2.1 hrtimer hygiene item.
- **X3-23** [rejected/refuted/weak] Stop lvdesk's heartbeat in fullscreen — its headline fix is **already in the tree** (`lvdesk.c:4089 fs_render_set()` swaps the display refresh timer's callback to `fs_render_noop` for the whole fullscreen period; commit 08355a8, zero refreshes verified); the only delta is the poll-timeout change the code records as collapsing prboom from ~500 to 80 passes per 5 s. Independent gain **+0.1% to +0.4%** on the fullscreen timedemo, i.e. 0.02-0.07 fps. Hidden cost the mechanism lens found: there is **no POLLOUT anywhere** in lvdesk or xshim, so a socket client with a non-zero `c->pendn` retries only on the next loop pass and a long poll cap stalls that retry by the full cap.

### Memory and paging
- **C48 / X1-10** [rejected/refuted/weak] zswap at a small pool cap — +0.5-1.5%, not separable from zero; ceiling is the 3.5% of wall that is paging.
- **C50** [rejected/refuted/weak] `vm.min_free_kbytes` 512-768 — +0.2 fps, sign not reliable.
- **C52** [novel/refuted/weak] `vm.compaction_proactiveness=0` — ~0.1% of one core; one whole-zone pass per 32 s.
- **C53 / X1-28** [rejected/refuted/refuted] Swappiness sweep — −2% to +0.5%, more likely negative; the file LRU argument inverts under XIP.
- **C54** [novel/refuted/weak] MGLRU via FLATMEM — ~+1% with a real chance of negative; unmeasurable here.
- **C56 / X3-4 / X3-9** [novel,rejected,rejected/refuted/weak] Sv32 walker snooping, set_pte writeback, lazy MMU — the entire cache-maintenance subsystem is 18-24 of 12,000 samples (0.17-0.20% of CPU0).
- **C57** [novel/refuted/weak] KSM / dedup census — +0.1-0.2 fps; 2 MB mergeable is more than plausible and still only +3.9%.
- **C58** [rejected/refuted/weak] Re-measure the 13.6 MB/s read() ceiling — warm headroom is 15-30%, not 2-3x; 0% on the headline.
- **C59 / C61** [rejected/refuted/weak] Decompose swap-in latency / bio-based swap device — +0.2-0.8% and +0.5-1.2% as an increment over C60; the 3x claim is arithmetically impossible.
- **C63** [novel/refuted/weak] I/O scheduler priority for synchronous reads — 0-0.35%; QD1 with 21.4 reads/s.
- **C65** [rejected/refuted/weak] ext4 geometry on the running card — ~165-170 kB permanent RAM, +0.01-0.08 fps.
- **X1-29** [rejected/refuted/weak] Pre-evict the idle platform set before launch — reachable pool ~450 kB gross, 0 to +1%.
- **X1-30** [rejected/refuted/plausible] `CONFIG_VM_EVENT_COUNTERS` — 0% direct; one of its three questions is already answerable from `/proc/vmstat` today.
- **X1-32** [rejected/refuted/weak] Re-run the read_ahead_kb sweep — Quake is mechanically 0% (page-cluster 0 swap faults never touch `read_ahead_kb`); Doom +0.2-0.6%.
- **X2-6** [rejected/refuted/weak] `IOCB_DONTCACHE` for app reads — governs read()-admitted folios only; the demo stream is ~780 kB at ~13 kB/s.
- **X2-19 / X2-20 / X2-21 / X2-22** [rejected/refuted/weak-refuted] Contiguous swap batching, VA-indexed slot allocation, a raw chunk cache, file-backed heap — all inside the same ≤7.3% ceiling; the file-backed heap additionally puts 10 MB of re-dirtied memory into dirty accounting with a dirty threshold of tens of kB.
- **X3-17** [retry/refuted/weak] A rasterizer-shaped working-set sweep (spanbench) — history rates it retry-justified but not novel: its scanout-DMA arm is already measured on a real game and null (dead-end #12, 59/50/42/35 Hz = 45.3/38.4/32.3/26.9 MB/s against windowed Doom at 31.3-32.6 fps). Physics refuted at none/negative: zero as an instrument, because the decomposition it promises was extracted host-side in minutes from the existing 12,000-sample capture, and 0 to +0.35 fps (0-2%) for the levers it exists to unlock — inside the project's own ±10% lottery. Mechanism weak and does not hold: **the headline "144 cycles/pixel" is inflated ~5x** (it is all user-mode time divided by screen pixels), the addressable surface is 22.5% of CPU0, not 59%, D_DrawSpans8 is 11.90% = 29 cycles/pixel against a ~17-20 cycle compute floor, and the three contention axes are already measured at ~0 (59→35 Hz scanout moved fps less than the within-setting spread). Its cost is a 2-D sweep crossed with four contention axes at a fresh boot per arm — many hours of a shared board, against the 10-minute rule.
- **X3-18** [rejected/refuted/refuted] Minimum folio order for SD-backed page cache — mechanism refuted: both implementation forms compile to a **literal no-op** on rv32; independently confirmed at `pagemap.h:460-474` (the gates at :461-466 and :471-472) with `arch/riscv/Kconfig:148` selecting THP only on 64BIT, so `mm/readahead.c:482-485` discards `ra->order` before it is used. History already-rejected on measured magnitude: the strictly larger interventions — XIP TyrQuake and `rootfs/locktext` — already returned zero. Physics none/negative: 0% on Quake, 0 to +1% on Doom, with a real chance of negative, since form (b)'s `s_min_folio_order=2` would demand order-2 allocations on a board at MemAvailable 316 kB with `watermark_boost_factor=0`.
- **X3-19** [rejected/refuted/weak] The `mmap_miss` ratchet and splitting `ra_pages` — history already-rejected: its headline prboom table is quoted from the block that the same file formally **withdraws** (`S02s31-blockdev:5-13`). The code reading is accurate to within a line or two (`filemap.c:3258` `MMAP_LOTSAMISS (100)`, the ratchet at :3346-3357 ahead of both the VM_EXEC and read-around branches, read-around size at :3404-3406 literally `ra->ra_pages`), but the latch is not the dominant regime (faults still scale 314→97 with the window) and the sequential curve it would free is flat at 12-14 MB/s. Physics refuted, small: −1% to +1.5% on Quake, centred on ~0; mechanism weak at 0 to +0.5% with a materially non-zero chance of a small regression — and it would be the **first mm patch in the tree** (none of the 40-57 entries in `patches/` touches `mm/`), carried forever on the hottest page-fault function for a sub-1% effect.
- **X3-20** [rejected/refuted/refuted] Back the heap with a file in musl — its premises check out (Quake's heap really is one `malloc`, `sys_unix.c:345-348`, hence one MAP_PRIVATE|MAP_ANONYMOUS VMA; swap-in really is unbatched, `swap_state.c:759-761`), but write faults on a MAP_SHARED heap take `do_shared_fault`, not fault-around, and land in `balance_dirty_pages`; also the wrong musl (1.2.5 via crosstool-NG, not the Buildroot 1.2.6 package). Mechanism refuted on a system-wide correctness break: MAP_SHARED changes `fork()` semantics for every allocation above the threshold in the **only libc on the board**, so a parent that forks and whose child touches a >128 kB block before exec now corrupts the parent's heap — which reaches bluetoothd, our a2dp daemon, wpa_supplicant and lvdesk. The honest upper bound if the entire paging tax vanished is 16.2 → 17.4 fps (+7.1%), and the mechanism cannot reach it.

### Storage
- **C64** [rejected/refuted/weak] `mintsts=0x200` data-interrupt latency — ~0% on fps, upper-bounded at ~0.15% by the event rate.
- (C59, C61, C63, C65 listed above.)

### Display, compositor and X
- **C66** [rejected/refuted/weak] Line-replicating scanout descriptors — ceiling ~1% Doom, ~0.3% Quake; scanout bytes are unchanged either way.
- **C67** [rejected/refuted/weak] Price/lower the 46 MB/s scanout — 0-2% with a definite flicker cost; 60→30 Hz frees 23 MB/s against a ~192-210 MB/s ceiling.
- **C69** [rejected/refuted/weak] 32-bit windows converted by the PPA — 0%; every benchmark app is 8- or 16-bit.
- **C71** [novel/refuted/weak] PPA draw unit for LVGL — 0-3% on drag frames, 0 on the games; the claimed −30-40% is ~10x inflated.
- **C72** [retry/refuted/weak] Terminal glyph atlas, scroll coalescing, drag-ghost leftovers — hard 0 on the headline workloads.
- **C73 / X2-13** [rejected/refuted/refuted] PPA CLUT expand chained into the SRM scale — −3% to +1% on Quake; the windowed form already measured −7.5%.
- **C74 / X1-34 / C75 / X2-12** [rejected/refuted/weak] Present-path variants (D-cache pricing, a scaled-mode PRESENT ioctl, skipping the atomic commit) — 0.25-0.9 ms of **CPU1** time per frame, on a hart that is 64% idle and off the client's critical path.
- **C76** [rejected/refuted/weak] Quiesce lvdesk in fullscreen — +0 to +1% Quake; net negative if its third lever ships, because that partially undoes late present. Its unresolved sub-question (3 ms/frame in `xshim_poll_ready`) is carried in section 5 as an attribution task, not a win.
- **C78** [rejected/weak/weak] Cold-pixmap run-length store in xshim — ~0.8-0.95 MB of Shmem on a three-client desktop, 0 ms on any timed workload.
- **C79** [retry/refuted/weak] lvdesk idle poll back-off at 250 ms — 0.1-1.2% of one core at desktop idle only, 0% with any client running.
- **C80** [rejected/refuted/weak] Per-window child/borrower lists instead of O(MAXRES) scans — ~1% of one core only while the pointer is in continuous motion.
- **C81** [novel/refuted/weak] LVGL pthread draw units on CPU1 — 0%: one `lv_image` per X window is one draw task.
- **X1-37** [done/refuted/refuted] Derive the frame gate from 42.1 Hz — the panel runs at 59-60 Hz (`pclk_khz = 25623`), and 861×496/25623 = 16.67 ms already rounds to the shipped 16.
- **X2-11** (fullscreen half), **X2-24**, **X2-26**, **X3-12** [rejected/refuted/weak-refuted] Double-hashing the fullscreen frame, splitting the compositor frame onto a second thread, the "2.5 Hz beat", standing down the row hash — 0 to −1.4%; the row-hash arm was measured on prboom and disabling it was worse.
- **X3-6** [rejected/refuted/plausible] Ungated I-cache flush on XIP overlay mmap — 0% steady state (XIP vmas are PFNMAP, established once); ≤0.4-1 ms per fork/exec.

### Audio
- **C82** [novel/refuted/weak] Flatten the Bluetooth audio path — 0.0% on both headline workloads; Quake and Doom run 11 kHz through the codec, not aloop.
- **C83** [rejected/refuted/refuted] Cut SYNC_PTR ioctls in s31route — `snd_pcm_sync_ptr` is 4 of 12,000 samples.
- **C84** [rejected/refuted/weak] Add 22050 to the I2S rate mask — Quake is exactly 0.0 (it opens `s31route_11k`); Doom +0.4-0.6 fps only if its sink is at 44100.
- **C85** [rejected/refuted/refuted] Hardware ASRC driver — 0% and negative if built; both games are native 11025 end to end.

### Platform libraries, boot, packaging
- **C87** [rejected/weak/weak] Redirect statically linked soft-double helpers from `ld.so` — +2.5-3.5% claimed, but this is the redline route; the legal part is C86, and the increment is ~0.2-0.3% of CPU0.
- **C88** [novel/refuted/weak] Deadline-driven SDL1.2 timer thread — 0.00%, provable: none of the shipped binaries passes `SDL_INIT_TIMER`.
- **C89** [violates-rules/refuted/refuted] SDL2 software-renderer fast paths — 0% on every goal app; none of them links libSDL2.
- **C90** [rejected/refuted/weak] RAM copies of hot library pages in `ld.so` — whole envelope is 7.04% of CPU0 across every platform library; 0 to +0.5% with a real chance of negative.
- **C91 / X1-31** [done,rejected/weak/weak] Boot parallelisation and unbinding fbcon — the 50 s baseline is wrong (desktop at 22.3 s, rcS complete 30.8-31.4 s over 5 boots); 0.30-0.40 s from the fbcon half, and 0% on every runtime metric.
- **C92** [rejected/refuted/refuted] `MADV_WILLNEED` exec-time text batching + `fault_around_bytes` — 0.0% steady state, bounded by the XIP TyrQuake control; ~8 ms of launch.
- **X2-9** [rejected/refuted/weak] musl XEspV forcing cross-hart bounces — measured at ~3 bounces/s board-wide.
- **X3-11** route (b) [violates-rules] Double-buffering the MIT-SHM surface inside stock SDL — patches stock SDL. **Route (a) as originally written also patches stock SDL 1.2** and carries the same recorded verdict; the xlite-only variant in T2.4 is what survives.

### Instruments with no path to a lever
- **C09** [novel/weak/weak] Fix the h1s sampler's 1 kHz phase lock to the 100 Hz tick — real aliasing (one residue per run is 61.5-97.8% kernel), 0% fps, under 1% through anything it leads to. Worth doing as hygiene when the reporter is next touched.
- **X1-25** [novel/refuted/weak] `mcycle`/`minstret` per-task IPC — see X2-15; no instruction counter on this core.
- **X1-26** [rejected/refuted/weak] Settle the lottery with conflict counters and kill it with prelock — capped at +6.5% on the mean and +0% on the best boot; simulated on the real PC data it comes out ~0.
- **X2-14 / X2-18 / X3-10** are in T2.5 rather than here; only their expensive halves are rejected.

---

## 8. Dead-end register

98 recorded dead ends, with the numbers that closed them.

| # | Idea | Recorded result |
|---|---|---|
| 1 | Reclaim/swap/rmap/page_io/lzo in `.text..fast` | 33 kB: 17.1 vs 17.6 kswapd ticks/s; 97 kB no effect. Reclaim is data-bound |
| 2 | zram, any form (24 MB / 4 MB / 512 kB cap) | Weston median 13 → 60 s; 4 MB device filled 3,484 kB and spilled 1,808 kB anyway; capped MemAvailable 476 vs 2,932 kB; compaction freed 0 kB |
| 3 | Xorg-era levers (Xorg from SD, extension pruning, native mode, SW cursor) | Xorg from SD 4.7 fps (4x worse); pruning 6,040 → 6,028 kB; native +553 kB; SW cursor 19 ms/move |
| 4 | `vm.vfs_cache_pressure=500` | Worse, and it wedged the board |
| 5 | Slab as a memory lever | CLOSED. kernfs_node 838 kB; 300-500 kB only by deleting function; SReclaimable 0 is a SLUB_TINY artefact; SLUB_TINY off costs ~390 kB |
| 6 | PPA completion by sleep / raw-bit poll | FIXED: 300 us spin 14,527 → 276 us/op; DMA RX EOF 1,322 → 169 us per 64x64 blend |
| 7 | PPA below ~128 kB; `ppa_min_bytes` sweeps | Sleep 0.7-1.1 ms vs ~160 us of engine; `ppa_min 65536` under Quake 13.3 vs 14.8 fps |
| 8 | LVGL/LVPROF instruments | Profiler inflates refresh 2,372 → 5,517 ms; monotonic timing summed to 101% of wall |
| 9 | Palette-expansion micro-optimisations | Chain theory refuted (dep 49-51 vs nodep 60-66 ns/px); only word8 (~8%) real; vector stores 4,550/6,149 vs 3,062 us scalar |
| 10 | Uncached PSRAM alias / "write-combined" kms_map | Alias is unbuffered at 7.6 MB/s, slower than cached + clean |
| 11 | PPA CLUT expansion (XSHIM_PPACLUT) | 320x200: 27.2 vs 29.0-29.9 fps (−7.5%); adaptive 22.8 vs 23.7 (−4%) |
| 12 | GDMA for the DRM damage copy | DIRTYFB 10.6 vs 7.9 (CPU) / 7.8 (PPA) ms; gdma_copy off: DIRTYFB −27%, Doom ~+2% |
| 13 | Lower panel refresh for bandwidth | 59/50/42/35 Hz = 45.3/38.4/32.3/26.9 MB/s; fps 31.3-32.6, inside the spread; CoreMark 950 vs 949 |
| 14 | Retarget the cyclic scanout DMA at lvdesk's buffer | −ENXIO then; superseded by direct scanout |
| 15 | MIT-SHM adoption / removing the request-boundary copy | 31.7 fps adopted but flickering vs 29.4 copying vs 28.6 off. Illegal |
| 16 | af_unix and net spine in `.text..fast` | 32.2 vs 32.2 fps; net spine ~70 kB: loopback 237 → 219 KB/s; reverted |
| 17 | Syscall entry / vDSO / clock_gettime as a tax | getpid 1.5-1.6 us, clock_gettime 2.8 us; lvdesk ~600 syscalls/s = 0.09% |
| 18 | I-cache autoload via devmem or `cache_ll` | From Linux the hart died instantly, twice; from the loader a no-op (still reads 0x2) |
| 19 | Single cold runs / same-boot A/B as evidence | Cold 29.1 vs warm 30.6 fps; same-boot arms position-biased (ON 211→614, OFF 285→781 ms) |
| 20 | `vm.swappiness=0` | OOM-killed Quake with 62-63 MB of swap free |
| 21 | No `CONFIG_FUTEX` | SDL2 indexed 101.962 vs 45.216 ms; codec underran 40/40 |
| 22 | s31route buffer/poll/pacing mistakes | FIXED: a signalled poll fd cost 20,153 ctx switches (→ ~1,200; Doom 15.1 → 27.1 fps) |
| 23 | s31route deep buffering / bigger DMA ring | Staging ring underran at start, EPIPE killed sound; ring is 85 ms, ≤128 ms |
| 24 | Drop the 22050→44100 resampler; force 44100; `S31ROUTE_NICE` | Quake 11 kHz +41% (10.7 → 15.1) shipped; NICE=−10 gave the audio thread 92-100% of the core |
| 25 | Weston/Wayland-era levers | Closed; render-below curve flattened (−36% pixels bought −7%); weston-terminal 3,462 ms vs foot 237.7 ms |
| 26 | Timer+storage subsystems in `.text.fast` (113 kB) | Real: timer IRQ 0.864 → 0.465 ms; SD 12.07 → 8.05 ms/request |
| 27 | Flash mode/clock as a lever | Harvested: DIO → QIO gave SD 4k −46%, CoreMark +17%. Already QIO-80 at 22-32 MB/s |
| 28 | Userspace library text XIP; LD_LIBRARY_PATH | XIP harvested (libpixman Rss 16 vs 388 kB); LD_LIBRARY_PATH misses libc: 2,586 vs 2,237 ms |
| 29 | Hot userspace text copied from XIP into RAM | footprintbench 33.14 vs 32.66 ns/call for ~600 kB RSS; lvdesk HOTTEXT 23.1/23.5 vs 24.3 fps |
| 30 | CMA reusable vs coherent; shrinking CMA | Reusable shipped: MemTotal 12,844 → 14,888 kB, foot keystroke 193.8 → 20.7 ms. Shrinking 4 → 2 MiB gave +136 kB |
| 31 | SD controller/card causes (pre-SRAM) | All eliminated: clock gating 3.781 vs 3.778 ms; the pre-SRAM ~6.5 ms was CPU-bound |
| 32 | `work_pending()` guard; MMC HSQ | Guard: nothing. HSQ: 4K −12% but 256K +26%, 1M +86%; OFF |
| 33 | HZ as a lever | HZ=100 better on 7.1: SD 4k p50 3.76 vs 4.00 ms; kworker wait not tick-quantised (7.4 vs 8.0 ms) |
| 34 | `vm.page-cluster > 0` (VMA mode) | Xorg era noise; Quake at 48 kHz pc3 did not finish in 170 s vs 167. Physical-slot mode never tried |
| 35 | SD bandwidth/bus/buffered-read tuning | All arms 12-14 MB/s; model 2.49 ms + size/48 MB/s; no S18A so no UHS |
| 36 | Deferring the damage copy to a kworker | 2.15-2.26x worse: 3.66 vs 1.62 ms; 107 flush_work waits at 7.4 ms each |
| 37 | dwc2 deferral and USB root-port experiments | `defer_irq_reenable` off: ctxt 1,601 → 21/s, SD 14.51 → 10.14 ms. All moot since USB moved to hart0 |
| 38 | OpenSBI trap path into SRAM / PSRAM | SRAM shipped: switch 280.7 → 57.2 us, SD 4k p50 3.79 → 2.21 ms. PSRAM does not boot |
| 39 | Qt from SD; lazy PLT; `-Bsymbolic-functions` | Qt removed (>120 s to a window); lazy PLT closed (whole chain dlopens in 10-20 ms) |
| 40 | MGLRU / THP / mTHP swap | Unavailable: LRU_GEN needs 64BIT and !SPARSEMEM; THP is 64BIT-only on riscv |
| 41 | Zero-page handling via zram same_pages | Already free: the 7.1 swap zeromap never writes zero folios |
| 42 | Measurement traps | dd startup ~150 ms; `profile=2` allocates ~4 MB; 200 Hz SIGPROF wedges the board; membw before the canary shifts the boot; a screenshot inside a timed window costs 26% |
| 43 | Kernel static RAM and debug trims | FTRACE off −1,056,072 B; LOG_BUF −264 KiB; DEBUG_FS off 1.14 MB |
| 44 | Shrink or free fbcon's buffer | `video=400x240` costs ~190 KiB MORE; unbinding vtcon gives ~20 kB |
| 45 | Bounce buffers, BitScrambler, EXA, per-window planes | Alias 7.6 MB/s; EXA ceiling 1.2-1.7x; per-window planes ~13% for ~1 MB |
| 46 | Wi-Fi throughput causes | It was power save: PS_NONE took TCP 290 → 680 KB/s. Copies <1% |
| 47 | Boot-time levers | nice 19: rcS 84.35 → 42.97 s; static busybox spawn 53.6 → 29.4 ms; backgrounded coldplug starved the network |
| 48 | mmc DRTO/CRC and the 139 s Oops as causes | Not reproduced: 192 MB of sustained reads, zero errors |
| 49 | `mlockall` on lvdesk; MAP_POPULATE | Lag 367/411 vs 170/313 ms, worst 1,163 vs 610; MAP_POPULATE 3,149 vs 2,549 ms/maximise |
| 50 | Watermark tuning | wsf 500: 11.0 fps; default boost OOM'd with 6 MB "free" (3,624 kB of it CMA); boost 0 shipped (20.5 vs 17.8 fps) |
| 51 | bluealsa; evicting bluetoothd from XIP | bluealsa spun at 92% of the core (lag 170-411 vs 25 ms); replaced by s31-bt at 0.6% |
| 52 | A2DP daemon tuning; hosted-IRQ blame | One read per packet: 50.1 → 20.4%. The "~4 ms per hosted IRQ" figure was retracted |
| 53 | `napi_defer_hard_irqs` / `gro_flush_timeout` | Inert: 155-168 irq/s at any setting; 15-25% worse on cpubench |
| 54 | Coex tuning; BT discovery during A2DP | A2DP bit set while the link exists: late_max 2,832 → 0 ms. Discovery during playback: late_max 4,987 ms |
| 55 | cpufreq governor as a lever | hart0 holds 320 MHz always (4,388 transitions, nothing changed) |
| 56 | Whole IRQ spine / chip.o / CLIC driver in RAM | Three arms emitted 0 bytes or did not boot; the 10 kB dispatch pre-fix arm took the context switch 107.8 → 171.3 us. softirq.o alone −19.7% (675 → 542 us), shipped. chip.o post-fix boots at <1% (#337: 15.8/16.1 fps) |
| 57 | sched_clock.o, spinlock.o, memcpy, `arch_sync_dma_for_device` in RAM | No boot (bootstrap self-reference); cache maint in RAM: SD 4.0-5.3 vs 10.1-12.7 MB/s |
| 58 | Object-granular `.text..fast` batches | #320 43 kB: −5% on sdl1; rcu/tree.o −1.7%/+4%; cold xattr.o −7.1%; input+evdev +3.8% |
| 59 | Touch-poll and `hw_vblank` as wakeup levers | I2C IRQ + 60 ms backoff: timer 669 → 125-154/s, busy ticks unchanged. hw_vblank=1: 119-136/s either way |
| 60 | BLE LE encryption fixed host-side | The controller blob is at fault (MIC failure); Espressif's own host fails too |
| 61 | Frozen full-screen drag backdrop (5 attempts) | 42.8 / 58.6 / 52.8 / 37.6 / 43.1 ms per frame vs a 33 ms baseline |
| 62 | Crypto self-test boot stall | FIXED: desktop 26.43 → 21.00 s |
| 63 | PIE/HWLoop hook: off, lazy save, expensive save | Hook off: bluetoothd SIGSEGV. Lazy save 162 vs 155 us. The cost was the cold ecall (1.8 vs 16-18 us) |
| 64 | TLB/ASID; `__switch_to` flash fetch | Under 1 kB of switch code cannot explain the cost (it was OpenSBI). The 2.6x switch gave spawn −8%, lag 22 → 18 ms |
| 65 | X/xshim protocol-path levers | "46% of maximise is syscalls" was false (1.4%); shared window buffers +5.6% against a predicted +20-25% |
| 66 | Game config/binary changes | −mb 4: 15.4 vs 16.2 fps; −mem 12/10/9: 9.8/10.9/10.1; XIP Quake 10.7 vs 10.9 (18% fewer faults); locktext 863 vs 825 faults |
| 67 | "Fps dips are swap" / page-cache warming / readahead as a Quake lever | Dips are CPU contention (r=0.04-0.33; half the slow intervals had zero faults); 512 kB read 98 MB vs 5.4 MB per demo |
| 68 | Scheduler tunables and preemption model | The 31% "win" was a spinning st client (VOID). PREEMPT_NONE vs LAZY: nothing |
| 69 | Tickless idle (NO_HZ_IDLE); NO_HZ_FULL | Silent hang, ~1 death per 2 runs vs 7/8 + 5/5; broke /proc/stat. NO_HZ_FULL needs 64BIT |
| 70 | Forced placement, pinning, capacity, RPS/wq steering | Pinned lvdesk +15.3%; co/split 16.43/16.13 vs 13.83 free; capacity 1024/512 worse |
| 71 | Per-boot lottery eliminations | Not placement, not an lvdesk restart, not the scanout buffer, not CMA, not PSRAM chase latency (slow boot at 274.8 ns, fast at 316) |
| 72 | Cheapening the cross-hart crossing inside hart0 | Doorbell ISR is 0.66 us of 165 us; priority 1 → 10 nothing; FPU world swap ~0.2 us |
| 73 | Polling idle on the lent CPU via trapped wfi | Canaries +11% worse (16.3 vs 14.7 ms): 460k M-mode traps/s |
| 74 | Hardware S-mode interrupts on hart0 | Impossible: a 0xff level sentinel; emulation costs 1.14 us; an IPI via M-mode never woke hart1 |
| 75 | PIE lease prototype; cross-hart AMO lost unlocks | AMOs: 0 erased in 5M cross-hart. The lease stall was the CLIC-level bug |
| 76 | Late present measured neutral on the canary | INVALID (flag never exported). Properly enabled: **+13% Doom, 34.6 → 39.1 fps** |
| 77 | XLITE-RING, PRESENT ioctl, fast present | Ring +5.5% (31.2/32.0 → 33.3/33.4). PRESENT fps-neutral. Fast present: lvdesk −9%, fps in noise |
| 78 | s31fp on one core; SDL1 timer-thread fix | s31fp mul 2.5x bit-exact but OpenTyrian underruns only 102 → 73-77/10 s. Not enabled |
| 79 | XEspV memcpy/memset in musl | memset 1.00x, memcpy 1.10-1.11x; libc mem* is 0.73% of Quake's CPU0 |
| 80 | libSDL into XIP; bespoke SDL; −O2 for SDL/asound | XIP: 16.2 → 16.0 fps (RSS 204 → 0 kB). All non-libc shared libs are 1.4% of Quake's CPU0 |
| 81 | Windowed 32-bit SDL2 visual; hide the cursor in fullscreen | chocolate-doom 10.7 → 5.5 fps; cursor 20.9 → 21.3 fps (noise) |
| 82 | Moving JPEG work to hart0 / concurrent encode+decode | No gain; concurrent encode and decode hard-wedges the SoC |
| 83 | Desktop LVGL experiments | STYLE_CACHE noise; PARTIAL buffers monotonically worse; canvas terminal 3,230 → 3,410 ms; PPA cursor 407-491 vs CPU 241-315 us |
| 84 | X server / stock X library alternatives | Xorg ~4.7 MB → the xshim window at 53 kB; xcalc 2,104 → 180 kB; xtlite 747,572 → 26,092 B |
| 85 | xshim internals already fixed or rejected | XPM per-run 10,577 → 1,001 requests; drag ghost majflt 439 → 21; whole-window invalidation −52% |
| 86 | LVGL/lvdesk bring-up choices | fbdev emulation 83.1 ms vs KMS 23.3-25.6 ms; a usleep loop costs 18% of a core idle |
| 87 | hart0 audio pipeline; GDMA m2m ring copies | GDMA m2m stalled after 15 blocks; unconditional encode ~30% of a core (gated ~2%) |
| 88 | Moving work to hart0 beyond radios | SD proxy adds 165 us to 2.49 ms; display ~1% of CPU0; device IRQs ~1% of a core; Wi-Fi RX 0.13% |
| 89 | Small kernel-side costs judged not levers | RSEQ/MM_CID 0 samples; barriers ≤0.6%; SLUB_TINY fast paths 0.2%; TLB-shootdown IPIs ~0.1%; whole-kernel −O2 does not fit (119,803 B slack) |
| 90 | Hardware that is not available | Cache geometry fixed, no L2; LP SRAM is hart0's heap; only 6,360 B free in the OpenSBI window |
| 91 | Paging ideas rejected on reasoning | KSM ~130 kB of rmap slab; hugetlbfs pins 12 MB; a prefetch daemon has no predictor at 316 kB free; folios off without THP |
| 92 | XSync pipelining / client-side copy in xlite | ppoll is 1.8% of Quake's main thread (~1.05 ms/frame); net ~0.3 ms (0.5%) Quake, 2-3% Doom |
| 93 | Audio chain collapse / hart0 ASRC / userspace rdtime | snd_* is 0.46% of CPU0; ASRC buys nothing at native 11025; rdtime clock_gettime 0.06% |
| 94 | Diagnostics left on control-flow paths | Three times the diagnostic *was* the failure being chased |
| 95 | All I/O via a FreeRTOS mailbox; `maxcpus=1` then online CPU1 | A mailbox round trip is tens-to-hundreds of us; maxcpus=1 still failed 1 boot in 5 |
| 96 | Enlarge or reconfigure the caches from the loader | Not available: L1 I 16 kB, D 64 kB, 64 B lines, no L2 |
| 97 | Evicting a daemon from XIP; udevd into XIP | XIP_SKIP left a stale SD copy running; udevd needs 264 kB against 57 kB free |
| 98 | OpenTyrian rate cap like s31route_11k | Does not help: the OPL synth runs at a fixed 44,100 and converts afterwards |

Plus two items dropped as dead before lensing: the open audio-path list (staging queue, SDL1 callback jitter, native 11025 — all built, failed or already done; the audio thread is ~1.5% of a core) and reading/raising the SD bus mode (4-bit, no S18A, so no UHS; the marginal rate is already 48-51.6 MB/s).

---

## 9. Suggested order of work

Ordered by information per hour, not by size. The first five are cheap relative to what they decide, but only one of them is now a no-build item.

1. **The 240 MHz mirror arm (X3-1's gate) — build it properly or it lies.** Not one line and not 7 minutes: `esp32s31.dtsi:196 timebase-frequency` **and** `esp32s31.c:41 S31_TIMEBASE_HZ` to 240000000, `make linux` **and** `make opensbi`, the bootloader frequency in `sdkconfig` (not `sdkconfig.defaults`, which has no such line), then flash. **Validate the arm before believing it:** read the loader's `cpu_start: cpu freq:` line, and stopwatch the demo's wall time on the host — if the timebase is left at 320e6 the fps line is inflated by exactly 320/240 and a true −25% slowdown reads as zero change. ~40-60 min. It prices the whole clock family *and* bounds T1.2, T1.4, T1.6 and T1.8 by telling you how steep the fps-vs-clock slope is. **If a validated arm comes back shallow**: X3-1 is a +6% item at best, the frame is memory-latency-bound rather than issue-bound, and the entire flash-text family should be discounted toward the low end of its band.
2. **Two sysfs knobs (T2.3).** `vma_ra_enabled=0` + `page-cluster=2`, two fresh-boot timedemos judged on majflt and `/sys/block/mmcblk0/stat`, never on fps. ~7 min, no build. Settles or retires the entire swap-readahead family, which has four duplicate proposals in it.
3. **One build with no board time: C37.** `--disable FRAME_POINTER`, `make linux`, read the partition-free line. If the slack does not go 119,803 → ~300,000 B it is dead for free; if it does, it ships with the next kernel and raises C39's ceiling.
4. **The cache counters (T1.5).** One `runsh.py` script, ~8 min, no build — with the `DR_REG_CACHE_BASE` caveat from T1.4 in force (record state, `reset.py` ready). From here on, text-placement candidates are ranked by refills instead of by argument — which is the single change most likely to stop the next four batches measuring zero.
5. **The storage bound (T1.9's gate) and the free instrument upgrades (T2.5).** Sectors ÷ majflt from one existing timedemo sizes C60 before it is written; porting `rootfs/pcsample.c`'s user-PC symbolisation into `h1s-report.py` is ~30 lines host-side and retro-fits every capture already on disk; `voluntary_ctxt_switches` is three lines in a script that already runs.

Then, in order:

6. **C18** (hours) — unblocks everything below it, and fix the systimer rating in the same change.
7. **C20 + C19 + X1-5 arm 1** (days) — one arm with the bootable subset, measured on h1s inside one Quake run.
8. **C23** (hours to a number) — the prelock sweep, A/B/A within one boot, 2/4/8 kB, A-to-A gap reported as the error bar.
9. **The hygiene bundle T2.1** (one day, one build) — judged on `/proc/interrupts` and sample share. Excluding the PPA/GDMA arms and the entropy arm, with `noirqdebug` used only as a knob and C42 shipped through an `early_initcall` rather than a patch to `kernel/sched/features.h`.
10. **C32** (hours) — after the TLB stride probe, and with SECRETMEM disabled in the same build.
11. **C62** (one line) as a rider on any of the above — **only after** CMD13 is shown to be redundant for this host, since it cannot be scoped to reads and the alternative mitigation is an edit to upstream MMC core.
12. **C39** (days) — the real project, built on C04's rankings and C37's slack.
13. **C86** (days) — orthogonal to all of it, after the LD_PRELOAD caller census.
14. **C60** (week+) — only if step 5's bytes-per-fault number justifies it.
15. **X3-1 at 360 MHz, then 400 with a soak** (week+) — only if step 1's *validated* slope was steep.

**What to abandon if the first experiments come back flat.** If a **validated** 240 MHz mirror arm shows fps scaling far below the clock ratio, drop X3-1 entirely and stop treating "memory-bound" as an open question — that also closes X3-13, X3-17, X3-3, X3-5 and X2-16, which all exist to price the same thing. (An unvalidated arm proves nothing: the default failure mode is a false null that looks exactly like a shallow slope.) If the cache counters show that `.text..fast` is not displacing D-cache lines, the layout-control half of C39 can be dropped and the region can simply grow; if they show it is, C39's function granularity becomes the only safe way to add text and C20/C23 must be sized against it. If the swap knobs move majflt by less than ~15%, close the entire paging family — the two maximal controls already returned zero fps and a third null makes further work indefensible. And if step 5's bytes-per-fault comes back at 24-80 kB rather than 4 kB, C60 loses its premise and the storage ceiling of +6.3% should be treated as already mostly harvested.

Finally, the largest unattributed number in the compositor is **C76's own unresolved sub-question**, not a new find: **~3 ms/frame of lvdesk sits in `xshim_poll_ready` outside `handle()`**, roughly 20% of the 14.7 ms windowed canary frame. C76 is rejected at +0 to +1% on Quake, lvdesk recomputes to 17.3 ms per client frame (not 18.5), and CPU1 is 63.7% idle, so anything freed there returns to a hart with spare time and reads as 0% on the fullscreen metric. It still costs only one `XSHIM_PROF`/`LVDESK_PROF` session on a warm board, and it should be done before writing any compositor code — as attribution, not as a lever.