#!/bin/sh
# build-lib.sh - compile and link libGL.so.1. Shared by gl/build.sh (RV32,
# in the build container) and gl/host-build.sh (host rig, in s31-glref).
# Not meant to be run directly.
#
# In:  GL      the gl/ directory (e.g. /src/gl)
#      CC NM   compiler and nm
#      ARCHFLAGS  extra compile flags (march/mabi)
#      XINC    -I flags for X11 headers (GLX layer only)
#      XLIBS   X libraries the GLX layer links against
#      OBJ     scratch object directory (emptied)
#      OUT     output path of libGL.so.1 (removed first)
#      LDFLAGS extra link flags (optional; Buildroot's TARGET_LDFLAGS)
#      ZDEFS   unset: -Wl,-z,defs; set empty to link without it (Buildroot)
#      S31GL_JOBS  parallel compiles (default: every host core)
#      S31GL_OBJONLY=1  compile (and generate the stubs) but do not link:
#              gl/bench links the objects into bare-metal qemu images
# Out: $OUT, $OBJ/*.o (reused by the tests), $OBJ/core.list (non-GLX
#      objects), $OBJ/gen_stubs.c + gen_procs.inc (generated, not in the tree)
set -e
rm -f "$OUT"            # a failed build must leave nothing to ship
rm -rf "$OBJ"
mkdir -p "$OBJ"

echo "--- enum values: TinyGL header against Khronos"
python3 "$GL/api/enumcmp.py" "$GL/tinygl/include/GL/gl.h" \
	"$GL/include/GL/gl.h" "$GL/include/GL/glext.h"

# no unwind tables: C code, nothing unwinds through GL; they were 44 kB of
# the 230 kB library, all of it flash the XIP image pays for
# -ffunction-sections + --gc-sections: hidden TinyGL functions nothing calls
# (upstream conversions, helpers the ABI layer replaced) leave the image
COMMON="-fPIC -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-ffunction-sections -fdata-sections -Wall -Wno-unused-parameter $ARCHFLAGS"
# Frame pointers: kept for the thin ABI and GLX layers (backtraces through
# libGL stay walkable), dropped for the rasteriser. There they reserved s0
# and added a prologue/epilogue to every filler and helper, and from XIP
# code size is time (assessment 2.2); profiling here is PC sampling
# (KALLSYMS/addr2line), which needs none. Review finding R6; the size delta
# is in artifacts/gl/phase1/INTEGRATION.md "Review fixes". S31GL_TGL_FRAMEPTR=1
# puts them back for a board A/B.
FP="-fno-omit-frame-pointer"
TGLFP=""
[ -n "$S31GL_TGL_FRAMEPTR" ] && TGLFP=$FP
# TinyGL keeps its own GL/gl.h (renamed entry points, Khronos enum values);
# glu.c is dropped (plan 2.3: GLU is Mesa's libGLU) and ostinygl.c is
# replaced by source/s31_ctx.c.
# the rasteriser is hot: -O2. The ABI layer is thin wrappers and cold stubs: -Os.
# (phase 3a G01: -fno-math-errno measured inert - GCC already emits a bare
# fsqrt.s for sqrtf here; gears/teapot/glxgears instruction counts were
# identical to the last digit, .text -16 B - so it is not used.
# -fsingle-precision-constant likewise: after G01's explicit float edits
# every TinyGL object it compiles is byte-identical to the build without
# it, so it buys nothing and would silently narrow any future double
# constant)
TGLFLAGS="$COMMON $TGLFP -O2 -std=gnu99 -fvisibility=hidden -DNDEBUG -DTGL_FEATURE_RENDER_BITS=16 \
	${S31GL_NO_RAMTEXT:+-DS31GL_NO_RAMTEXT} \
	-I$GL/tinygl/include -I$GL/tinygl/source \
	-Wno-unused-but-set-variable"
# (only the template leftovers - zz/sx set but unused in ztriangle.h/zline.h -
# are silenced. -Wno-maybe-uninitialized and the other suppressions hid
# nothing on either compiler, 2026-09-25, and are gone: an uninitialised
# read is exactly the warning this project has paid for ignoring.)
# the ABI layer: the STANDARD headers only
APIFLAGS="$COMMON $FP -Os -std=gnu99 -fvisibility=hidden -I$GL/include -I$GL/api \
	-I$GL/tinygl/source -Wno-unused-function"

