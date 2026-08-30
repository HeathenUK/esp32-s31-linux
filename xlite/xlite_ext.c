/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The XESet* extension hooks, kept in a file that does NOT include Xlibint.h.
 *
 * Xlibint.h declares each of these with its own elaborate function-pointer
 * return type - thirteen distinct shapes. Every parameter and every return is
 * a pointer, so plain pointer signatures are ABI-identical, and writing them
 * here rather than matching thirteen declarations keeps the noise out of the
 * request encoders.
 */
#include <X11/Xlib.h>

/*
 * Listed here for the stub generator, which greps the source rather than
 * expanding macros and so cannot see the names the macro below defines:
 * XLITE_IMPL(XESetCloseDisplay) XLITE_IMPL(XESetCreateGC)
 * XLITE_IMPL(XESetCopyGC) XLITE_IMPL(XESetFlushGC) XLITE_IMPL(XESetFreeGC)
 * XLITE_IMPL(XESetCreateFont) XLITE_IMPL(XESetFreeFont)
 * XLITE_IMPL(XESetWireToEvent) XLITE_IMPL(XESetEventToWire)
 * XLITE_IMPL(XESetWireToError) XLITE_IMPL(XESetError)
 * XLITE_IMPL(XESetErrorString) XLITE_IMPL(XESetPrintErrorValues)
 * XLITE_IMPL(XESetCopyEventCookie) XLITE_IMPL(XESetWireToEventCookie)
 */
#define XESET(name) \
	void *name(Display *dpy, int ext, void *proc) \
	{ (void)dpy; (void)ext; (void)proc; return NULL; }

XESET(XESetCloseDisplay)
XESET(XESetCreateGC)
XESET(XESetCopyGC)
XESET(XESetFlushGC)
XESET(XESetFreeGC)
XESET(XESetCreateFont)
XESET(XESetFreeFont)
XESET(XESetWireToEvent)
XESET(XESetEventToWire)
XESET(XESetWireToError)
XESET(XESetError)
XESET(XESetErrorString)
XESET(XESetPrintErrorValues)
XESET(XESetCopyEventCookie)
XESET(XESetWireToEventCookie)
