################################################################################
#
# jwm
#
################################################################################

JWM_VERSION = 2.4.6
JWM_SOURCE = jwm-$(JWM_VERSION).tar.xz
JWM_SITE = https://github.com/joewing/jwm/releases/download/v$(JWM_VERSION)
JWM_LICENSE = MIT
JWM_LICENSE_FILES = LICENSE

JWM_DEPENDENCIES = xlib_libX11 xlib_libXft xlib_libXpm xlib_libXext \
	fontconfig host-pkgconf

# Everything optional is off EXCEPT xrender, which is not optional in practice:
# Xft draws through XRender, so --disable-xrender silently disables the Xft path
# that --enable-xft asks for. The symptom is "could not load font" for a font
# fontconfig resolves perfectly well, which sends you looking at fonts rather
# than at the build flags. libXrender is already in the flash image anyway.
#
# cairo, librsvg, libpng, libjpeg and Xinerama stay off - icon and background
# handling this board does not need.
JWM_CONF_OPTS = \
	--disable-cairo \
	--disable-rsvg \
	--disable-jpeg \
	--disable-png \
	--disable-xinerama \
	--disable-debug \
	--enable-xft \
	--enable-xrender \
	--enable-xpm

$(eval $(autotools-package))
