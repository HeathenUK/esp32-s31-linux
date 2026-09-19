# Performance opportunities: stock X11 and SDL clients

Source review, 2026-09-13. No board benchmark, firmware modification, or flash was performed. The existing Colima VM was started to inspect the actual build environment. Existing lvdesk source and staged-binary modifications were left untouched.

## Non-negotiable boundary

The purpose is to run off-the-shelf applications. **Never modify client software, including Doom and Quake, to obtain a performance result.** Stock SDL 1.2/2 is the baseline. An independent SDL ABI/rendering compatibility layer integrated with lvdesk is a possible future option, not blanket permission to replace SDL or alter applications. It must be assessed separately from improvements beneath stock SDL.

SDK API coverage is not the silicon capability boundary. Use S31 register definitions, low-level code, hardware documentation and bounded silicon experiments. The user explicitly encourages researching better-documented relatives such as P4 to infer mechanisms and candidate tests; distinguish those clues from verified S31 behaviour. Conversely, an isolated register comment is not proof of working hardware.

## Assessment

There is substantial architectural headroom, especially for SDL2 applications spending time in the software renderer. There is no established universal 2x speedup. The most promising work removes intermediate image traffic and exposes rendering operations before SDL turns them into pixels. Another isolated PPA copy substitution is unlikely to transform this system.

Several structural ideas below already appear in earlier project plans. This review distinguishes implementations that were measured from designs that remain unbuilt, and narrows some conclusions that were stronger than their evidence.

## Evidence and baseline corrections

Inspected the active Linux PPA/LCD/cache/DMA drivers, lvdesk presentation and event paths, xshim and xlite image handling, LVGL's PPA draw unit, board harnesses, and these container sources:

- ESP-IDF `/opt/esp-idf`, `v6.1-dev-7447-g2067f3ae32`.
- `/src/build/buildroot/build/sdl-1.2.15`.
- `/src/build/buildroot/build/sdl2-2.32.10`.
- `/src/build/buildroot/build/chocolate-doom-3.1.1`.
- `/src/build/crosstool-ng/.build/src/musl-1.2.5`.

The generated Linux `.config` and `xipImage` are dated September 13, approximately 10:50 UTC. They are build artifacts, not proof of the currently flashed image. Kernel config extraction from the host `images/xipImage` did not find an embedded config.

### Futex support has disappeared from the build configuration

`/src/build/linux/.config` and `linux-71-port/arch/riscv/configs/esp32s31_defconfig` both disable `CONFIG_FUTEX`; the current Makefile contains no futex enable. This contradicts the September 9 note recording futex enablement as an SDL audio fix.

This has an actual mechanism: musl's `__wait()` retries futex while the condition remains unchanged. With ENOSYS, it repeatedly enters the kernel instead of sleeping. `__timedwait_cp()` normalizes ENOSYS to a retry-compatible result, and contended mutex acquisition loops around it. SDL2 is configured to use pthreads and POSIX semaphores.

The effect depends on contention; it is not a claim that every lock or every SDL frame spins. Verify on the running kernel with a bounded futex probe, then restore support and assess CPU time, audio and responsiveness. Do this before trusting another sound-enabled benchmark or designing a futex-based queue.

### The current image paths still copy

`xlite/xlite_req.c:XPutImage()` maps the server's drawable, copies rows with `memcpy`, and sends damage. It avoids socket pixel payloads; it is not a zero-copy producer-to-display path.

`lvdesk/xshim.c:handle_mitshm()` case 3 explicitly copies the client segment. Its earlier adoption description is stale. The copy preserves X11 semantics: after server processing/XSync, the client may overwrite its source. Retaining that source for later composition caused visible sprite flicker in the previous adoption experiment.

The useful distinction is between avoiding socket copies, avoiding the snapshot copy, and avoiding the final display copy. These are separate accomplishments.

### SDL2's current built renderer is software

The generated SDL2 config disables OpenGL/GLES/DirectFB/Metal rendering backends. Its renderer registry is compiled from a static backend list. Setting a renderer environment variable does not conjure a PPA backend or load a new public renderer plugin.

The actual Chocolate Doom source uses `SDL_LockTexture`, `SDL_LowerBlit` from indexed pixels into the returned ARGB memory, then `SDL_UnlockTexture`. SDL's software `SW_LockTexture()` returns the texture surface directly; `SW_UnlockTexture()` is empty. Therefore the old description of an unconditional separate `SDL_UpdateTexture` copy is not accurate for this source path. Preserve that existing saving when costing a replacement.

