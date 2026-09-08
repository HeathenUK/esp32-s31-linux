# Painting straight into the scanout buffer

**Status: design only. Nothing here has been built or measured.**

The goal is to delete the per-frame DIRTYFB damage copy. It measures **7.8 ms of
a 31.8 ms frame** (after `gdma_copy` was defaulted off), of which roughly 6.7 ms
is 256 kB moved at the ~38 MB/s this machine sustains.

Today there are two buffers:

- **lvdesk's render target** — a 770,048-byte dumb buffer (`lvdesk/kms.c:216-247`),
  `mmap`ed at `lvdesk/kms.c:270-273`.
- **the driver's private scanout buffer** — `lcd->scan_cpu` / `lcd->scan_phys`,
  768,000 bytes, allocated by `esp32s31_lcd_alloc_scanout()`
  (`esp32s31-lcd.c:953-981`).

The LCD's cyclic DMA reads the second (`esp32s31-lcd.c:1316`, and the board
confirms it: `diag: enable scanout at 0x50800000 ... from private buffer`,
printed at `esp32s31-lcd.c:1386-1388`). Every DIRTYFB copies damage from the
first into the second (`esp32s31_lcd_copy_one()`, `esp32s31-lcd.c:2331-2400`).

**The shape that is wrong** is pointing the DMA at lvdesk's buffer. Retargeting
the free-running cyclic DMA is the `-ENXIO` path (`esp32s31-lcd.c:1784-1813`),
and console↔lvdesk switching works precisely because fbcon and lvdesk are
independent sources copying into one permanent target. This plan does the
inverse: **the driver keeps owning a permanent buffer, and lvdesk maps and
paints into it.**

---

## 1. How lvdesk gets a mapping of the scanout buffer

### The key fact that makes this easy

`of_reserved_mem_device_init(dev)` at `esp32s31-lcd.c:3784` points the DMA
allocator at the reserved region, so **`CREATE_DUMB` and `dma_alloc_coherent()`
already carve from the same pool**. lvdesk's buffer and the scanout buffer are
neighbours in one 4 MB CMA region today. Nothing exotic is needed to make one
serve as the other.

### Options weighed

| option | verdict |
|---|---|
| **A. Driver-private ioctl returning an mmap offset** | Rejected. lvdesk would get a mapping but still no `fb_id`, and DRM needs a framebuffer on the primary plane for a commit and for DIRTYFB to hang damage on. It would leave a vestigial second buffer, defeating the point. |
| **B. Export the scanout buffer as a dma-buf, import it back** | Rejected as needless. It is the same memory in the same device; PRIME adds an import/export round trip and a second GEM object to reason about, for no isolation we need. |
| **C. Make the scanout buffer a GEM object and hand lvdesk a handle to it** | **Chosen.** |

### Option C in detail

Change `esp32s31_lcd_alloc_scanout()` (`esp32s31-lcd.c:953`) to allocate a
`drm_gem_dma_object` via `drm_gem_dma_create()` instead of a bare
`dma_alloc_coherent()`, keep it in `lcd->scan_gem`, and set
`lcd->scan_cpu = gem->vaddr`, `lcd->scan_phys = gem->dma_addr`. Every existing
user of `scan_cpu`/`scan_phys` is unchanged.

Add one driver-private ioctl alongside the six already there
(`esp32s31-lcd.c:3412-3424`):

```
DRM_IOCTL_ESP32S31_SCANOUT_GET  ->  { __u32 handle; __u32 pitch; __u32 size;
                                      __u32 width; __u32 height; }
```

implemented as `drm_gem_handle_create(file, &lcd->scan_gem->base, &handle)`.

lvdesk then replaces its `CREATE_DUMB` (`lvdesk/kms.c:228-241`) with this call
and continues exactly as now: `MAP_DUMB` → `mmap` (`lvdesk/kms.c:265-273`),
`ADDFB` (`lvdesk/kms.c:255-264`), `SETCRTC` (`lvdesk/kms.c:276-287`).

**Why this is the clean one.** The plane framebuffer then *is* the scanout
buffer, so `obj->dma_addr == lcd->scan_phys`. `pipe_enable` already sets
`fb_addr = lcd->scan_phys` (`esp32s31-lcd.c:1316`) — **the DMA's target address
does not change by a single byte**, so the `-ENXIO` retarget problem never
arises. The copy in `pipe_update` becomes source-equals-destination and is
simply skipped.

