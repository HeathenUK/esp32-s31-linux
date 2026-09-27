#!/bin/sh
# Provision a board-less x86_64 Linux host (a Claude Code cloud container, or
# any Ubuntu 24.04 box) to cross-compile for the S31 and run the result under
# QEMU user-mode. Idempotent: each step checks for its output and skips.
#
#   sh tools/cloud/setup.sh          # or: make cloud-setup
#   . tools/cloud/env.sh             # PATH for s31-cc / s31-qemu / qemu
#
# What it installs, all under build/cloud/ (gitignored) unless stated:
#   toolchain/   the SHIPPING toolchain, via the existing `make toolchain`
#   qemu/        qemu-riscv32 10.0.6 with TCG plugins (libinsn.so for icount;
#                V2-REPORT.txt's counts were taken on 10.0). Ubuntu's own
#                qemu-user is 8.2 and has no plugin support.
#   sysroot/     a QEMU-ONLY musl 1.2.5 + libgcc (see below)
#   /src         symlink to the repo, the path the Docker build contract and
#                the s31fp build scripts use
#
# Why a separate sysroot: the shipping toolchain's GCC carries Espressif's
# xesploop patch and its musl the xespv string patch (both listed in
# toolchain/.../s31-patch-sha256.txt). Its libgcc __addtf3/__subtf3/__multf3
# contain esp.lp.setup hardware loops and its memcpy/memchr/strcmp/memcmp use
# esp.vld/vst PIE vector ops. QEMU executes neither, so even printf("%g")
# dies with SIGILL. The sysroot is the same musl version built from upstream
# source, and those three libgcc members rebuilt from the same Espressif GCC
# revision, both for plain rv32imafc. Results are the same; only the loop and
# copy code differ. It is for QEMU runs only - never ship anything linked
# against it.
#
# The kernel cannot be built here: linux-71-port/ is not in the repository.
set -eu

R=$(cd "$(dirname "$0")/../.." && pwd)
C=$R/build/cloud
S=$C/src
TC=$R/toolchain/riscv32-esp-linux-musl
X=$TC/bin/riscv32-esp-linux-musl-
NOESP=-march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_zba_zbb_zbc_zbs
JOBS=$(nproc)

QEMU_V=10.0.6
QEMU_SHA=c7c40c4b166871e775804e97fce4da65665d1cc93a5c6c9e2ede9d9ee992e7a0
MUSL_V=1.2.5
MUSL_SHA=a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4
# toolchain/riscv32-esp-linux-musl/crosstool-ng.config CT_GCC_DEVEL_REVISION
GCC_REV=0dbf584943ac179894690b389f3a37926bb4cd33

say() { printf '\n=== %s\n' "$*"; }
fetch() { # url sha256 out
	[ -f "$3" ] && echo "$2  $3" | sha256sum -c --quiet - 2>/dev/null && return
	curl -fsSL --retry 3 -o "$3.part" "$1"
	echo "$2  $3.part" | sha256sum -c --quiet -
	mv "$3.part" "$3"
}
SUDO=; [ "$(id -u)" = 0 ] || SUDO=sudo

mkdir -p "$S"

say "host packages"
need=
for p in build-essential curl xz-utils git python3 python3-venv ninja-build \
	pkg-config libglib2.0-dev flex bison device-tree-compiler bc cpio rsync gawk; do
	dpkg -s "$p" >/dev/null 2>&1 || need="$need $p"
done
if [ -n "$need" ]; then
	$SUDO apt-get update -qq
	DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y -qq $need >/dev/null
fi
echo ok

say "toolchain (make toolchain)"
[ "$(uname -m)" = x86_64 ] || { echo "the released toolchain is x86_64 only" >&2; exit 1; }
make -C "$R" --no-print-directory toolchain

say "/src -> $R"
if [ "$(readlink /src 2>/dev/null)" != "$R" ]; then
	if [ -e /src ] && [ ! -L /src ]; then
		echo "/src exists and is not a symlink - leaving it alone" >&2
	else
		$SUDO ln -sfn "$R" /src
	fi
fi
echo ok

say "qemu-riscv32 $QEMU_V (plugins)"
if [ -x "$C/qemu/bin/qemu-riscv32" ] && [ -f "$C/qemu/plugins/libinsn.so" ] &&
	"$C/qemu/bin/qemu-riscv32" --version | grep -q "$QEMU_V"; then
	echo "already built"
