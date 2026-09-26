RAMTEXT for phase 5 (lever L1, extended): libGL's hot code copied to
anonymous RAM at the first context and mlock'd. Host-built and host-tested
2026-09-26; NOT yet on the board. s31, MIT.

WHAT CHANGED
- gl/api/ramtext.list: phase 4's list plus QuakeSpasm's phase 5 path, chosen
  from the board's own PC-sample profile of stock QuakeSpasm on the phase 5
  library (artifacts/gl/phase5/board/prof/p5prof.*: 8000 samples, 4181 in
  libGL). Every libGL function down to ~0.2% of libGL, except the GL API
  wrappers the application calls through its PLT (its GOT holds their XIP
  address) and the texture-upload converters.
- Entry through data, not only s31_rt: ramtext.py writes a second table,
  s31_ramtext_dw[], of every word in libGL's writable image holding a hot
  address (op tables, clip_proc[], the fused/texel stage tables, the GOT);
  dw_patch() rewrites them to the copy (RELRO pages through a temporary
  PROT_WRITE).
- One address per hot function at run time (s31_ramtext.h). zpf_select and
  friends match stages by pointer equality, so an XIP and a RAM address for
  the same stage would change which filler runs:
    S31_RT_RAM   at the 9 places cold code takes a hot function's address
                 to store it (zp_color_fn, zp_texidx_fn, zpx_base8, ...)
    S31_RT_XIP   in the 3 cold predicates that compare stage pointers
    S31_RT_ENTER at the top of the hot functions cold code calls directly
                 (gl_add_op, gl_build_pipe, gl_update_raster, glopBegin/End,
                 zp_run(_mt), tu_swap, tgl_multi_tex_coord, gl_vertex4f,
                 gl_draw_triangle*, set_flat): the XIP copy re-enters RAM.
                 The test is on the pc (auipc / adr), never on &f.
  ramtext.py's build line counts what needs them ("cold call sites",
  "cold address-takes"); cold-refs-aarch64.txt is the full list.
- mlock of the copy's pages only (40-48 kB), so reclaim can never evict the
  hot code - under GLQuake's ~14 MB of swap this is the point of the arm
  against SD, where libGL's page-cache text is clean and evictable.
- Default ON in the board build: buildroot-external/package/s31-libgl/
  s31-libgl.mk passes -DS31GL_RAMTEXT_DEFAULT=1 (with the existing
  -DS31GL_TEXFILTER_DEFAULT=0, which was uncommitted in the tree and is
  what the shipped SD library was built with). S31GL_RAMTEXT=0 turns it
  off per process; the host build (gl/build.sh, gl/host-build.sh) keeps 0.
- Works with libGL mapped from XIP flash or from the SD card: it copies from
  wherever the library is mapped and writes only the copy and the private
  RW segment.
- Fixed on the way (AArch64 host rig only, latent since phase 4): the copy
  now keeps the range's offset mod 4096 on AArch64. An adrp from inside the
  range to inside it (a jump table moved with its function) is page-, not
  pc-relative, so a move that is not a page multiple broke it (core_test
  SIGSEGV in "smooth w2", via comb_prog's switch).

NUMBERS (host builds, gl/build.sh -O2 RV32 and the AArch64 rig)
- range: RV32 41,208 bytes, 11 pages, 45,056 bytes mlocked; 499 fixup sites
  (auipc 319, jal 180), 21 data words retargeted. AArch64 48,146 bytes, 13
  pages, 53,248 bytes mlocked. (The board's -Os build will be somewhat
  smaller; the line "ramtext: libGL.so.1.2.0: N bytes" in the Buildroot log
  is the figure.) Phase 4's range was 10,772 bytes.
- coverage, board profile: the listed functions hold 90.1% of libGL's
  samples (3765 of 4181; 47.1% of all CPU samples). The largest left out:
  glMultiTexCoord2fARB 0.7%, tgl_glVertex4f 0.4%, t8_row / st565_rows 0.3%
  each (uploads), glopEnableDisable 0.3%; the PLT-called API wrappers
  together 3.9%.
- coverage, executed: rv32-trace.txt (headless_gears under qemu-user with
  -d exec): 96.8% of hot-range blocks run from the RAM copy; the rest are
  gl_add_op's first block, entered from the API wrappers, which re-enters
  the copy.
- correctness: rv32.txt and host.txt - headless_gears frames identical
  on/off (md5 34a082d3...), core_test 290/292 passed, raster_gate 51,
  filt_test 32, zepoch_test identical, on both architectures;
  rv32-objdump.txt: the table against objdump, 0 problems.
- rv32-dyn.txt: hg_dyn (musl ld.so under qemu-user) exits 132 (SIGILL)
  with S31GL_RAMTEXT=0 as well as 1, so it is the dynamic rig, not this
  change; not investigated in this round.

BOARD A/B TO RUN (stock QuakeSpasm, timedemo demo1, 320x240 fullscreen,
fresh boot per arm, >= 3 runs; the current shipped config is the SD arm)
  SD        libGL.so.1.2.0 on the SD root, S31GL_RAMTEXT=0 (7.3-7.5 fps)
  XIP+RT    the new library in XIP image 1, S31GL_RAMTEXT=1 (the default)
  SD+RT     optional third arm: the new library on SD, default on
Run with S31GL_RAMTEXT=1 set explicitly once per arm: libGL then prints
"libGL: ramtext on: N bytes in P pages, ..., K bytes mlocked" to stderr,
the proof of which arm ran (it is silent when the default is used).

BOARD RESULT, 2026-09-26 23:10 (coordinator). QuakeSpasm timedemo demo1,
fullscreen, fresh boot per run, kernel #393:
  libGL from SD, RAMTEXT off (the shipped state)   7.3, 7.4, 7.5 fps
  libGL from SD, RAMTEXT on                         7.2
  libGL in XIP image 1, RAMTEXT on                  7.0, 6.5

On the board the copy engages: "libGL: ramtext on: 41980 bytes in 11 pages,
873 fixup entries, 4 entry points, 17 data words, 45056 bytes mlocked", and
VmLck reads 44 kB.

Verdict: RAMTEXT does not beat the SD-loaded library. The 10% of libGL's
samples left in flash (the API wrappers, the PLT entries and the other
code) cost more than the copy saves. The Buildroot default is back to 0;
libGL stays on the SD root, and XIP image 1 stays without it.
