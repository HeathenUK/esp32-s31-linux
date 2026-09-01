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

# A real directory on the SD ext4, so pairings persist across boots. This
# was a symlink into /run (tmpfs) from before Bluetooth had any real use,
# which silently forgot every pairing at reboot.
rm -rf "${target_dir}/var/lib/bluetooth"
mkdir -p "${target_dir}/var/lib/bluetooth"

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
#
# `if`, not `[ -e ] &&`: this script runs under `set -e`, and a trailing
# `&&` list that evaluates false IS a failing command. The rename only
# happens on the first build - afterwards the S30 name is already gone - so
# the second and every later incremental rootfs build died here, in
# target-finalize, printing nothing whatsoever. Buildroot reports it as
# `Error 1` from a script that produced no output.
if [ -e "${TARGET_DIR}/etc/init.d/S30dbus-daemon" ]; then
	mv "${TARGET_DIR}/etc/init.d/S30dbus-daemon" \
	   "${TARGET_DIR}/etc/init.d/S45dbus-daemon"
fi
if [ -e "${TARGET_DIR}/etc/init.d/S40bluetoothd" ]; then
	mv "${TARGET_DIR}/etc/init.d/S40bluetoothd" \
	   "${TARGET_DIR}/etc/init.d/S46bluetoothd"
fi

# syslogd and klogd start at S01/S02, before S05xip mounts the overlays, so
# their busybox text was SD-ext4-backed - measured 412 KB and 556 KB resident
# against ~50 KB when the same applets run from the cramfs XIP image, and the
# SD copy of busybox ends up cached twice. Start them after the overlay.
if [ -e "${TARGET_DIR}/etc/init.d/S01syslogd" ]; then
	mv "${TARGET_DIR}/etc/init.d/S01syslogd" \
	   "${TARGET_DIR}/etc/init.d/S06syslogd"
fi
if [ -e "${TARGET_DIR}/etc/init.d/S02klogd" ]; then
	mv "${TARGET_DIR}/etc/init.d/S02klogd" \
	   "${TARGET_DIR}/etc/init.d/S07klogd"
fi

# The X11 shim libraries ship in XIP and ONLY in XIP. A stock copy left in
# the target becomes the SD lower layer under the overlay, and an overlay
# failure then silently demotes every client to the fat upstream libs
# instead of failing loudly. Delete them from the target outright; the
# overlay staging is the sole source. (Live card cleaned 2026-09-01; this
# makes re-imaging preserve that.)
for lib in libX11.so.6.4.0 libXt.so.6.0.0 libXaw7.so.7.0.0 libXmu.so.6.2.0 \
           libICE.so.6.3.0 libSM.so.6.0.1 libXext.so.6.4.0 libXpm.so.4.11.0 \
           libXrender.so.1.3.0 libXft.so.2.3.9 libfontconfig.so.1.16.0 \
           libXcursor.so.1.0.2; do
    ov="${BR2_EXTERNAL_ESP32_S31_PATH}/board/esp32-s31/overlay/usr/lib/${lib}"
    tg="${TARGET_DIR}/usr/lib/${lib}"
    if [ -f "$tg" ] && [ -f "$ov" ] && ! cmp -s "$tg" "$ov"; then
        rm -f "$tg"
    fi
done