else
	fetch "https://download.qemu.org/qemu-$QEMU_V.tar.xz" "$QEMU_SHA" "$S/qemu-$QEMU_V.tar.xz"
	rm -rf "$S/qemu-$QEMU_V"
	tar -xJf "$S/qemu-$QEMU_V.tar.xz" -C "$S"
	(cd "$S/qemu-$QEMU_V" &&
		./configure --prefix="$C/qemu" --target-list=riscv32-linux-user \
			--enable-plugins --disable-docs --disable-werror >"$S/qemu-configure.log" 2>&1 &&
		make -j"$JOBS" >"$S/qemu-build.log" 2>&1 &&
		make install >>"$S/qemu-build.log" 2>&1 &&
		make -j"$JOBS" plugins >>"$S/qemu-build.log" 2>&1) ||
		{ echo "qemu build failed - see $S/qemu-*.log" >&2; exit 1; }
	mkdir -p "$C/qemu/plugins"
	cp "$S/qemu-$QEMU_V"/build/tests/tcg/plugins/*.so "$C/qemu/plugins/"
	cp "$S/qemu-$QEMU_V"/build/contrib/plugins/*.so "$C/qemu/plugins/" 2>/dev/null || true
	"$C/qemu/bin/qemu-riscv32" --version | head -1
fi
# icount.sh / oplcount.sh use the container path /plugins/libinsn.so
[ "$(readlink /plugins 2>/dev/null)" = "$C/qemu/plugins" ] || [ -e /plugins ] ||
	$SUDO ln -sfn "$C/qemu/plugins" /plugins

say "QEMU sysroot: libgcc without xesploop"
SR=$C/sysroot
SF=$S/softfp-$GCC_REV
if [ ! -f "$SR/lib/libgcc.a" ]; then
	mkdir -p "$SF" "$SR/lib"
	G=https://raw.githubusercontent.com/espressif/gcc/$GCC_REV
	while read -r sha path; do
		fetch "$G/$path" "$sha" "$SF/$(basename "$path")"
	done <<-EOF
	32928b45f0903afd120cbd22e0a92edd4d8667a4de890734da6ef58156fb6134 include/longlong.h
	7fc42415f61db8c0846a5ce0d6e76959bf6e410eb1c9208cf461afd771c712d5 libgcc/soft-fp/op-1.h
	59ac7703ba552e1afd2cf073cf22ea40b1db25709bfe63c184749a7fe1b2b49f libgcc/soft-fp/op-2.h
	9fa281109906af976c5833b80996aba63c88cf6ade53945e9857b4c3ce967190 libgcc/soft-fp/op-4.h
	2ebf5df13b9643ea3f85959ec8427ea2810dca3db68f42462b8f897fe5a91962 libgcc/soft-fp/op-8.h
	d1d425b8db4c446d78143d39ad4d40e4489c78a8dae229111c4789d95e2ff1eb libgcc/soft-fp/op-common.h
	c6529400e44b7931eb00cbe54e0725da1f22bb06c8e13e0f71adaeb862f8fb40 libgcc/soft-fp/quad.h
	5732ddd92e309a9a78b60c2fa833c74abb40c00cde35328de135b2312b1fce09 libgcc/config/riscv/sfp-machine.h
	e8183cf960a0771a80d5e33e58ea6d64150ed213d3e107b3cdbc88f5f921cd08 libgcc/soft-fp/soft-fp.h
	23a4e6349d2d27435736acc7f14241f1277acca5a27e3545e67267610bdea8fa libgcc/soft-fp/addtf3.c
	844b3e9141e55278efeaef5b9d9570497cc13264a5895e3f1fbee1666eea9008 libgcc/soft-fp/multf3.c
	2c77427bc17ed180c7425a405ef72767cd29b96bdc41de67a1b80c09cca9c320 libgcc/soft-fp/subtf3.c
	EOF
	W=$(mktemp -d)
	cp "$("${X}gcc" -print-libgcc-file-name)" "$W/libgcc.a"
	for f in addtf3 subtf3 multf3; do
		"${X}gcc" $NOESP -mabi=ilp32 -O2 -fPIC -I"$SF" -c -o "$W/$f.o" "$SF/$f.c"
	done
	(cd "$W" && "${X}ar" d libgcc.a addtf3.o subtf3.o multf3.o && "${X}ar" rs libgcc.a addtf3.o subtf3.o multf3.o)
	mv "$W/libgcc.a" "$SR/lib/libgcc.a"
	rm -rf "$W"
fi
echo ok

say "QEMU sysroot: musl $MUSL_V without xespv"
if [ ! -f "$SR/lib/libc.so" ]; then
	fetch "https://musl.libc.org/releases/musl-$MUSL_V.tar.gz" "$MUSL_SHA" "$S/musl-$MUSL_V.tar.gz"
	rm -rf "$S/musl-$MUSL_V"
	tar -xzf "$S/musl-$MUSL_V.tar.gz" -C "$S"
	(cd "$S/musl-$MUSL_V" &&
		CROSS_COMPILE="$X" CC="${X}gcc" CFLAGS="$NOESP -mabi=ilp32 -O2" LIBCC="$SR/lib/libgcc.a" \
			./configure --target=riscv32-esp-linux-musl --prefix=/ --syslibdir=/lib \
			--disable-wrapper >"$S/musl-configure.log" 2>&1 &&
		make -j"$JOBS" >"$S/musl-build.log" 2>&1 &&
		make DESTDIR="$SR" install >>"$S/musl-build.log" 2>&1) ||
		{ echo "musl build failed - see $S/musl-*.log" >&2; exit 1; }
	# the dynamic loader IS libc.so; install makes it an absolute symlink
	ln -sfn libc.so "$SR/lib/ld-musl-riscv32-sf.so.1"
	cp -a "$TC/riscv32-esp-linux-musl/lib/libgcc_s.so"* "$SR/lib/"
fi
echo ok

say "no esp.* instructions left in the QEMU sysroot"
bad=$("${X}objdump" -d "$SR/lib/libc.so" "$SR/lib/libc.a" "$SR/lib/libgcc.a" 2>/dev/null |
	awk '/^[0-9a-f]+ <[^.].*>:/{f=$2} $3 ~ /^esp\./{print f}' | sort -u | tr '\n' ' ')
# libgcc's __strub_leave keeps one; it only runs under -fstrub
bad=$(echo "$bad" | sed 's/<__strub_leave>://')
[ -z "$(echo "$bad" | tr -d ' ')" ] || { echo "still present: $bad" >&2; exit 1; }
echo ok

say "smoke test"
sh "$R/tools/cloud/check.sh"
