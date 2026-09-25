# lvdesk RAM review (2026-09-25)

Short review, by request: the few safest, cheapest cuts, with their kB and
risk. Everything here is read from the source and from
`artifacts/lvdesk-ram/board-smaps.txt`. Nothing was built or run. Symbol sizes
come from `lvdesk/lvdesk.dbg` (24 Sep build). The current `.bss` is 1,062,852 B.

## The premise, corrected

The 1.1 MB figure came from a test copy run off the SD card (`/root/lvdesk.new`),
where 548 kB of code counts as RssFile. The **shipped** `/usr/bin/lvdesk` runs
from XIP, so its text is free. Measured idle with no X clients, it has
**VmRSS 192 kB** (RssAnon 184, RssFile 8) and VmSwap 0. Where the 184 kB goes:

| kB | what |
|---:|---|
| 64 | one anonymous mmap, fully resident: musl mallocng heap groups (see cut 3) |
| 60 | `.bss` pages actually touched (the 1,062,852 B `.bss` is mostly never touched) |
| 16 | stack (VmStk is 132 kB mapped, but only 16 kB is resident) |
| 16 | libasound RELRO/data (COW relocations) |
| ~28 | libc data, vdso, heap meta, hot text copied to RAM (`hottext.c`, ~12 kB, deliberate) |

VmData is 1,128 kB, but that is address space, not RAM. The big `.bss`
reservations are:

- `work_mem_int`, 409,600 B: the LVGL TLSF pool (`LV_MEM_SIZE`). Max used is 20 kB.
- `cli`, 329,664 B: see cut 1.
- `partial_buf`, 102,400 B: untouched in direct mode.
- `res`, 88,320 B.
- `term`, 60,240 B: already lazy (A4).
- `mitems`, 23,808 B.

These cost only the pages that get written. lvdesk is already small at idle.
The real opportunity is what it keeps **after X clients have connected**.

## Applied, 2026-09-25

- **Cut 1 shipped.** `cli_in`/`cli_out` are now page-aligned static arrays,
  with pointers in `struct cli`. All six `sizeof` sites became INBUF/OUTBUF.
  `client_drop()` releases both with `MADV_DONTNEED`.
- **Measured.** After xcalc, st, prboom -window and cdoom had each come and
  gone (`x11-compat-gate.sh`, `GATE_STEPS="xcalc st prboom cdoom"`), lvdesk
  RssAnon+VmSwap was:
  - 132 + 76 = **208 kB** on the new build;
  - 300 + 68 = **368 kB** shipped.

  That is one arm each. Idle before the clients was 180 kB on both.
- **Cuts 2 and 3** are still to be checked, measurement first, as written
  below.

## Ranked cuts

### 1. Stop `memset` from touching each client slot's 80 kB of buffers, and release them on disconnect (large, safe)

**Where.** `struct cli` (`xshim.c` ~292-398) embeds `uint8_t in[INBUF]`
(`INBUF` 65,536) and `uint8_t out[16384]`, so `sizeof(struct cli)` is 82,416 B.
`MAXCLI` is 4, so `cli[]` is 329,664 B. Two things make these pages resident:

- **On accept** (`xshim_poll`, ~10640): `memset(&cli[j], 0, sizeof(cli[j]))`
  writes every page of the slot. 82,416 B spans **21 pages (84 kB)**, and they
  are dirtied the moment any client connects, whatever it sends. Nothing
  reads `in[]` beyond `c->n` or `out[]` beyond `c->outn`, so the zeroing buys
  nothing.
- **At init** (`xshim_init`, ~9002): `cli[i].fd = -1` for each slot. The slots
  are 82 kB apart, so this touches 4 separate pages, **~12-16 kB at idle**,
  to store four ints.

**Cost today.** The standard set is xcalc, st and windowed prboom. SDL 1.2
opens **two** connections, so that is four connections and all four slots.
Together that pins **4 x 84 = 336 kB** of RssAnon+VmSwap for the rest of
lvdesk's life, because `client_drop()` (~9840) never releases the slot.

