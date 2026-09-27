# s31fp arithmetic and OPL roadmap (2026-09-27)

> **Scope, stated plainly.** This is the NARROW result: soft-double arithmetic,
> the OPL synth, libm floor/ceil/trunc, placement and the patch engine. It is
> not the requested wide survey of S31-hostile code across executables,
> libraries and syscalls that could be replaced the way soft-double, the fast
> clock and the string routines were. That survey is a separate document.

**Audience:** the project owner and future agents.
**Status:** design only. No board was attached. Every timing figure is quoted from repo measurements, and every new number is a QEMU result: correctness and instruction counts only, with no timing model and no esp.* execution.
**Labels:** MEASURED means taken from a cited file. INFERRED means reasoned, not measured.
**Evidence convention:** `E/` is `artifacts/s31fp-evolution/probes/evolution-evidence/`, and a bare `probes/<dir>/` path means `artifacts/s31fp-evolution/probes/<dir>/`. Each evidence file records the command, cwd, input md5s and full stdout. `run.sh`/`run2.sh` re-run them after `make cloud-setup` (tools/cloud/). Built binaries and five large disassembly dumps (`*.dis`, 2.6-6.6 MB, regenerable with the shipping objdump) were not committed.

---

## 1. Summary

**Thesis.** v2 already made each soft-double helper cheap. On silicon, mul went from 654 to 281 ns, add from 345 to 260 and div from 904 to 554 (`rootfs/s31fp/v2/V2-REPORT.txt` §5). What remains falls into three buckets:

1. **Call count and fenv overhead in fixed compiler chains.** The OPL title song runs 4,783 instructions/sample against a floor of 1,478 outside the helpers. `s31v2_muldf3` is 52.5% at 53.69 calls/sample (`results/oplbench-profile-36.txt`). Removing every CSR access took the title from 18.89 to 16.87 us/sample.
2. **libc-resident soft-double that v2 cannot reach.** In the software-Quake profile it is 4.17% of CPU0 (3.17% helpers plus 0.91% libm bodies). That figure comes from capture h1s-062518, which is not in the tree, taken on the tiopex binary rather than today's sdlquake, on a 16.4 fps boot (`perf-review-2026-09-23.md:96-104`). In GLQuake it is about 2% (prof9).
3. **Placement.** At an identical instruction count, the coloured copy runs 19.9-20.6 us/sample while static v2 runs 18.2-18.4.

The evolved s31fp addresses these with:
- generic fused call-site rewriting in the main executable;
- exact interposition of exported libm symbols, which writes no text;
- alignment and colour-aware copy placement;
- instruments that establish heat before anything ships;
- a transactional, fault-injected patch engine as the precondition for wider scope.

Hardware engines cannot serve per-call work. PIE is a CPU0-only copy accelerator, and v3 already routes it. Most of the multi-x headroom sits behind owner rulings (§4).

**Top 5 bets**

| # | Bet | Expected gain | Evidence |
|---|---|---|---|
| 1 | **Generic fused helper chains** (W2): call-site thunks, one fcsr read per chain | Not forecast. Bounded between 4,783 and 1,478 instr/sample on OPL; removing CSR alone was worth -2.02 us/sample. Go/no-go depends on the audio-thread split (W0c). | V2-REPORT §5/§9; oplbench-profile-36; cave `E/cave_sizes.txt` |
| 2 | **libm exports**: floor/ceil/trunc (W6), then musl-rebuilt sin/cos/sincos (W7) | floor 427→38, ceil 426→39, trunc 140→34 instr/call (QEMU). Priced by T1.7 at **+0.2-0.35 fps** (physics lens) to 16.8-17.2 fps on software Quake (`perf-review:100-102`). This is a per-call win, not a proven fps win. | `E/sqrt_floor_icount_libcimage.txt`, `E/floor_ceil_exact.txt`, `E/libm_rebuild_vs_libc_*.txt` |
| 3 | **Float-idiom lowering** (`ext→op→trunc` becomes one F instruction), as a rewriter rule (W8) | About 400-650 ns becomes one F op per site (INFERRED from v2 ns). Bounded by the app-static helper share; the libc 3.17% is out of reach. | `E/double_rounding*.txt` |
| 4 | **Copy placement** (W5): 4-byte-aligned copies, then forced colour permutations | About 0.3-0.6 us/sample. Colour matching reproduces a mediocre assignment. | board-b7 (14/20 uncoloured runs at 19.46-19.83) vs board-b8 (20 colour-matched runs at 19.91-20.56), across different boots |
| 5 | **Instruments and engine** (W0, W9) | No direct gain. Decides bets 1-4 and unblocks DSO coverage. | T1.7 shim never run; `faults-board.txt` |

---

## 2. Architecture of the evolved s31fp

There is one preload, `libs31fp.so`, delivered system-wide by `/etc/s31fp.env`. `S31FP=0` kills everything, and every new feature has its own toggle, **defaulting off at first ship**.

**2.1 Body patcher (v2, extended)**
- Full-body matching with relocation masks, never prefix matching.
- The main executable is patched at constructor time.
- **Copy mode is the shipped default**, from commit 737faaa ("Ship board-tested s31fp copy and string policy from SD") via `overlay/etc/s31fp.env:5`.
- Stale docs that still say copy is opt-in, to be fixed:
  - `preload3.c:39,194,479-484` (the compiled default is still trampoline when the env is absent);
  - `V2-REPORT.txt:510,574-575`;
  - `docs/s31fp-handoff-2026-09-27.md:59`;
  - `docs/status-and-todo-2026-09-27.md:85-93,116-117`.
