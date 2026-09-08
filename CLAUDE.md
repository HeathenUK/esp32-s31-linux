# Working on this board

Linux 7.1.10 on an ESP32-S31-Korvo-1 V1.1 (the tree is `linux-71-port/`, and
`make linux` builds THAT, not the `linux-esp32-s31` submodule): dual hart
(hart0 runs ESP-IDF and owns
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
    scripts/board/screenshot-hw.py <out.jpg>    # same, via the JPEG codec, ~7 ms

On the board itself, for recording and for driving the desktop:

    mjpegrec <out.mjpeg> <secs> [fps] [q] [kb]  # record video, ~2.4% CPU
    uinject <demo|drag|type|park|keytest>       # inject input and nothing else
    jpegcap <fps> <secs>                        # pace encodes without forking
    keylog                                      # every evdev key, per device

See `scripts/board/README.md`. **Do not film the desktop by grabbing frames one
at a time over the console** - `mjpegrec` records into kernel RAM on damage and
is drained afterwards, so it costs ~2.4% while the screen is changing and ~1.7%
idle. A timer-driven loop cost 15% and the naive version cost 42%, which is
enough to change whatever you were trying to measure.

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

## The board is almost never wedged. You are holding the console.

Three failures look identical - every tool reports `NO_SHELL` or `STAGE
SILENT` - and only one of them is the board's fault. Before diagnosing
hardware, rule out the first two, because on 2026-09-07 they accounted for
most of a day's "wedges":

1. **Another tool has the port.** Two readers on one tty steal each other's
   bytes, so a probe run while a background job is driving the board CANNOT
   succeed. `console.py` now takes an **advisory flock** (`take_port_lock`)
   and every tool goes through `console.open_port()`, so this reports
   `SERIAL PORT BUSY - held by pid N: <what>` and says "This is NOT a dead
   board". **If you see that, stop the holder - do not reset anything.**
   Never run a board tool while a background measurement is in flight.
2. **Your script outlived its runsh window.** runsh gives up, the board's
   login shell keeps running the script, and the console stays occupied.
   `runsh.py` now installs a **board-side watchdog** that kills the script at
   `timeout + 10 s` and prints `RS_TIMEKILL`, which runsh turns into "THE
   BOARD KILLED THIS SCRIPT - the board is FINE; its effects are
   HALF-APPLIED." A 1.45 MB copy and a `find /` over the SD card both did
   this. **Anything slow goes `setsid` with output to a file on the card**,
   and you collect the file afterwards - never inline in a runsh script.
3. **The board really died.** Only now is this worth believing. Record it:
   `conlog.py <out.log> <secs>` listens read-only and prints `ALARM` on
   panic/BUG/hung-task/OOM. Fire the workload with runsh FIRST (setsid, output
   to a file), let runsh exit, then record - conlog holds the port too.
   **`dmesg` is useless here by construction**: you can only read it from a
   board that is still alive, so the one failure worth diagnosing is the one
   that erases its own evidence.

**Do not re-derive the screenshot path.** The scanout address is allocated, not
fixed, and `/dev/fb0` is fbdev emulation rather than what Xorg actually paints.
`screenshot.py` handles both - and the **geometry is not always 800x480**. If
the CMA pool is exhausted by client buffers the driver logs `no scanout buffer
... scaling off` and scans out the client plane directly at 640x384 / 491,520
bytes. Assuming 800x480 then decodes 1280-byte rows as 1600 and reads 276,480
bytes past the end, producing a tiled, sheared image with RGB noise across the
bottom third that looks like a dead panel. The debugfs `size=` field does not
help - it still reports the native 768,000 in that mode. Take the geometry from
the last `scanout started` line, and the address from debugfs `scanout=`, not
from dmesg (which records it at mode-set time and goes stale).

There is a correct tool for each of these and improvising has repeatedly wasted
whole afternoons:

- **Build**: `./docker/build.sh 'cd /src && $S31_MAKE <target>'`. It defaults to
  the host architecture (arm64 here); an emulated amd64 build is ~10x slower.
  **Never run two builds against the same output volume** - the tree ends up
  unbuildable and the only honest fix is a full rebuild.
- **Vendor sources**: the S31-capable ESP-IDF is **`/opt/esp-idf` INSIDE the
  build container**, v6.1-dev, and is the only tree here with an `esp32s31` soc
  target - read it with `./docker/build.sh 'ls /opt/esp-idf/components/soc/esp32s31'`.
  **`~/esp/esp-idf` on the host is v5.5 and has no esp32s31 directory at all.**
  Its nearest sibling is the P4, which shares the PPA, LCD_CAM and dwc2 blocks,
  so a P4 header reads like a real S31 answer and is not one. An
  `idf6.0_py3.12_env` under `~/.espressif` is a tool env, not a source tree.
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
- **Reverting source is not undo if the change wrote flash.** NVS, retention
  registers and the card all survive `git checkout` and a reflash, and the stock
  code then keeps acting on what you stored. An early-association flag left in
  NVS tore the LCD splash and cost hours, because a byte-identical rebuild
  changed nothing. Clear the store: `esptool erase-region 0x11000 0xF000` is the
  `nvs` partition. Record any flash state you write - that note is the only undo.
- **The loader app is `hello_world.bin` at 0x20000**, not `bootloader.bin`.
  Those `boot:` log lines come from it. Repartitioning means reflashing all
  three.
- **OBSOLETE since 2026-09-02 - profiling now fits with the radios.** With
  debugfs compiled out (`DIAG=0`, the default) and 256 KB moved from the
  linux partition to rootfs, `make linux PROF=1` builds a kernel with
  `CONFIG_PROFILING`, Wi-Fi, Bluetooth and sound at 5,968,137 of 6,160,384
  bytes, and appends `profile=6` to `CONFIG_CMDLINE` (the command line is
  `CMDLINE_FORCE`, not the DTS). Capture with `echo > /proc/profile`, the
  workload, then `gzip -c /proc/profile | base64` to the host and
  `scripts/board/resolve-profile.py System.map dump.b64`. Only kernel-mode
  ticks are sampled, and idle shows up as `cpu_idle_poll` only because it
  shares a 64-byte bucket with `default_idle_call` (WFI) - the idle loop does
  not poll. Resolution is the bucket, not the symbol. The paragraph below is kept for the history of the trade.
- **The kernel does not fit with both profiling and the radios.** (historical) The linux
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
- **RESOLVED in kind, 2026-08-31: the whine is COIL WHINE from the board, not
  the speaker.** Ear-tested: disabling the PA enable (GPIO7 low, verified
  written) changed nothing, and a finger on the speaker cone did not muffle
  it - a component on a switching supply is singing. The 2026-08-27 incident
  above was almost certainly the same misattribution; do not chase the codec
  or amplifier for a "speaker whine" again until the cone-touch test says the
  speaker is actually moving.
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
- **A flash target can silently write a stale image.** `flashfile` falls back
  to `images/<name>` when the build directory is absent - and it usually is,
  because `$(BUILD_DIR)` is a Docker volume. So a build that succeeded inside
  the container and was never copied out gets flashed as whatever `images/`
  still holds; esptool writes it, **verifies the hash, and reports success**
  while putting old content on the board. Run **`make sync-images`** after any
  containerised image build, and read the `--- flashing <path> / built <date>`
  line the flash targets now print. Two full flashes were wasted on 2026-09-05
  before the board's library sizes gave it away.
- **Stopping `bluetoothd` can wedge the board, and a stock daemon spinning in
  `strlen` usually means corrupt *data*, not corrupt code.** BlueZ serialises
  its device store on SIGTERM, so if a record in `/var/lib/bluetooth` has gone
  bad the graceful stop is the most expensive thing you can ask for - one
  3 MB `info` file made `S46bluetoothd stop` OOM the board three times running.
  The init script now quarantines any store file over 64 kB before start and
  bounds its stop wait at 5 s. **Never `cat` a suspect store file**: 3 MB on one
  line is a 45 s console flood, and awk/sed buffer the whole line. Use
  `head -6` and `tail -c 160`. Full account in `docs/current-state.md`.
- **A backup in `/etc/init.d` is executed.** busybox `rcS` globs
  `/etc/init.d/S??*`, so `S40lvdesk.bak` runs alongside `S40lvdesk` - two
  desktops, the second failing to take DRM master, and a board that sat silent
  through two resets before the cause was obvious. Keep backups somewhere else.
- **`dev_info()` in a per-frame path costs ~1 ms a frame.** It writes to a
  1 Mbps serial console synchronously. Logging once per encoded frame was the
  single largest cost in the capture path - 15% of the CPU down to 5.5% when
  removed - and it floods the ring buffer badly enough to scroll away the
  `scanout started` line that tooling parses panel geometry from. `dev_dbg` for
  anything that happens per frame.
- **busybox applets fork, so a shell loop is not a cheap harness.** Pacing with
  `usleep` cost more per iteration than a hardware JPEG encode did, and was
  charged to the driver until the harness was measured on its own. Write the
  pacer in C (`rootfs/jpegcap.c`).
- **Buildroot ignores unknown defconfig symbols.** Always grep the generated
  `.config` to confirm a package is actually enabled.
- **`/usr/bin`, `/usr/lib` and `/lib` are read-only overlays** stacking two
  cramfs XIP images over the ext4 root, with **no upperdir**. Nothing can be
  written to them at runtime, so installing anything fails with "Read-only file
  system". Write to the ext4 underneath via a non-recursive `mount --bind /`
  (`/usr/sbin/s31-opkg` does this) and reboot for the overlay to restack. This
  is what makes userspace cost zero RSS - it is the feature, not a defect.
- **Never base64 a large file over the serial console.** At ~65 KB/s a 52 MB
  recording is 13 minutes during which the board is unusable and every other
  tool reports NO_SHELL; a caller that retries starts another. Use
  `s31-record serve` on the board and `curl` from the host - 915 KB/s measured.
  Reading small things back over the console is fine and fast.
- **Ask `scripts/board/alive.py` whether the board is alive**, not `runsh.py`.
  NO_SHELL means "no prompt seen", which is equally true of a healthy board
  sitting at a login prompt and a dead one. alive.py reports a stage and pokes
  the console. A warm reboot takes ~85 s; polling before that is not evidence.
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
  is slab. X wants ~4.7 MB and each client costs the server another 0.4-2 MB.
  Clients page out under a full desktop, and that is what makes clicks and app
  launches slow.
- **The slab is NOT a lever - do not propose reclaiming it.** Bottomed out
  2026-09-04. `SReclaimable: 0` is an artefact of `CONFIG_SLUB_TINY`, which
  omits the reclaimable accounting; without it the same board reports 900 kB of
  5,192 reclaimable, so it was never "all unreclaimable". The breakdown has no
  large item: the biggest cache is the device model itself (`kernfs_node`,
  838 kB in 9,752 nodes), then ~1.9 MB spread across generic `kmalloc-*` that
  cannot be attributed without tracing this kernel does not have. 300-500 kB is
  recoverable at best, only by deleting functionality. `/proc/slabinfo` needs
  `CONFIG_SLUB_DEBUG`, which depends on `!SLUB_TINY`, so `make linux SLABDIAG=1`
  is the only way to look - and its total is not the shipping total. Full
  reasoning in `docs/current-state.md`.
- **`read()` is capped at ~13.6 MB/s by PSRAM copy bandwidth**, not by storage -
  `copy_to_user` costs 2.4x the SD read itself, and readahead, `max_sectors_kb`,
  the I/O scheduler and concurrency are all measured inert. `mmap()` does not
  pay it. The SD per-request cost is `2.49 ms + size/48 MB/s` with a flat
  ~2.0 ms hardware floor below 16 KiB, so **request size is the only lever**:
  6 MB as 4 KiB faults is 3.4 s, as 256 KiB reads it is 0.18 s.
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
