# Moving to a newer kernel

## The version is decided by XIP, not by what is newest

**RISC-V XIP kernel support was removed from mainline in v7.1.** It survives in
`arch/arm` and `arch/xtensa` only; `arch/riscv` has no reference to
`XIP_KERNEL` at all, and `vmlinux-xip.lds.S` is left behind orphaned. Checked
tag by tag:

	v6.13 .. v7.0   XIP_KERNEL present
	v7.1, v7.2      removed

This board executes its kernel from flash because it has 15.4 MB of RAM. So the
practical ceiling is v7.0 - and v7.0 is not in kernel.org's maintained list, so
it gets no fixes.

**The last kernel with *working* XIP is 6.14**, not 6.18:

	v6.12 .. v6.14   riscv runtime-const absent   XIP works
	v6.15 .. v7.0    runtime-const present        XIP present but BROKEN
	v7.1+            XIP removed

So "6.18 still has XIP" is true and misleading - it is broken there too, and the
runtime-const fix below is needed on *any* base past 6.14. Choosing 6.18 buys
LTS maintenance, not working XIP.

## The BSP is small, which is what makes this tractable

The vendor tree is a **single squashed import** of 6.12 - 86,729 files in one
commit - with the S31 work already mixed in. There is no upstream history to
rebase onto and no pristine base to diff against inside the repo.

But diffing it against a pristine 6.12 tarball shows the BSP is only:

	45 added paths        self-contained drivers, DTS, defconfigs
	101 modified files    ~3,100 changed lines
	0 deleted

and most of those 101 are one-line `obj-$(CONFIG_...)` registrations. The real
surface is `arch/riscv` (24 files: XIP, CLIC, custom extension state, cache and
DMA coherency), `drivers/usb`, `drivers/mmc` and `kernel/time`.

`scripts/port-kernel.sh` automates carrying that forward: diff old-pristine
against ours, apply to new-pristine as a patch, copy the purely-ours paths, and
carry anything upstream deleted.

**Count `.rej` files, not log lines.** `patch` writes "hunks failed" in lower
case; an early version of the script grepped for `FAILED` and cheerfully
reported a clean apply while rejects were being written to disk.

## What actually broke, 6.12 -> 6.18

Applying the BSP left 14 files with rejects, of which 10 were mechanical and 2
were stmmac (dropped - this board has no Ethernet PHY). The rest, plus what the
compiler then found, was genuine API churn:

| Change | Fix |
|---|---|
| `CACHEMAINT_FOR_DMA` became a menuconfig | move `select DMA_DIRECT_REMAP` to `SOC_ESP32S31`, or it closes a dependency loop with `ERRATA_STARFIVE_JH7100` |
| `folio->flags` -> `folio->flags.f` | mechanical |
| `struct function_desc.func` is now a `const struct pinfunction *` | drop the post-hoc fixup; `pinmux_generic_add_function()` already builds it |
| `PIN_CONFIG_OUTPUT` -> `PIN_CONFIG_LEVEL` | rename; this is the path `output-high`/`output-low` arrive on |
| `gpio_chip->set`/`set_multiple` now return `int` | return 0 |
| `platform_driver.remove_new` removed | rename to `.remove`, already the right shape |
| `hrtimer_init()` + assigning `.function` | `hrtimer_setup(timer, fn, clock, mode)` |
| `drm_fbdev_dma_setup()` removed | `DRM_FBDEV_DMA_DRIVER_OPS` in the driver + `drm_client_setup_with_fourcc()` |
| `cpufreq_generic_attr` removed | drop `.attr`; the core publishes it now |
| akcipher `.sign`/`.verify` removed | drop them; raw sign/verify moved to the `sig` alg type |
| IIO `write_event_config` state `int` -> `bool` | mechanical |
| `MODULE_IMPORT_NS(X)` -> `MODULE_IMPORT_NS("X")` | mechanical |
| `dw_mci_init_slot()` restructured | our `goto err_host_allocated` had no label; return directly |

All of it is in `patches/0001-esp32s31-6.18-api-fixes.patch`: 20 files, 387
lines.

## Making it fit: done

6.18 is bigger than 6.12, and this configuration carries sound, Bluetooth and
Wi-Fi. The space came from the userspace XIP image, which was 6.9 MB of Xorg,
its modules and X11 fonts for a board that now boots to text mode:

	XIP image   6,926,336 -> 3,596,288 bytes   (XIP_ROOTS trimmed to the text set)
	linux       0x560000  -> 0x620000          6,422,528 bytes
	rootfs      0x960000/0x6A0000 -> 0xA20000/0x5E0000

The 6.18 kernel is 5,966,709 bytes and now has 455 KB spare. The desktop root
set is kept as `XIP_ROOTS_DESKTOP` - it was tuned by measurement and should not
be re-derived.

**This layout is live and verified with the 6.12 kernel**: Wi-Fi, sound, both
cramfs XIP mounts and opkg all work on it.

## Why upstream removed it, and why that matters to us

Commit `9b3a2be84803` ("riscv: Remove support for XIP kernel", Nam Cao,
2026-04-04):