- The `MAXSITE=32`/`POOL=16` globals become per-object tables with hard caps and a skip-on-overflow rule.
- **These tables live in constructor-local or munmapped scratch memory, never in the RW segment.** Today's RW segment already spans two pages in every process (`readelf -lW` on 9767181c: vaddr 0xad3c, memsz 0x6dc, ending at 0xb418). The "one private data page" in V2-REPORT §6 is stale. Any growth of the RW segment costs 4 kB times every process.
- Phdrs come from `getauxval(AT_PHDR)` instead of `/proc/self/auxv`, saving 3 syscalls. This is folded in silently.

**2.2 Call-site rewriter (new; generic rules, option (a) of §4.1)**
- **Where it runs.** At first launch, inside the existing scan. The scan already walks the text in 2-byte steps; it now also records `jal`/`auipc+jalr` targets that land on matched helper entries, then decodes RV32IMAFC+Zba/Zbb/Zbs windows around those sites only. Results go into the per-exe cache and are re-verified by bytes on every hit.
- **Budgets.** Decoder, matcher and emitter must fit in 6 kB of shared text (INFERRED 3-6 kB). First-launch cost is bounded by the scan rate of 27.7-29.4 ns/B (V2-REPORT §6). A warm launch adds only the byte re-verify.
- **Offline per-binary tables are not used.** They would make the rewriter app-specific.
- **Window whitelist.**
  - Moves, immediates and ALU ops.
  - Loads that cannot fault: sp-relative within the frame, or into a read-only PT_LOAD.
  - **sp-relative stores (spills)**, replayed in their original order.
  - Rejected: non-stack stores, branches, CSR access, indirect jumps other than the call, esp.*.
- **Call forms.**
  - `jal` (±1 MiB): retarget in place.
  - `c.jal` (±2 KB): refuse unless the cave is in range.
  - Unrelaxed `auipc+jalr` pair: rewrite both instructions. This is safe because all rewriting is pre-main and single-threaded.
- **Only the head call is retargeted.** Interior bytes are unchanged, so a branch into the interior (tiopex `0xb924`) still runs the original code.
- **Exit state.** `ra` must equal the address after the last original call. Every s-register and stack slot written by the window gets the original's values. `a0/a1` (and `fa*` where used) hold the final result. Other caller-saved registers are ABI-clobbered by the final call. The symbolic model (W0g) proves the thunk writes no register or slot that the original leaves intact.
- **Fallback.** For frm≠RNE (RMM above all), NaN, subnormal or overflow, restart the **original sequence from the head** through the v2 routines. Never resume from a half-fused state.
- **Cave.** Thunks go only into dead tails of libgcc bodies **on pages the patch already COWs**. If no cave space fits, the site is refused; no page is ever allocated for it.
- **Heat** comes from the counting build (§2.7), never from an in-process sampler.
- **Profiling note for h1s users:** under rewriting, fused time is attributed to cave addresses inside libgcc bodies, so it symbolises as `__adddf3` and friends.

**2.3 Interposer (v3, extended)**
- Strings and the vDSO clock are shipped today. The new part is libm exports: 0 private pages, no text writes. Internal uses bind to hidden aliases.
- Each export delegates to libc through an `RTLD_NEXT` pointer **resolved lazily on first use**, not in the constructor, so programs that never call it pay nothing.
- It delegates when its toggle is off, when `S31FP=0`, or when frm≠RNE. The frm check costs about 25-30 ns (one CSR read, V2-REPORT §5). It is **omitted for floor/ceil/trunc**, whose musl results are mode-independent (MEASURED: `musl_mode_dependent=0`, `E/floor_ceil_exact.txt`).

**2.4 Patch scope**

| Object | Policy |
|---|---|
| Main executable | Patched pre-main |
| SDL-1.2, SDL_mixer (DT_NEEDED) | Constructor-time, **default-off per library**, enabled only on W0a/W0b heat |
| SDL2 | Only if an installed SDL2 workload is confirmed. The only candidate in the evidence is tyr-glquake (`artifacts/gl/tyrquake/nm/`). prboom, sdlquake, QuakeSpasm and OpenTyrian are all SDL 1.2 (`artifacts/s31fp-next/imports.txt`). |
| libasound | Only after body verification. Its matches are nm-only. |
| libX11 (first-party xlite), libGL (TinyGL, `forward-plan:12`) | Excluded; fix at source |
| libc/ld-musl, vdso, s31fp | Always excluded. Identified by `dlpi_addr == getauxval(AT_BASE)`, `AT_SYSINFO_EHDR` and the constructor's own address, not by name. `AT_BASE==0` disables DSO patching. |
| dlopen'ed objects | Not patched (§4.5) |

Constructor-first ordering holds only while s31fp is the **sole** LD_PRELOAD entry with 0 DT_NEEDED. With `LD_PRELOAD=libB:libpre`, libB's constructor ran first (`E/ctor_order_two_preloads.txt`). Measurement setups that prepend ioctlprof must disable DSO patching.

**2.5 Hardware dispatch**
- **F unit.** Carries all new arithmetic. F instructions use static `rm=rne`, or `dyn` after an frm check. fcsr is saved on entry and written back as `pre | expected_flags`.
- **PIE.** CPU0 only, via v3's rseq `cpu_id` dispatch with a scalar twin.
  - Kept for in-cache copies of 64 B and up: 4 KB memcpy 2.86 vs 8.98-10.7 us.
  - Compares stay scalar: memcmp 4 KB 28.1→17.4 us; strcmp 64 B 2.43→0.71 us.
- **Never used:** esp.lp (59 faults in 4,000 rounds), CFG unaligned mode, `esp.fft.cmul.s16.st.xp`.
- **No DMA engine behind any intercepted call.** PPA costs 9-13 us to program plus a 0.7-1.1 ms IRQ wake, with a crossover around 128 KB. GDMA costs about 479 us at 4 KB. Both are CMA-only (`accel-plan.md:21-57,98-140`).