The expensive remaining shape is indexed-to-ARGB conversion, software clear/copy/scaling through renderer targets, window-format conversion as applicable, X11 transfer into server storage, then lvdesk presentation. The optional nearest-upscale followed by linear-downscale path is controlled by the application's existing settings. Its two stages are not mathematically interchangeable with an arbitrary single scale.

## Track A: keep stock SDL and all clients

### 1. Delete the native-resolution render-target-to-scanout copy

The private physical scanout allocation still exists in `esp32s31-lcd.c:esp32s31_lcd_alloc_scanout()`. The normal desktop has a separate KMS dumb buffer. `lvdesk/lvdesk.c:kms_flush_cb()` accumulates damage and calls `kms_dirty_rects()`, after which the driver copies or transforms the damaged source into scanout.

The existing `docs/scanout-direct-plan.md` is a real architectural opportunity, not another spelling of the already-shipped direct palette expansion. Current direct expansion skips an intermediate RGB image; it still writes the KMS source buffer rather than merging that buffer with the physical LCD target.

For a simplified native-resolution indexed window, with N changed pixels:

| Stage | Logical traffic, excluding renderer/cache/hash/scanout reads |
|---|---:|
| Snapshot client indices into server storage | 2N bytes |
| Expand indexed server image into RGB565 render target | 3N bytes |
| Copy RGB565 target into physical scanout | 4N bytes |
| Total | 9N bytes |

For 320x200 this is 576,000 bytes. Removing the last copy saves 256,000 bytes, or 44% of these stages' traffic. **This is not a 44% FPS prediction.** It also potentially removes approximately 768 KB of duplicate native framebuffer allocation. Scaling fullscreen paths have different traffic and cannot inherit this calculation.

A sound first implementation keeps the driver's permanent scanout allocation and exports a GEM mapping of it to lvdesk. It does not retarget the live LCD DMA ring. Give one owner control of cursor restore, drawing and cursor repaint; initially use lvdesk's software cursor or restrict the proof to a hidden-cursor mode. Include cache publication, console takeover and capture in the design.

The current kernel cursor relies on a clean separate source image to erase itself. Mapping the same storage at both ends without changing that invariant corrupts the picture. An unsynchronized save-under buffer is not a fix.

### 2. A persistent, bounded PPA surface and command interface

The current interface exposes useful pieces, but not a full accelerated 2D platform:

- RGB565 copy and fixed-alpha blend ioctls.
- Indexed expansion through the blender.
- RGB565/XRGB8888 scaling on presentation paths.
- Per-pixel ARGB sprite blending inside the kernel, currently not a comparable general client interface.
- Fill and additional formats in hardware/driver code without broad use by XRender/LVGL/SDL.

Extend our own driver and shim around named surfaces: handle, format, pitch, bounds, generation, CPU/device ownership, and completion. Keep CPU access cached where beneficial; transition ownership explicitly and retain device ownership across consecutive operations. Queue related operations and collect completion only when pixels must be read or reused. Do not turn every rectangle into a blocking ioctl and a pair of cache transitions.

This is the foundation for accelerating large XRender composites, LVGL fills/images, 32-bit window conversion, and eventually SDL operations. Use CPU paths for small or unsupported jobs. Batch independent glyph/icon work where the semantics and hardware descriptor handling permit it; do not assume one PPA call per glyph will win.

The IDF `ppa_blend.c` explicitly accepts inputs in RAM/flash/PSRAM. The Linux scaler already permits trusted coherent allocations outside its nominal reserved range. Thus the numeric CMA range is not the hardware's entire addressable universe. However Linux anonymous virtual memory is not automatically physically contiguous or DMA-safe. Use validated allocations/imports, pinning or proven descriptor support, with bounded residency; never simply remove the range check and pass virtual addresses.

Large surfaces should be born in suitable shared storage. Repeated bounce-in/operation/bounce-out is the implementation earlier tests correctly rejected. Moving every small X resource into scarce pinned CMA is equally wrong.

### 3. Reopen CLUT on specific changed premises

Three concrete investigations justify reopening it:

