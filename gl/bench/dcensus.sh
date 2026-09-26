#!/bin/bash
# dcensus.sh OBJDIR - soft-double and libm call sites in each library
# object, by function (objdump -dr, R_RISCV_CALL(_PLT) relocations).
# s31, MIT.
OD=${OD:-$HOME/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin/riscv32-esp-elf-objdump}
cd "$1"
for o in *.o; do
	$OD -dr "$o" | awk -v o="$o" '
		/^[0-9a-f]+ <[^.].*>:$/ { fn = $2; gsub(/[<>:]/, "", fn) }
		/R_RISCV_CALL/ && $3 ~ /^(__[a-z]+df[a-z0-9]*|sqrt|sin|cos|sincos|pow|exp|log|floor|ceil|fmod|sqrtf|sinf|cosf|sincosf|powf|expf|logf|floorf|ceilf|fmodf|fabs)$/ { print o, fn, $3 }'
done | sort | uniq -c
