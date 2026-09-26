/*
 * gltrace_rt.h - runtime of the recording libGL.so.1 (tools/glref/gltrace):
 * what the generated wrappers (gen.py wrap) call. s31, MIT.
 */
#ifndef GLTRACE_RT_H
#define GLTRACE_RT_H
#define GL_GLEXT_PROTOTYPES 1
#include <stdint.h>
#include <string.h>
#include <X11/Xlib.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include "gltrace.h"

#define TR_EXPORT __attribute__((visibility("default")))

enum {
	TRC_KEEP, TRC_VERTEX, TRC_COLOR, TRC_SECCOLOR, TRC_TEXCOORD, TRC_MTEXCOORD,
	TRC_NORMAL, TRC_FOGCOORD, TRC_EDGE, TRC_INDEX, TRC_BEGIN, TRC_END, TRC_DRAW,
	TRC_QUERY, TRC_GLX
};

extern const char *const tr_names[];
extern const int tr_nnames;
extern const unsigned char tr_class[];
extern void *tr_real[];
extern void *tr_tramp_real[];
extern const char *const tr_tramp_names[];
extern const int tr_ntramp;

/* nesting: the real library calling its own exports through the PLT lands
   in our wrappers again; only depth 0 is the application */
extern __thread int tr_depth;
extern int tr_on;

void tr_resolve(int id);
#define TR_RESOLVE(i) do { if (!tr_real[i]) tr_resolve(i); } while (0)

uint32_t *tr_begin(int id, unsigned nwords);
void tr_end(int id, uint32_t *w);
void tr_unhandled(int id);
void tr_glx_note(int id);
int tr_arrays_off(int id);

static inline void tr_putf(uint32_t **w, float f) { memcpy(*w, &f, 4); (*w)++; }
static inline void tr_put8(uint32_t **w, const void *p) { memcpy(*w, p, 8); (*w) += 2; }
static inline void tr_putblob(uint32_t **w, const void *p, long sz)
{
	**w = (uint32_t)sz;
	(*w)++;
	if (sz > 0) {
		unsigned n = ((unsigned)sz + 3) / 4;
		(*w)[n - 1] = 0;
		memcpy(*w, p, (size_t)sz);
		*w += n;
	}
}

long tr_image_bytes(long w, long h, long d, GLenum format, GLenum type, int pack);
long tr_bitmap_bytes(long w, long h, int pack);
long tr_type_bytes(GLenum type);
long tr_pname_count(GLenum pname);
long tr_teximage_bytes(GLenum target, GLint level, GLenum format, GLenum type);

/* id of a name in tr_names, or -1 */
int tr_id(const char *name);
#endif
