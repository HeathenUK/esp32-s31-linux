# OpenGL packaging: s31-libgl as Buildroot's libgl provider (2026-09-25)

This doc covers the Buildroot side of docs/gl-plan-2026-09-25.md (sections 1,
2.3, 4.2 and stage 2). It says:
- what was added and why;
- the exact defconfig lines, and what each one pulls in;
- where it collides with the xlite strategy;
- how SDL is kept GL-off regardless of build order;
- where libGL goes in XIP.

**Status.** These are files only. No Buildroot build and no make target was
run: the build volume belongs to another process. What was checked, and how,
is in section 9. Anything not listed there is untested.

## 1. What changed

| file | change |
|---|---|
| `buildroot-external/package/s31-libgl/Config.in` | new: `BR2_PACKAGE_S31_LIBGL` (libgl provider) plus two stage-5 switches, both off/inert |
| `buildroot-external/package/s31-libgl/s31-libgl.mk` | new: builds `libGL.so.1.2.0` from `gl/` through `gl/build.sh`, and installs it with gl.pc and glx.pc |
| `buildroot-external/package/s31-libgl/gl.pc.in`, `glx.pc.in` | new |
| `buildroot-external/Config.in` | sources the new Config.in |
| `buildroot-external/external.mk` | adds: the SDL 1.2 GL switch (always explicit); the SDL2 option-B hook (inert); mesa3d-demos `-Dwith-system-data-files=true`; libGLU `-static-libstdc++` with a check |
| `Makefile` | adds: `XIP_ROOTS_GL ?= usr/lib/libGL.so.1.2.0`, appended to `XIP_ROOTS` (section 6) |

Not touched:
- `buildroot-external/configs/esp32s31_rootfs_defconfig` (the lines to add
  are in section 3);
- `board/esp32-s31/post-build.sh`;
- `gl/`, `xlite/`, `lvdesk/`, `buildroot/`.

## 2. The package, and the contract with gl/build.sh

`s31-libgl` follows nvidia-driver, Buildroot's example of a third-party
libgl provider:
- **Kconfig** (buildroot/package/nvidia-driver/Config.in:24-34): select
  `BR2_PACKAGE_HAS_LIBGL` and `BR2_PACKAGE_MESA3D_HEADERS`, and give
  `BR2_PACKAGE_PROVIDES_LIBGL` a default under `if`.
- **Makefile** (nvidia-driver.mk:23-24): `DEPENDENCIES = mesa3d-headers
  xlib_libX11 xlib_libXext` and `PROVIDES = libgl`.
- **Enforcement.** The `libgl` virtual package errors out if HAS_LIBGL is set
  with no provider (package/pkg-virtual.mk:38-42). pkg-generic.mk:1195-1201
  checks that the package claiming `libgl` is the configured provider.
- **Exclusions.** `depends on !BR2_PACKAGE_MESA3D && !BR2_PACKAGE_LIBGLVND`.
  mesa3d-headers refuses to coexist with mesa3d anyway
  (mesa3d-headers.mk:9-11).
- **Headers.** mesa3d-headers installs `GL/` only when HAS_LIBGL is set
  (mesa3d-headers.mk:30-32), which gives gl.h, glext.h, glx.h, glxext.h and
  `KHR/`. With XORG7 it also installs dri.pc and dri_interface.h
  (lines 34-49), which are harmless.
  - It downloads the 26.1.2 Mesa tarball just for these headers.
  - gl/include holds a separate (libglvnd/Khronos) copy of the same headers.
    Both are Khronos headers, so the ABI is the same, but see the open issues.

**Installed files:**
- staging and target: `usr/lib/libGL.so.1.2.0`, `libGL.so.1 ->`, `libGL.so ->`;
- staging only: `usr/lib/pkgconfig/gl.pc` and `glx.pc`.

**The soname is `libGL.so.1`.** SDL 1.2 (SDL_x11gl.c:38) and SDL2
(SDL_x11opengl.c:47) dlopen exactly that name. The build step fails if the
soname is wrong.

**Source.** `SITE_METHOD = local` and `SITE = $(BR2_EXTERNAL_ESP32_S31_PATH)/../gl`,
on the s31-tools pattern:
- Buildroot implements `local` as an override srcdir that it rsyncs
  (pkg-generic.mk:645-647 and :228).
