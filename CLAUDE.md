# Working on this board

Linux 6.12 on an ESP32-S31-Korvo-1 V1.1: dual hart (hart0 runs ESP-IDF and owns
the radios and audio, hart1 runs Linux), **15.4 MB of usable RAM**, kernel
executing XIP from 80 MHz flash, rootfs on microSD.

**It is not a soft-float machine.** The kernel reports
`rv32imafc_zicntr_zicsr_zifencei_zca_zcf_zba_zbb_zbc_zbs`: `f` is a real
single-precision FPU and `zcf` its compressed loads and stores, with
`CONFIG_FPU=y` so the kernel saves and restores that state. What *is*
soft-float is the **ABI** - `-mabi=ilp32` passes floats in integer registers
while still computing them in F registers. There is no `d`, so
**`float` is hardware and fast, `double` is a library call and slow**. Do not
reach for `double` here, and do not describe the board as soft-float.

Every rule below cost hours to learn. Read `docs/current-state.md` first after a
context reset - it records what is true now and what has already failed.

## Never hand-roll the tooling

**The scripts already exist, in `scripts/board/`. Use them.** Every one of them
has been re-written from scratch mid-task at least twice, badly, while the
original sat a directory away:

    scripts/board/reset.py                      # reset the board
    scripts/board/runsh.py  <script.sh> [t] [w] # run a shell script on it
    scripts/board/deploy_bin.py <f.b64> <dest>  # ship a binary to it
    scripts/board/screenshot.py <out.png>       # capture the real panel

**Do not write a DTR/RTS reset sequence.** Not once, not "just quickly". DTR
drives EN and RTS drives IO0, both inverted, and any sequence that fails to
drive EN low leaves the reset dependent on the pins' prior state - sometimes it
resets, sometimes it does nothing, sometimes it parks the board in download
mode where the ROM emits a few bytes and then goes silent forever. That makes
"no output" mean nothing at all, and it has already poisoned four consecutive
diagnoses. `scripts/board/reset.py` calls esptool, which does it correctly.

**Do not write another serial-console runner.** `runsh.py` ships the script as
a file (flattening it into `; ` one-liners breaks every multi-line construct
and yields empty output that looks like a hardware fault), and it handles both
ways the console lies:

- *Kernel messages on the login line.* The LCD driver prints mode-set messages
  exactly when getty shows its prompt, so the prompt is not last in the buffer.
  It searches the whole buffer. (`rstrip().endswith('# ')` can never be true -
  rstrip removes the trailing space it then tests for.)
- *Racing the boot.* Reaching a prompt from a hard reset takes ~50 s here,
  because X starts on the way. It now watches for `login:`/the prompt and
  returns the moment either appears, so a booted board answers in <1 s and a
  booting one is tracked, not slept through.

**`NO_SHELL` now means something.** The message says whether the board was
emitting bytes (alive, still booting) or silent (off, held in reset, or in
download mode). Read it instead of re-running blind.

**Do not re-derive the screenshot path.** The scanout address is allocated, not
fixed, and `/dev/fb0` is fbdev emulation rather than what Xorg actually paints.
`screenshot.py` handles both.

There is a correct tool for each of these and improvising has repeatedly wasted
whole afternoons:

- **Build**: `./docker/build.sh 'cd /src && $S31_MAKE <target>'`. It defaults to
  the host architecture (arm64 here); an emulated amd64 build is ~10x slower.
  **Never run two builds against the same output volume** - the tree ends up
  unbuildable and the only honest fix is a full rebuild.
- **Flash**: IDF's esptool **5.3.1** at `-b 2000000` on `/dev/cu.usbserial-130`
  (`~/.espressif/python_env/idf6.0_py3.12_env/bin/esptool`). Homebrew's 5.2.0
  has no S31 stub and fails in confusing ways. Build output lives in a Docker
  volume - copy it to `images/` first.
- **Reset / run / deploy / screenshot**: `scripts/board/`, as above. Not a
  scratchpad copy, not a fresh one written inline.