> XIP has a history of being broken for long periods of time. In 2023, it was
> broken for 18 months before getting fixed. In 2024 it was 4 months. And now
> it is broken again since commit a44fb5722199 ("riscv: Add runtime constant
> support"), 10 months ago. These are clear signs that XIP feature is not being
> used. [...] Remove XIP support. Revert is possible if someone shows up
> complaining.

So it went for lack of users, not because it is unfixable - and the middle
sentence is the operative one for us.

**Runtime constants are incompatible with XIP, and that is what stopped our
first 6.18 boot dead.** `runtime_const_init()` rewrites the `lui`/`addi`
parcels of an instruction *in place*; under `CONFIG_XIP_KERNEL` that memory is
the read-only flash window, so the fixup faults or is silently dropped. Its
only user is `fs/dcache.c`, from `dcache_init_early()` - early enough in
`start_kernel()` that there is no console yet, which is exactly why the board
was silent rather than printing an oops.

The fix is three lines: under `CONFIG_XIP_KERNEL`, include
`asm-generic/runtime-const.h` instead, which just uses the symbols directly.
`arch/riscv/include/asm/runtime-const.h` in the port does this. With it, 6.18
boots through DRM, the hosted radio, `wlan0`, `hci0`, microSD and EXT4.

## 7.2, attempted

Done, and it builds. `patches/0003-revert-riscv-remove-xip.patch` is commit
`9b3a2be84803` reversed - 973 lines, 17 files - and reverts cleanly onto 7.2
apart from two hunks: `xip_fixup.h` (patch cannot name a file it is re-creating
from a deletion, so it lands as `Oops.rej`) and two `!XIP_KERNEL` guards in
`arch/riscv/Kconfig` whose context drifted. Note the follow-up "riscv: further
remove XIP" landed in the 7.3 merge window, so **7.2 needs only the one
revert**.

On top of that, 7.2 needs everything 6.18 needed **plus** six more API changes:

| Change | Fix |
|---|---|
| `clk_ops.round_rate` removed | `determine_rate(hw, struct clk_rate_request *)`, reports through the request |
| ASoC `pcm_construct` | renamed `pcm_new`; our callback already had the right signature |
| `snd_soc_dapm_kcontrol_component/dapm` | `..._to_component` / `..._to_dapm` |
| cfg80211 `get_station` | takes `struct wireless_dev *`, not `struct net_device *` |
| `bin2hex` | moved to `linux/hex.h` |
| `esp32_uart.c` `.remove_new` | it is our file now, so it never got upstream's conversion |

`patches/0002-esp32s31-7.2-api-fixes.patch`: 31 files, 682 lines. It builds at
6,094,841 bytes with 327 KB spare.

**It does not boot** - completely silent, like 6.18 was before the DTB fix. But
the config is verifiably right this time (`XIP_KERNEL=y`, `XIP_PHYS_ADDR`,
`BUILTIN_DTB_NAME` populated, runtime-const falling back), so this is a
different and so-far unidentified fault, and it needs its own early-boot
investigation.

**6.18 is much further along** - it boots the entire kernel and reaches
userspace. That is the honest reason to prefer it today, as opposed to the
reason originally given here, which was wrong: 6.18's in-tree XIP is broken too.

## Reaching 7.1 / 7.2

Two routes, and they are genuinely different in kind:

1. **Revert the removal.** `9b3a2be84803` (17 files, -351/+37) and
   `8b5b048277e2`-era "riscv: further remove XIP" (4 files, -148/+3) are both
   published, so XIP can be restored onto 7.2 mechanically - roughly 500 lines -
   plus the runtime-const fix above, which upstream never made. We would then be
   maintaining a feature mainline deleted, but the commit explicitly invites it:
   "Revert is possible if someone shows up complaining."
2. **Stop using XIP.** `annoyedmilk/esp32-s31-linux` runs 7.1 on this same SoC by
   copying the kernel into PSRAM at `0x50000000` and booting a BusyBox
   initramfs - so the removal costs them nothing. That is the cheaper path in
   maintenance and the expensive one in RAM: our kernel is ~6 MB of the 15.4 MB
   total, and memory is the binding constraint on this board. It would also
   *gain* speed, since kernel code from flash is ~6x slower than from RAM.

Route 2 is worth measuring rather than assuming, now that the desktop is gone
and text mode has ~5 MB free.

## Status: 6.18 boots and runs

	kernel     6.18.46
	xip        3 cramfs mounts, 2 overlays  (userspace still runs from flash)
	bluetooth  hci0
	sound      Korvo1, simple-card
	drm        card0
	wifi       associated to the AP
	opkg       0.7.0
	memory     15 MB total, 6 MB available

### The bug that stopped it: a signal frame sized without our record

`init` reached userspace, ran the first line of `/init`, and then died with
`epc = ra = 0`. Three things had to be established to find it, and two of the
obvious suspects were wrong:

1. Tracing `START_THREAD` showed exec was **fine** - `/init` and `/bin/mount`
   both got sane entry points and load biases. So it was not PIE loading.
2. The oops register dump had `a7 = 0x5f` = **95, `waitid`** - so init was
   asleep waiting for the `mount` child, and died on the way *out* of a
   syscall, not in userspace code.
3. Tracing `ra`/`epc` across syscalls for pid 1 caught the culprit:
   **syscall 139, `rt_sigreturn`**. init took SIGCHLD, ran a handler, and
   returning from it restored a corrupt context.

The cause: `get_rt_frame_size()` had lost its

	total_context_size += ESP32S31_EXT_SC_SIZE;

`save_esp32s31_ext_state()` appends an extension record to every signal frame,
so the frame must be sized for it. Without that line the record is written
**past the end of the frame, over the user stack**, and `rt_sigreturn` then
restores whatever survived.

It went missing because upstream changed the adjacent line from `has_vector()`
to `has_vector() || has_xtheadvector()`, so the hunk's context no longer
matched and the auto-applier skipped it - a one-line omission that only
manifests on the first signal a process receives.

**The lesson is about the tooling, not the kernel:** a patch hunk that fails to
apply silently is far more expensive than one that fails loudly. Count `.rej`
files, and when a file is partially applied by hand, diff the result against the
old tree rather than assuming the remaining hunks were cosmetic.

### Still open

- **`Illegal instruction` (SIGILL)** from some userspace - seen in
  `dbus-daemon` inside `libexpat`, and from `ifup`. The affected commands still
  complete (Wi-Fi associates), so it is not fatal, but it is real and
  unexplained. Suspect the F-extension or custom-extension state, since the
  ABI is ilp32 on a hart that has `f`.
- An `awk` SIGSEGV early in boot, possibly the same cause.

## Superseded: what it looked like before the fix

With the runtime-const fix, 6.18 boots through the whole kernel - DRM, the
hosted radio transport, `wlan0`, `hci0`, microSD, EXT4 - and then:

	init[1]: unhandled signal 11 code 0x1 at 0x00000000
	epc : 00000000 ra : 00000000   cause: 0000000c
	Kernel panic - not syncing: Attempted to kill init!

`epc` and `ra` both zero means the process entered userspace with a zeroed
context, i.e. the ELF entry point came out as 0. Userspace here is PIE (see
`s31-pie-cases`), so the interpreter's load bias is the thing to suspect.

Ruled out already: the `esp32s31_ext` field is in `struct thread_struct` at the
same position as 6.12, and `flush_icache_pte()`'s signature and callers are
identical between the two, so neither the custom extension state nor the
I-cache path explains it.

Earlier, before the runtime-const fix, it produced no output at all. Also
established then:

- **Not the partition geometry.** The flashed table decodes exactly as intended,
  and the 6.12 kernel boots from the same layout.
- **Not a loader restart loop.** One boot banner, no loader error, no reset.
- **Not the image header.** Both images carry the same `RISCV` magic and header
  layout; only the size field differs.
- **Not a missing device tree - but that *was* one bug.**
  `CONFIG_BUILTIN_DTB_SOURCE` was renamed `CONFIG_BUILTIN_DTB_NAME`, so the
  Makefile's `--set-str` set a symbol that no longer exists and the first
  attempt linked **no DTB at all**. The Makefile now sets both names. Fixing it
  grew the image by the size of the DTB, and changed nothing about the silence.
- **It dies before `earlycon`.** Adding
  `earlycon=esp32s31uart,mmio32,0x2038a000,1000000n8` produced no output either,
  so it is failing in early assembly or the XIP page-table setup, before console
  init.

The next place to look is the XIP early-boot path - `head.S`, and the 4K
leaf-mapping code in `create_kernel_page_table()` that this board needs because
RV32 folds the PMD. A `.text.fast`-style direct UART poke in `head.S` would
localise it, since there is no console to print from yet.


## What annoyedmilk/esp32-s31-linux teaches about 7.1

That project runs **7.1 on this same SoC**, so it is worth being precise about
why, and their kernel patches are public (`linux/patches/`, nine files, ~20 KB
total against our 146 files).

**They never use XIP.** Their loader "copies the exact Linux image ... through
the cacheable aperture at `0x50000000`" and boots a BusyBox **initramfs**, so
the kernel runs from PSRAM. Removing XIP in 7.1 cost them nothing.

Their `0001-riscv-esp32s31-soc-support.patch` and our BSP agree, line for line,
on every early-boot arch detail:

- `.balign 64` before `handle_exception`, because stvec is forced to CLIC mode
- `ori a0, a0, 3` on the trap vector for the CLIC mode bits
- avoiding `csrw CSR_IE/CSR_IP` in `_start_kernel` and using
  `csrc sstatus, 2` instead - **`sie`/`sip` are not implemented on this hart**,
  so writing them traps before a handler exists
- carrying `esp32_uart.c` out-of-tree with its Kconfig and Makefile entries
  restored, since 7.x deleted it from mainline

We already do all of that, and it is present and identical in both our 6.18 and
7.2 ports. So the arch bring-up is not what stops 7.2.

**By elimination, our 7.2 failure is in the XIP path** - the one part of the
system their build does not exercise, and therefore the one part their work
cannot validate for us.

### Why we cannot simply copy their approach

Running from RAM would sidestep XIP entirely and unblock 7.x. It is not
available to us at this feature set: the image header reports
`img_size = 0x899000`, so text+data+bss is **~9 MB of our 15.4 MB**. Their
kernel is far smaller - initramfs, no DRM, no X, no sound stack. For us the
kernel lives in flash precisely because the RAM is not there.

### What is actually needed next

An output channel that works before the console. Three candidates, in order of
cost:

1. **SBI early console.** Their README notes "its SBI early console hands off to
   UART0", so *their* OpenSBI implements the legacy console extension. Ours
   apparently does not - `earlycon=sbi` with `CONFIG_RISCV_SBI_V01` produced
   nothing. Rebuilding OpenSBI with console support would give a channel that
   works from the first kernel instruction.
2. **JTAG.** `openocd-esp32` is installed on the host; halting the hart would
   show exactly where it spins.
3. **A version bisect** - port to 7.0 (last release with XIP in tree, no revert
   needed) and then 7.1. If 7.0 boots, the fault is in 7.1/7.2 churn; if it does
   not, it is in the 6.19-7.0 range and the revert is a red herring.

**A raw UART poke does not work and should not be tried again.** Writing
characters straight to `UART_FIFO_REG` at `0x2038a000` from `head.S` produced no
output *even on the 6.18 kernel that boots*, which is what proved the technique
dead rather than the kernel. Always run that control before trusting a silent
instrument.


## Version bisect: 7.0 works, 7.1 is close, 7.2 is silent

With no early-boot output channel available, boot-versus-silence is itself a
usable signal. Porting to each release in turn located the faults precisely.

| Base | Result |
|---|---|
| **6.18.46 LTS** | **works fully** - DRM, sound, BT, Wi-Fi, XIP mounts, opkg |
| **7.0** | **works** - shell, Wi-Fi, opkg (DRM did not probe; unexplained) |
| 7.1.10 + XIP revert | boots, SD detected, data read errors during transfers |
| 7.2 + XIP revert | silent before console init |

**7.0 booting is the important result: it proves the XIP revert is sound.** 7.0
carries XIP in tree, needs no revert, and runs the same BSP - so nothing between
6.19 and 7.0 broke XIP beyond the runtime-const fix. Whatever stops 7.2 arrived
in 7.1 or 7.2.

### 7.1: the dw_mmc slot refactor

7.1 removed the `dw_mci_slot` abstraction and merged
`dw_mci_prepare_desc64`/`desc32` into one `dw_mci_prepare_desc()`. Three of the
BSP's cache-coherency hunks therefore had nowhere to land, and **failed
silently**:

1. `dw_mci_idmac_sync_for_device()` at the end of descriptor preparation - the
   writeback that publishes the ring before the IDMAC reads it. Missing it gave
   `mintsts=0x200` (DRTO) and "error -110 whilst initialising SD card".
2. The open-coded OWN-bit poll, which 7.1 replaced with
   `readl_poll_timeout_atomic()`. That reads the descriptor **without
   invalidating**, so on this SoC the IDMAC's clear of OWN is invisible and the
   poll spins on a stale value. Restored as a noncoherent branch that
   invalidates each iteration.
3. Both were caught by *counting call sites against the working 7.0 tree* and by
   a `defined but not used` warning - not by anything the patch tooling said.

With those two fixed the card initialises and `mmcblk0` appears, the interrupt
storm ("interrupt status did not quiesce", our own message from the budget loop)
is gone, and the kernel reaches ~4 s before data read errors during real
transfers. Every remaining coherency call site has been checked to be present,
in the same enclosing function as 7.0, and the `IDMAC_DESC_NONCOHERENT` quirk is
confirmed set. The residual fault is somewhere else in the 7.1 dw_mmc rework.

**7.1 is not yet usable.** It is much closer than 7.2, and the failure is now
confined to one driver rather than "the kernel is silent".


## 7.1.10 works

	kernel     7.1.10
	xip        2 cramfs mounts, 2 overlays
	drm        card0
	sound      Korvo1, simple-card
	bluetooth  hci0
	wifi       associated
	opkg       0.7.0
	rootfs     ext4 on microSD, mounted rw

Four more faults had to be found, and every one of them was a **silently
dropped patch hunk** caused by 7.1 restructuring the code the BSP patches.

### The dw_mmc slot refactor, in detail

7.1 removed `struct dw_mci_slot` and merged `dw_mci_prepare_desc64`/`desc32`
into one `dw_mci_prepare_desc()`. That broke four things:

1. **The descriptor writeback** at the end of preparation. Without it the IDMAC
   reads stale descriptors: `mintsts=0x200` (DRTO) and "error -110 whilst
   initialising SD card".
2. **The OWN-bit poll.** 7.1 replaced the open-coded loop with
   `readl_poll_timeout_atomic()`, which reads the descriptor without
   invalidating - so the IDMAC's clear of OWN is invisible and the poll spins on
   a stale value until it times out. Restored as a noncoherent branch.
3. **`spin_lock_init(&host->irq_handler_lock)`.** The lock is declared and taken
   by our budget-loop handler on every interrupt, but its initialiser was
   dropped. It survived only because `devm_kzalloc()` happens to zero it.
4. **The descriptor stride - the one that actually mattered.** The BSP spaces
   descriptors `desc += 4`, i.e. **one per 64-byte cache line**, so a CPU write
   to one descriptor cannot clobber a neighbour the engine owns. This is a
   *four-part coordinated change*: ring capacity
   (`DESC_RING_BUF_SZ / (sizeof(desc) * 4)`), the forward-link addresses
   (`(i + 1) * 4`), and the advance in both prepare paths. The first three
   applied; **the advance did not**, so `dw_mci_idmac_init()` laid the ring out
   with 64-byte spacing while `prepare_desc()` walked it in 16-byte steps and
   filled descriptors the chain never pointed at.

With those fixed, EXT4 mounts and the system boots. Two retried `data error`
lines remain per boot and the filesystem is clean afterwards.

### And the DRM driver was not being built at all

`drivers/gpu/drm/Kconfig` had lost its `source "drivers/gpu/drm/espressif/Kconfig"`
line, so `CONFIG_DRM_ESP32S31_LCD` did not exist and the driver was silently
absent - which is why 7.0 and the first 7.1 boots had no `/dev/dri`. It also
needs `#include <drm/drm_print.h>` now, since `drm_warn`/`drm_info`/`drm_err`
no longer arrive transitively.

**Counting call sites against a known-good tree is what found all of these** -
that, and one `defined but not used` warning. The patch tooling reported a
clean apply every time.

## 7.2: still silent

7.2 now builds **with DRM enabled** (6,205,513 bytes, 217 KB free), which
required one more API change: `struct drm_atomic_state` is renamed
`drm_atomic_commit` throughout the plane-helper path. Note the earlier "7.2
builds" claim was weaker than it looked - DRM was not being compiled at all.

It still produces no console output. Ruled out with evidence: the XIP revert
(7.1 boots with the same revert), the dw_mmc refactor, DRM, the partition
geometry, the image header, and the config delta against working 7.1 - which is
now down to `ARCH_USERFLAGS`, `GENERIC_BITREVERSE`, `NET_VENDOR_ALIBABA` and the
SBI earlycon symbols. The `GENERIC_LIB_*DI3` difference was checked and is
benign: both kernels define `__ashldi3`/`__ashrdi3`/`__lshrdi3`.

`CONFIG_ARCH_USERFLAGS="-march=rv32g -mabi=ilp32d"` appears in 7.2 and not 7.1.
`rv32g` implies **D**, and this hart has `f` without `d`. That is worth chasing -
if it reaches the vDSO, every userspace call into it would trap - but it does
not obviously explain a hang before console init.

**7.2 needs a real early-boot channel** (OpenSBI console driver, or JTAG) rather
than another hypothesis.


## The F-without-D limitation: why everything floating-point was broken

Every kernel past 6.12 rejected this hart's FPU outright. `/proc/cpuinfo` told
the story once it was compared side by side:

	6.12:  rv32imafc_..._zca_zcf_...   f and zcf present
	7.1:   rv32imac_..._zca_...        both gone

Upstream added a validator for the `f` extension after 6.12:

	static int riscv_ext_f_validate(...)
	{
		if (!IS_ENABLED(CONFIG_FPU))
			return -EINVAL;
		if (!__riscv_isa_extension_available(isa_bitmap, RISCV_ISA_EXT_d)) {
			pr_warn_once("This kernel does not support systems with F but not D\n");
			return -EINVAL;
		}
		return 0;
	}

**It requires D.** This hart is `rv32imafc` - F with no D - so `f` was dropped
from the ISA bitmap, `has_fpu()` returned false, `start_thread()` never set
`SR_FS_INITIAL`, and `sstatus.FS` stayed **Off**. With FS off, *any* access to
an FP CSR is an illegal instruction. That is the whole explanation for the
"Illegal instruction" messages seen from `ifup`, `dbus-daemon` inside
`libexpat`, `awk` and CoreMark on 6.18, 7.0 and 7.1.

Decoding the trap made it unambiguous: `badaddr = 0x00132073` is
`csrs fflags, t1`, and `status: 80018020` has FS = 0.

The support is real on our side - the BSP already patches `fpu.S` to save and
restore F-only state with `fsw`/`flw`, because this hardware has always been
F-without-D. Upstream simply declined to support the combination. Relaxing the
check for `CONFIG_SOC_ESP32S31` restores `rv32imafc..._zcf_...`, and `fptest`
passes: float add, float mul, double add and `ceilf` all correct.

**This is worth reporting upstream.** F-without-D is a legal RISC-V
configuration, the kernel's own `fpu.S` can be built for it, and the rejection
is a policy choice rather than a technical limit.

## Does 7.1 actually perform better than 6.12?

Measured on the board, text mode, five CoreMark runs per arm, fresh boot each:

	              6.12        7.1.10
	median       1013.63     1018.69     +0.5%
	spread        0.26%       0.13%
	MemAvailable  4868 kB     5984 kB     +1116 kB (+23%)

**The CPU difference is real but negligible**; the ranges do not overlap
(6.12 peaks at 1014.04, 7.1 bottoms at 1018.17), so +0.5% is a genuine
measurement rather than noise, and it is not worth an upgrade on its own.

**The memory difference is the real gain: 23% more MemAvailable at idle.** On a
board where memory is the binding constraint that matters far more than half a
percent of CPU.

Caveat worth stating: CoreMark is a pure integer CPU benchmark and exercises
none of the memory-management work, so it is the *wrong* instrument for the
question - it is reported here only because it is the arm that can be compared
cleanly. The MemAvailable figure is the one to act on.

An earlier single 6.12 run read 997.55, below the entire five-run range, which
is the usual reminder that one run is not a measurement.


## Does 7.2 or HEAD offer more CPU or RAM?

Short answer: **no material gain over 7.1, and the one promising lever measures
far smaller than it looks.**

### What 7.2 actually adds

- **New MM options are hardening, not savings.** The only `mm/Kconfig` symbols
  7.2 adds over 7.1 are `KMALLOC_PARTITION_CACHES/RANDOM/TYPED`, which partition
  kmalloc caches against heap exploits. They *cost* memory.
- **`arch/riscv/mm` changes are three files and mostly cosmetic**: a
  `dev_assign_dma_coherent()` rename, a `new_vmalloc[]` array that becomes
  64-bit-only (a few bytes on RV32), and a swiotlb refinement. **We allocate no
  swiotlb at all**, so the last one buys nothing here.

### The one real difference: PAGE_BLOCK_MAX_ORDER

`CONFIG_PAGE_BLOCK_MAX_ORDER` exists in **7.1 and 7.2 but not 6.12**. It sets
the pageblock order independently of `MAX_PAGE_ORDER`, and
`CMA_MIN_ALIGNMENT_PAGES` is `pageblock_nr_pages` - so it controls the
granularity of the CMA pool.

This is exactly what the framebuffer node in `esp32s31.dtsi` says is
impossible: "Shrinking it is also not possible without patching
ARCH_FORCE_MAX_ORDER into arch/riscv/Kconfig, which does not define it." That
remains true (no riscv version defines it), but `PAGE_BLOCK_MAX_ORDER` reaches
the same result from the other side. With `=8` the granularity drops from 4 MiB
to 1 MiB and a 2 MiB pool is accepted, where 3 MiB was previously rejected
outright.

**And it is worth almost nothing.** Measured, fresh boot and 25 s settle per arm:

	                4 MiB CMA    2 MiB CMA
	MemAvailable      5684 kB      5820 kB    +136 kB
	MemFree           2680 kB      2744 kB
	CmaTotal/CmaFree  4096/404     2048/640
	Slab              4380 kB      4232 kB

The boot line makes it look like +2 MB ("13068K/16384K available" against
"11020K"), and that is misleading: the pool is `reusable`, so the kernel already
lends free CMA to movable allocations. `CmaFree` was only 404 kB of 4096 - the
other 3.6 MB was in use as page cache, and counted toward `MemAvailable` all
along. Shrinking the pool moves memory between buckets rather than creating it.

**Kept at 4 MiB.** The 136 kB does not justify losing the headroom the desktop
would need if it ever comes back, and the DTS records why 4 MiB was chosen.

### Where the memory actually is

	MemTotal   15440 kB
	Slab        4232 kB   <-- all SUnreclaim, SReclaimable = 0
	KernelStack  328 kB
	PageTables   296 kB
	VmallocUsed  400 kB

**Slab is the target, and it is version-independent.** `SLUB_TINY` is already
enabled, so the obvious lever is spent. Naming the consumers needs
`CONFIG_SLUB_DEBUG` for `/proc/slabinfo`, which costs memory itself - a
diagnostic build, not a shipping one.

The genuine 6.12 -> 7.1 memory gain (+1116 kB MemAvailable, measured with the
same script at the same point) comes from the kernel's own allocator and
memory-management work, not from anything configurable.


## 7.3: actively worse for this board

7.3 is in the merge window now. Checked against mainline since v7.2:

- **`riscv: further remove XIP`** (Jisheng Zhang, 2026-08-07, 4 files -148/+3)
  is *in* 7.3. It deletes `vmlinux-xip.lds.S` and the remaining scaffolding that
  7.2 still carried orphaned. **Our revert would become two commits instead of
  one.**
- **The riscv content is server-class**: Ssqosid (QoS), Ssccfg/Smcdeleg (counter
  delegation), Smcsrind/Sscsrind (indirect CSR access), plus cpufeature/hwprobe
  entries for Ziccamoa, Ziccif, Ziccrse, Za64rs, Zicclsm. None of it means
  anything on a 15.4 MB microcontroller.
- **The mm content is routine**: page-allocator refactors, lockdep annotations,
  uffd-wp batching, memory hotplug. Nothing aimed at footprint.
- `riscv: Standardize extension capitalization` touches nine files including
  `cpufeature.c`, so it will likely conflict with our F-without-D patch.

**The trend is the point.** XIP went in 7.1 and its remains go in 7.3; each
release makes the downstream revert larger, while offering this board nothing.
The cost curve rises monotonically and the benefit curve is flat.

## Where to rest

	6.18.46 LTS   works, maintained into 2027+   <- longevity
	7.1.10        works, newest usable           <- current board state
	7.2           builds, silent before console
	7.3           adds cost, no benefit

7.1 is a stable series, not LTS, so it will EOL when 7.2/7.3 mature. 6.18 is the
LTS and the sensible resting place if the board is to be left alone.

**Note: the 6.18 tree does not yet carry the F-without-D fix.** It was found
while working on 7.1, so a 6.18 image built today still has the FPU disabled and
will SIGILL on any floating-point userspace. Apply
`scripts/apply-s31-fixes-7x.py` to that tree before using it.


## Real-workload benchmark: 7.1.10 against 6.12

Same microSD, same rootfs, same binaries, same corpus (4 MB and 16 MB built from
real `/usr/bin` content). Only the kernel differs. Caches dropped before every
timed run; three iterations per arm; fresh boot per arm.

	task (seconds, lower better)   6.12              7.1.10
	gzip 4 MB                      18.71 21.85 19.74  20.73 18.47 18.82
	sha256sum 16 MB                 5.31  5.36  5.22  11.91 12.19  5.40
	grep 16 MB                     39.72 39.37 54.49  72.74 64.63 52.70
	tar /usr                       21.01 20.83 20.83  43.96 40.21 22.54
	find / -xdev                    6.30  6.26  6.18   7.96  6.96  6.74

	MemAvailable at settle          4836 kB           5788 kB   +952 kB (+20%)
	raw SD read (16 MB, dd)        12.6 MB/s          2.35 MB/s  5.4x SLOWER

### What it says

- **CPU-bound work is at parity.** `gzip` of a cached 4 MB file is ~19-21 s on
  both, consistent with the CoreMark result (+0.5%). There is no CPU win in 7.1
  and there is no CPU loss.
- **Memory is a real win: +952 kB MemAvailable (+20%)**, matching the earlier
  +1116 kB measurement. On this board that is the gain worth having.
- **Storage is a serious regression: 7.1 reads the card at 2.35 MB/s against
  6.12's 12.6 MB/s.** Everything that touches the SD inherits it - `sha256sum`,
  `grep` and `tar` are all roughly 2x slower, and the variance is much worse
  (`tar` ranges 22.5-44.0 s where 6.12 sits at 20.8-21.0 s).

### It is not what it looks like

Three plausible causes were checked and eliminated:

- **Not retried errors.** `data error` count was 3 before the workload and 3
  after; the errors happen at init, not under load.
- **Not bus negotiation.** Both kernels end up identical: 40 MHz, 4-bit,
  `sd high-speed`, "new high speed SDXC card". An earlier 20 MHz reading came
  from a pre-fix boot and was misleading.
- **Not optional.** Removing the per-descriptor cache invalidate makes the board
  fail to boot at all, so the invalidate is load-bearing, not overhead that can
  simply be deleted.

The cost is in the descriptor coherency path that had to be **rewritten** for
7.1 - upstream replaced the open-coded OWN-bit loop with
`readl_poll_timeout_atomic()`, which cannot invalidate between reads, so the
loop was written back by hand. 6.12 carries the same logic as an applied patch
hunk and is fast; the hand-written 7.1 version is correct but expensive. That
is where to look next, not at the bus or the card.

**Verdict: on current evidence 6.12 is the better performer for anything that
touches storage, and 7.1 is better only on memory headroom.** 7.1 should not be
treated as a straight upgrade until the SD path is back to ~12 MB/s.


## The SD regression on 7.1: fixed

**Symptom.** 7.1 read the card at 2.35 MB/s against 6.12's 12.6 MB/s, took one
interrupt per 4 KB descriptor instead of ~3 per request, and - once the card
happened to come up in the wrong state - flooded the console with "interrupt
status did not quiesce" and never mounted the rootfs at all.

**Cause.** The port dropped one line from `dw_mci_interrupt_once()`:

	 	u32 pending;
	 
	-	pending = mci_readl(host, MINTSTS); /* read-only mask reg */
	-
	 	if (pending) {

`pending` was left **uninitialised**, so the handler dispatched on whatever was
on the stack. Every branch - clear the error, clear DATA_OVER, clear CMD_DONE -
is guarded by a bit test against that garbage, so the real status bits were
usually never written back. The interrupt therefore re-asserted immediately,
which is both the per-descriptor interrupt storm and the quiesce flood; and
because the bottom half that resets the controller after an error is starved by
that storm, a card that hit a command timeout during init could never recover.

The non-determinism was the tell and should have been read as one much earlier:
identical images alternately booted, hung at the first command, or hung after
enumeration, because an uninitialised stack slot is not the same twice.

**GCC had been reporting it the whole time:**

	drivers/mmc/host/dw_mmc.c:3139:12: warning: 'pending' is used uninitialized

It was missed because the build log was being grepped for `error:` only. Grep
for `warning:` too - see the note in `CLAUDE.md`.

**Result**, three 16 MB reads per arm, cache dropped between each, fresh boot per
arm, same script both sides:

	                mean       throughput   interrupts / 48 MB
	 6.12        1.267 s        12.6 MB/s          793
	 7.1 before       -          2.35 MB/s      ~11,000
	 7.1 after   1.540 s        10.4 MB/s          721

The 6.12 arm reproduces its historical 12.6 MB/s exactly, which validates the
method. Interrupt count is now marginally *better* than 6.12, so the remaining
**~18% gap is not interrupt overhead** and is something else in 7.1 - worth a
look if storage throughput ever becomes the binding constraint, but it is no
longer a regression of a different order.

### Four hypotheses tested and eliminated before finding it

Recorded because each cost a build/flash/measure cycle and none should be
re-tried:

1. **Chain bit.** 7.1 clears `IDMAC_DES0_CH` on the last descriptor where the
   BSP keeps it. Restoring it changed nothing (1.62 MB/s).
2. **PIO fallback.** `err_own_bit` was made visible; it fires **zero** times.
3. **The descriptor code.** 6.12's `dw_mci_prepare_desc64/32` were transplanted
   verbatim with a dispatcher. Still 2.22 MB/s. **Kept anyway**, since it
   replaces a hand-written OWN-bit loop with proven code.
4. **DMA coherency.** The theory was that `dma_sync_single_for_device()` was a
   no-op leaving the engine on stale descriptors. Instrumented directly:
   `dma_coherent=0 noncoherent_quirk=1`, so the syncs are real. Dead.

A fifth change - gating the poll timer's raw `RINTSTS` RXDR/TXDR test on
`host->sg` - was written, tested and **reverted**. The reasoning was sound (those
bits are masked during IDMAC transfers and only the PIO path ever writes them
back, so the condition is permanently true during DMA) but it was a guess at a
symptom, and with `pending` fixed the poll timer is no longer hot. Left alone
deliberately; revisit only with a measurement.

## Does 7.1's swap rework buy us anything? Measured: no

7.1 carries the swap-table rework (`mm/swap_table.h` exists; 6.12 has no such
file), which replaces the flat `swap_map` byte array and the separate swap
cgroup array with per-cluster tables allocated on demand. It is real and we get
it for free. It is also, here, worth nothing measurable, for two reasons.

**We barely swap.** In text mode the board runs with a 64 MB SD swapfile
(`ZRAM_MB=0`, `SD_SWAP=1`) of which **36 kB is in use**, against 5.8 MB
MemAvailable. There is no swap traffic for a faster swap path to speed up. The
rework's wins - cheaper swap-cache lookup, less lock contention - are aimed at
many-core servers under swap pressure, which is the opposite of this board.

**The static saving is inside the noise.** Attaching and detaching the swapfile
three times per kernel, reading MemFree either side:

	          attach cost (kB)
	 6.12       24, 56, 24
	 7.1        16, 40, 24

The ranges overlap; there is no signal. That is unsurprising in hindsight -
`swap_map` for 64 MB of swap is 16 kB, so the most the old scheme could have
been wasting was tens of kilobytes.

**Where it would matter** is a desktop under memory pressure, which is the load
that made swap interesting in the first place and which is currently set aside.
If X comes back, re-measure then rather than assuming this result carries over -
and note that the earlier zram decision flipped once the thing it was
compensating for was fixed, so neither result is permanent.

## Why 7.1 was slower, and what fixed it

The regression was not where it looked. Measured, fresh boot per arm:

	                        6.12      7.1 before    7.1 after
	 read()+write() pair   15.2 us      43.0 us       9.0 us
	 bare X repaint        43.0 ms      47.0 ms      49.7 ms
	 loaded X repaint      ~95 ms      ~375 ms      ~290 ms
	 SD 4k O_DIRECT       3.4-3.8 ms    4.6 ms        4.6 ms
	 SD sequential        12.6 MB/s    2.35 MB/s    10.4 MB/s

**The finding: syscalls cost 2.83x more on 7.1.** riscv is converted to the
generic entry framework in 7.x, so the whole syscall entry/exit path is
`__always_inline` C from `kernel/entry/` compiled into `do_trap_ecall_u`, where
6.12 did it in `entry.S` assembly. That code sat in flash `.text` and is
refetched on every syscall, interrupt and page fault - which is exactly the case
the 5.98x flash-vs-RAM penalty applies to. A bare X repaint is mostly userspace
pixel work and barely noticed; a thrashing desktop is nothing but syscalls,
faults and interrupts.

**And `.text.fast` had been silently broken on 7.x the whole time.** From 7.x
`TEXT_MAIN` is unconditionally

	.text .text.[_0-9A-Za-df-rt-z]* ...

which matches `.text.fast` (`f` is in `f-r`). The flash `.text` output section is
emitted first, so it claimed every `__fasttext` function from any object not
named in `S31_FAST_OBJS`, and the attribute became a no-op. 6.12 defined
`TEXT_MAIN` as plain `.text` without LTO, so it worked there. The section is now
`.text..fast` - the double dot is the kernel's own convention for this
(`.data..percpu`, `.bss..page_aligned`) and no MAIN glob matches it.

Result: **9.0 us per syscall pair, 4.7x better than 7.1 was and 1.67x better
than 6.12 ever managed.**

### What that did and did not buy

The loaded desktop went from ~375 ms to ~290 ms per repaint - real, about 25%,
but **not** the 4x. It remains ~3x worse than 6.12 and that is unexplained.
Since syscalls are now faster than 6.12, whatever is left is elsewhere. Do not
re-litigate the syscall path.

Note the noise: loaded-desktop medians across the last three builds ran
258/241/242, 293/271/274 and 312/316/276. Those builds are **not** separable at
that spread; only the move from ~375 is outside it.

### Tested and rejected, with numbers

- **Preemption model.** 6.12 is `PREEMPT_NONE`; 7.1 defaults to `PREEMPT_LAZY`,
  which selects `UNINLINE_SPIN_UNLOCK` and turns every `spin_unlock` into an
  out-of-line call into flash. Plausible, and wrong: forcing `PREEMPT_NONE`
  (which needs a Kconfig patch, since upstream now gates it on
  `ARCH_NO_PREEMPT`) gave loaded medians of 399/412/265 against 366/386/372.
  **Kept anyway**, because it removes a config difference from the kernel being
  compared against, but it bought nothing measurable.
- **Userspace XIP.** Suspected broken on 7.1 because Xorg's text mapping showed
  452 kB resident. 6.12 shows 332 kB in the same state - same mechanism, not the
  differentiator.
- **Swap.** 7.1's pure swap-in is *faster* (10.9-11.6 MB/s against 5.0-5.6).
- **SD.** Real but small: 13-31% per request, not 4x. An earlier reading of 75
  and 302 ms/req was a cold-start artefact that five repeats did not reproduce.
- **MGLRU, THP, memcg, zswap.** None built in either kernel.
- **Debug/hardening config.** No KASAN, LOCKDEP, FORTIFY or HARDENED_USERCOPY
  difference.

## Repaint is not slower on 7.1 - it is quantised to the panel

An earlier note here claimed 7.1 repainted more slowly than 6.12 (43.0 against
49.7 ms). **That was wrong**, and worth recording as a measurement error rather
than quietly deleting: the two figures came from two different scripts run on
single boots. With one script across three boots each:

	 6.12   47.3  43.7  47.1  46.8  47.1  43.5     median 46.95
	 7.1    42.0  38.9  48.1  49.6  47.0  47.0     median 47.0

Identical central tendency. 7.1 has the wider spread, and any single pair of
boots can be made to show either kernel winning.

**What actually sets the number.** The panel runs at 42 Hz, a frame period of
23.725 ms, and the driver's own gap histogram shows where the time goes:

	gap_frames=0,0,61,15,1,3   frame_period_ns=23725333

61 updates took exactly two frame periods and **none took one**.
2 x 23.725 = 47.45 ms, which is the median on both kernels. A full-screen xfill
repaint is ~42 ms of real work against a 23.7 ms frame, so waiting for vblank
always rounds it up to two whole frames.

The driver is not the cost. Splitting one run on 7.1: `upd_ns` is 9.5 ms per
update (~20% of the repaint), of which the PPA is 0.66 ms and cache flushes
0.30 ms. The rest is X's software fill - `AccelMethod none` with a shadow
framebuffer, which is the documented ceiling. Proof that driver time is not
what is being measured: one boot chose the CPU path instead of the PPA and
`upd_ns` collapsed from 9.7 ms to 0.45 ms per update **with no change at all to
the repaint median** (47.0 against 48.1).

### The fix, and what it costs

`wait_vblank` now defaults to false. Alternating the toggle within one boot,
which controls for the boot-to-boot spread above:

	 wait_vblank=Y   49.2  49.9  49.7 ms    max 77-103
	 wait_vblank=N   42.6  42.4  41.0 ms    max 54-76

15% off the median and a third off the tail, and it carries into the loaded
desktop: 238.5/241.4/239.1 ms against 275-315 before.

The cost is a possible tear on full-screen primary-plane updates, since the copy
can now race scanout. There is no client buffer being recycled underneath -
scanout is free-running cyclic DMA into a buffer the driver copies into - so the
exposure is a seam, not corruption, and the panel was checked at 640x384. It is
a runtime toggle:

	echo Y > /sys/module/esp32s31_lcd/parameters/wait_vblank

### Raising the panel refresh: considered, not done

60 Hz would need pclk 25.6 MHz against the present 18 MHz (Espressif's own
value for this ST7262E43). It is not obviously a win and was not attempted
blind:

- It would help the **common** case - typical desktop damage is ~69 KB, ~6 ms of
  work, so those repaints are pure one-frame waits and would go 23.7 -> 16.7 ms.
- It would **hurt** full-screen updates: 42 ms of work against a 16.7 ms frame
  is three frames, 50 ms, worse than the 47.45 it costs now.
- It raises scanout DMA from 32.3 to 46 MB/s of continuous PSRAM reads on a
  board where memory bandwidth, not CPU, is the binding constraint.

Worth measuring if small-update latency ever becomes the complaint; the arms are
`.clock` in `espressif_sub3_mode` and a `make linux`.

## The "loaded desktop is 3x slower on 7.1" result was an artefact

It was not real, and the retraction is more useful than the number was.

The loaded benchmark launched **two `st` terminals**, and `st` busy-redraws for
about 25-30 seconds after it starts - measured on one instance, per 10 s window:

	st      803  786  429  0  0  0    jiffies (~80%, 79%, 43% of the core)
	xterm   177    0    0             idle almost immediately

So the benchmark was timing *how far through st's spin-down the run happened to
land*. Tracking both together makes it unambiguous - repaint median follows st's
CPU exactly, on both kernels:

	           7.1                        6.12
	 r1    354.9 ms / 3903 jiffies     128.3 ms / 3435
	 r2    243.8 ms / 1416             87.5 ms /  876
	 r4    143.8 ms /  979              4.3 ms /    0
	 r6     10.1 ms /    0              4.1 ms /    0

Both collapse to single-digit milliseconds once the terminals stop. 6.12 got
there in four rounds and 7.1 in six, which is the entire reason 6.12 "looked"
three times faster.

**The desktop does not use st.** `/etc/system.jwmrc` launches `xterm`. The
spinner was introduced by the measurement harness, not by the system under test.

### The real numbers, with clients that behave

jwm + 2 xterm + xcalc, settled, three runs:

	 6.12    19.7  22.8  19.1 ms
	 7.1     18.6  15.9  15.6 ms

And with non-spinning clients but no window manager occlusion (jwm + xcalc):

	 6.12    47.8  47.7 ms
	 7.1     34.4  33.2 ms

**7.1 is faster in both.** There is no loaded-repaint regression to fix.

### What to take from this

- A client that spins invalidates any whole-desktop measurement, and the spin
  can be transient - so a single arm measured shortly after launch is worthless.
  Sample per-process `utime+stime` alongside the result and check it is flat
  before believing anything.
- Scheduler tuning was attempted against this (`RUN_TO_PARITY`,
  `base_slice_ns`, `DELAY_DEQUEUE`) and appeared to give 31%. It was noise on
  top of the spin-down; the "improvement" reversed when the default was
  re-measured last. `SCHED_DEBUG` was enabled for that and has been reverted.