- `S31_LIBGL_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS` keeps out `gl/ref-apps`
  (91 MB of host reference builds), `gl/out` and `gl/build`.
- A local package is **not** rebuilt when gl/ changes. After editing gl/,
  run `s31-libgl-rebuild` (see open issue 5).

**The compile is not duplicated in the .mk.** It calls `gl/build.sh`, so the
host iteration build and the Buildroot build cannot drift. **Contract for
gl/build.sh** (the gl/ owner implements it; nothing here enforces it beyond
the output checks):

| variable | Buildroot passes | build.sh must |
|---|---|---|
| `S31GL_CC` | `$(TARGET_CC)` | use it instead of its hard-coded /src/toolchain gcc |
| `S31GL_CFLAGS` | `$(TARGET_CFLAGS)`, which includes `BR2_TARGET_OPTIMIZATION` = `-Os -march=rv32imafbc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs -mabi=ilp32 -mtune=esp-base` (defconfig:24) | put them FIRST, then append its own `-O2`/`-fPIC`/`-fsingle-precision-constant`, so its own choices win |
| `S31GL_LDFLAGS` | `$(TARGET_LDFLAGS)` = `-Wl,--as-needed -Wl,-Bsymbolic-functions` (defconfig:259) | use them: these are exactly the flags our out-of-tree libs kept missing (memory s31-our-libs-miss-buildroot-flags) |
| `S31GL_OUT` | `$(@D)/_br_out` | write `libGL.so.1.2.0` there with `-Wl,-soname,libGL.so.1`, and nothing outside it or its own tree |
| `S31GL_STRIP` | `0` | not strip. Buildroot strips the target copy, and staging keeps symbols for debugging |

When the variables are unset, build.sh keeps its host defaults (writing to
/src/images and so on).

It must **not** link with `-Wl,-z,defs`. It links against the stock
libX11/libXext in staging and resolves against xlite/xstubs on the board, as
every X client here does. XliteShm* symbols exist only in xlite
(`libX11.so.6` in the overlay exports XliteShmMap and XliteShmDamaged).

## 3. Defconfig lines (proposed; not applied)

Add to `buildroot-external/configs/esp32s31_rootfs_defconfig` for **stage 2**:

```
# --- OpenGL (docs/gl-plan-2026-09-25.md, docs/gl-packaging.md) ---------------
# Our own libGL (TinyGL-based, buildroot-external/package/s31-libgl) as
# Buildroot's libgl provider, so stock GL packages build against it.
BR2_PACKAGE_S31_LIBGL=y
# Mesa GLU 9.0.3, unmodified; linked -static-libstdc++ by external.mk.
BR2_PACKAGE_LIBGLU=y
BR2_PACKAGE_LIBFREEGLUT=y
# glxgears, glxinfo, the xdemos and (with freeglut) the GLUT demos.
BR2_PACKAGE_MESA3D_DEMOS=y
```

Nothing to remove. The stage-5 switches (`BR2_PACKAGE_S31_LIBGL_SDL_OPENGL`,
`BR2_PACKAGE_SDL2_OPENGL`) stay off: they are **not** lines to add now.

**Evaluated, not guessed.** Buildroot's own Kconfig tree (2026.05.1) plus
this br2-external was loaded with kconfiglib 14.1.0. The br2-external
fragments came from `support/scripts/br2-external -d`. The committed defconfig
was loaded, with and without the four lines. **The complete difference in the
resulting .config is:**

```
+BR2_PACKAGE_S31_LIBGL=y          +BR2_PACKAGE_HAS_LIBGL=y
+BR2_PACKAGE_PROVIDES_LIBGL="s31-libgl"
+BR2_PACKAGE_MESA3D_HEADERS=y     +BR2_PACKAGE_LIBGLU=y
+BR2_PACKAGE_LIBFREEGLUT=y        +BR2_PACKAGE_MESA3D_DEMOS=y
+BR2_PACKAGE_LIBGLEW=y
+ blind "supports" symbols only: GLMARK2_FLAVOR_ANY/X11_GL,
  KODI_PLATFORM_SUPPORTS(_X11), QT5_GL_AVAILABLE, QT6_GL_SUPPORTS,
  WAFFLE_SUPPORTS_GLX (none of those packages is enabled)
```

