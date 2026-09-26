/*
 * replay.h - the platform-independent half of the gltrace replayer
 * (tools/glref/gltrace). The same replay.c and generated dispatch run in
 * the host replayer (replay_host.c: X11/GLX, Mesa or ours) and in the RV32
 * bare-metal bench (gl/bench/q_replay.c: our library objects, qemu -icount).
 * s31, MIT.
 */
#ifndef REPLAY_H
#define REPLAY_H
#define GL_GLEXT_PROTOTYPES 1
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include "gltrace.h"

typedef void (*rp_fn)(const uint32_t *w);

/* the generated dispatch (gen.py dispatch) */
extern const char *const rp_names[];
extern const int rp_nnames;
extern const rp_fn rp_fns[];
extern void *rp_fp[];                /* --mode table only */

/* 1 while a texture call runs (the bench counts its allocations as texture
   memory) */
extern volatile int rp_cls;

void *rp_scratch(uint32_t bytes);
const GLuint *rp_texnames(const uint32_t *q, uint32_t bytes);
GLuint rp_texname(GLuint recorded);
void rp_gen_textures(const uint32_t *recorded, const GLuint *got, uint32_t n);

/* ids the loader writes into the records */
#define RP_IGNORE 0xFEFu             /* a glX* note: nothing to replay */

struct rp_trace {
	uint32_t *w;                 /* first record */
	size_t nw;                   /* words of records */
	int nnames;
	uint32_t frames, full, counted, windows;
};

struct rp_platform {
	void (*newctx)(uint32_t id, uint32_t share);
	void (*delctx)(uint32_t id);
	void (*ctx)(const uint32_t *a);          /* TR_CTX words */
	void (*swap)(const uint32_t *a);         /* TR_SWAP words */
	void (*unhandled)(const char *name);
};

/* Parses the header of buf (bytes long, 4-aligned), maps every name the
   records use to a dispatch index and rewrites the ids in place. Returns 0,
   or -1 with a message on stderr (a name the dispatch lacks, a bad record).
   `missing` (optional) says whether a name the library lacks is fatal. */
int rp_load(struct rp_trace *t, void *buf, size_t bytes, int (*have)(int idx));
void rp_run(const struct rp_trace *t, const struct rp_platform *p);
const char *rp_trace_name(int trace_id);

#endif