> **Uncertainty.** I have not verified in this tree that `MAP_DUMB`
> (`drm_mode_mmap_dumb_ioctl`) accepts a handle that did not come from
> `dumb_create`. It should — it resolves the handle and calls
> `drm_gem_create_mmap_offset` — but it is the first thing to prove, and it is
> a ten-line test program, not a rebuild of anything.

---

## 2. The cursor: the hard part

**lvdesk uses the DRM cursor plane.** `kms_cursor_init()`
(`lvdesk/kms.c:365-420`) issues `DRM_IOCTL_MODE_CURSOR2` and succeeds; this
build logs `lvdesk: hardware cursor plane`. The `defer_copy` comment at
`esp32s31-lcd.c:1101-1106` claiming `cursor_moves` reads 0 is **stale — do not
trust it.**

### What breaks

`esp32s31_lcd_cursor_lift()` (`esp32s31-lcd.c:2495-2545`) erases the cursor by
**re-copying a rectangle of the client's framebuffer over the scanout buffer**:

```c
const u8 *src = (const u8 *)obj->vaddr + row * fb->pitches[0];   /* :2529 */
u8 *dst = (u8 *)lcd->scan_cpu + (row + oy) * lcd->native.hdisplay * 2;
```

With one shared buffer, `src` and `dst` are the same memory. The lift becomes a
no-op and the cursor smears across the screen.

The same assumption is stated at `esp32s31-lcd.c:1770-1776`: *"the client's
framebuffer is never written — it stays the clean source this repaint copies
from."* That sentence stops being true.

### Why a naive save/restore is not enough

The obvious fix — an 8 kB (64×64×2) backing store holding what was under the
cursor — has an ordering hazard that does not exist today. Today DIRTYFB
*repairs* the buffer: whatever state the scanout buffer is in, the copy
overwrites the damaged region with clean client pixels
(`esp32s31-lcd.c:1770-1776`). **With no copy, the buffer is the only truth**, so
anything that corrupts it stays corrupted until lvdesk happens to redraw.

Concretely: lvdesk paints content over the cursor's rectangle, then the pointer
moves before the next DIRTYFB. The lift restores a save taken *before* lvdesk
drew, putting stale pixels over fresh content — and nothing repairs them.

### The invariant that does work

The standard software-cursor algorithm, stated precisely:

- `cur_save` holds the true content under the cursor, and is valid whenever
  `cur_on` is set.
- **Cursor move**: restore `cur_save` at the old position; save the new
  position's content; paint. (`esp32s31-lcd.c:2904-2905` already has this
  lift/paint pair.)
- **Content damage intersecting the cursor rect**: lvdesk's writes replaced the
  cursor pixels there with real content, so **re-save the intersection** from
  the buffer, then repaint the cursor. Do *not* lift first.
- **Content damage not intersecting**: `cur_save` is still valid; leave it.

Partial intersections must re-save only the intersecting sub-rectangle — the
rest of `cur_save` is still good.

Hook points: `esp32s31-lcd.c:1726-1728` and `:1777-1778` (post-copy repaint,
which becomes post-damage re-save-and-repaint), and `:2904-2905` (the move
path).

This is implementable, but it is genuinely fiddly, and it is the largest
correctness risk in the plan.

### Recommended staging

**Stage 1 — merge the buffers with the LVGL cursor.** lvdesk already has a
software cursor fallback (`cursor_obj` / `lv_image_set_src` around
`lvdesk/lvdesk.c:7141-7159`). Take it, skip `kms_cursor_init()`, and the entire
hazard above disappears: nothing but lvdesk ever writes the shared buffer.
Measure the gain here.

**Stage 2 — reintroduce the hardware cursor** with the save/restore invariant,
only if Stage 1's measurement justifies it.

The 19 ms software-cursor figure quoted in `esp32s31-lcd.c:1094-1099` is **X's**,
not lvdesk's; an LVGL image blit is a normal object redraw and is very likely
far cheaper. That is an assumption, not a measurement — Stage 1 should record
`in_lag` or pointer-move cost before and after so the question is settled with a
number rather than inherited from an X11-era note.

---

## 3. Lifetime: restart, crash, and console switching

This is the user's primary concern, so here is the proof rather than an
assurance.

### The buffer is permanent, and provably so

- Allocated once in `get_modes` (`esp32s31-lcd.c:850`), deliberately early —
  the comment at `:834-848` explains it is taken there because CMA migration
  fails later under memory pressure.
- The allocator is idempotent: `if (lcd->scan_cpu) return 0;`
  (`esp32s31-lcd.c:957-958`).
