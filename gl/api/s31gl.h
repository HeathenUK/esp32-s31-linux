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

/* Phase 3a (dirty boxes): the caller promises that nothing but the core
   writes into the colour buffers it binds to ctx (the GLX layer's SHM and
   malloc'd buffers: the X server only reads them). A full glClear then
   writes only what was drawn into since the last full clear to the same
   value. Default 0: a caller that fills its own buffer between frames
   keeps every clear whole. */
S31GL_API void s31gl_set_retained(s31gl_ctx *ctx, int retained);

/* bytes the core holds for ctx's depth buffer right now (0 until the first
   draw); for RSS accounting in gates */
S31GL_API int s31gl_depth_bytes(s31gl_ctx *ctx);

/* free the depth buffer now (it is reallocated at the next draw); a
   caller-owned one (s31gl_bind_depth) is only forgotten */
S31GL_API void s31gl_release_depth(s31gl_ctx *ctx);

/* bytes the caller allocates after the w * h depth values of
   s31gl_bind_depth: the core's depth-epoch state (phase 3a G03,
   gl/tinygl/source/s31_zepoch.c; ZB_DEPTH_TAIL in zbuffer.h) */
#define S31GL_DEPTH_TAIL 20

/* Render depth into caller-owned memory: w * h 16-bit values for the size
   bound by the last s31gl_bind_color, followed by S31GL_DEPTH_TAIL bytes.
   Their contents may be anything (GL leaves a depth buffer undefined until
   it is cleared): the core takes the tail over at the first bind (a magic
   word) and makes the first full depth clear after that a real one. The
   tail then holds the state every context bound to the same memory
   shares, so while the core may be using it the caller must not write the
   memory. To hand the core memory whose contents it did not write at this
   size - a buffer reused after a resize down and back up, say - zero the
   tail (or all of it) first. GL's depth buffer belongs to the drawable, so
   the GLX layer gives every context current on one window the same one.
   The core never frees it; a bind_color that changes the size forgets it,
   so rebind after one. NULL returns to a private, lazily allocated buffer.
   0 = success, -1 = no size bound. */
S31GL_API int s31gl_bind_depth(s31gl_ctx *ctx, void *depth);

/* RENDER SCALE (plan G04). The colour buffer bound next (and every one
   after it until this is called again) holds the window at 1/2^shift of its
   size in each axis: bind_color gets the BUFFER's size (the window's >>
   shift). GL's window coordinates stay the application's - the window's
   real size - and are mapped onto the buffer: viewport, scissor, raster
   position and glWindowPos, glBitmap/glDrawPixels/glCopyPixels/glReadPixels/
   glCopyTex*, line width and point size; every glGet reports the
   application's values. The GLX layer uses it for panel-size fullscreen
   windows that the server scales back up (gl/glx/glx_present.c). 0 =
   success, -1 = unsupported shift (0..2 are supported; 0 is native). */
S31GL_API int s31gl_set_render_scale(s31gl_ctx *ctx, int shift);

/* STENCIL (phase 4 F8). GL_STENCIL_BITS of ctx: 0 (the default: no stencil
   buffer, no stencil memory, the stencil test passes) or 8. The GLX layer
   sets it from the config (GLX_STENCIL_SIZE). With 8 the context renders
   stencil into the buffer bound by s31gl_bind_stencil, or else into a
   private one allocated lazily with the depth buffer (w * h bytes). */
S31GL_API void s31gl_set_stencil_bits(s31gl_ctx *ctx, int bits);

/* bytes the caller allocates after the w * h stencil values of
   s31gl_bind_stencil: the core's state of the buffer (its dirty range;
   ZB_STENCIL_TAIL in gl/tinygl/source/zbuffer.h) */
#define S31GL_STENCIL_TAIL 20

/* Render stencil into caller-owned memory: w * h bytes for the size bound
   by the last s31gl_bind_color, followed by S31GL_STENCIL_TAIL bytes, with
   the same contract as s31gl_bind_depth: any contents (GL leaves the buffer
   undefined until it is cleared; the core takes the tail over and makes its
   first full clear write everything), shared by every context bound to it,
   never freed by the core, forgotten by a size change. NULL returns to a
   private buffer. 0 = success, -1 = no size bound. */
S31GL_API int s31gl_bind_stencil(s31gl_ctx *ctx, void *stencil);

/* The caller states that the w * h stencil values at stencil are all 0 (a
   fresh calloc), before binding it: the core then trusts them, so a full
   clear to 0 writes nothing until something is drawn - a stencil that is
   only ever cleared never becomes resident. Without it the first full
   clear writes the whole buffer (review 4 R3-stencil) */
S31GL_API void s31gl_stencil_zeroed(void *stencil, int w, int h);

/* bytes of stencil the core renders into for ctx right now (0 before the
   first draw or with no stencil bits); for RSS accounting in gates */
S31GL_API int s31gl_stencil_bytes(s31gl_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* S31GL_H */
