#!/bin/sh

set -eu

target_dir="$1"

chmod 0755 "${target_dir}/init"

# The cross-toolchain includes G++, but this compact image has no C++ target
# packages. Buildroot installs libstdc++ based on toolchain capability alone;
# omit that otherwise-unused runtime to keep the squashfs inside its partition.
rm -f "${target_dir}"/lib/libstdc++.so*

rm -rf \
	"${target_dir}/tmp" \
	"${target_dir}/run" \
	"${target_dir}/var/log" \
	"${target_dir}/var/tmp"

mkdir -m 1777 "${target_dir}/tmp"
mkdir -m 0755 "${target_dir}/run" "${target_dir}/var/log"
ln -s /tmp "${target_dir}/var/tmp"

rm -rf "${target_dir}/var/lib/bluetooth"
ln -s /run/bluetooth "${target_dir}/var/lib/bluetooth"

rm -rf "${target_dir}/var/lib/seedrng"
ln -s /run/seedrng "${target_dir}/var/lib/seedrng"

rm -f "${target_dir}/etc/mtab" "${target_dir}/etc/resolv.conf"
ln -s /proc/mounts "${target_dir}/etc/mtab"
ln -s /run/resolv.conf "${target_dir}/etc/resolv.conf"

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
project_dir="$(CDPATH= cd -- "${script_dir}/../../.." && pwd)"
dtbo_dir="${project_dir}/build/linux/arch/riscv/boot/dts/espressif"
install_dir="${target_dir}/usr/lib/s31-overlays"

mkdir -p "${install_dir}"
for dtbo in "${dtbo_dir}"/esp32s31-overlay-*.dtbo; do
	[ -f "${dtbo}" ] || {
		echo "Missing S31 DT overlays in ${dtbo_dir}" >&2
		exit 1
	}
	cp "${dtbo}" "${install_dir}/"
done

# Push dbus and bluetoothd past the desktop.
#
# Buildroot's packages install them at S30/S40, ahead of lvdesk, and together
# they cost 9.0 s of a boot (dbus 6.34 s, bluetoothd 2.67 s) that nothing on
# the way to a desktop needs: lvdesk talks to wpa_supplicant over its own
# control socket and does not use dbus at all. Moving them behind lvdesk took
# the desktop from 33.2 s to 21.5 s. They still start, ~3 s later than the
# desktop, so Bluetooth is available as before.
#
# Renamed here rather than in the overlay because the overlay cannot remove the
# names Buildroot installs - it would leave both copies, and rcS would run each
# service twice.
for f in "${TARGET_DIR}/etc/init.d/S30dbus-daemon" ; do
	[ -e "$f" ] && mv "$f" "${TARGET_DIR}/etc/init.d/S45dbus-daemon"
done
for f in "${TARGET_DIR}/etc/init.d/S40bluetoothd" ; do
	[ -e "$f" ] && mv "$f" "${TARGET_DIR}/etc/init.d/S46bluetoothd"
done