# Parallel compilation (every host core unless S31GL_JOBS says otherwise).
# Each object is compiled by exactly the command the serial loops used, and
# every list below is in glob (sorted) order, so the objects, core.list and
# the link line - hence the library - are identical to a serial build.
JOBS=${S31GL_JOBS:-$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
CMDS="$OBJ/compile.cmds"
: > "$CMDS"
# one shell command per line; run them JOBS at a time, fail if any fails
run_cmds() {
	tr '\n' '\0' < "$CMDS" | xargs -0 -n 1 -P "$JOBS" sh -c
	: > "$CMDS"
}

echo "--- compiling TinyGL core, the GL ABI layer and GLX ($JOBS jobs)"
# Cold rasteriser sources - run per state change or per upload, never per
# vertex or pixel - are built -Os (flash is time from XIP, and these are
# the bulk of plan F3-F6's code that no frame loop executes). Plan F7's
# pixel paths (s31_draw: glBitmap, glDrawPixels, glReadPixels ...) and its
# state commands (s31_rpos) are cold too; its per-vertex half (s31_xform:
# texgen, user clip planes) stays -O2
# S31GL_TGLCOLD (set, even empty) replaces the list: gl/bench/build_q.sh
# compiles a pre-F3 baseline tree with it empty, as that tree's own
# build script built every TinyGL file -O2 (review P5b)
TGLCOLD=${S31GL_TGLCOLD-" raster_sel texture s31_pixels s31_state get s31_rpos s31_draw s31_zepoch s31_stencil s31_ramtext s31_tex8 "}
for f in "$GL"/tinygl/source/*.c; do
	b=$(basename "$f" .c)
	case $b in glu|ostinygl) continue ;; esac
	case "$TGLCOLD" in
	*" $b "*) echo "$CC $TGLFLAGS -Os -c '$f' -o '$OBJ/tgl_$b.o'" >> "$CMDS" ;;
	*) echo "$CC $TGLFLAGS -c '$f' -o '$OBJ/tgl_$b.o'" >> "$CMDS" ;;
	esac
done

# gl_attrib.c (generated: glVertex*, glNormal*, glColor*, glTexCoord* ...)
# is nothing but tail calls into TinyGL, once per vertex attribute in
# immediate mode. With a frame pointer each built and tore down a frame
# before its tail call - 4 of glVertex3f's 10 instructions - and the frame
# is gone at the tail call anyway, so backtraces lose nothing without it
# (phase 3a G14)
for f in "$GL"/api/*.c; do
	b=$(basename "$f" .c)
	[ $b = procs ] && continue      # needs the generated table: below
	case $b in
	gl_attrib) echo "$CC $APIFLAGS -fomit-frame-pointer -c '$f' -o '$OBJ/api_$b.o'" >> "$CMDS" ;;
	*) echo "$CC $APIFLAGS -c '$f' -o '$OBJ/api_$b.o'" >> "$CMDS" ;;
	esac
done

GLXOBJ=""
LIBS="-lm"
if [ -z "$S31GL_NO_GLX" ] && [ -z "$S31GL_OBJONLY" ] && ls "$GL"/glx/*.c >/dev/null 2>&1; then
	# default visibility: glx.h declares its functions without an
	# attribute, and every glX* name must be exported
	for f in "$GL"/glx/*.c; do
		b=$(basename "$f" .c)
		echo "$CC $COMMON $FP -O2 -std=gnu99 -I$GL/include -I$GL/api -I$GL/glx $XINC -c '$f' -o '$OBJ/glx_$b.o'" >> "$CMDS"
		GLXOBJ="$GLXOBJ $OBJ/glx_$b.o"
	done
	LIBS="$XLIBS -lm"
else
	echo "--- GLX not built (S31GL_NO_GLX / S31GL_OBJONLY, or no gl/glx/*.c): the core alone"
fi
run_cmds

echo "--- export surface (mkstubs.py)"
$NM -g --defined-only "$OBJ"/api_*.o | awk '$2 ~ /^[TW]$/ && $3 ~ /^gl[A-Z]/ {print $3}' \
	| sort -u > "$OBJ/implemented.txt"
python3 "$GL/api/mkstubs.py" "$GL/include/GL/gl.h" "$GL/include/GL/glext.h" \
	"$OBJ/implemented.txt" "$OBJ/gen_stubs.c" "$OBJ/gen_procs.inc"
echo "$CC $APIFLAGS -c '$OBJ/gen_stubs.c' -o '$OBJ/api_gen_stubs.o'" >> "$CMDS"
echo "$CC $APIFLAGS -I'$OBJ' -c '$GL/api/procs.c' -o '$OBJ/api_procs.o'" >> "$CMDS"
run_cmds
ls "$OBJ"/tgl_*.o "$OBJ"/api_*.o > "$OBJ/core.list"

# phase 4 L1 (tinygl/source/s31_ramtext.c): the hot functions of
# api/ramtext.list into one section, "s31hot_text" - a rename after
# compiling, so every object's code is exactly what the compiler made.
# Every link of these objects for Linux then needs api/ramtext.ld, which
# places that section, and api/ramtext.py fix, which writes the table the
# RAM copy needs (S31GL_RAMTEXT=1). S31GL_NO_RAMTEXT=1 builds without it
# (s31_ramtext.c then keeps the XIP copy). The bench's bare-metal images get
# the renamed section too, and link it wherever their script puts it.
OBJCOPY=${OBJCOPY:-${NM%nm}objcopy}
if [ -z "$S31GL_NO_RAMTEXT" ]; then
	# phase 6 tier 6: S31GL_HOTORDER (a file; default api/hotorder.list when
	# present, "" for none) lays the range out hottest first (ramtext.py)
	HO=${S31GL_HOTORDER-$GL/api/hotorder.list}; [ -f "$HO" ] || HO=
	python3 "$GL/api/ramtext.py" rename "$OBJCOPY" "$GL/api/ramtext.list" "$OBJ" $HO
	RAMLD="-Wl,-T,$GL/api/ramtext.ld"
fi
[ -n "$S31GL_OBJONLY" ] && exit 0

echo "--- linking $OUT"
# -Bsymbolic-functions: internal calls bind at link time (xstubs/build.sh);
# -z defs: an unresolved symbol fails the build instead of the app's load
link() {
	$CC -shared -fPIC $ARCHFLAGS $LDFLAGS -Wl,-soname,libGL.so.1 -Wl,-Bsymbolic-functions \
		${ZDEFS--Wl,-z,defs} -Wl,--gc-sections $RAMLD "$@" $(cat "$OBJ/core.list") $GLXOBJ $LIBS
}
link -o "$OUT"
if [ -n "$RAMLD" ]; then
	# L1: the same link with the linker's relocations kept (-q), read by
	# ramtext.py, which checks the two are identical and patches $OUT
	link -Wl,-q -o "$OBJ/libGL.q.so"
	python3 "$GL/api/ramtext.py" fix "$OBJ/libGL.q.so" "$OUT" || { rm -f "$OUT"; exit 1; }
fi
