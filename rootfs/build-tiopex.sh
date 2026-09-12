#!/bin/sh
# Cross-build tiopex/sdlquake for the board, unmodified: TyrQuake's engine
# with sdlquake's 8-bit SDL 1.2 video glue, written for MIPS handhelds
# ("SDL 2.0 is too slow on those platforms"). Its Makefiles are per device
# and hardcode a toolchain, so this reuses the K3S file list (generic
# vid_sdl.c, not the RS97 one) with our toolchain and flags.
#
#   tyrquake/sdlquake-master/   the unpacked GitHub master tarball (gitignored)
#   rootfs/tiopex-quake         the output binary (+ .dbg unstripped)
set -e
SRC=/src/tyrquake/sdlquake-master
OUT=/src/rootfs/tiopex-quake
CC=/src/build/buildroot/host/bin/riscv32-esp-linux-musl-gcc
SDLCFG=/src/build/buildroot/staging/usr/bin/sdl-config
CFLAGS="-std=gnu11 -O2 -ffast-math -fsingle-precision-constant -fno-common -w \
 -DNQ_HACK -DNDEBUG -DELF -DTYR_VERSION=0.62 -DQBASEDIR=. -Isource $($SDLCFG --cflags)"
cd "$SRC"
mkdir -p /tmp/tpq; OBJS=""
for f in source/alias_model.c source/cd_common.c source/cd_null.c source/chase.c source/cl_demo.c source/cl_input.c source/cl_main.c source/cl_parse.c source/cl_tent.c source/cmd.c source/common.c source/console.c source/crc.c source/cvar.c source/d_edge.c source/d_fill.c source/d_init.c source/d_modech.c source/d_part.c source/d_polyse.c source/d_scan.c source/d_sky.c source/d_sprite.c source/d_surf.c source/d_vars.c source/draw.c source/host_cmd.c source/host.c source/keys.c source/mathlib.c source/menu.c source/model.c source/net_common.c source/net_dgrm.c source/net_loop.c source/net_main.c source/net_none.c source/nonintel.c source/pr_cmds.c source/pr_edict.c source/pr_exec.c source/r_aclip.c source/r_alias.c source/r_bsp.c source/r_draw.c source/r_edge.c source/r_efrag.c source/r_light.c source/r_main.c source/r_misc.c source/r_model.c source/r_part.c source/r_sky.c source/r_sprite.c source/r_surf.c source/r_vars.c source/rb_tree.c source/sbar.c source/screen.c source/sdl_common.c source/shell.c source/snd_dma.c source/snd_mem.c source/snd_mix.c source/snd_sdl.c source/sprite_model.c source/sv_main.c source/sv_move.c source/sv_phys.c source/sv_user.c source/sys_unix.c source/vid_mode.c source/vid_sdl.c source/view.c source/wad.c source/world.c source/zone.c ; do
	o=/tmp/tpq/$(basename ${f%.c}).o
	$CC $CFLAGS -c "$f" -o "$o"
	OBJS="$OBJS $o"
done
$CC -O2 -o "$OUT.dbg" $OBJS $($SDLCFG --libs) -lz -lm
cp "$OUT.dbg" "$OUT"; /src/build/buildroot/host/bin/riscv32-esp-linux-musl-strip "$OUT"
ls -la "$OUT"
