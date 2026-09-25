#ifndef _tgl_features_h_
#define _tgl_features_h_

/* It is possible to enable/disable (compile time) features in this
   header file. */

#define TGL_FEATURE_ARRAYS         1
#define TGL_FEATURE_DISPLAYLISTS   1
#define TGL_FEATURE_POLYGON_OFFSET 1

/*
 * Matrix of internal and external pixel formats supported. 'Y' means
 * supported.
 * 
 *           External  8    16    24    32
 * Internal 
 *  15                 .     .     .     .
 *  16                 Y     Y     Y     Y
 *  24                 .     Y     Y     .
 *  32                 .     Y     .     Y
 * 
 *
 * 15 bpp does not work yet (although it is easy to add it - ask me if
 * you need it).
 * 
 * Internal pixel format: see TGL_FEATURE_RENDER_BITS
 * External pixel format: see TGL_FEATURE_xxx_BITS 
 */

/* enable various convertion code from internal pixel format (usually
   16 bits per pixel) to any external format */
/* s31: the colour buffer is the caller's RGB565 memory and is never
   converted (s31_ctx.c renders into it in place), so only 16 stays: the
   others were dead code in XIP flash, and 8-bit dithering could exit(1) */
#define TGL_FEATURE_16_BITS        1


#ifndef TGL_FEATURE_RENDER_BITS
//#define TGL_FEATURE_RENDER_BITS    15
#define TGL_FEATURE_RENDER_BITS    16
//#define TGL_FEATURE_RENDER_BITS    24
//#define TGL_FEATURE_RENDER_BITS    32
#endif

#endif /* _tgl_features_h_ */
