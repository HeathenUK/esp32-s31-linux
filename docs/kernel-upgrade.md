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
