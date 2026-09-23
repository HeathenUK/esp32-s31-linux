#!/bin/bash
# build-oc.sh <mhz> - build an OVERCLOCK trio (loader + OpenSBI + kernel) for
# one CPU frequency into images/oc<mhz>/, leaving every source at 320.
#
# What has to move together (memory s31-clock-arm-traps): the loader's CPU
# clock (here: an in-app switch, bootloader/main/s31_oc.h), OpenSBI's
# S31_TIMEBASE_HZ, the kernel DTS timebase-frequency. The vendor clock code
# knows only 240/320: the same build patches /opt/esp-idf INSIDE the container
# for this build only (fb_div = mhz/40 on CPLL; dividers mem 2, sys 4, apb 2).
# Flash the result with scripts/board/flash-trio.sh, never with make flash-*
# (sync-images would replace it with the volume's current builds).
set -eu
cd "$(dirname "$0")/../.."
MHZ=${1:?mhz}
case $MHZ in 360|400) ;; *) echo "only 360 or 400 (fb_div 9 or 10 on a 40 MHz XTAL)"; exit 2;; esac
DTS=linux-71-port/arch/riscv/boot/dts/espressif/esp32s31.dtsi
SBI=opensbi-esp32-s31/platform/generic/espressif/esp32s31.c
OCH=bootloader/main/s31_oc.h
restore() {
	sed -i '' "s/timebase-frequency = <${MHZ}000000>;/timebase-frequency = <320000000>;/" "$DTS"
	sed -i '' "s/S31_TIMEBASE_HZ         ${MHZ}000000UL/S31_TIMEBASE_HZ         320000000UL/" "$SBI"
	sed -i '' "s/#define S31_CPU_OC_MHZ $MHZ/#define S31_CPU_OC_MHZ 0/" "$OCH"
	echo "sources restored to 320 / OC 0"
}
trap restore EXIT
grep -q "timebase-frequency = <320000000>;" "$DTS" || { echo "DTS not at 320 - refusing"; exit 3; }
grep -q "S31_TIMEBASE_HZ         320000000UL" "$SBI" || { echo "OpenSBI not at 320 - refusing"; exit 3; }
grep -q "#define S31_CPU_OC_MHZ 0" "$OCH" || { echo "s31_oc.h not at 0 - refusing"; exit 3; }
sed -i '' "s/timebase-frequency = <320000000>;/timebase-frequency = <${MHZ}000000>;/" "$DTS"
sed -i '' "s/S31_TIMEBASE_HZ         320000000UL/S31_TIMEBASE_HZ         ${MHZ}000000UL/" "$SBI"
sed -i '' "s/#define S31_CPU_OC_MHZ 0/#define S31_CPU_OC_MHZ $MHZ/" "$OCH"
OUT=images/oc$MHZ; mkdir -p "$OUT"
./docker/build.sh "python3 - <<'PY'
import re
mhz=$MHZ; fb=mhz//40
H='/opt/esp-idf/components/esp_hal_clock/esp32s31/include/hal/clk_tree_ll.h'; s=open(H).read()
s=s.replace('    case CLK_LL_PLL_320M_FREQ_MHZ:\n    default:\n        /* Configure 320M CPLL */\n        fb_div = 8;\n        ref_div = 1;\n        break;',
 '    case %d:\n        fb_div = %d;\n        ref_div = 1;\n        break;\n    case CLK_LL_PLL_320M_FREQ_MHZ:\n    default:\n        /* Configure 320M CPLL */\n        fb_div = 8;\n        ref_div = 1;\n        break;' % (mhz, fb),1)
s=s.replace('    HAL_ASSERT(cpll_freq_mhz == CLK_LL_PLL_320M_FREQ_MHZ);','    HAL_ASSERT(cpll_freq_mhz == CLK_LL_PLL_320M_FREQ_MHZ || cpll_freq_mhz == %d);' % mhz,1)
open(H,'w').write(s)
R='/opt/esp-idf/components/esp_hw_support/port/esp32s31/rtc_clk.c'; s=open(R).read()
s=s.replace('    case 320:\n        mem_divider = 2;\n        sys_divider = 3;\n        apb_divider = 2;\n        break;',
 '    case %d:\n        mem_divider = 2;\n        sys_divider = 4;\n        apb_divider = 2;\n        break;\n    case 320:\n        mem_divider = 2;\n        sys_divider = 3;\n        apb_divider = 2;\n        break;' % mhz,1)
s=s.replace('    } else if (freq_mhz == 320) {','    } else if (freq_mhz == %d) {\n        real_freq_mhz = freq_mhz;\n        source = SOC_CPU_CLK_SRC_CPLL;\n        source_freq_mhz = %d;\n        divider.integer = 1;\n    } else if (freq_mhz == 320) {' % (mhz, mhz),1)
open(R,'w').write(s)
print('vendor patched for', mhz, 'fb_div', fb, ':', s.count('freq_mhz == %d' % mhz), open(H).read().count('case %d:' % mhz))
PY
cd /src && \$S31_MAKE bootloader > /src/$OUT/build-loader.log 2>&1 && echo LOADER_OK || echo LOADER_FAIL
\$S31_MAKE opensbi > /src/$OUT/build-opensbi.log 2>&1 && echo OPENSBI_OK || echo OPENSBI_FAIL
\$S31_MAKE linux > /src/$OUT/build-linux.log 2>&1 && echo LINUX_OK || echo LINUX_FAIL
cp /src/bootloader/build/hello_world.bin /src/build/fw_payload.bin /src/build/xipImage /src/build/System.map /src/$OUT/ && ls -la /src/$OUT | awk '{print \$5, \$9}'
grep -c 'OVERCLOCK' /src/bootloader/main/main.c
strings /src/$OUT/xipImage | grep -ao '#3[0-9][0-9] SMP' | head -1"
grep -aiE "error" "$OUT"/build-*.log | grep -v "Werror\|error-inject\|fserror\|fdt_strerror\|-Wno-error" | head -5 || true
echo "trio in $OUT (loader/opensbi/kernel at $MHZ MHz). Flash with scripts/board/flash-trio.sh $OUT"