- **Write the microSD**: the **SD imager**, `docs/sd-imager.md`. The card is
  soldered down, so it is written in place over the console:

      make imager && make flash-imager
      imager/send_image.py build/buildroot/images/rootfs.ext2 --port /dev/cu.usbserial-130
      make flash-linux flash-xip-rootfs        # BOTH - see below

  Do **not** dribble files onto the card over the serial console instead. The
  imager exists precisely so that does not happen, and once `wlan0` is up
  `wget` from a host HTTP server is faster still (191 KB/s measured, against
  minutes for the same file in printf chunks).

  The imager kernel is **larger than the linux partition and overruns into
  rootfs by design** - that is expected and fine, because it is transient. It
  does mean the restore is **two** commands: flashing only the kernel back
  leaves a corrupt userspace XIP image, and the symptom is cramfs failing to
  mount, which points nowhere near the cause.
- **Timeouts**: size them to the work (flash ~40 s, `make linux` ~200 s, a board
  script ~60 s). A long default looks like progress while nothing happens.

Pick the **narrowest** build target that reaches the goal. `make linux` alone
for kernel work; the rootfs only needs rebuilding when userspace changes.

## Things that silently do the wrong thing

- **The Makefile overrides the kernel defconfig.** It runs `kconfig-tweak
  --disable PROFILING`, `--disable IPV6` and others after applying the
  defconfig. Setting those in the defconfig does nothing.
- **Partition geometry lives in THREE places** that must agree:
  `bootloader/partitions.csv`, the `*_PARTITION_SIZE` vars in `Makefile`, and
  constants compiled into `bootloader/main/main.c` (**twice**, ~line 71 and
  ~line 346). The loader validates and resets ~350 ms in when they disagree,
  which reads as the loader dying during PSRAM init.
- **The loader app is `hello_world.bin` at 0x20000**, not `bootloader.bin`.
  Those `boot:` log lines come from it. Repartitioning means reflashing all
  three.
- **The kernel does not fit with both profiling and the radios.** The linux
  partition is 5,636,096 bytes and there is very little slack. In-core profiling
  (`CONFIG_PROFILING`, which *selects* `PERF_EVENTS`) and Wi-Fi + Bluetooth +
  sound are mutually exclusive: the three features are ~500 KB, the partition
  has ~53 KB spare. **This is a deliberate, reversible trade, not a bug** -
  radios and sound get compiled out to make room whenever X is being profiled,
  because they are not needed to profile X. Put them back afterwards.
  - `--disable PROFILING` alone does **not** clear `PERF_EVENTS`. PROFILING
    *selects* it, and clearing a selector leaves the selectee set; `olddefconfig`
    then keeps it because it is user-selectable in its own right. Disable both.
  - Every committed defconfig has `CONFIG_PROFILING=y`; the Makefile's
    `--disable PROFILING` is what actually turns it off. Do not read the
    defconfig and conclude profiling is on.
- **A speaker whine appeared while sound was compiled out, and only a hard
  power cycle cleared it.** The cause is **not established** - treat the
  following as observations, not a mechanism:
  - It began after flashing a kernel with `CONFIG_SND_SOC_ES8389` off, and the
    amplifier enable is a **GPIO hog** on GPIO7 (`esp32s31_generic.dts`) held
    high unconditionally because "output muting is the codec's job" - so the
    plausible story is an enabled amp with nothing initialising the codec.
  - **But restoring the codec driver did NOT silence it.** Nor did muting
    DACL/DACR, zeroing the ADC->DAC mixer, unbinding the driver, or driving
    GPIO7 either way. The codec was verifiably in the driver's STANDBY state
    (`0x00=0x3E`, `0x03=0x00`, `0x61=0x59`, `0x64=0x00`) with the DAC muted
    (`0x40=0x03`) *while it was still whining*.
  - **An esptool reset is not a power cycle.** It pulls the SoC's EN line only;
    the codec and amplifier have their own power domain and survive it. That is
    what makes this class of fault look software-shaped when it is not.
  - Separately and definitely true: `GPIO_ENABLE` bit 7 was **clear**, so the
    hog never programmed the pad and GPIO7 was floating, and `devmem` writes to
    `GPIO_OUT` did not reach the pin either (the IO MUX has to route the pad to
    the GPIO function first). **Any `gpio-hog` on this board may be a no-op** -
    check `/sys/kernel/debug/gpio` *and* the register, because debugfs happily
    reports a level it is not driving.

  To actually establish cause, flash a sound-less kernel and power-cycle: if it
  whines from cold, it is the config; if not, it was a latched analog state.
