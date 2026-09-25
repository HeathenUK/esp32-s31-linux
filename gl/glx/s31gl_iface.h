/*
 * s31gl_iface.h - the GLX layer's view of the rasteriser core. MIT.
 *
 * gl/api/s31gl.h is the one authority for the core API (contexts, colour
 * binding, hooks, frame_end, get_proc and the additions GLX uses:
 * set_doublebuffer, release_depth). GLX was written in parallel with it and
 * this header used to carry a fallback copy of the declarations; after
 * integration it includes the core's header and nothing else, so a change
 * there that GLX does not follow is a compile error, not a silent drift.
 * glx_core.c is the only file in gl/glx/ that calls these functions.
 */
#ifndef S31GL_IFACE_H
#define S31GL_IFACE_H
#include "s31gl.h"	/* -I gl/api (gl/api/build-lib.sh) */
#endif
