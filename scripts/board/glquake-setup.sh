# glquake-setup.sh - ON the board: the card-side pieces GLQuake (stock
# QuakeSpasm 0.96.3, /root/quake/quakespasm) needs besides its binary.
#
#   /root/quake/td/id1/autoexec.cfg   "timedemo demo1", beside a pak0.pak symlink
#
# Why a second basedir: QuakeSpasm IGNORES "+command" arguments with the
# shareware pak - Cmd_StuffCmds_f reads the "cmdline" cvar, and
# COM_CheckRegistered (common.c) sets that only when gfx/pop.lmp exists. So
# "+timedemo demo1" never reaches the console and the attract loop runs
# instead. quake.rc execs autoexec.cfg before startdemos, and startdemos does
# nothing while a demo plays, so an autoexec in its own basedir is the app's
# own way to start a timedemo; the play entries keep /root/quake untouched.
# Idempotent. Ship the binary itself with scripts/board/deploy.py
# rootfs/quakespasm /root/quake/quakespasm.
d=/root/quake/td
mkdir -p $d/id1
ln -sf /root/quake/id1/pak0.pak $d/id1/pak0.pak
printf '// timedemo basedir (scripts/board/glquake-setup.sh)\ntimedemo demo1\n' > $d/id1/autoexec.cfg
ls -la $d/id1
