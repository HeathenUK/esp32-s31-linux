#!/bin/sh
# Start wpa_supplicant, then wait for association before ifupdown runs DHCP.
#
# ifupdown fires udhcpc as soon as pre-up returns. Associating with a hidden
# SSID takes about seven seconds here (scan, then the WPA handshake, both driven
# by hart0 over the private control channel), so DHCP would otherwise always
# time out and the interface would come up addressless.
#
# Polls for the association rather than sleeping a fixed time: a warm associate
# returns in about a second, and a genuinely broken one still gives up in thirty.
# Markers, kept deliberately. This script is silent for most of a minute and
# reads as a hang; these are how the boot cost was finally attributed. They
# also make the split visible - association is ~6 s and has always been fine,
# while everything AROUND it was being starved by udev's coldplug.
K() { echo "WLAN0 $(cut -d' ' -f1 /proc/uptime) $*" > /dev/kmsg; }
K "pre-up begins"
pidof wpa_supplicant >/dev/null 2>&1 || \
	wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf -D nl80211 || exit 1
K "wpa_supplicant started"

i=0
while [ $i -lt 30 ]; do
	iw dev wlan0 link 2>/dev/null | grep -q '^Connected' && { K "associated"; exit 0; }
	sleep 1
	i=$((i + 1))
done

# Do not fail the boot over it - let DHCP report the problem instead.
echo "wlan0: no association after 30s" >&2
exit 0