**2.6 Safety framework**
- **Transactional engine (W9).**
  - Build each patched page in a **never-executed** anonymous frame.
  - Seal it RX (`flush_icache_pte` → `esp32s31_cache_sync_for_exec`, which invalidates both I-caches under SMP: `patches/0054…/esp32s31_cache.c:510-523`).
  - `MREMAP_FIXED` it into place. Contiguous pages are coalesced into one frame run.
- **Rollback.** Keep a pristine RX anonymous copy of the whole run. Exit 125 happens only before main.
- **Never re-RW a frame that has been RX.** INFERRED from patches/0005: `PG_dcache_clean` survives user stores, so a second RX mprotect skips the writeback.
- The runtime fault injector exists in the test build only.
- **esp.\* gate:** 0 `esp.*` of any kind in the final linked board-toolchain libs31fp.
- Crash capture is opt-in (W0i).

**2.7 Observability**
- `libs31fp-count.so`, enabled only in measurement sessions via the env. It counts calls per helper, per rewritten window and per interposed entry, keyed by ra, and dumps once at exit to `$S31FP_CACHE/count.<exe>`.
- It forces trampoline mode: counters inside copied bodies would break the TAILREF-only relocation invariant (`build-preload3.sh:18-31`).
- Counts are approximate lower bounds (non-atomic; `_exit`/`abort` skip the dump).
- There is no always-on stats ring.

**2.8 Placement**
- **Geometry is unresolved.** Colour matching assumes hart 1's I-cache is 32 kB, 2-way and physically indexed (`artifacts/gl/phase6/tier6/REPORT.txt`, CACHESIZE_CONF_REG 0x80). IDF Kconfig and `perf-review` T1.4 say 16 kB. `patches/0054…/esp32s31_cache.c:263-264` calls 16,383 B "the whole cache". Hart 0 did not show the 32 kB pattern.
- **I-cache prelock is closed negative:** +24% to +86% (`perf-plan-2026-09-23.md:129`).
- **New:** align copies inside slot slack.
- **No mlock**, because nothing shows patched pages are ever evicted (see P4 in §5).
- A copied page of an XIP library turns 0 RSS into 4 kB private per process.

**2.9 Budgets (INFERRED unless cited)**
- **Memory.** OpenTyrian today: +16-20 kB Private_Dirty (V2-REPORT §6). Worst case per OpenTyrian process after this roadmap:

  | Item | Pages |
  |---|---|
  | Main-exe copies (`E/dso_copy_pages.txt`) | 3 |
  | W2 call-site pages (opl.o is 14 kB) | ≤4 |
  | SDL-1.2 + SDL_mixer | 3 + 2 |
  | libs31fp RW | 2 |
  | **Total** | **≤14 pages ≈ 56 kB**, i.e. about 36-40 kB above today |

  Measured headroom at the end of a GLQuake run: MemAvailable 540 kB (prof9). Every other process pays only the 2 RW pages it already pays, provided no item grows the RW segment.
- **Launch.** Measured base: copy+colour is +17-27 ms over plain (`board-b10.txt`: oplbench-dyn 46.5→73.5 and 49.0→66.0; sdltone1 54.0→73.5; amixer 74.0→95.5).
  - New warm-launch work: `dl_iterate_phdr` plus an allowlist check (0 syscalls), and a byte re-verify of cached windows.
  - First launch: about 12 ms scan for SDL-1.2 (INFERRED from its size and 28.5 ns/B) and about 33 ms for SDL2's 1.15 MB.
  - `RTLD_NEXT` resolution is lazy.
  - **Kill threshold:** warm-launch regression beyond the ±8 ms launch noise (V2-REPORT §6), or a helper-free warm launch above today's 7 traced syscalls. Busybox scripts exec constantly.

---

## 3. Roadmap

**Order** follows the owner's execution order (`forward-plan:14-17`): OPL arithmetic, then memory operations, then selective coverage, then placement, then wider candidates. The libm and float-idiom items belong to "wider candidates" (forward-plan item 6). They are cheap to prototype in the cloud but ship after W2-W5 unless the owner re-orders (§4.10).

**Common rules**
- **Build.** Make targets via `./docker/build.sh`, with `-march=$(S31_USER_ISA)` (`Makefile:38`). Gate the final linked artifact with `riscv32-esp-linux-musl-objdump -d X | grep -c 'esp\.'` == 0.
- **Cloud.** `. tools/cloud/env.sh`, then `s31-cc`, `s31-qemu`, `-plugin /plugins/libinsn.so -d plugin`.
- **Board.** `scripts/board/` tools only, with one serial owner. Launch with setsid, output to the card, collect after the bounded run. Fresh boot per arm, 5+ repeats, 3 interleaved reversed pairs for A/Bs, report the tail, no probing during windows. Use `/root/afp2/oncpu`; `taskset`/`timeout` are absent (preflight with `command -v`).
- **Rollback for every ship.** Env kill switch, default off at first ship. Keep #402 plus SD library 9767181c as rollback until the replacement passes (`forward-plan:89-91`). Never run a bare `make sync-images` with a variant (AGENTS.md).

### W0. Instruments and evidence gates (no shipped behaviour change)

