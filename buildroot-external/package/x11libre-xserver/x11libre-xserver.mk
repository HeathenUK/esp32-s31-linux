################################################################################
#
# x11libre-xserver
#
################################################################################

X11LIBRE_XSERVER_VERSION = xlibre-xserver-25.2.2
X11LIBRE_XSERVER_SITE = $(call github,X11Libre,xserver,$(X11LIBRE_XSERVER_VERSION))
X11LIBRE_XSERVER_LICENSE = MIT
X11LIBRE_XSERVER_LICENSE_FILES = COPYING
X11LIBRE_XSERVER_INSTALL_STAGING = NO

X11LIBRE_XSERVER_DEPENDENCIES = \
	xorgproto \
	xlib_libXfont2 \
	xlib_xtrans \
	pixman \
	host-pkgconf

# Only Xfbdev. Everything else is off deliberately: this exists because the
# full Xorg server is 3.0 MB and the flash budget is 7.4 MB shared with the
# whole userspace, so anything not needed to put pixels on a framebuffer and
# read a USB keyboard and mouse is weight we cannot spend.
# Option names checked against meson_options.txt in the tree - X11Libre has
# dropped some that upstream has (there is no -Dxwayland, for instance) and
# meson hard-errors on an unknown option rather than warning.
X11LIBRE_XSERVER_CONF_OPTS = \
	-Dxfbdev=true \
	-Dxorg=false \
	-Dxephyr=false \
	-Dxvfb=false \
	-Dxnest=false \
	-Dglamor=false \
	-Ddri1=false \
	-Ddri2=false \
	-Ddri3=false \
	-Ddrm=false \
	-Dgbm=false \
	-Dglx=false \
	-Dxinerama=false \
	-Dxvmc=false \
	-Ddga=false \
	-Dvgahw=false \
	-Dint10=false \
	-Dpciaccess=false \
	-Dsystemd_logind=false \
	-Dseatd_libseat=false \
	-Dxdmcp=false \
	-Dxdm-auth-1=false \
	-Dxselinux=false \
	-Dxcsecurity=false \
	-Ddocs=false \
	-Ddevel-docs=false \
	-Dtests=false \
	-Dkdrive_evdev=true \
	-Dkdrive_kbd=true \
	-Dkdrive_mouse=true \
	-Dudev=false \
	-Dxkb_dir=/usr/share/X11/xkb \
	-Dxkb_output_dir=/var/lib/xkb

$(eval $(meson-package))
