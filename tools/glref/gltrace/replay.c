/*
 * replay.c - load and run a gltrace (replay.h). s31, MIT.
 */
#include "replay.h"
#include <stdio.h>
#include <stdlib.h>

volatile int rp_cls;

/* ---------------------------------------------------------- scratch */

#define SCRATCH0 (64 * 1024)
static uint32_t scratch0[SCRATCH0 / 4];
static void *scratch = scratch0;
static uint32_t scratch_cap = SCRATCH0;
/* the bench builds replace these to keep the replayer's own memory out of
   the library's accounting */
void *rp_alloc(size_t n) __attribute__((weak));
void *rp_alloc(size_t n) { return malloc(n); }

void *rp_scratch(uint32_t bytes)
{
	if (bytes > scratch_cap) {
		void *p = rp_alloc(bytes);
		if (!p) {
			fprintf(stderr, "replay: no memory for %u bytes of scratch\n", bytes);
			exit(3);
		}
		scratch = p;
		scratch_cap = bytes;
	}
	return scratch;
}

/* ---------------------------------------------------------- texture names */

#define TEXMAP 16384
static GLuint texmap[TEXMAP];
static int texmap_init, texmap_warned;
static GLuint texarr[4096];

static void tm_init(void)
{
	for (int i = 0; i < TEXMAP; i++)
		texmap[i] = (GLuint)i;
	texmap_init = 1;
}

GLuint rp_texname(GLuint n)
{
	return n < TEXMAP ? texmap[n] : n;
}

const GLuint *rp_texnames(const uint32_t *q, uint32_t bytes)
{
	uint32_t n = bytes / 4;
	if (bytes == 0xFFFFFFFFu)
		return NULL;
	GLuint *o = n <= 4096 ? texarr : rp_scratch(bytes);
	for (uint32_t i = 0; i < n; i++)
		o[i] = rp_texname(q[i]);
	return o;
}

void rp_gen_textures(const uint32_t *rec, const GLuint *got, uint32_t n)
{
	if (!texmap_init)
		tm_init();
	for (uint32_t i = 0; i < n; i++) {
		if (rec[i] < TEXMAP)
			texmap[rec[i]] = got[i];
		else if (rec[i] != got[i] && !texmap_warned) {
			texmap_warned = 1;
			fprintf(stderr, "replay: texture name %u beyond the map and not reproduced\n", rec[i]);
		}
	}
}

/* ---------------------------------------------------------- load */

static char **tnames;
static int ntnames;

const char *rp_trace_name(int id)
{
	return id >= 0 && id < ntnames ? tnames[id] : "?";
}

static int find(const char *n)
{
	int lo = 0, hi = rp_nnames - 1;    /* rp_names is sorted (gen.py) */
	while (lo <= hi) {
		int m = (lo + hi) / 2, c = strcmp(rp_names[m], n);
		if (!c)
			return m;
		if (c < 0)
			lo = m + 1;
		else
			hi = m - 1;
	}
	return -1;
}

int rp_load(struct rp_trace *t, void *buf, size_t bytes, int (*have)(int))
{
	uint32_t *w = buf, *e = w + bytes / 4;
	int bad = 0;
	uint16_t *map;
	if (!texmap_init)
		tm_init();
	memset(t, 0, sizeof *t);
	if (bytes < 12 || w[0] != TR_MAGIC || w[1] != TR_VERSION) {
		fprintf(stderr, "replay: not a gltrace v%d file\n", TR_VERSION);
		return -1;
	}
	ntnames = (int)w[2];
	tnames = rp_alloc(sizeof(char *) * (size_t)ntnames);
	map = rp_alloc(sizeof(uint16_t) * (size_t)ntnames);
	w += 3;
	for (int i = 0; i < ntnames; i++) {
		uint32_t n = *w++;
		char *s = rp_alloc(n + 1);
		memcpy(s, w, n);
		s[n] = 0;
		tnames[i] = s;
		w += (n + 3) / 4;
		if (!strncmp(s, "glX", 3))
			map[i] = RP_IGNORE;
		else {
			int k = find(s);
			map[i] = k < 0 ? 0xFFFFu : (uint16_t)k;
		}
	}
	t->w = w;
	t->nnames = ntnames;
	while (w < e) {
		uint32_t h = *w, id = h & TR_ID_MASK, nw = h >> TR_ID_BITS;
		if (nw == TR_NW_EXT)
			nw = w[1];
		if (nw == 0 || w + nw > e) {
			fprintf(stderr, "replay: corrupt record at word %ld\n", (long)(w - (uint32_t *)buf));
			return -1;
		}
		if (id < TR_PSEUDO_MIN) {
			uint16_t k = (int)id < ntnames ? map[id] : 0xFFFFu;
			if (k != RP_IGNORE && k < 0xFFFEu && have && !have(k))
				k = 0xFFFFu;
			if (k == 0xFFFFu) {
				/* reported once per name, then 0xFFFE */
				fprintf(stderr, "replay: the library under test has no %s\n", rp_trace_name((int)id));
				bad++;
				if ((int)id < ntnames)
					map[id] = 0xFFFEu;
			}
			if (k >= 0xFFFEu)
				k = RP_IGNORE;
			*w = (h & ~TR_ID_MASK) | k;
		} else if (id == TR_SWAP) {
			t->frames++;
			if (w[2] & TRS_FULL)
				t->full++;
			if (w[2] & TRS_COUNT)
				t->counted++;
			if (w[2] & TRS_FULL && w[6] + 1 > t->windows)
				t->windows = w[6] + 1;
		} else if (id == TR_UNHANDLED) {
			fprintf(stderr, "replay: the trace holds an UNHANDLED %s\n", rp_trace_name((int)w[1]));
			bad++;
		}
		w += nw;
	}
	t->nw = (size_t)(w - t->w);
	return bad ? -1 : 0;
}

void rp_run(const struct rp_trace *t, const struct rp_platform *p)
{
	const uint32_t *w = t->w, *e = w + t->nw;
	while (w < e) {
		uint32_t h = *w, id = h & TR_ID_MASK, nw = h >> TR_ID_BITS;
		const uint32_t *r = w;
		if (nw == TR_NW_EXT) {
			nw = w[1];
			r = w + 1;       /* the payload starts at r[1] */
		}
		if (id < RP_IGNORE)
			rp_fns[id](r);
		else switch (id) {
		case RP_IGNORE: break;
		case TR_SWAP: p->swap(r + 1); break;
		case TR_CTX: p->ctx(r + 1); break;
		case TR_NEWCTX: p->newctx(r[1], r[2]); break;
		case TR_DELCTX: p->delctx(r[1]); break;
		case TR_END: break;
		case TR_UNHANDLED: if (p->unhandled) p->unhandled(rp_trace_name((int)r[1])); break;
		}
		w += nw;
	}
}