**W0a. Counting build (§2.7).**
- **Targets:** per-app rates of floor/ceil/sqrt/sin/sincos/sincosf/pow and helper-chain sites.
- **Also, for the A3 queue (`gl-performance-campaign:194-199`):** `SDL_ConvertAudio`, `SDL_UpperBlit/SDL_SoftBlit/SDL_ConvertSurface`, `SDL_MixAudio`, plus the kernel PIE-bounce counter before and after each session.
- **Cloud:** counts equal libinsn counts for oplbench (53.69 muldf3/sample). The dump fires on exit, return and SIGTERM.
- **Board:** one fixed-duration session each for prboom, sdlquake, QuakeSpasm, OpenTyrian and tyr-glquake (if installed).
- **Kill:** counts disagree with libinsn, or the counting preload moves oplbench us/sample by more than 5%. It is never an A/B arm.

**W0b. h1s user-PC profiles (new; X2-14).**
- **Mechanism:** port `rootfs/pcsample.c` symbolisation into `scripts/board/h1s-report.py` (~30 lines, `perf-review:171`).
- **Targets:** profile the **installed prboom (9aa9beaa)**, which has never had a function-level profile despite being the headline benchmark (47.1 fps). Also profile the current sdlquake, whose identity against tiopex is unconfirmed.
- **Board:** quiet fresh boot, no polling (polling cost 3-5 fps).
- **Kill/decision:** every Quake/prboom body item is re-priced from this profile or stays deferred.

**W0c. Tyrian audio-thread split (new).**
- **Mechanism:** h1s on the audio TID in #402 with copy on: synth vs SDL_mixer vs conversion vs path.
- **Why:** the 22.7 us/sample budget covers the whole callback. Static v2 with no CSR (16.87 us, 0.74 core) leaves "little margin", and song 5 needs 1.1 cores (V2-REPORT §9, around line 450).
- **Decision:** W2 is go only if synth share × plausible reduction brings the heavy song's callback under budget.

**W0d. Installed-bytes census (new; stop/go for W2 and W8).**
- **Mechanism:** fetch hashes and text of the installed OpenTyrian, sdlquake, prboom and SDL libraries (via `s31-record serve` and curl, never base64 over the console).
- Re-run the window census on those bytes. The 122 fusible OPL pairs and the cave figures come from a cloud-built opl.o/oplbench, and the tiopex chain counts from an unconfirmed binary (`forward-plan:133-137`).
- **Kill:** fewer than 10 fusible dynamic-hot windows, or no cave on COW'd pages, in the installed OpenTyrian.

**W0e. PIE and engine price list (HW2).**
- **Measure:** per-op PIE throughput in-cache and PSRAM, entry cost, and one bounce.
- **Bounce method:** land on CPU1, restore the mask to {0,1}, then issue the instruction. A mask that excludes CPU0 pins the task instead (`esp32s31-ext.c:336-343`).
- **Output:** crossover table and bounce distribution with tail.
- **Kill rule for consumers:** less than 1.5x over the scalar twin means ship scalar.

**W0f. Runtime fault injector plus QEMU matrix (S4).**
- `-DS31FP_FAULTHOOK` in the same source as the shipped library; `S31FP_FAULT=<nr>:<nth>[:errno|destructive|partial]`.
- **Matrix:** every ordinal × mode × {exe, exe+DSO}; cache corruption; fork/vfork/posix_spawn/exec/static; SIGALRM storm; S31FP=0 vs patched dump md5.
- **Invariant:** rc and output equal the unpatched run, or 125 before main. Never 139, never a hang.
- **Caveat:** QEMU uses host mm, with no XIP and zero PFNs, so a pass is necessary but not sufficient.
- **Kill:** none as a tool. Any cell that violates the invariant blocks the item under test.

**W0g. Rewrite prover (R4).**
- Symbolic straight-line model: registers, fcsr, frame memory; helpers as uninterpreted functions with ABI clobbers.
- QEMU sandbox differential from 10k random states per site.
- Per-core oracles: 20M sets vs `lgref.o` × 5 frm with random preset flags.
- Two-process dumps, because the matcher can patch its own oracle (TYRIAN-ASM.md).
- **Kill:** a window class that cannot be proven is not emitted.

**W0h. Build gates and hygiene.**
- 0 `esp.*` of any kind in final linked libs31fp. The arch-tag `xesploop` check is advisory only, because crt/libgcc merge the tag in.
- `.hidden` on `s31v2_gtdf2/ltdf2/nedf2`; `-z start-stop-visibility=hidden` for `__start/__stop_s31fix`.
- mksig sub-body uniqueness assert; test that the oracle never self-patches; per-signature kill bits (`S31FP_KILL=`).
- **Kill:** none (gates).

**W0i. Crash capture (S3, revised).**
- Extend opt-in `rootfs/segvtrap.c` (`SEGVTRAP_HOLD=1`, already in `v2/board/pb-run.sh`).
- On a fatal signal, dump: s31fp's site table via a hidden accessor, PC/ra in a patched range, a byte re-verify, pagemap swapped/anon bits, **colour now vs at patch time** (detects migration), CPU, fcsr.
- **Soak:** prboom in copy mode under the original conditions (fresh boot, OpenTyrian arm first).
- **Stop:** after **20 clean runs**, record "not reproduced at N=20" next to the copy code and proceed. A capture blocks W2/W3/W8 until attributed.
- **"W0i clean at N=20" is a ship precondition for W2 and W3**, since both widen the copy path while the crash cause is unproven and copy is already the default.

### W1. (Reserved slot kept at W0; OPL work follows.)

### W2. Generic fused helper chains (bet 1)

- **Mechanism.** §2.2 with fused cores `si→mul`, `mul→mul`, `mul(const)`, `mul→fix`, `gt→mul`, `mul/add`.
  - Each operation still rounds to binary64 exactly as libgcc does, but intermediates stay unpacked.
  - One frm read per chain; flags are ORed and written once.
  - Constants come only from read-only PT_LOADs. A power of two becomes an exponent add, with fallback.
