#!/bin/bash
# run_q.sh ELF [timeout_s] - one bench image under qemu-system-riscv32 with
# exact instruction counting (-icount shift=0; minstret counts instructions).
# Output: the image's own line (see q_ui.c). The .raw frame lands in the cwd.
T=${2:-300}
qemu-system-riscv32 -machine virt -cpu rv32,zba=true,zbb=true,zbc=true,zbs=true \
	-bios none -m 512M -nographic -semihosting-config enable=on,target=native \
	-icount shift=0 -kernel "$1" &
P=$!
# the watchdog must not hold our stdout: a reader waits for EOF
( sleep "$T"; kill $P ) >/dev/null 2>&1 &
K=$!
wait $P
kill $K 2>/dev/null
wait $K 2>/dev/null
[ -n "$(jobs -p)" ] && kill $(jobs -p) 2>/dev/null
exit 0