There is also a latency cost. Once those pages have gone to swap (on SD), the
next connection's memset write-faults every one of them back in before
overwriting it. That is ~21 major faults x ~3.2 ms, **~67 ms added to an app
launch**, all to read back garbage.

**Change** (about 15 lines, `xshim.c` only):

1. Move the buffers out of the struct into page-aligned static arrays:
   `static uint8_t cli_in[MAXCLI][INBUF] __attribute__((aligned(4096)))`, plus
   the same for `cli_out[MAXCLI][OUTBUF]`. Keep `uint8_t *in, *out` in
   `struct cli` and set them after the memset at accept.
   `struct cli` drops to ~900 B, so all four headers share one or two pages.
2. **Replace all six `sizeof(c->in)` / `sizeof(c->out)` uses with `INBUF` /
   `OUTBUF`** (lines ~3371, 3373, 10331, 10351, 10436, 10466). This is the one
   trap: once these are pointers, `sizeof` silently becomes 4 and the build
   stays clean.
3. In `client_drop()`, add
   `madvise(cli_in[owner], INBUF, MADV_DONTNEED)` and the same for
   `cli_out[owner]`. The buffers are whole, aligned pages, so all 80 kB goes
   back. The next client zero-fills only the pages its traffic touches: minor
   faults, no SD I/O.

**Saving.**

- **Idle:** ~12 kB (3 pages).
- **With clients:** 84 kB per slot minus that connection's real high-water in
  `in`/`out`. For small-request clients like xcalc and st, and prboom's
  MIT-SHM, that high-water is likely a few kB, so **roughly 200-300 kB**
  across the four slots. This needs measuring: a burst of large non-SHM
  PutImage or RENDER AddGlyphs pushes a slot's high-water up.
- **After clients exit:** **all ~320 kB** comes back, where today none of it
  does.

**CPU.** One `madvise` per disconnect, and zero-fill minor faults on the next
use. Nothing per frame.

**Risk.** Low. There is no change to protocol, look or behaviour.

**Verify.** Use RssAnon+VmSwap from `/proc/<pid>/status` at three points:
idle, with xcalc, st and `prboom -window` up, and after closing all three.
Take 3 fresh boots per arm. The `.bss` line in smaps (`000f9000-001fd000`
on the shipped layout) shows it most directly as Rss+Swap. Then run
`x11-compat-gate.sh` and the acceptance gate.

### 2. Give back the write-spill buffer once it has drained (size depends on the client, cheap)

**Where.** `pend_add()` (`xshim.c` ~3284) realloc-grows `c->pend` from 64 kB
by doubling, up to `PEND_MAX` (2 MB), when a client stops reading. This is the
SDL XSync stall case. It is freed **only** in `client_drop()`. So a client
that stalled once keeps its high-water spill resident (or in swap) for as long
as it stays connected, even though `pendn` went back to 0 long ago.

**Change.** In `xshim_close_tick()` (the existing periodic tick), free
`c->pend` when `pendn == 0` and nothing has spilled for ~5 s. This does not go
in `out_flush`, so a client that stalls every frame does not churn malloc per
frame.

**Saving.** 0 for a well-behaved client. It equals the spill high-water, from
64 kB to 2 MB, for one that stalled, and only while it stays connected. How
often this happens is **unmeasured**. Before shipping, add a one-line
`pendcap` field to the existing "client N gone" log in `client_drop` and read
it after a session of the standard clients. If it always reads 0, drop this cut.

**CPU.** Nil. **Risk.** Very low.

### 3. The ALSA mixer and config tree held for life: probable, needs a no-build check first