- **Target.** Rules apply to any executable. The first measured beneficiary is OpenTyrian `operator_output` (7 chained calls, 6 fusible links) plus `operator_attack/decay/release`. Claims for other programs require their own W0a counts.
- **Basis.** 4,783 vs 1,478 instr/sample. Per sample: muldf3 53.69, floatsidf 20.40, fixdfsi 10.15, gtdf2 10.44 calls. #402 runs 19.52 us title and 27.95 us heavy against a 22.7 us budget, with 558 underruns per 60 s.
- **Memory.** Cave on COW'd pages: oplbench copy mode leaves 5,766 B dead, largest piece 1,554 B (`E/cave_sizes.txt`; cloud binary, see W0d). Plus up to 4 call-site pages.
- **Risks.**
  - Mid-chain fflags differ from per-call libgcc as seen by a signal handler (§4.3).
  - Copy-path crash inheritance.
  - xruns/s moved the wrong way under v2 (14.2-14.6→26.6, V2-REPORT §8), so it is not a metric.
- **Cloud validation.**
  - `build-opl.sh` flags under `s31-cc`.
  - Title/heavy sample dumps plus per-call fcsr, S31FP=0 vs rewritten, md5-equal.
  - 10^7 random operator states × 5 frm.
  - W0g per core.
  - `oplcount.sh`/libinsn. **Stop/go: materially under 4,783 on the installed-bytes build.**
- **Board validation.** oplbench us/sample on CPU0 and CPU1 vs #402; the 60 s OpenTyrian window (underruns plus ear check); Private_Dirty; launch ms; silicon dump md5.
- **Ship gate.** Owner rulings §4.1 and §4.3; W0c/W0d go; W0i clean; heavy us/sample beyond spread; underruns fall. `S31FP_FUSE=0`, default off at first ship.
- **Kill.** No QEMU instruction cut, a board result inside spread, or W0c showing the synth cannot bring the callback under budget.

### W3. Memory-operation matrix (forward-plan item 2; kept, not dropped)

- **Mechanism.** Measure before shipping anything:
  - installed musl generic memset (8 `sw` per 10 instructions) vs a Zbb/unrolled scalar variant;
  - short memcpy and memset through v3 routing;
  - sizes 1 B-256 KB, alignments, CPU0/CPU1, migration, and the dispatch cost of short calls.
- **Basis.** memset is 0.7% of GLQuake prof9, equal to libc `__adddf3`. PIE memset is 1.00x (`xespv-libc.md:17-20`), so only a scalar or short-call win is possible.
- **Memory.** Under 1 kB of shared text.
- **Cloud.** Exactness vs libc over all size and alignment pairs up to 512 B plus random large; icount per size.
- **Board.** The matrix, 5 repeats.
- **Ship.** Per-size win beyond spread at sizes W0a shows are called, with no regression at any size. `S31MEM=0`.
- **Kill.** No size bucket wins.

### W4. DSO coverage, default-off (C2/C3/S2/P3/R7 merged)

- **Mechanism.** Constructor-time `dl_iterate_phdr`, the §2.4 allowlist, per-object tables outside the RW segment, and cache records keyed (soname, memsz, segment-relative offsets) with negative entries and byte re-verification.
- **Basis.** 13/12/8 full-body matches in SDL-1.2/SDL2/SDL_mixer. MEASURED heat is low: SDL1 0.2% and libasound 0.4% of GLQuake; all non-libc DSOs 1.4% of Quake CPU0.
- **Memory.** SDL-1.2 3, SDL2 3, SDL_mixer 2 copied pages (`E/dso_copy_pages.txt`). On XIP these are pure added RSS.
- **Risks.** COW/remap of XIP DSO text has never been done on the board; non-root processes get no colour.
- **Cloud.** Probe DSO carrying real libgcc.a bodies: patched before its constructor, ASLR-stable cache, no bytes outside the sites change, 0 mismatches.
- **Board.** First a 1-page XIP COW test in a trivial SDL program (maps, smaps). Enable per library only on W0a/W0b heat.
- **Ship.** Heat, W0i clean, a Private_Dirty budget, cold/warm launch within §2.9. `S31FP_DSO=0`.
- **Kill.** No library helper above noise.

### W5. Copy placement (bet 4)

- **Mechanism.** Pad each copied body with `c.nop` so it starts 4-byte (or line) aligned inside the libgcc slot when `copy_len+2 ≤ sig_len`. floatsidf copies 0x36 B into an 80 B slot at entry `0x5ca5a`, which is 2 mod 4. Then add a debug env that forces colour permutations.
- **Basis (MEASURED, different boots):**
  - `board-b7.txt`: 14 of 20 uncoloured runs at 19.46-19.83 us, and slow runs up to 31.09 when pages shared colour 0.
  - `board-b8.txt`: all 20 colour-matched runs at 19.91-20.56, each reproducing the page cache's sequential 0/1/2 colours.
  - Static v2: 18.12-18.43.
  - So colour matching reliably lands about 0.4-0.6 us above good assignments. Boot is a confound.
- **Memory.** 0.
- **Risks.** The gap may be neither alignment nor colour (the gedf2/ledf2 jump-back, a separate link). The tier-6 libGL colour copy was null (8.38 vs 8.58/8.50).
- **Cloud.** Exactness; icount unchanged (4,783); TAILREF re-encoding across the shift.
- **Board.** Arms: static, today, aligned, forced permutations, same boot where possible, colours logged.
- **Ship.** Median and tail beat today beyond ±0.5%. `S31FP_ALIGN=0`.
- **Kill.** Inside spread. Do not build a caller-page colour census; tier-6 showed pages-per-colour does not predict slow boots.

