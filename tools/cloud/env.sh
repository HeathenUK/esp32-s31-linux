# shellcheck shell=sh
# Source me:  . tools/cloud/env.sh
# Puts s31-cc, s31-qemu, qemu-riscv32 (plugin build) and the toolchain on
# PATH, and names the shipping compiler for board binaries.
# shellcheck disable=SC3028 # bash when sourced from bash; the git fallback covers sh
_r=$(cd "$(dirname "${BASH_SOURCE:-$0}")/../.." 2>/dev/null && pwd)
[ -f "$_r/tools/cloud/setup.sh" ] || _r=$(git rev-parse --show-toplevel 2>/dev/null)
export S31_ROOT=$_r
export S31_CC=$_r/toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc
export PATH=$_r/tools/cloud:$_r/build/cloud/qemu/bin:$_r/toolchain/riscv32-esp-linux-musl/bin:$PATH
export S31_TARGET_SYSROOT=$S31_ROOT/build/cloud/sysroot
export S31_QEMU_LDFLAGS="-L$S31_TARGET_SYSROOT/lib"
unset _r
