#!/bin/bash
# Re-runs the probes cited in the s31fp evolution design and records command, input hashes and stdout.
. /home/user/esp32-s31-linux/tools/cloud/env.sh
P=/home/user/esp32-s31-linux/build/cloud/probes
E=$P/evolution-evidence
rec(){ name=$1; shift; out=$E/$name.txt
  { echo "# probe: $name"; echo "# date: $(date -u +%FT%TZ)"; echo "# cwd: $PWD"; echo "# cmd: $*";
    echo "# qemu: $(qemu-riscv32 --version 2>/dev/null | head -1)"; echo "# S31_QEMU_CPU=${S31_QEMU_CPU:-board}";
    for f in $INPUTS; do echo "# md5 $(md5sum $f)"; done; echo "# ---- stdout/stderr ----";
    t0=$SECONDS; "$@" 2>&1; echo "# rc=$? elapsed=$((SECONDS-t0))s"; } > $out 2>&1; }
cd $P/arith;  INPUTS="fl.c fl" rec floor_ceil_exact s31-qemu ./fl 0 2000000
INPUTS="flneg.c flneg" rec floor_negctl_noNX s31-qemu ./flneg 0 20000
INPUTS="flneg2.c flneg2" rec floor_negctl_sign s31-qemu ./flneg2 0 20000
INPUTS="rmm.c rmm" rec sqrt_rmm_eq_rtz env S31_QEMU_CPU=max s31-qemu ./rmm
INPUTS="ud.c ud" rec udivdi3_exact s31-qemu ./ud 0 5000000
for m in 10 11 12 13; do for n in 100000 200000; do INPUTS="fl.c fl" rec floor_icount_m${m}_n${n} s31-qemu -plugin /plugins/libinsn.so -d plugin ./fl $m $n; done; done
cd $P/safety/order; INPUTS="app.c pre.c libA.c libD.c build.sh" rec ctor_order_app env LD_PRELOAD=./libpre.so LD_LIBRARY_PATH=. s31-qemu ./app
INPUTS="app.c pre.c" rec ctor_order_app2 env LD_PRELOAD=./libpre.so LD_LIBRARY_PATH=. s31-qemu ./app2
INPUTS="app.c pre.c" rec ctor_order_two_preloads env LD_PRELOAD=./libB.so:./libpre.so LD_LIBRARY_PATH=. s31-qemu ./app
cd $P/rewriter/claims; INPUTS="cave.py /home/user/esp32-s31-linux/rootfs/s31fp/v2/sigs3.h /home/user/esp32-s31-linux/rootfs/tiopex-quake.dbg /home/user/esp32-s31-linux/rootfs/audiofp/oplbench" rec cave_sizes python3 cave.py /home/user/esp32-s31-linux/rootfs/tiopex-quake.dbg /home/user/esp32-s31-linux/rootfs/audiofp/oplbench
INPUTS="dround.c dround" rec double_rounding s31-qemu ./dround 2000000
INPUTS="tiny.c tiny" rec double_rounding_tiny s31-qemu ./tiny 400000
cd $P/placement; INPUTS="pages.py" rec dso_copy_pages python3 pages.py *.nm /home/user/esp32-s31-linux/artifacts/gl/glquake/nm/libX11*.nm /home/user/esp32-s31-linux/artifacts/gl/glquake/nm/libasound*.nm
cd $P/simd; INPUTS="rseq.c rseq" rec rseq_enosys s31-qemu ./rseq
INPUTS="chunk.c chunk" rec rseq_chunk_abort s31-qemu ./chunk