### W6. libm floor/ceil/trunc interposition (bet 2, part 1; forward-plan item 6 names these)

- **Mechanism.**
  - Integer word-pair masks with Zbb/Zbs; `csrsi fflags,1` exactly when result≠x.
  - **NaN is returned unchanged with no flags; sNaN does not raise NV** (`snan_raised_NV=0`).
  - No frm check (§2.3).
  - floorf/ceilf are already 8 F instructions and fmod/frexp/modf/ldexp/scalbn are integer in musl, so they are left alone. The rint family waits for its own harness, with non-RNE delegated.
- **Target.** tiopex FloorDivMod (4 sites), QuakeSpasm, libasound (ceil), tyr-glquake.
- **Basis.**
  - 0/20,000,000 bit+flag mismatches over 5 frm (`E/floor_ceil_exact.txt`). Negative controls: 41,225 and 20,580 of 200,000.
  - Harness A (toolchain libc image): floor 427→38, ceil 426→39, trunc 140→34 (`E/sqrt_floor_icount_libcimage.txt`).
  - Harness B (sysroot musl via PLT, `fl.c` integer version): floor 446→27.6 (`E/floor_icount_*`).
- **Memory.** About 1 kB of shared text.
- **Cloud.** Extend `probes/arith/fl.c` against the toolchain libc image with subnormals and 2^52 boundaries; run on the **board-toolchain-built** libs31fp.
- **Board.** Dump comparison vs installed libc 352e9417 (S31FP=0 vs `S31LM=1`, md5-equal); ns/call on both CPUs.
- **Ship gate (forward-plan item 6).** W0a caller rate × per-call saving is a nonzero, stated share in a named target; dump equality; board per-call win. `S31LM=0`, default off.
- **Kill.** Dump mismatch, no board per-call win, or no measured caller.

### W7. musl-rebuilt sin/cos/sincos (bet 2, part 2; census-gated)

- **Mechanism.**
  - musl 1.2.5 source for `__sin/__cos/__rem_pio2/sin/cos/sincos`; add `__sindf/__cosdf/__rem_pio2f/sinf/cosf/sincosf` only if called.
  - Flags: `-fno-builtin -ffp-contract=off -frounding-math`.
  - Helper binding via `objcopy --redefine-sym __muldf3=s31v2_muldf3` (etc.) on the musl objects, same class as `build-opl.sh`'s `--defsym`.
  - Non-RNE, and |x| large enough to reach `__rem_pio2_large`, delegate the **whole call** to libc via `RTLD_NEXT`. That is exact by construction and drops the cold 5,174 B routine.
- **Size (MEASURED, sysroot libc.a, `E/libm_trig_sizes.txt`):** the full set is 12,856 B, about 34% of the 38 KB RX segment. Of that, `__rem_pio2_large` is 5,174 and `__rem_pio2` 2,408. INFERRED hot sets without the large reduction: about 4.4 kB double, 3.2 kB float. No PSRAM tables. pow/exp/log stay out: their data tables repeat tier-3 (-4.7% instructions, +2% time).
- **Basis.**
  - 0 bit and 0 flag mismatches, 500,000 cases each, for sin/cos/sincos/exp/log/pow/atan/atan2/sinf/cosf/sincosf/powf vs the toolchain libc (6041b2cd) under random frm (`E/libm_rebuild_vs_libc_*.txt`). Negative control: 72,546/100,000.
  - libc helpers equal libgcc: 0 value and 0 fcsr mismatches, 200,000 pairs × 5 frm × {mul, add, div}, RMM included (`E/libc_helpers_vs_libgcc_allmodes.txt`).
  - Caveat: the 0-mismatch run was musl compiled by the cloud release GCC.
- **Cloud.** Re-run the oracle on the **board-toolchain (crosstool-NG 15.2.0) build** of libs31fp; objdump diff of helper-call sequences vs the libc bodies; specials and subnormals.
- **Board.** Dump vs 352e9417; ns/call; rate × saving.
- **Ship / Kill.** As W6. Also kill on algorithm divergence from the board libc.
- **sqrt.** A separate RNE-only microbench on harness A: 229→159 instructions (the `sq.c` 203 figure is harness B and is not comparable). The fsqrt.s seed needs fcsr save/restore. Ship only on a board ns/call win; it doubles as the first fsqrt.s/mulhu latency measurement.

### W8. Float-idiom lowering (bet 3; a W2 rule)

- **Mechanism.** When both operands come from `__extendsfdf2` (or a float-exact read-only constant) and the result goes directly to `__truncdfsf2`, emit `fmv.w.x; f{add,sub,mul,div}.s; fmv.x.w`.
  - frm is read first; RMM falls back.
  - Liveness must prove the double intermediate is dead.
  - No fused multiply-add for two-operation chains.
- **Target.** tiopex (pending W0d identity): 16+4 complete chains within 64 B; pairs `ext→mul` 44, `add→trunc` 43, `sub→trunc` 22.
- **Basis.**
  - 0 mismatches over 32M chains in RNE/RTZ/RDN/RUP. RMM: add 708,325, sub 707,621, mul 636,485, div 867,628 of 2M each (`E/double_rounding.txt`).
  - 2,663,272 directed near-2^-126 and FLT_MAX cases: 0 mismatches, 470,121 raising UF, 115,654 inexact results equal to ±MIN_NORMAL (`E/double_rounding_tiny.txt`).
- **Board.** Silicon chain vs fop.s over 8.4M+ sets including tininess edges (hw.c pattern); per-site rdtime.
- **Ship.** Owner §4.4, silicon equality, W0a dynamic share.
- **Kill.** Any QEMU/silicon disagreement, or negligible share.