**No new X library is enabled.** libXi, libXmu, libXrandr, libXxf86vm,
libXext, libXcursor, libXrender and libXfixes are already `y` in today's
config. Among other things, today's SDL2 is configured with
`--enable-video-x11-xinput` (shipped config.log line 7), so libXi is already
built and in the target.

What each line selects or depends on, with file:line:

| symbol | depends on | selects | notes |
|---|---|---|---|
| `S31_LIBGL` | XORG7 (y, defconfig:206); !MESA3D; !LIBGLVND | HAS_LIBGL, MESA3D_HEADERS, XLIB_LIBX11, XLIB_LIBXEXT | package/s31-libgl/Config.in |
| `LIBGLU` | HAS_LIBGL (libglu/Config.in:3) | - | meson, `-Dgl_provider=gl`, so it needs our gl.pc (libglu.mk:14). **C++**: see section 4.3 |
| `LIBFREEGLUT` | HAS_LIBGL, XORG7 (libfreeglut/Config.in:3-4) | LIBGLU, XLIB_LIBXI, XLIB_LIBXRANDR, XLIB_LIBXXF86VM (:5-8), all already y | CMake. `FIND_PACKAGE(OpenGL REQUIRED COMPONENTS OpenGL)` (freeglut CMakeLists.txt:410) accepts a legacy libGL: FindOpenGL sets `OpenGL_OpenGL_FOUND` when either libOpenGL or libGL is found (cmake 4.x FindOpenGL.cmake:657-664). So it needs `libGL.so` in staging, which we install, and no GLVND |
| `MESA3D_DEMOS` | INSTALL_LIBSTDCPP (y, via TOOLCHAIN_EXTERNAL_CXX, defconfig:23); HAS_LIBGL (mesa3d-demos/Config.in:3-5) | LIBGLEW, LIBGLU, XLIB_LIBX11, XLIB_LIBXEXT when XORG7 && HAS_LIBGL (:6-9) | `-Dgl=enabled -Dx11=enabled` (mesa3d-demos.mk:17-19). glut demos only when LIBFREEGLUT=y (:53-56). xcb/xkbcommon are required only with vulkan (demos meson.build:64-69), which is off |
| `LIBGLEW` (selected) | XORG7, HAS_LIBGL (libglew/Config.in:3-4) | X11, XEXT, XI, XMU (:5-8), all already y | **dead weight**: mesa-demos 9.0.0 uses its own glad, and no meson.build in it references glew. It is built only because Buildroot's mesa3d-demos.mk:18 lists it. It goes to the SD card and nothing loads it |

**mesa3d-demos notes:**
- **glx.pc is load-bearing.** `dep_glx = dependency('glx', required: false,
  disabler: true)` (meson.build:101). Without glx.pc, all of src/xdemos,
  glxgears included, is silently disabled. We install it.
- **Data path.** The demos compile in `DEMOS_DATA_DIR="../data/"` unless
  `-Dwith-system-data-files=true` (meson.build:31-36), but install the files
  to `/usr/share/mesa-demos`. Stock Buildroot passes neither
  (mesa3d-demos.mk:13-15). external.mk now adds the option, so texcyl,
  isosurf, terrain, geartrain and the rest find their data from any CWD.
  - This is the package's own build option, not an app patch.
  - Owner check: it touches rule 1 only in spirit. Drop it if you read it as
    steering.
- `glsl/gsraytrace` is C++ and GL 2. It will fail at load (libstdc++ is
  deleted) and could not run anyway; ignore it.
- About 150 demo binaries go to /usr/bin on the card. The ext4 image is
  192 MB (defconfig:99), so size is not an issue.

## 4. Conflicts with the xlite replacement strategy

### 4.1 Build-time: no conflict, and the stock libX11 is still built

- Buildroot builds the **real** libX11 and friends: `BR2_PACKAGE_XLIB_LIBX11=y`
  (defconfig:207). Everything links against them in staging, headers
  included.
- At runtime the overlay libraries win:
  - defconfig:388-390: "SDL links the STOCK libX11 at build time and runs
    against xlite at runtime";
  - `X11_REPLACEMENTS` (Makefile:1013) and `x11-stage` (Makefile:1019) copy
    images/ into overlay/usr/lib;
  - post-build.sh:92-107 deletes a differing stock copy of 12 of them from
    the target.
