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
TGLFLAGS="$COMMON $TGLFP -O2 -std=gnu99 -fvisibility=hidden -DNDEBUG -DTGL_FEATURE_RENDER_BITS=16 \
	-I$GL/tinygl/include -I$GL/tinygl/source \
	-Wno-unused-but-set-variable"
# (only the template leftovers - zz/sx set but unused in ztriangle.h/zline.h -
# are silenced. -Wno-maybe-uninitialized and the other suppressions hid
# nothing on either compiler, 2026-09-25, and are gone: an uninitialised
# read is exactly the warning this project has paid for ignoring.)
# the ABI layer: the STANDARD headers only
APIFLAGS="$COMMON $FP -Os -std=gnu99 -fvisibility=hidden -I$GL/include -I$GL/api \
	-I$GL/tinygl/source -Wno-unused-function"

echo "--- compiling TinyGL core"
for f in "$GL"/tinygl/source/*.c; do
	b=$(basename "$f" .c)
	case $b in glu|ostinygl) continue ;; esac
	$CC $TGLFLAGS -c "$f" -o "$OBJ/tgl_$b.o"
done

echo "--- compiling the GL ABI layer"
for f in "$GL"/api/*.c; do
	b=$(basename "$f" .c)
	[ $b = procs ] && continue      # needs the generated table: below
	$CC $APIFLAGS -c "$f" -o "$OBJ/api_$b.o"
done

echo "--- export surface (mkstubs.py)"
$NM -g --defined-only "$OBJ"/api_*.o | awk '$2 ~ /^[TW]$/ && $3 ~ /^gl[A-Z]/ {print $3}' \
	| sort -u > "$OBJ/implemented.txt"
python3 "$GL/api/mkstubs.py" "$GL/include/GL/gl.h" "$GL/include/GL/glext.h" \
	"$OBJ/implemented.txt" "$OBJ/gen_stubs.c" "$OBJ/gen_procs.inc"
$CC $APIFLAGS -c "$OBJ/gen_stubs.c" -o "$OBJ/api_gen_stubs.o"
$CC $APIFLAGS -I"$OBJ" -c "$GL/api/procs.c" -o "$OBJ/api_procs.o"
ls "$OBJ"/tgl_*.o "$OBJ"/api_*.o > "$OBJ/core.list"

GLXOBJ=""
LIBS="-lm"
if [ -z "$S31GL_NO_GLX" ] && ls "$GL"/glx/*.c >/dev/null 2>&1; then
	echo "--- compiling the GLX layer (gl/glx)"
	# default visibility: glx.h declares its functions without an
	# attribute, and every glX* name must be exported
	for f in "$GL"/glx/*.c; do
		b=$(basename "$f" .c)
		$CC $COMMON $FP -O2 -std=gnu99 -I$GL/include -I$GL/api -I$GL/glx $XINC \
			-c "$f" -o "$OBJ/glx_$b.o"
		GLXOBJ="$GLXOBJ $OBJ/glx_$b.o"
	done
	LIBS="$XLIBS -lm"
else
	echo "--- no gl/glx/*.c: building the core alone"
fi

echo "--- linking $OUT"
# -Bsymbolic-functions: internal calls bind at link time (xstubs/build.sh);
# -z defs: an unresolved symbol fails the build instead of the app's load
$CC -shared -fPIC $ARCHFLAGS $LDFLAGS -Wl,-soname,libGL.so.1 -Wl,-Bsymbolic-functions \
	${ZDEFS--Wl,-z,defs} -Wl,--gc-sections -o "$OUT" $(cat "$OBJ/core.list") $GLXOBJ $LIBS