### W9. Transactional engine and coherence hammer (S1/S7; prerequisite for widening)

- **Mechanism.** §2.6. The in-place path stays for the pre-main executable until the engine is proven.
- **Measured cost:** coalesced swap of 6 pages is 3 syscalls, per-page 13. Rollback re-mmap of the main executable works (+3 syscalls) (`E/strace/`).
- **Board.**
  - Confirm #402 runs the dual-I-cache path of 0054.
  - `cohammer` cells: {CPU0, CPU1, migrating} × {trampoline, copy} × {fresh frame, recycled frame, `MADV_PAGEOUT` then refault, **CMA pressure via DRM client buffers, `/proc/sys/vm/compact_memory`**}.
    - The migration and compaction cells are INFERRED hazards: the page moves to a random colour, and the handling of `PG_dcache_clean` across migration is unverified because the kernel source is absent. Level load under pressure is when the prboom crash happened.
  - Positive control: RW rewrite of an already-executed frame, last, in its own boot. Use setsid and conlog.
- **Ship.** 0 stale, 0 wedges, W0f clean, no launch regression.
- **Kill.** The positive control is clean (the hammer is insensitive: fix the test), or fresh frames go stale (fix our arch cache code before W4).

---

## 4. Owner decisions needed

1. **Is W2 generic or app-specific?**
   - Written rule: "must need no app-specific knowledge" (`gl-performance-campaign-2026-09-27.md:182-185`).
   - "A fused soft-double operator_output for OpenTyrian's OPL" is held for the owner (`status-and-todo-2026-09-27.md:167-171`; `gl-performance-campaign:201-203`).
   - This design proposes option **(a)**: generic chain rules with no per-app tables. Its first measured beneficiary is exactly the held item.
   - Please rule whether (a) is covered by the hold. Option (b), OpenTyrian-specific body signatures, would need the hold lifted per body with installed-hash identity and per-body kill switches.
   - The relayed request ("copy and replace code across the main executable… while staying within our rules") does not settle this.
2. **libm exports beyond floor/ceil/trunc (W7).** "musl libc, ld.so and libm are off-limits" (`status-and-todo:172`). Forward-plan item 6 names only floor/ceil/trunc/scalbn/ldexp. Interposition writes no libm bytes (v3 precedent). Please confirm.
3. **Mid-chain fflags (W2).** Visible to a signal handler mid-chain. This is wider than the NX-only-if-unset caveat (V2-REPORT §3; forward-plan item 4, "Review the documented NX/signal caveat"). Accept it, or require a flush at every original call boundary.
4. **Float-idiom lowering (W8)** vs "No generic double-to-float substitution" (`forward-plan:52-53`). It applies only where the program already rounds to float, and it is proven exact. Please acknowledge the distinction.
5. **dlopen patching.** "Do not patch concurrently executing dlopen objects" (`forward-plan:43-44`). SDL starts its audio thread inside `SDL_OpenAudio`. Recommendation: out of scope.
6. **frm-only / no-NX helper variants.** Measured 17.94 and 16.87 us/sample (V2-REPORT §9). Untouched here.
7. **In-memory libc text patching** (libc-build helper family, libc PIE entry routing). Excluded by `forward-plan:40`. Killed here; reopen only by explicit ruling.
8. **Stale rule text.** `docs/accel-plan.md:207-209` still says PIE is disabled for userspace, superseded by v3. The copy-default docs listed in §2.1 are also stale.
9. **Own loadable offload driver.** Allowed by `CLAUDE.md:383-384`, but in tension with `forward-plan:63-66`. Recommendation: closed on economics; reopen only for a profiled CMA-resident bulk case of 128 KB or more, via lvdesk.
10. **Ordering.** Cheap cloud prototypes of W6-W8 ahead of the ship order? Recommendation: prototype in parallel, ship in owner order.

---

## 5. Rejected or deferred