- GL changes none of this. freeglut, GLU, glew and the demos link the stock
  staging libs, like xcalc does.

### 4.2 Runtime: symbols xlite does not export (**blocks stage 2**)

musl binds eagerly, so one missing import aborts the process at load (plan
F1). **Method:** the X calls in the stock sources were scanned against the
dynamic symbol tables of the actual overlay libraries
(`buildroot-external/board/esp32-s31/overlay/usr/lib/*.so*`, identical to
images/). The scan excludes macros (XIMaskIsSet, XGetPixel, XDestroyImage).
**Scope:** this is a source scan. Confirm it on the Buildroot-built binaries
with the check at the end of this section.

| consumer | missing from xlite | where |
|---|---|---|
| **glxgears**, glxheads, manywin, multictx, glxgears_fbconfig, glxswapcontrol, glsync, glthreads, glxcontexts | `XSetNormalHints`, `XSetStandardProperties` (libX11) | e.g. mesa-demos src/xdemos/glxgears.c. **The stage-2 gate app does not start without these.** |
| **libglut.so.3** (every GLUT demo) | `XGetEventData`, `XFreeEventData` (fg_xinput_x11.c), `XGetPointerMapping` (fg_state_x11.c), `XGetWMName` (fg_spaceball_x11.c), `XStoreColor` (fg_cmap_x11.c): libX11. `XRRConfigTimes`, `XRRSetScreenConfig` (fg_gamemode_x11.c): xlite's libXrandr.so.2 | freeglut-3.8.0 src/x11/ |
| **libXi.so.6** (stock, NEEDED by libglut: the reference build's NEEDED is `libX11 libXrandr libXxf86vm libXi libGL libm libc`) | `_XUnknownNativeEvent` | images/libXi.so.6.1.0's undefined symbols against xlite + xstubs; everything else it imports (the Xext display helpers, printf, strncpy) resolves |

So xlite needs 7 libX11 stubs or implementations and 2 libXrandr ones. None
is needed for glxinfo, offset, glxdemo, or the GLU/GLUT-free apps.
- `XSetStandardProperties` and `XSetNormalHints` are old ICCCM wrappers over
  `XSetWMProperties`/`XSetWMNormalHints`, which xlite already has.
- `_XUnknownNativeEvent` can be a no-op returning 0.
- This is the xlite owner's work. Nothing was edited in xlite/.

Today's SDL2 already dlopens libXi.so.6 (`--enable-video-x11-xinput`). If
that dlopen fails for the same missing symbol, SDL2 falls back without
XInput2, which is the status quo. **Adding `_XUnknownNativeEvent` to xlite
therefore changes what SDL2 does**, because XInput2 would then load. SDL2
queries `XInputExtension` first, and if xshim reports it absent the path is
unchanged. Re-run the 4.2 regression gate for chocolate-doom when that stub
lands.

Check on the built binaries, run on the host once Buildroot has built them:

```
NM=build/buildroot/host/bin/riscv32-esp-linux-musl-nm   # or the toolchain's
OV=buildroot-external/board/esp32-s31/overlay/usr/lib
T=build/buildroot/target
$NM -D --defined-only $OV/lib*.so* $T/usr/lib/libXi.so.6 $T/usr/lib/libGL.so.1 \
    $T/lib/libc.so 2>/dev/null | awk '{print $NF}' | sort -u > /tmp/have
for f in $T/usr/bin/glxgears $T/usr/lib/libglut.so.3 $T/usr/lib/libXi.so.6 \
         $T/usr/lib/libGLU.so.1; do
  echo "== $f"; $NM -D --undefined-only "$f" | awk '$1=="U"{print $2}' | sort -u | comm -23 - /tmp/have
done
```

### 4.3 libGLU is C++, and post-build.sh deletes the C++ runtime

- **The dependency.** GLU 9.0.3's NURBS tessellator is C++: the libnurbs .cc
  files in glu-9.0.3 src/meson.build:4-111, and bufpool.h:130 overloads
  `operator new`. The host reference build (gl/ref-apps/prefix/lib/libGLU.so.1.3.1)
  has NEEDED `libGL.so.1 libstdc++.so.6 libm libgcc_s libc`.
- **The deletion.** post-build.sh:12 does `rm -f lib/libstdc++.so*`
  unconditionally ("this compact image has no C++ target packages"). As
  shipped, **every GLU app (rRootage, the GLUT demos, prboom-plus GL) would
  fail at load.**
- **Done, in external.mk: `LIBGLU_LDFLAGS += -static-libstdc++`.** This
  links only the members GLU uses into libGLU.so.1.
  - It is possible because the toolchain's libstdc++.a is PIC. Its 191
    objects carry GOT/PCREL relocations (types 20/23/24/25) and zero
    R_RISCV_HI20/LO12 (types 26-28), counted from the archive.
  - `LIBGLU_LDFLAGS` is read by the meson cross-file sed at configure time
    (pkg-meson.mk:143, :154), so appending from external.mk takes effect.
  - A `LIBGLU_POST_INSTALL_TARGET_HOOKS` check **fails the build** if
    libGLU.so.1 still NEEDs libstdc++.
  - **Untested**: meson and the link have not run.
- Keeping libstdc++ instead would cost a 1.69 MB library (532 kB text, 39 kB
  writable, measured from the toolchain's libstdc++.so.6.0.34) mapped from
  the card and relocated in every GLU process. It does not fit XIP either.
- **Fallback if the static link fails:** make the deletion conditional
  (a proposed diff to post-build.sh, not applied):

```
-rm -f "${target_dir}"/lib/libstdc++.so*
+# Keep it only when a target ELF NEEDs it.
+cxx_user=$(find "${target_dir}/usr" "${target_dir}/bin" "${target_dir}/sbin" \
+	-type f -size +1k -exec grep -l -F 'libstdc++.so.6' {} + 2>/dev/null | head -1 || true)
+if [ -z "$cxx_user" ]; then
+	rm -f "${target_dir}"/lib/libstdc++.so*
+else
+	echo "post-build: keeping libstdc++ (NEEDED by ${cxx_user#${target_dir}})"
+fi
```

  If this is used, add `libstdc++.so.6.0.34` to `XIP_SKIP` so the library
  stays on the card.

### 4.4 freeglut's .bss

The reference build has 409,616 bytes of .bss. It is almost all the
teapot/teacup vertex and normal caches (`vertsTeapotW` and `normsTeapotW`,
38,400 bytes each, and so on), in float. It is zero-fill and only becomes
resident when an app draws those shapes: roughly 150 kB for a teapot demo.
Measure it on the board rather than assume.

## 5. SDL: GL-off explicitly, whatever the build order

**The hazard is real.**
- SDL-1.2.15 configure.in:1650-1672 defaults `enable_video_opengl=yes` and
  turns GL on if `GL/gl.h`, `GL/glx.h` **and** `GL/glu.h` compile.
- sdl.mk passes no opengl switch (package/sdl/sdl.mk:31-33, 76-82).
- The shipped config.log shows it is GL-off only by absence:
  `checking for OpenGL (GLX) support` then
  `fatal error: GL/gl.h: No such file or directory`, and the resulting
  SDL_config.h:300-301 has `SDL_VIDEO_OPENGL` / `_GLX` undefined.
- Once mesa3d-headers and libglu are staged, an SDL reconfigure (a clean
  build, `sdl-reconfigure`, anything after `make clean`) would silently turn
  GL on.

**Mechanism (implemented in external.mk):**

```
ifeq ($(BR2_PACKAGE_SDL),y)
ifeq ($(BR2_PACKAGE_S31_LIBGL_SDL_OPENGL),y)
SDL_CONF_OPTS += --enable-video-opengl
$(SDL_TARGET_CONFIGURE): | libgl libglu
else
SDL_CONF_OPTS += --disable-video-opengl
endif
endif
```

**Why a later `+=` in external.mk is effective, with evidence:**
- **Include order.** buildroot/Makefile:550 includes every
  `package/*/*.mk`, which evals sdl.mk. **Then** Makefile:564 includes
  `$(BR2_EXTERNAL_MKS)`, which is our external.mk.
- **CONF_OPTS is read at recipe time.** The autotools configure command is
  generated with a deferred `$$($$(PKG)_CONF_OPTS)`
  (package/pkg-autotools.mk:193). pkg-meson.mk:171 and pkg-cmake.mk:122 do
  the same. It is expanded only when the configure recipe runs, after every
  makefile has been read.
- **The variable is recursive.** `SDL_CONF_OPTS` was first assigned with
  `+=` on an undefined variable (sdl.mk:31), which creates a recursive
  variable, so `+=` appends.
- **Autoconf takes the last switch,** so the append wins even over an
  earlier contrary one. There is none today.
- **Hooks are also read at recipe time**
  (pkg-generic.mk:262-264: `$(foreach hook,$($(PKG)_POST_CONFIGURE_HOOKS),...)`
  inside the stamp recipe).
- **`<PKG>_DEPENDENCIES` is NOT read at recipe time.**
  `$$($(2)_TARGET_CONFIGURE): | $$($(2)_FINAL_DEPENDENCIES)`
  (pkg-generic.mk:940) is expanded as the eval parses it, so appending
  `SDL_DEPENDENCIES` from external.mk would be silently ignored for
  ordering. That is why the GL-on arm adds its own order-only prerequisite
  to the stamp. make merges prerequisites from several rule lines.
  - With `BR2_PER_PACKAGE_DIRECTORIES` (not set here) the per-package
    staging is built from FINAL_DEPENDENCIES at recipe time
    (pkg-generic.mk:256), so a PPD build would need `SDL_DEPENDENCIES`
    appended as well.
- **The GL-on arm selects `LIBGLU`,** because SDL 1.2's probe includes
  `GL/glu.h`. Without glu.h it would silently stay off even when asked.

**The shipped SDL 1.2 is unchanged by this.** Its configure already reached
"GL off". An explicit `--disable-video-opengl` produces the same
SDL_config.h, and Buildroot does not rebuild a configured package just
because its options changed.

**SDL2** needs no guard while GL is off. It already passes
`--disable-video-opengl` unless `BR2_PACKAGE_SDL2_OPENGL` (sdl2.mk:133-138),
and the shipped config.log confirms it. Enabling freeglut or glew does not
change its X11 options either: every libX* they select is already enabled
(section 3).

**Other enabled packages that could autodetect GL from staging:** none found.
- prboom-plus passes `--disable-gl` (prboomplus.mk).
- chocolate-doom and opentyrian are SDL2 or SDL-only.
- st, xfiles, xcalc, xclock, oclock and xdpyinfo have no GL probe.

## 6. XIP placement

**Budget,** from the current images/ (built 2026-09-25 19:44):

| image | partition | used | free |
|---|---|---|---|
| rootfs-xip.cramfs (image 1) | 6,422,528 (`ROOTFS_PARTITION_SIZE`, Makefile:775) | 5,906,432 | **516,096** |
| rootfs-xip2.cramfs | 1,638,400 (Makefile:776) | 1,449,984 | 188,416 |

**The flow** (Makefile:935-966, 1064-1090; memory s31-x11-libs-ship-path):
- `xip-fast` copies the overlay into `build/buildroot/target`.
- `xip-image` then runs rootfs/mkxipstage.py on `XIP_ROOTS` against the
  target tree. It walks each root's NEEDED closure, honours `XIP_SKIP`, and
  recreates the soname links (mkxipstage.py:103-134).
- Image 2 excludes everything already in image 1.
- A root that matches nothing prints `WARNING: root matched nothing` and is
  skipped (mkxipstage.py:58).
- An image over its partition fails the build (Makefile:1189-1190).

**libGL goes to image 1, and that change is made** (additive, Makefile:934
and the tail of `XIP_ROOTS`):

```
+XIP_ROOTS_GL ?= usr/lib/libGL.so.1.2.0
 XIP_ROOTS ?= ... \
-	usr/bin/xfilesctl usr/bin/s31-open usr/bin/s31-thumb usr/bin/xfilesthumb usr/bin/s31-thumbs
+	usr/bin/xfilesctl usr/bin/s31-open usr/bin/s31-thumb usr/bin/xfilesthumb usr/bin/s31-thumbs \
+	$(XIP_ROOTS_GL)
```

- libGL comes from the Buildroot target tree, installed by the package, not
  from x11-stage.
- Its closure is libX11.so.6, libXext.so.6 and libc, all already in image 1,
  so it adds only itself.
- Until `BR2_PACKAGE_S31_LIBGL=y` the root matches nothing: one warning, no
  change to the image.
- `XIP_ROOTS_GL=` on the command line takes it out again.
- Ship path: `s31-libgl-rebuild` (or `rootfs`), then `xip-fast` or
  `xip-rootfs`, then `sync-images`, then `flash-xip-rootfs` (the first
  image).

**libGLU and freeglut: proposed, not applied, pending measurement.** The
plan (stage 7) wants both in image 1. The aarch64 reference builds give an
order of magnitude only:

| library | aarch64 reference .text + .rodata | RV32C -Os expectation (unmeasured) |
|---|---|---|
| libGLU.so.1.3.1 | 475 kB + 23 kB (+ 58 kB eh_frame) | about 250-350 kB, plus the static libsupc++ pieces |
| libglut.so.3.13.0 | 129 kB + 80 kB | about 150-200 kB |
| libGL.so.1 (erysdren base, plan 2.1) | 36.6 kB .text at -Os | 60-150 kB once F1-F7 and GLX land |

- libGL, GLU and freeglut together probably exceed the 516 kB left in
  image 1. Image 2 has 188 kB.
- **Proposal:** after the first build, read the three stripped sizes from
  `build/buildroot/target/usr/lib`, then:
  1. keep libGL in image 1 (done);
  2. add `usr/lib/libglut.so.3.13.0` to `XIP_ROOTS_GL` if it fits. Every GLUT
     demo runs its event loop out of it, so it is hot;
  3. treat libGLU as the candidate to leave on the card (`XIP_SKIP +=
     libGLU.so.1.3.1`). GLU apps touch mipmap, project and parts of tess at
     setup, and NURBS never. Per memory s31-xip-is-for-things-that-run, text
     that is mostly never faulted is the wrong use of flash. Measure its
     resident text in smaps on the first GLU app (plan 2.3) before deciding.
- If both must be in flash, the room has to come from evicting something
  else. That is an owner decision, not a packaging one.
- With the post-build fallback of 4.3, `libstdc++.so.6.0.34` must be in
  `XIP_SKIP`: 1.69 MB does not fit either image.

## 7. SDL2 without SDL's GL renderer (plan 4.2 option B): written, inert

**The mechanism** (external.mk, active only when
`BR2_PACKAGE_S31_LIBGL_SDL2_NO_GL_RENDERER=y`):
- That symbol `depends on BR2_PACKAGE_SDL2_OPENGL` and defaults to y. It is
  therefore invisible and off today, and turns on by default the moment
  stage 5b enables SDL2 GL.
- The hook:

```
define S31_SDL2_DROP_GL_RENDERER
	$(SED) '/^#define SDL_VIDEO_RENDER_OGL 1$$/d' $(@D)/include/SDL_config.h
	! grep -q '^#define SDL_VIDEO_RENDER_OGL ' $(@D)/include/SDL_config.h
	grep -q '^#define SDL_VIDEO_OPENGL_GLX 1' $(@D)/include/SDL_config.h
endef
SDL2_POST_CONFIGURE_HOOKS += S31_SDL2_DROP_GL_RENDERER
```

**Evidence (SDL2 2.32.10):**
- **Where the define comes from.** configure.ac:5 writes
  `include/SDL_config.h` from SDL_config.h.in (which carries
  `#undef SDL_VIDEO_RENDER_OGL` at :423). On Linux, `CheckOpenGL`
  (configure.ac:2566-2582, called at :3827) defines `SDL_VIDEO_OPENGL` and
  `SDL_VIDEO_RENDER_OGL` together when GL/gl.h+glext.h compile.
- **There is no configure switch for the render driver alone.**
  `--disable-render` (configure.ac:514) removes the whole render subsystem.
- **The fallback value.** With the define gone, SDL_internal.h:142-144
  defines it to 0, so SDL_render.c:113 leaves "opengl" out of the renderer
  list, and SDL_render_gl.c and SDL_shaders_gl.c compile empty (their
  line 23).
- **GLX survives.** `SDL_VIDEO_OPENGL_GLX` comes from CheckGLX
  (configure.ac:2516-2530).
- **Nothing regenerates the header during the build.** Makefile.in has no
  rule that re-runs config.status for the header, and its only
  config.status rule is for `Makefile` (:147-148).
- **The installed header** (Makefile.in:182) is the edited one, so apps
  compiled against SDL2 see the same value.

The two checks turn a changed header format into a build failure instead of
silently shipping SDL's GL renderer. The hook ordering is the same deferred
recipe-time expansion as section 5 (pkg-generic.mk:264).

**Stage 5a (SDL 1.2 GL on)** is `BR2_PACKAGE_S31_LIBGL_SDL_OPENGL=y`. It is
off, and not in the proposed defconfig lines.

## 8. Open issues

1. **xlite exports (4.2) block stage 2.** glxgears needs 2 symbols; freeglut
   needs 7 libX11, 2 libXrandr and `_XUnknownNativeEvent` for libXi. These
   come from a source scan. Confirm them with the nm check on the built
   binaries.
2. **gl/build.sh must implement the section 2 contract.** It does not exist
   yet: gl/ holds only include/ and ref-apps/ at the time of writing. Until
   it does, `s31-libgl` fails with "gl/build.sh is missing", which is
   deliberate.
3. **Two copies of the GL headers.** mesa3d-headers (Mesa 26.1.2) is in
   staging, and gl/include (libglvnd) is in the repo.
   - If gl/build.sh adds `-I gl/include`, libGL compiles against one copy and
     every consumer against the other. They are Khronos-generated and
     ABI-identical, but a GL_GLEXT_VERSION skew could change which prototypes
     glext.h declares.
   - Simplest fix: build.sh uses gl/include only when `S31GL_CC` is unset (the
     host build), and the staging headers otherwise.
4. **LICENSE.** `S31_LIBGL_LICENSE_FILES` picks up `gl/LICENSE` when it
   exists, and is empty otherwise, which only `legal-info` notices. gl/
   should carry TinyGL's MIT COPYING plus our notice.
5. **Local package, no auto-rebuild.** `rootfs` already force-rebuilds
   s31-tools (Makefile:855). The proposed equivalent for libGL (not applied,
   because it changes a build recipe) is:
   ```
   +	grep -q '^BR2_PACKAGE_S31_LIBGL=y' $(BUILDROOT_OUT)/.config && \
   +		$(BUILDROOT_MAKE) s31-libgl-rebuild || true
   ```
   It needs the guard: `make <pkg>-rebuild` builds a package even when it is
   not enabled.
6. **libglew is built for nothing** (section 3). Leaving it costs build time
   and SD space only. Removing it means overriding
   `MESA3D_DEMOS_DEPENDENCIES`. Dependencies cannot be changed after the
   eval (section 5), so it would need a rule-level hack. Not worth it.
7. **mesa3d-headers downloads the whole Mesa 26.1.2 tarball** for the
   headers. That is a one-off, cached in dl/.
8. **Unexercised:**
   - the SDL 1.2 order-only rule `$(SDL_TARGET_CONFIGURE): | libgl libglu`
     (GL-on arm only);
   - the libGLU `-static-libstdc++` link and its check;
   - the SDL2 hook.

   All are inert or harmless with today's config. None has run.

## 9. What was checked, and how

- **Kconfig, executed.** kconfiglib 14.1.0 loaded the real tree
  (buildroot/Config.in with the generated br2-external fragments) and the
  committed defconfig.
  - The new Config.in parses.
  - `BR2_PACKAGE_PROVIDES_LIBGL` resolves to `"s31-libgl"`.
  - The four stage-2 lines enable exactly the set in section 3.
  - `S31_LIBGL_SDL_OPENGL=y` selects LIBGLU.
  - `S31_LIBGL_SDL2_NO_GL_RENDERER` defaults to y once `SDL2_OPENGL=y`, and
    is n otherwise.
- **Make semantics: read from source, not executed.** No make of any kind
  was run, per the task rules. The include order, deferred expansion and
  eval-time dependency claims come from buildroot/Makefile and
  package/pkg-*.mk, cited above.
- **Symbol gaps: measured.** They were checked against the real overlay
  binaries by parsing ELF dynsym (section 4.2). The consumers side is a
  source scan.
- **libstdc++: measured.** PIC-ness was counted from libstdc++.a
  relocations, and its size from the section headers of libstdc++.so.6.
- **SDL facts: read from** the shipped config.log and SDL_config.h files
  (under the research scratchpad), and from SDL-1.2.15 / SDL2-2.32.10
  source.
- **Not run:** any Buildroot configure, build or install of these packages;
  any board test.
