#!/bin/sh
# QEMU: per-sample instruction count of oplbench2 for each helper build, and
# the output hash (identical hash = bit-identical audio). cwd = out/.
cnt() { qemu-riscv32 -cpu max -plugin /plugins/libinsn.so -d plugin ./oplbench-$1.q music.mus $2 $3 2>&1; }
printf "%-5s %-4s %12s %10s\n" song impl insn/sample hash
for song in 36 0 9 5; do
	for v in ${VARIANTS:-lg old v2 v2n}; do
		a=$(cnt $v $song 1 | awk '/total insns/{print $3}')
		o=$(cnt $v $song 3); b=$(echo "$o" | awk '/total insns/{print $3}')
		h=$(echo "$o" | awk -v s=$song '$1==s{print $NF}')
		echo "$a $b" | awk -v s=$song -v v=$v -v h=$h '{printf "%-5s %-4s %12.1f %10s\n", s, v, ($2-$1)/(2*44100), h}'
	done
done