- **It is never freed.** `grep dma_free_coherent` over the file returns exactly
  one hit, `esp32s31-lcd.c:2716`, and that frees the *cursor sprite*
  (`lcd->cur_argb`), not the scanout buffer.

Under option C the driver holds a permanent reference to `lcd->scan_gem` taken
at allocation. A client handle is an *additional* reference.

### lvdesk exits or crashes

`drm_release` drops the process's GEM handles. The refcount falls from 2 to 1;
the driver's own reference keeps the object alive. `scan_phys` is unchanged, the
cyclic DMA keeps reading the same valid physical memory, and the panel holds the
last frame — exactly today's behaviour. **The DMA cannot read freed memory,
because nothing frees it.**

### fbcon ↔ lvdesk switching

Unchanged, because **fbcon does not use the shared buffer**. It keeps its own
framebuffer and remains a copying source:

- `master_set` → `client_wanted = false` → `client_work` unregisters the fbdev
  client (`esp32s31-lcd.c:3211-3218`).
- `master_drop` → `client_work` shuts the pipeline down and recreates the client
  (`esp32s31-lcd.c:3221-3226`), which allocates its own buffer and modesets.
- On that modeset, `pipe_enable` takes the `scaling` branch
  (`esp32s31_lcd_composite()` returns `true` unconditionally,
  `esp32s31-lcd.c:1171-1183`), so `fb_addr = lcd->scan_phys` — **the same
  address as before** — and the fbdev framebuffer becomes the plane fb, copied
  into the scanout buffer by the existing path.

So the copy is not removed in general; it is removed only when the plane fb *is*
the scanout buffer. fbcon keeps the old behaviour verbatim.

**The one rule to enforce:** fbcon and lvdesk must never both be active, or one
would copy into the buffer the other is painting. The existing handover already
guarantees this and must not be weakened.

### A bonus worth having

`esp32s31-lcd.c:3175-3186` documents that three 768 kB allocations do not fit in
the 4 MB CMA pool, which is why the handover dance exists at all. After the
merge lvdesk no longer allocates its own 770,048 bytes — **that memory comes
back on a board where memory, not CPU, is the binding constraint.** On a 15.4 MB
machine this may matter more than the frame time. The `CREATE_DUMB` retry loop
at `lvdesk/kms.c:228-241` becomes unnecessary, though leaving it costs nothing.

---

## 4. Cache coherency

`dma_alloc_coherent()` returns **cached** memory on this SoC — stated twice, at
`esp32s31-lcd.c:966-976` and `:2781-2786`, and the reserved region is used
precisely because "PSRAM has no uncached alias" (`esp32s31-lcd.c:3761-3765`).
So explicit maintenance is required and already present.

**Today, two flushes per frame:**

1. lvdesk's fb, damaged rows, `DMA_TO_DEVICE`, so the copy engine can read it —
   `esp32s31-lcd.c:1615-1625`.
2. the scanout buffer, damaged rows, so the LCD DMA can see the copy's result —
   e.g. `esp32s31-lcd.c:2386-2389`.

**After the merge, one flush:** lvdesk writes the (cached) scanout buffer
directly; the driver flushes the damaged rows so the LCD DMA sees them.

The pleasing part: the existing per-damage-clip flush at
`esp32s31-lcd.c:1615-1625` flushes `obj->dma_addr + y1 * pitch`, and with
`obj == scan_gem` that address **is** the scanout buffer. **The correct flush is
already written and needs no change** — only the copy that follows it is
deleted.

Keep it as one range per clip, not per row: the driver's own measurement at
`esp32s31-lcd.c:2603-2612` found per-row maintenance is 783 µs against 437 µs
for one range, because the per-call cost dominates.

---

## 5. Tearing: neutral, with one caveat

Not a regression in kind. `wait_vblank` already defaults to false, and
`esp32s31-lcd.c:3475-3480` states the consequence plainly: *"a primary-plane
copy can now race scanout, so a full-screen update can tear ... the exposure is
a visible seam, not corruption. Verified against the panel at 640x384: clean."*
Scanout is free-running cyclic DMA into a buffer the driver writes while it is
being displayed. lvdesk writing that same buffer directly is the same exposure
against the same buffer.

**The honest caveat:** the *duration* of the window changes. A `memcpy` of the
damaged region is a short burst; lvdesk's palette expansion takes ~5 ms and
writes progressively. A wider window means a seam is more likely to be caught by
any given frame. This is a plausible perceptual regression, not a correctness
one, and it must be **looked at on the panel**, not reasoned about. `wait_vblank=Y`
remains the runtime escape hatch.

