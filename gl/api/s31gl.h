/*
 * s31gl.h - the rasteriser core's context API, for the GLX layer and for
 * headless users (gl/tests/headless_gears.c). s31, MIT.
 *
 * This is the authority for the contract gl/glx/s31gl_iface.h was written
 * against in parallel; the names, argument order and semantics below are
 * identical to that file, with a few additions at the end.
 *
 * Model
 *   A context owns GL state (TinyGL's GLContext) and, lazily, a private
 *   16-bit depth buffer unless the caller binds one (s31gl_bind_depth). It never owns colour memory: the caller binds an
 *   RGB565 buffer it allocated (the MIT-SHM segment, a malloc'd block...)
 *   and the rasteriser writes into it directly - there is no copy inside
 *   the library. Row 0 of the buffer is the TOP row of the image; GL's
 *   window y axis (viewport, scissor, raster position) points up and is
 *   converted internally.
 *
 *   One context is current per process (TinyGL keeps one global current
 *   state). The core does no locking: callers serialise (the GLX layer
 *   does). With nothing current, GL calls go to an internal context that
 *   has no buffer, so they are harmless no-ops rather than crashes.
 *
 * Buffers and the frame hook
 *   Nothing is allocated for pixels by create, make_current or bind. At the
 *   first colour/depth access after make_current, bind_color or frame_end
 *   (a clear, any primitive, glBitmap, glDrawPixels, glCopyPixels,
 *   glReadPixels, glCopyTexImage/SubImage) the core calls hooks.frame_begin
 *   ONCE. The hook may bind (or rebind) the colour buffer - that is how it
 *   is allocated lazily - and may wait for the previous frame's present.
 *   Then, if a colour buffer is bound, the depth buffer is allocated if
 *   needed (w * h * 2 bytes; dropped when the size changes). With no colour
 *   buffer the access is dropped, never a crash.
 *
 *   The viewport may exceed or lie outside the bound buffer, as GL allows:
 *   primitives are clipped to the buffer (see gl_eval_viewport in
 *   gl/tinygl/source/vertex.c), at no per-pixel cost.
 *
 * Log
 *   s31gl_create_context prints one line to stderr per context:
 *     libGL: context N created (pid P, comm NAME)
 *   so a gate can prove which processes made a GL context.
 *
 * Exported from libGL.so.1 (namespaced; not a public GL API).
 */
#ifndef S31GL_H
#define S31GL_H

#ifdef __cplusplus
extern "C" {
#endif

#define S31GL_API __attribute__((visibility("default")))

typedef struct s31gl_ctx s31gl_ctx;

struct s31gl_hooks {
	void *user;
	/* once before the first buffer access after make_current, bind_color
	   or frame_end; may call s31gl_bind_color */
	void (*frame_begin)(void *user);
	/* from glViewport, before it takes effect; may call s31gl_bind_color */
	void (*viewport)(void *user, int x, int y, int w, int h);
	/* from glFlush (finish = 0) and glFinish (finish = 1) */
	void (*flush)(void *user, int finish);
};

/* A context with no colour and no depth buffer. share: display lists and
   texture objects are shared with it (glXCreateContext share_list). NULL
   on allocation failure. */
S31GL_API s31gl_ctx *s31gl_create_context(s31gl_ctx *share);

/* Frees everything the core allocated for ctx (never the colour buffer).
   If ctx is current, nothing is current afterwards. */
S31GL_API void s31gl_destroy_context(s31gl_ctx *ctx);

/* ctx becomes current (NULL: none). Re-arms frame_begin. 0 = success. */
S31GL_API int s31gl_make_current(s31gl_ctx *ctx);

/* Render into caller-owned RGB565 memory: w x h pixels, pitch BYTES per row
   (>= 2*w and even; XShm pads odd widths to 2*w+2). pixels == NULL unbinds
   (the depth buffer is kept for a rebind of the same size). The first bind
   with a non-zero size sets the viewport and scissor box to (0, 0, w, h), as
   the first MakeCurrent does in GLX. Re-arms frame_begin. May be called
   from inside frame_begin and viewport. 0 = success, -1 = bad arguments. */
S31GL_API int s31gl_bind_color(s31gl_ctx *ctx, void *pixels, int w, int h,
			       int pitch);

/* hooks is copied; NULL clears them */
S31GL_API void s31gl_set_hooks(s31gl_ctx *ctx, const struct s31gl_hooks *hooks);

/* The frame was presented: re-arm frame_begin. */
S31GL_API void s31gl_frame_end(s31gl_ctx *ctx);

/* All rendering issued so far is in the colour buffer. TinyGL renders
   synchronously, so this returns at once; it does NOT call hooks.flush. */
S31GL_API void s31gl_finish(s31gl_ctx *ctx);

/* A GL entry point by name (the core GL names and the ARB/EXT aliases
   libGL exports), or NULL for anything not really implemented: never a
   catch-all stub, because a stub makes SDL believe a feature exists. */
S31GL_API void *s31gl_get_proc(const char *name);

/* ---- additions to s31gl_iface.h ---- */

/* the current context, or NULL */
S31GL_API s31gl_ctx *s31gl_get_current(void);

/* GL_DOUBLEBUFFER as glGet reports it, and the default GL_DRAW_BUFFER /
   GL_READ_BUFFER (GL_BACK or GL_FRONT). Default: double-buffered. */
S31GL_API void s31gl_set_doublebuffer(s31gl_ctx *ctx, int doublebuffer);

/* bytes the core holds for ctx's depth buffer right now (0 until the first
   draw); for RSS accounting in gates */
S31GL_API int s31gl_depth_bytes(s31gl_ctx *ctx);

/* free the depth buffer now (it is reallocated at the next draw); a
   caller-owned one (s31gl_bind_depth) is only forgotten */
S31GL_API void s31gl_release_depth(s31gl_ctx *ctx);

/* Render depth into caller-owned memory: w * h 16-bit values for the size
   bound by the last s31gl_bind_color, initialised by the caller (0 is the
   far plane). GL's depth buffer belongs to the drawable, so the GLX layer
   gives every context current on one window the same one. The core never
   frees it; a bind_color that changes the size forgets it, so rebind after
   one. NULL returns to a private, lazily allocated buffer. 0 = success,
   -1 = no size bound. */
S31GL_API int s31gl_bind_depth(s31gl_ctx *ctx, void *depth);

#ifdef __cplusplus
}
#endif

#endif /* S31GL_H */