- **`CONFIG_BT` and `CONFIG_SND` are not in the committed defconfig**, yet the
  board has working Bluetooth and audio - they have been living as uncommitted
  working-tree edits, so a clean checkout builds a kernel with no sound. Check
  the working tree, not just HEAD, before concluding what the board runs.
- **A DTS edit needs both `make linux` and `make opensbi`** - the kernel uses a
  builtin DTB, so rebuilding only OpenSBI silently leaves the old tree in force.
- **Grep the build log for `warning:`, not just `error:`.** A dropped
  `pending = mci_readl(host, MINTSTS);` in the 7.1 dw_mmc port left the MMC
  interrupt handler branching on an uninitialised stack value - 2.35 MB/s
  instead of 12.6, console floods, and boots that hung in different places each
  time. GCC had printed `'pending' is used uninitialized` on every build for
  days. **Non-deterministic symptoms from a deterministic image mean
  uninitialised memory** - check the warnings before theorising.
- **Buildroot ignores unknown defconfig symbols.** Always grep the generated
  `.config` to confirm a package is actually enabled.
- **`/usr/bin`, `/usr/lib` and `/lib` are read-only overlays** stacking two
  cramfs XIP images over the ext4 root, with **no upperdir**. Nothing can be
  written to them at runtime, so installing anything fails with "Read-only file
  system". Write to the ext4 underneath via a non-recursive `mount --bind /`
  (`/usr/sbin/s31-opkg` does this) and reboot for the overlay to restack. This
  is what makes userspace cost zero RSS - it is the feature, not a defect.
- **The SD card holds state the repo does not.** `/etc/init.d/S40xorg` and
  friends have been edited in place. Re-imaging loses it.

## Measuring anything

The single most expensive mistake in this project has been believing a
single-run difference.

- **Establish the noise floor before believing a result.** Repeat the identical
  measurement 5+ times and report the spread. Two separate changes here looked
  like 12-18% wins and were both inside a ±22-40% band.
- **Fresh boot per arm.** Performance decays run-over-run as memory pressure
  builds, so two arms sharing a boot measure the decay.
- **Discard warm-up.** The first runs after boot can read 0.00 fps while
  everything pages in.
- **Set a root cursor** (`xsetroot -cursor_name left_ptr`) before measuring
  anything pointer-related, or the pointer damages nothing and every run reads
  zero - which looks exactly like dead input.
- **Watch the tail, not the median.** The complaint is almost always the worst
  case; medians hide it.

There is **no perf, no ftrace, no PMU** in the shipping kernel. See
`docs/cursor-latency.md` for the instruments that do work: an `LD_PRELOAD`
ioctl timer (`rootfs/ioctlprof.c`), driver-side ktime counters in debugfs, and
a diagnostic kernel with `CONFIG_PROFILING=y` for `/proc/profile`.

## Standing constraints

- **Memory is the binding constraint**, not CPU. 15.4 MB total, of which ~4.3 MB
  is unreclaimable slab. X wants ~4.7 MB and each client costs the server
  another 0.4-2 MB. Clients page out under a full desktop, and that is what
  makes clicks and app launches slow.
- **Userspace binaries belong in XIP flash** - they then cost **zero RSS**
  (`/usr/bin/Xorg` maps 1,832 kB at Rss 0). Running the same binary from SD
  measured 4x worse. Kernel code is the opposite: it already runs from flash,
  where it is ~6x slower than RAM, and `.text.fast` moves it out.
- **Off-the-shelf software only.** Do not patch Xorg, mesa, or mature kernel
  drivers. A new loadable driver of our own is fine; patching theirs is not.
- **Do not disable Bluetooth** (or other features) to buy performance.
- Prefer a **runtime toggle** (module param, config file) over rebuilding once
  per hypothesis - most of the useful comparisons here are `echo x >
  /sys/module/...`.

## Where things are written down

- `docs/current-state.md` - what is true now, and the dead ends
- `docs/cursor-latency.md` - the pointer-latency investigation and open leads
- `docs/accel-plan.md` - which engine moves pixels and why (CPU vs PPA vs GDMA)
- `etc/s31-swap.conf` on the card - swap decisions with their measurements
- `docs/hot-text-plan.md` - the XIP flash penalty and `.text.fast`

When something is measured and rejected, record it **with its numbers** next to
the code it concerns. Several ideas here have been tried twice because the first
rejection was only in a commit message.