| Idea | Verdict | Why |
|---|---|---|
| libc helper family / PIE routing via libc text swap (C5, A7) | Killed | `forward-plan:40`, `status-and-todo:172`. 16-20 kB private per process (INFERRED). Swapping live libc under `do_init_fini`. W6/W7 reach the libm share with no text writes. |
| 64-bit division helpers and constant-divide thunks (C6, A6, R6) | Killed | Replacement 69 vs libgcc 51 instructions. Fast path 47 vs 51 above 2^32. K=1000 thunk 35 vs 81-83, not 10-15 (`probes/arith/ud.c`, `rewriter/claims/spec.c`; counts not re-captured, exactness in `E/udivdi3_exact.txt`: 0/5M). Hot callers are first-party (lvdesk 75+15, s31route 8+1): fix at source. |
| SDL_MixAudio kernel (HW3) | Killed | OpenTyrian cf5dbeb6 mixes in its own float loop and tiopex does not import it. U8 twin 19.0 vs 17.0 instr/sample. GCC already emits Zbb min/max. Other A3 functions go to W0a instead. |
| Quake span loops (R5, HW6, C4-Quake) | Deferred | D_DrawSpans8 is 11.90% at about 29 cyc/px against a 17-20 floor (`perf-review:9,334`), so its INFERRED ceiling is 11.90 × (9-12)/29 ≈ **3.7-4.9% of CPU0**. The top-5 pixel loops total 22.50%. About 10% of CPU0 is PSRAM stall inside these loops, so much of the gap is stall. The "360 MHz +4%" figure is from X11 TyrQuake (`perf-plan:880-1000`), not h1s-062518. Deferred for identity (W0b), the owner hold, and a twin only INFERRED at 13→11-12 instr/px. |
| rseq critical-section PIE kernels (HW1) | Deferred | Residual bounces are attributed to other tasks and libc-internal calls, and the fps bimodality persists in scalar arms (`artifacts/gl/dips/README.md`). rseq is ENOSYS in QEMU (`E/rseq_enosys.txt`). Forward memmove under abort mismatched 15,623/21,675 at gap 1 (`E/rseq_chunk_abort.txt`). |
| DMA engines behind interception | Closed (HW4) | Fixed costs and CMA-only reach (§2.5). |
| In-process heat sampler plus live swap (R3) | Killed | EINTR in unmodified apps, contaminated runs, post-main mremap fatality. |
| Entry-redirect hot pool (P7, C7-B) | Killed | Entry-only redirects touch the same page count as full copies (`E/dso_copy_pages.txt`), and re-add the 1.92 instr/call hop. |
| mlock of patched pages (P4) | Deferred | No evidence of eviction; mlock also does not stop migration. The lvdesk mlockall result is `MCL_FUTURE` pinning client pixmaps and is not evidence either way. First check premise with W0i colour/swap bits. |
| divdf3 copy-in-place (P5) | Deferred (hygiene) | 0.00 calls/sample on OPL title. Fits (610 vs 1,562 B) only with `-mno-relax`. |
| Syscall trims as a gain (P6) | Folded | 3-4 syscalls ≈ 5-12 us against +17-27 ms measured. Measure the ms breakdown first. |
| Float-float sinf/powf fast path (A5) | Deferred | powf/atan2f domain is 2^64. The host fenv oracle is not soft-fp. Rebuild first (W7). |
| Always-on stats ring, sigaction interposition, auto-quarantine | Dropped | SD write every exec; SI_USER swallowing, inherited SIG_IGN, direct `__sigaction` binding in musl. |
| Class-A SHA-256 identity, class-M closure | Deferred | No approved body yet. Use a cheap cached hash when one exists. |
| libX11, libGL in the allowlist | Removed | First-party; libX11/libasound matches are nm-only. |
| Critic fix 1 (evidence into `artifacts/s31fp-evolution/`) | Applied | Promoted at commit time; see the evidence convention at the top. |
| Cited "0/10^8 libc-helper pairs" (`lcmp.c`) | Withdrawn | It cycled 256 distinct pairs. Replaced by `E/libc_helpers_vs_libgcc_allmodes.txt`. |

---

## 6. What QEMU established, and what only the board can answer

**Established** (QEMU 10.0.6, board ISA; correctness and counts only; files in `E/`):

- **floor/ceil:** 0/20,000,000 bit+flag mismatches under 5 frm; musl's result is mode-independent; no NV on sNaN. Controls: 41,225 and 20,580 of 200,000.
- **Instruction counts, harness A** (toolchain libc image, `sqrt_floor_icount_libcimage.txt`): floor 426.8→38.1, ceil 426.4→38.9, trunc 140.1→33.6, sqrt 229.4→158.9.
- **Instruction counts, harness B** (`floor_icount_*`): floor 446.4 and ceil 443.9 → 27.6.
- **Rebuilt musl libm:** 12 functions, 500,000 cases each, 0 bit and 0 flag mismatches vs toolchain libc 6041b2cd. **Not yet tested against board libc 352e9417.**
- **libc helpers vs libgcc:** 0 value and 0 fcsr mismatches, 200,000 pairs × 5 frm × 3 operations.
- **sqrt under RMM:** musl sqrt RMM==RTZ in 1,000,000/1,000,000 and RMM==RNE in 499,148 (`sqrt_rmm_eq_rtz.txt`). libgcc soft-fp has no RMM case.
- **Double rounding through binary64:** 0/32M outside RMM; RMM mismatches 636k-868k per operation; 0/2,663,272 directed cases.
- **Constructor ordering:** the preload constructor runs before every DT_NEEDED constructor in both link orders, and sees all 7 objects. An earlier preload runs first.
- **`dlpi_adds`:** one global counter. It went 0→1→2→3→4 across new, repeat, loaded and NOLOAD dlopens, and did not change on a failed dlopen, `dlopen(NULL)` or dlclose. The new object's constructors run before dlopen returns.
- **rseq:** ENOSYS (errno 38).
- **Chunked abort:** memcpy and memchr idempotent; forward memmove not (g=1: 15,623).
- **Cave:** oplbench 5,766/7,794 B (copy/trampoline), largest piece 1,554/2,060. tiopex 6,258/8,422 B over 5 pages.
- **DSO copy pages:** SDL-1.2 3, SDL2 3, SDL_mixer 2, opentyrian 3, sdlquake 3, prboom 2. Entry-only redirects are equal.
- **libm trig object sizes:** 12,856 B in total.
- **Not re-captured (sources remain in the probe directories):**
  - libm helper shares: sincos 90.9%, sin 89.6%, sinf 88.7% (`coverage/v/sh.c`, `attr2.py`);
  - constructor syscalls: copy+colour 22-28, trampoline 10, helper-free 7, engine 13/3 (`E/strace/` holds the raw traces);
  - copy = static instruction parity and +2.0 instr per trampoline hop (`placement/`).

**Board-only:**
- all timing (fused chains, libm ns/call, fsqrt.s/mulhu latency, alignment and colour arms, launch-ms breakdown);
- PIE throughput and bounce price, and whether rseq abort works on 7.1;
- board libc identity;
- cross-hart coherence of sealed frames, and migration/compaction of patched text;
- hart-0 I-cache geometry and the 16 vs 32 kB question;
- XIP DSO COW and rollback;
- the prboom copy-mode crash;
- every call rate and every profile, including the first function-level profile of prboom and the Tyrian audio-thread split.