**Invisible foreground traffic.** `esp32s31_ppa_clut_expand()` configures and starts BG, FG and RX DMA. BG supplies one-byte indices; FG reads the RGB565 destination but contributes alpha zero; RX writes RGB565. The programmed streams total 5N bytes rather than a single-source expander's 3N. Verify actual reads, since the engine could suppress an unused stream internally.

The S31 register map defines `PPA_BLEND_BYPASS`; its low-level header describes forwarding background through the blender bypass. Test whether CLUT conversion survives bypass, and whether FG DMA can be omitted. If so, it removes an entire stream: a potential 40% reduction in this operation's programmed traffic, not a promised 40% reduction in runtime. Test arbitrary palettes, all indices, odd widths, cropped rectangles and untouched borders. This is not established by the header alone.

**Cache transitions.** The CLUT ioctl issues source sync, destination preparation and destination return calls per row: 600 API calls for 200 rows. Some may resolve to no-ops depending on architecture configuration. Actual cache operations acquire an IRQ-saving lock and issue register transactions; writeback has a required S31 workaround. A tightly packed source can be synchronized as one range. Device-owned scratch/output can avoid returning to CPU ownership between stages. Preserve adjacent dirty cache-line bytes; do not blindly replace safe partial updates with destructive whole-picture invalidation.

**Different destination.** Expand into the final presentation target where feasible, rather than expand and then pay a separate driver copy. In fullscreen, test a queued expand-then-scale with a device-owned intermediate; avoid CPU invalidation/republication between the two. The old windowed toggle did not measure that chain, as `docs/frame-path-plan.md` explicitly records.

Palette caching is already implemented. It is not a new opportunity. SRM async dispatch also exists; do not report adding it as new work. Its present source-reuse/lifetime caveat must be repaired before broadening users.

### 4. Accelerate general X11/LVGL where their operations are still visible

`xshim:render_composite()` remains CPU-based. The PPA can help compatible large blends, opaque copies, fills and solid-color coverage masks once their resources are available to it. XRender's complete operator set, arbitrary source-plus-mask compositions and premultiplied-alpha semantics exceed a naive two-layer blend. Preserve exact supported cases and fallback otherwise.

LVGL already contains `src/draw/espressif/ppa/`, but `LV_USE_PPA` is disabled. That code depends on ESP-IDF allocation/cache/driver APIs and does not become a Linux backend by flipping the switch. Implement a Linux adapter or a custom draw unit backed by our surface/command interface. This targets desktop drawing; it does not accelerate SDL2's earlier software renderer.

Extend format-aware presentation to 32-bit windowed clients. Today `fs_present()` can send raw XRGB rows to a 32-bpp mode buffer and let the driver convert/scale; the corresponding windowed path can still pay a CPU RGB conversion. Avoid solving this by expanding every buffer to 32-bit: panel RGB565 is 768 KB; full-panel ARGB is 1.536 MB. Preserve ARGB only where semantics require it, and convert as late as useful.

## Track B: optional SDL rendering ABI compatibility

This is the highest-ceiling route for SDL2 render-heavy applications. It leaves application source and binaries unchanged. It is a platform compatibility project, not a Doom optimization or a wholesale SDL rewrite.

An independent layer can own renderer/texture operations while delegating appropriate window, event, audio, timer and other functions to stock SDL. It must own a coherent object family: stock SDL cannot safely consume fabricated opaque SDL_Renderer/SDL_Texture objects. Inventory exported calls, dynamic resolution, destruction and internal-call behaviour. Ordinary LD_PRELOAD interposition alone is not proof that this boundary works.

The useful fast path:

1. `SDL_LockTexture` returns CPU-writable, DMA-capable backing storage.
2. The unchanged client writes the pixels it requested. No fictional zero-copy palette conversion: Chocolate Doom still calls a CPU indexed-to-ARGB blit unless the compatibility layer also correctly implements that surface operation.
3. Unlock publishes the changed region to the device.
4. RenderClear/RenderCopy and compatible blend/scale operations become PPA commands on persistent textures.
5. Present gives lvdesk a surface reference plus completion information; it does not necessarily materialize another full window-sized software image.
6. Re-lock/update/destroy waits for readers or selects a different backing buffer. SDL_UpdateTexture's source cannot be retained after its contract permits reuse.

