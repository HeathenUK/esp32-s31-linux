# tyrgl-setup.sh - ON the board: the card-side layout stock TyrQuake 0.71 GL
# (rootfs/build-tyrglquake-x11.sh -> /root/tyrgl/tyr-glquake) runs from.
#
#   /root/tyrgl/id1/pak0.pak   symlink to the shareware pak in /root/quake
#
# Its own basedir, so its config.cfg (id1/config.cfg, written at exit) and
# -condebug log (id1/qconsole.log) never mix with QuakeSpasm's or the
# software Quakes' in /root/quake/id1: they share cvar names (gamma,
# vid_*) with different meanings. TyrQuake stuffs "+commands" from the
# command line itself (cmd.c Cmd_StuffCmds_f reads com_argv), so unlike
# QuakeSpasm a timedemo needs no autoexec: "+timedemo demo1".
# With HOME=/root/tyrgl (the menu's), TyrQuake's writable game directory -
# config.cfg and the -condebug qconsole.log - is
# /root/tyrgl/.tyrquake/id1 (common.c COM_InitFilesystem adds $HOME/.tyrquake
# last).
# Idempotent. Ship the binary with scripts/board/deploy.py
# rootfs/tyr-glquake-x11 /root/tyrgl/tyr-glquake.
d=/root/tyrgl
mkdir -p $d/id1 $d/.tyrquake/id1
ln -sf /root/quake/id1/pak0.pak $d/id1/pak0.pak
ls -la $d $d/id1