---

## 6. Honest expected gain

**Do not assume the full 7.8 ms disappears.** What is actually deleted is the
copy — roughly 256 kB at ~38 MB/s, so ~6.7 ms. What remains inside DIRTYFB:

- the atomic commit itself and damage iteration (`esp32s31-lcd.c:1611-1626`);
- the cache writeback of the damaged rows, which **stays** and is not free: at
  200 rows × 1600-byte pitch it is 320,000 bytes of maintenance per frame,
  never measured on its own;
- the cursor repaint (Stage 2 only).

Estimate **4–6.5 ms/frame recovered**, i.e. 13–20% of a 31.8 ms frame. State it
as a range, because the flush component is unmeasured.

**Expect the Doom fps gain to be much smaller than that fraction**, and say so
up front. The GDMA change is the precedent: it removed 2.8 ms/frame and yielded
27% off DIRTYFB and ~11% more composited frames, but only ~2% on Doom's own fps,
because lvdesk spends part of what it saves compositing more frames and because
the fps band (31.0–32.6) is wider than the effect. The defensible headline here
will again be **DIRTYFB time and composited frame count**, plus the 770 kB of
memory returned — not fps.

Instrument with the existing `LVPROF=1` (`lvdesk/lvdesk.c`, bracketing the
DIRTYFB ioctl), which is stable to ±2% and is the right instrument precisely
because fps is not.

---

## 7. Risks, detection, rollback

| # | risk | how it shows | detection |
|---|---|---|---|
| 1 | **Cursor smear** (Stage 2). The lift has no clean source; the save/restore invariant is subtly wrong. | Trails of cursor pixels, or stale rectangles that never repair — because with the copy gone nothing repairs the buffer. | Move the pointer over a static region and screenshot. Any residue is a fail. Stage 1 avoids this entirely. |
| 2 | **`MAP_DUMB` refuses a non-dumb handle.** | `MAP_DUMB` fails at `lvdesk/kms.c:268`, lvdesk exits at startup. | Loud and immediate. Prove with a standalone test program before touching lvdesk. |
| 3 | **fbcon and lvdesk both active**, one copying into the buffer the other paints. | Console text bleeding through the desktop, or vice versa. | Exercise the handover: start/stop lvdesk repeatedly, watch for `handover:` lines (`esp32s31-lcd.c:3213-3226`). |
| 4 | **Wider tear window** from the slow expansion. | A visible seam during heavy motion. | Watch the panel under Doom. `wait_vblank=Y` is the runtime fallback. |
| 5 | **Aliased consumers.** With `obj == scan_gem`, several paths become src == dst: `gdma_rows` (`:2257`), the CPU and PPA copies (`:2372-2400`), `test_pattern` (`:1354`), and the page-flip retarget check (`:1784`). | Corruption, or a silent no-op copy that hides a bug. | Audit each; the retarget check is unreachable while `composite()` returns true, but do not leave it to luck. |
| 6 | **Gain is smaller than hoped** because the retained cache flush dominates. | DIRTYFB falls far less than 4 ms. | `LVPROF=1` says so directly. Measure before concluding anything. |

**Rollback.** Every step is additive and switchable:

- Gate the whole thing on a module parameter — `direct_scanout`, default off —
  so the old copy path stays compiled in and one `echo` restores it.
- The new ioctl is additive; lvdesk falls back to `CREATE_DUMB` if it returns
  `-ENOTTY`, so an old lvdesk works on a new kernel and vice versa.
- Stage 1 and Stage 2 are separately revertable, and Stage 1 is the one worth
  measuring first.

---

## What I could not settle from the code alone

1. Whether `MAP_DUMB` accepts a handle not created by `dumb_create` (risk 2).
2. The cost of the retained cache flush, which sets the true ceiling on the gain
   (risk 6). Nothing measures it in isolation today.
3. The real cost of lvdesk's LVGL software cursor. The 19 ms figure in the
   driver is X's, and using it to justify anything here would be inheriting a
   number from a different program.
4. Whether the widened tear window is perceptible. Only the panel can answer.
5. Whether `esp32s31_lcd_composite()` returning `true` unconditionally
   (`esp32s31-lcd.c:1171-1183`) should become conditional once the copy is gone
   for lvdesk. I have deliberately not proposed changing it: it is what keeps
   the fbcon path and the DMA address identical, and the whole safety argument
   in §3 rests on that address never moving.