**Where.** At startup, the volume restore (`lvdesk.c` ~14513) calls
`audio_set_pct()` -> `audio_open()` (~2816). That opens `snd_mixer` on `hw:0`,
which parses alsa.conf into ALSA's global config tree, and nothing ever closes
it. That is the most likely owner of most of the **64 kB heap mapping** that
is fully resident at idle. lvdesk's other startup mallocs are all small:
kms.c's connector and mode arrays, and hottext's `save` buffer, which is freed.

**Check with no build.** Remove the `volume=` line from `/etc/lvdesk/state`,
so the startup restore skips `audio_open`, and restart lvdesk. Then compare the
anonymous mapping just below `/dev/dri/card0` in smaps. Opening the audio
popover then shows the opposite step.

**Trade-off, and why this is third.** The bong child (`audio_bong`, ~2983)
deliberately inherits the parsed tree so that `snd_pcm_open` does not
re-parse alsa.conf from SD. The comment there names that parse as a candidate
for the reported click-to-sound delay. There are two options:

- **Keep the tree and close only the mixer.** Call `snd_mixer_close()` after
  the startup restore and when the audio popover closes, and reopen it lazily
  on a volume key or when the popover opens. This saves the mixer's element
  list (size unknown, probably 10-30 kB) and costs a few ms on the first
  volume change.
- **Also free the global config** (`snd_config_update_free_global()`). This
  saves more, but it moves the parse back into the bong and the next volume
  key. Take it only if the bong's `BONG_STAMP` phase timings show no rise.

**Saving.** Probably 20-60 kB idle, until the check says otherwise.
**Risk.** Low for the mixer-only variant. The full variant risks bong
latency, which counts as "feel".

## Large but not a free cut: the XLITE-RING shared memory

Every ring connection gets its own memfd of `XRING_TOTAL`: 4,096 + 65,536 +
32,768 = **100 kB** (`xlite/xring.h`). A ring buffer cycles through all of its
pages, so after a minute of traffic all of it is resident. Four connections
make **~400 kB of Shmem**. This is the largest RAM item with clients up. It
does not show in lvdesk's RssAnon, because it is RssShmem in both processes,
but it is real RAM and it goes to swap like anything else.

Shrinking it (for example C2S to 32 kB and S2C to 16 kB, saving ~50 kB per
connection) is **not** a free cut:

- The sizes are compile-time constants on both sides, so xlite in the XIP
  image and lvdesk must be rebuilt in lockstep.
- A request larger than the ring is streamed through several doorbell round
  trips. At ~1.3-5.8 ms per socket or eventfd syscall on this board, that
  shows up in large non-SHM PutImage bursts.

Worth an A/B later, measured on client startup time and `perframe.sh`. It is
not in the ranking above.

## Not worth doing, or already done (do not re-propose)

- **LVGL pool (`LV_MEM_SIZE` 400 kB).** The 400 kB is virtual. Only the
  touched pages count, ~20 kB at idle. Switching LVGL to libc malloc
  (`LV_STDLIB_CLIB`) would give freed objects back, but LVGL allocates draw
  tasks every frame, which puts malloc on the presenting path. Rejected.
- **`partial_buf` (102,400 B).** Used only in non-direct mode. The shipped
  `LVDESK_DIRECT=1` renders into the scanout buffer and never touches it.
- **`struct term` (60,240 B).** A4 already made the scrollback lazy and
  releases it with madvise on close.
- **mlockall.** Measured worse (`s31-lvdesk-mlockall`).
- **Stack.** 16 kB resident. Nothing to take.
- **`res[]` (320 x 276 B).** Pages are touched only as slots are used, at
  276 B per live resource. Small.
- **`mitems` (23,808 B).** Filled in proportion to menu.conf. Small.
- **Window pixel buffers, depth-8/32 shadows, pixmaps.** These are the
  clients' pixels and the presenting path's design. Changing them changes
  either the look or per-frame CPU.
- **libasound's own mapping.** Its 16 kB RELRO is the price of linking it.
  lvdesk uses ALSA at startup, so a lazy `dlopen` would not avoid it.