SDL_LockTexture explicitly permits the returned write-only storage not to contain previous texture data. This provides a legitimate ownership boundary unavailable to a server that merely adopts an ordinary XShmPutImage source. See [SDL_LockTexture](https://wiki.libsdl.org/SDL2/SDL_LockTexture).

Target-texture operations, clipping, viewport, blend/color/alpha modulation, readback, nearest/linear scaling, resize and destruction are part of the contract. Do not silently collapse nearest-then-linear into a different filter. Unsupported operations need correct software execution with ordered ownership transitions, or select a wholly stock renderer before creating incompatible objects. Merely returning success from stubs invalidates performance claims.

For SDL 1.2, a similar optional layer can accelerate compatible surface fill/blit/alpha/color-key operations and negotiate presentation storage. But SDL_Surface exposes memory that clients may read and write, including persistence and lock rules. SDL 1.2 is not a texture-command API; deferred operations need stricter care. PPA cannot accelerate the arbitrary perspective renderer already running inside an unchanged Quake binary.

Stock SDL's renderer preference selects existing compiled backends; it is not an external PPA plugin loader. See [SDL_HINT_RENDER_DRIVER](https://wiki.libsdl.org/SDL2/SDL_HINT_RENDER_DRIVER).

## Hardware research with a bounded budget

### Scaler CLUT: potentially valuable, currently low confidence

The exact S31 `ppa_reg.h` CLUT access comment mentions an SRM CLUT and address selection for it. But there is no `PPA_SRM_CLUT_DATA_REG` definition; `ppa_struct.h` marks offset 0x008 reserved. The source-mode register describes RGB and YUV, not indexed input. The earlier September 7 note overstates the evidence by saying the register exists. It noticed the hypothesis, but did not establish the capability.

This could be generator residue, a removed feature, or undocumented silicon functionality. A bounded isolated test should distinguish those possibilities. Palette RAM readback alone proves neither an indexed input decoder nor CLUT-before-scaling operation. Use a deliberately non-grayscale palette and test transformation output with canaries and a recoverable timeout. Do not base the main architecture on success.

The same S31 headers already disagree elsewhere: the SRM low-level code supports GRAY8 with encoding 12 while the generated register description says other encodings are reserved. This is why neither comments nor public enums alone settle the question.

Follow-up research into the [ESP32-P4 TRM, pre-release v0.7](https://documentation.espressif.com/esp32-p4_technical_reference_manual_en.pdf) strengthens the two-blender-CLUT interpretation. Figure 39.4-1 on page 2256 puts both CLUTs in BLEND, none in SRM. Section 39.5.1.2 and Table 39.5-3 on pages 2260-2261 describe two 256-entry tables, selected by address bits 11:10 as 01 for BLEND0 and 10 for BLEND1. Its register summary has CLUT ports at 0x0000 and 0x0004, with no scaler CLUT port. This is stronger evidence than the S31's isolated comment, though not proof against an undocumented S31-only feature.

The actual S31 `esp_hal_ppa/esp32s31/include/hal/ppa_ll.h` defines BLEND0 and BLEND1 CLUT memory offsets as `0x400` and `0x800`, matching P4. This directly conflicts with the generated S31 comment assigning those selectors to SRM and BLEND0 respectively; the low-level implementation is additional evidence of stale commentary.

The same manual, section 39.5.4.1 on page 2265, supports investigating blender bypass: it forwards the background, with a special case where an A4/A8 foreground supplies replacement alpha. The architecture places palette lookup upstream of the blend core. My inference is that indexed conversion through bypass is plausible. Whether the foreground DMA can be omitted, and how much traffic that saves on S31, still needs a controlled test. The evidence does not establish an internal streaming BLEND-to-SRM connection.

### DMA2D conversion and scheduling

S31 DMA2D has its own color-conversion and memory-to-memory modes; it is more than a byte mover. This offers another path for supported RGB/YUV conversions, potentially useful to SDL YUV/video workloads. Match actual planar/packed layouts, ranges and strides; an SDL format name is not proof of a matching hardware layout.

The current PPA driver serializes operations and shares channel/descriptor resources with JPEG. Hardware has separate SRM and blend engines and multiple DMA channels, with channel-specific reorder/CSC capabilities. Separate queues or overlap are research candidates, not a license to remove the mutex. Prove independence and lifetime/cache ordering first. The documented JPEG encode/decode contention remains relevant.

Burst size is fixed at 64 bytes in the Linux helper; IDF exposes 128 too. Benchmark burst/priority/macroblock options against **total application throughput and input latency**, because more aggressive DMA can starve the CPU. This is tuning after the traffic/ownership work, not the central performance thesis.

### Scatter-gather scanout

The AXI DMA descriptors contain individual buffer and next pointers. A row could in principle concatenate visible RGB565 spans from multiple surfaces. This could emulate opaque overlays and remove composition copies despite LCD_CAM having no overlay plane. The idea already appears in `docs/perf-ideas-sweep.md`; it is not implemented by the present contiguous cyclic-frame path.

It has substantial costs: descriptor SRAM and fetch bandwidth, alignment, cursor spans, safe ring updates at frame boundaries, surface lifetime, capture needing a composed image, and no automatic blending/scaling/index lookup. For example, 480 rows with three 16-byte descriptors per row need about 23 KB of descriptors before a second ring or bookkeeping. That exceeds the current dedicated 4 KB LCD link reservation. Treat it as later research, not the first implementation.

## What a large win would mean

Separate whole-application throughput from a primitive speedup. If 35% of execution is presentation and all of it disappeared, the maximum is 1/(1-0.35) = 1.54x. If 60% is avoidable library rendering/conversion, removing it would give 2.5x. These are conditional ceilings, not forecasts or current measurements.

This makes SDL2's software-rendering workloads the best candidates for a multiple-fold improvement. An SDL1 game dominated by its own renderer has a smaller platform-only ceiling. Freeing a duplicate framebuffer can also produce a disproportionate latency benefit at a paging threshold, but that is workload-dependent and must be measured.

Do not prioritize another scalar palette-loop unroll, blanket mlock, another X transport rewrite, hardware-loop re-enablement, or moving work onto hart 0 without a measured cost model. Xespv is more promising for data-parallel format conversion/blending than for the already-tested scalar palette gather. Preserve radios and audio; lower functionality is not a performance result.

## Suggested execution order and acceptance evidence

1. Establish exact artifact identity, effective config, selected SDL/X11 path, mode/depth and futex support. Use existing board tooling and one serial owner.
2. Test CLUT bypass/no-FG, contiguous source sync and validated allocation outside the nominal pool in a bounded diagnostic. Compare output to CPU reference, including random palettes, unaligned damage borders and source reuse.
3. Prototype permanent-scanout mapping in a hidden-cursor/native-resolution case. Measure bytes, memory, CPU and visible stability before integrating cursor/window overlap.
4. Build the shared surface/command foundation, then use it for large XRender/LVGL operations and format-aware windowed presentation.
5. If the SDL compatibility direction is chosen, prove one generic renderer/texture path with unchanged clients, then expand coverage. Keep stock SDL as a control.

Every performance arm needs repeated fresh-boot runs, warm-up separation, stable timekeeping, and equal resolution/depth/audio/workload. Record client CPU, compositor CPU, frame count, PPA hardware operations, faults/swap, memory, and input/audio tails. Correctness requires motion video and real interaction, not just a JPEG-size gate. Include resize/raise while idle, overlap/menus, palette-only changes, repeated source overwrite, and texture/window destruction while hardware work is pending.

The existing `verify-sdl.sh` is a no-sound test; supplement it for audio. Its source/log heuristics also need updating when a legitimate new path intentionally changes the evidence. Do not let the harness prohibit the architecture it is supposed to verify.


## September 13 benchmark follow-up

The new independent SDL clients confirmed ENOSYS on the actual #218 kernel.
History shows `0ea51f8` enabled FUTEX and `10821ca` removed that Makefile override.
The fix now persists in the Makefile, with an effective-config assertion; #219
has working futex support. See [benchmark investigation](sdlbench-results-2026-09-13.md)
for the measured results and their eligibility conditions.

The tests also identify an additional software opportunity: SDL1's Unix timer
thread wakes every millisecond, even without active timers. Enabling this
optional subsystem materially changes the synthetic frame workload. OpenTyrian
does not initialize it; Chocolate Doom's SDL2 path does initialize its timer
subsystem. Do not change clients to improve a result. For other SDL1 applications
that request timers, a future compatible, deadline-driven SDL timer linkage
could be worth investigating alongside the rendering ABI work.

Hardware screenshots have a persistent memory cost in the current recorder:
STOP retains its vmalloc ring until another START replaces it. Keep captures out
of baseline runs, and account for this allocation when comparing observer arms.


## Baseline closed; revised direction (September 13, evening)

The user explicitly prefers exposing hardware through established Linux interfaces
and existing userspace backends over building another SDL ABI shim. This applies
to PrBoom and general SDL1/SDL2 applications as much as OpenTyrian, and includes
non-graphics work. Client application source must never be modified. An SDL
compatibility implementation remains a fallback, not the default plan.

The baseline is now sufficient to begin implementation. Five gated runs per
kernel configuration found SDL2's indexed submission workload at 101.962 ms
without futexes versus 45.216 ms with them. SDL1 with timers was 59.072 versus
54.415 ms; without timers its ranges overlap. The final generic path smoke
completed 81 measurements across SDL1 depths 8/16/32 and SDL2 renderer/window
surface paths. See the [results](sdlbench-results-2026-09-13.md) and retained CLI
records. Do not keep expanding baseline sweeps before making an actual change.

### What stock software can actually discover

Read from the generated container configuration, not inferred from SDL's feature
list: SDL1 1.2.15 includes X11, DGA, fbcon and dummy video backends. SDL2 2.32.10
has X11 video support; the running renderer enumeration reports only `software`.
Neither current build enables DirectFB. A new kernel node alone will not make
this SDL2 binary choose a hardware renderer that was not compiled into it.

The Linux display driver already exposes RGB565/XRGB8888 primary formats,
a cursor plane and damage clips. These must not be proposed as missing features.
It uses a simple display pipe and currently does not advertise an indexed C8
primary format. Standard KMS plane state carries source/destination geometry,
while the driver implements the hardware behavior; exposing a property is useful
only when the caller uses it. See the [KMS documentation](https://cdn.kernel.org/doc/html/latest/gpu/drm-kms.html).

For renderer acceleration, investigate an existing backend before proposing a
new SDL ABI implementation. SDL documents a DirectFB backend for SDL1 and SDL2;
its SDL2 documentation describes driver-dependent 2D acceleration and hardware
YUV. That makes a platform/DirectFB driver a candidate worth evaluating against
our 2D engine. It is **not selected or proven feasible**: build availability,
memory, maintenance, windowing integration and supported operations all need
checking, and the documentation contains old setup instructions. See
[SDL1 backend selection](https://www.libsdl.org/release/SDL-1.2.15/docs/html/sdlenvvars.html)
and [SDL2 DirectFB](https://wiki.libsdl.org/SDL2/README-directfb).

Mesa/EGL is another established route for software using GL-based backends, but
PPA is not a programmable 3D GPU. A Mesa driver would need correct fallbacks for
unsupported operations and a measured memory/performance budget. Gallium exposes
copy/blit hooks, but their existence does not make all GLES drawing reducible to
a PPA blit. See [Gallium context interfaces](https://docs.mesa3d.org/gallium/context.html).

DMAengine similarly requires real consumers to allocate channels and submit
transfers. Registering a controller does not transparently replace arbitrary
userspace memcpy or SDL pixel loops. Audit actual peripheral/client paths before
claiming a system-wide gain. The [DMAengine guide](https://kernel.org/doc/html/latest/driver-api/dmaengine/client.html)
spells out this producer/consumer contract.

### Next work, in order

1. Prototype the existing permanent-scanout/GEM-mapping plan at native resolution,
   behind a runtime switch, with the existing hidden-cursor restriction. Remove
   the duplicate framebuffer/copy beneath unchanged SDL and X11 clients. The
   traffic calculation above saves 256 KB per 320x200 indexed update across the
   described stages and potentially ~768 KB of framebuffer storage; neither is
   a promised FPS gain. Use the relevant existing probe for the A/B.
2. Audit standard capability exposure with a named consumer for each capability:
   buffer sharing/ownership/fences and display transforms in DRM; DMA/controller
   integration and unnecessary wakeups in the kernel. Prioritize measured CPU,
   data-traffic or memory costs. Preserve the prior negative results about slab,
   readahead and reclaim tuning instead of reopening them without new evidence.
3. Evaluate existing accelerated library backends against the S31 operations and
   memory budget. Prefer a hardware/platform driver usable by stock libraries.
   Reconfiguring an unmodified library is different from replacing its ABI.
4. Consider SDL interception only if these routes cannot provide the needed
   operation coverage or fit the system. No change to client applications.

The CLUT/scaler register experiments remain useful bounded hardware work within
these paths. They should support a concrete consumer and avoid another open-ended
measurement phase. The unproven scaler-CLUT hypothesis remains unproven.
