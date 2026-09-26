/*
 * q_replay.c - the RV32 bare-metal gltrace replayer for gl/bench's qemu
 * instruction-count harness (qsreplay.sh). s31, MIT.
 *
 * Our library objects (the core of libGL.so.1, no GLX: build_q.sh's
 * obj/core.list) replay a trace recorded on the host rig
 * (tools/glref/gltrace/capture-qs.sh) through the generated dispatch
 * (gen.py dispatch --mode direct: every record is a direct call of the
 * entry point with the recorded arguments; texture/pixel data by value
 * from the trace in memory). The platform half does what the GLX layer
 * does (gl/glx/glx_core.c): per context set_doublebuffer, set_stencil_bits
 * and set_retained; per drawable two RGB565 colour buffers bound in turn
 * (GLX's two SHM segments for a drawable of 200 kB or less), a caller-owned
 * depth buffer (+ the S31GL_DEPTH_TAIL), and a zeroed caller-owned stencil
 * buffer when the config has stencil; s31gl_frame_end at every swap.
 *
 * Reads ./qs.gltr (semihosting), and ./qr.env if present ("KEY=VALUE"
 * lines, setenv()ed before the first context: the library's runtime
 * toggles such as S31GL_TEXFILTER). minstret is exact under qemu
 * -icount shift=0. Prints:
 *   qsrf <frame> <flags> <insn> <hash> <live> <same|DIFF>   every full frame
 *   qsr window <i> frames <a>-<b>: <M>/frame min <M> max <M> (frame <n>), ...
 *   qsr load: ... ; qsr heap: ... ; qsr done: ...
 * and writes qsr_f<N>.raw (RGB565, top row first) for every counted frame.
 * Frame N's count runs from the end of swap N-1 (after the previous hash)
 * to swap N, so it includes swap N-1's s31gl_frame_end and buffer bind, as
 * q_ui.c's frames include s31gl_frame_end.
 *
 * Memory: malloc/calloc/realloc/free are --wrap'ed. A block allocated while
 * a texture call runs (rp_cls, set by the dispatch around glTex*, glBind-
 * Texture, glGenTextures, glDeleteTextures, glCopyTex*...) is texture
 * memory; the rest is the library's other heap. The replayer's own memory
 * (the trace, scratch, the drawables' buffers) bypasses the wrappers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "replay.h"
#include "s31gl.h"

extern unsigned long dcalls;

static inline unsigned long long rdinstret(void)
{
	unsigned lo, hi, h2;
	do {
		__asm__ volatile("csrr %0,minstreth" : "=r"(hi));
		__asm__ volatile("csrr %0,minstret" : "=r"(lo));
		__asm__ volatile("csrr %0,minstreth" : "=r"(h2));
	} while (hi != h2);
	return ((unsigned long long)hi << 32) | lo;
}

/* ------------------------------------------------------------ memory */

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);

#define HMAGIC 0x51c0ffeeu
struct hdr { unsigned size, cls, magic, pad; };
static unsigned long live[2], peak_total, nallocs;
/* (phase 5) the split at the peak, and the frame it happened in */
static unsigned long peak_tex, peak_other;
static uint32_t peak_frame, cur_frame;

void *rp_alloc(size_t n) { return __real_malloc(n); }

static void acct(long d, unsigned cls)
{
	live[cls] += d;
	if (live[0] + live[1] > peak_total) {
		peak_total = live[0] + live[1];
		peak_tex = live[1]; peak_other = live[0]; peak_frame = cur_frame;
	}
}

void *__wrap_malloc(size_t n)
{
	struct hdr *h = __real_malloc(n + sizeof *h);
	if (!h)
		return NULL;
	h->size = (unsigned)n;
	h->cls = rp_cls ? 1 : 0;
	h->magic = HMAGIC;
	nallocs++;
	acct((long)n, h->cls);
	{
		/* QR_ALLOC_LOG=N (phase 5 (g)): every non-texture block of N
		   bytes or more, with its caller (resolve with addr2line) */
		static long lim = -1;
		if (lim < 0) { const char *e = getenv("QR_ALLOC_LOG"); lim = e ? atol(e) : 0; }
		if (lim > 0 && !h->cls && (long)n >= lim)
			printf("qsr alloc %u B from %p in frame %u\n", (unsigned)n,
			       __builtin_return_address(0), (unsigned)cur_frame);
	}
	return h + 1;
}

void __wrap_free(void *p)
{
	struct hdr *h;
	if (!p)
		return;
	h = (struct hdr *)p - 1;
	if (h->magic != HMAGIC) {
		printf("qsr: free of a block not from malloc (%p)\n", p);
		return;
	}
	h->magic = 0;
	acct(-(long)h->size, h->cls);
	__real_free(h);
}

void *__wrap_calloc(size_t a, size_t b)
{
	void *p = __wrap_malloc(a * b);
	if (p)
		memset(p, 0, a * b);
	return p;
}

void *__wrap_realloc(void *p, size_t n)
{
	struct hdr *h;
	void *q;
	if (!p)
		return __wrap_malloc(n);
	h = (struct hdr *)p - 1;
	q = __wrap_malloc(n);
	if (q) {
		memcpy(q, p, h->size < n ? h->size : n);
		__wrap_free(p);
	}
	return q;
}

/* ------------------------------------------------------------ platform */

#define MAXC 32
#define MAXD 64
static s31gl_ctx *ctx[MAXC + 1];
static int ctx_cfg[MAXC + 1];
static uint32_t ctx_share[MAXC + 1];
static struct { unsigned w, h, cur; unsigned short *fb[2]; void *z, *st; } drw[MAXD + 1];
static int cur_c, cur_d;

static void newctx(uint32_t id, uint32_t share)
{
	if (id == 0 || id > MAXC)
		return;
	if (ctx[id]) {
		s31gl_destroy_context(ctx[id]);
		ctx[id] = NULL;
	}
	ctx_share[id] = share;
	ctx[id] = s31gl_create_context(share && share <= MAXC ? ctx[share] : NULL);
	ctx_cfg[id] = 0;
	s31gl_set_retained(ctx[id], 1);
}

static void delctx(uint32_t id)
{
	if (id == 0 || id > MAXC || !ctx[id])
		return;
	if (cur_c == (int)id) {
		s31gl_make_current(NULL);
		cur_c = 0;
	}
	s31gl_destroy_context(ctx[id]);
	ctx[id] = NULL;
}

static void bind(void)
{
	s31gl_ctx *c = ctx[cur_c];
	int d = cur_d;
	s31gl_bind_color(c, drw[d].fb[drw[d].cur], (int)drw[d].w, (int)drw[d].h, (int)drw[d].w * 2);
}

static void on_ctx(const uint32_t *a)
{
	uint32_t c = a[0], d = a[1], w = a[2], h = a[3], stencil = a[5], db = a[6];
	if (c == 0 || d == 0 || c > MAXC || d > MAXD) {
		s31gl_make_current(NULL);
		cur_c = cur_d = 0;
		return;
	}
	if (!ctx[c])
		newctx(c, 0);
	if (!ctx_cfg[c]) {
		/* glx_core.c: the config's double buffer and stencil bits */
		s31gl_set_doublebuffer(ctx[c], (int)db);
		s31gl_set_stencil_bits(ctx[c], (int)stencil);
		ctx_cfg[c] = 1;
	}
	if (!drw[d].w) {
		drw[d].w = w;
		drw[d].h = h;
		drw[d].fb[0] = __real_calloc((size_t)w * h, 2);
		drw[d].fb[1] = __real_calloc((size_t)w * h, 2);
		drw[d].z = __real_calloc((size_t)w * h * 2 + S31GL_DEPTH_TAIL + 64, 1);
		if (stencil) {
			drw[d].st = __real_calloc((size_t)w * h + S31GL_STENCIL_TAIL, 1);
			s31gl_stencil_zeroed(drw[d].st, (int)w, (int)h);
		}
	}
	s31gl_make_current(ctx[c]);
	cur_c = (int)c;
	cur_d = (int)d;
	bind();
	s31gl_bind_depth(ctx[c], drw[d].z);
	if (drw[d].st)
		s31gl_bind_stencil(ctx[c], drw[d].st);
}

/* ------------------------------------------------------------ frames */

/* gl/bench/qemu/pcprof.c counts instructions between q_on() and q_off():
   the counted frames, without the swap handling between them */
/* (and the library's stage-list census, when it was built with
   -DS31GL_CENSUS: gl/bench/qscensus.sh; weak, so a normal build has none) */
extern void s31_census_set(int on) __attribute__((weak));
extern void s31_census_dump(void) __attribute__((weak));
__attribute__((noinline)) void q_on(void) { __asm__ volatile(""); if (s31_census_set) s31_census_set(1); }
__attribute__((noinline)) void q_off(void) { __asm__ volatile(""); if (s31_census_set) s31_census_set(0); }
#define MAXF 65536
static unsigned char counted_f[MAXF / 8];
static int is_counted(uint32_t f) { return f < MAXF && (counted_f[f >> 3] >> (f & 7) & 1); }

#define MAXW 8
static struct {
	uint32_t a, b, n;
	unsigned long long sum, min, max;
	uint32_t maxf, same, diff;
	unsigned long dcalls;
	unsigned long tex_at_start, heap_at_start;
} win[MAXW];
static unsigned long long t_prev, state_insn, warm_insn, load_insn;
static uint32_t nstate, first_full;
static unsigned long d_prev;
static unsigned long tex_at_first, heap_at_first;
static uint32_t nsame, ndiff;
static int started;
/* QR_PROF_LOAD (in qr.env, as QSR_ENV passes it): profile the load phase
   (every frame before the first full one) instead of the counted frames */
static int prof_load;

static uint32_t fb_hash(void)
{
	uint32_t hsh = 2166136261u;
	int d = cur_d;
	if (!d)
		return 0;
	const unsigned short *p = drw[d].fb[drw[d].cur];
	for (unsigned i = 0; i < drw[d].w * drw[d].h; i++) {
		hsh ^= p[i];
		hsh *= 16777619u;
	}
	return hsh;
}

static void save_frame(uint32_t frame)
{
	char path[64];
	FILE *f;
	int d = cur_d;
	snprintf(path, sizeof path, "qsr_f%u.raw", (unsigned)frame);
	f = fopen(path, "wb");
	if (f) {
		fwrite(drw[d].fb[drw[d].cur], 2, (size_t)drw[d].w * drw[d].h, f);
		fclose(f);
	}
}

static void on_swap(const uint32_t *a)
{
	unsigned long long now = rdinstret(), d = now - t_prev;
	uint32_t frame = a[0], fl = a[1], live_h = a[2], wi = a[5];
	cur_frame = frame + 1;
	q_off();
	if (fl & TRS_FULL) {
		uint32_t h = fb_hash();
		int same = h == live_h;
		if (!first_full) {
			if (prof_load) q_off();
			first_full = frame;
			tex_at_first = live[1];
			heap_at_first = live[0];
			load_insn = state_insn;
		}
		printf("qsrf %u %s %llu %08x %08x %s\n", (unsigned)frame, fl & TRS_COUNT ? "count" : "warm",
		       d, (unsigned)h, (unsigned)live_h, same ? "same" : "DIFF");
		if (same) nsame++; else ndiff++;
		if (wi < MAXW && (fl & TRS_COUNT)) {
			if (!win[wi].n) {
				win[wi].a = frame;
				win[wi].min = ~0ull;
				win[wi].tex_at_start = live[1];
				win[wi].heap_at_start = live[0];
			}
			win[wi].b = frame;
			win[wi].n++;
			win[wi].sum += d;
			if (d < win[wi].min) win[wi].min = d;
			if (d > win[wi].max) { win[wi].max = d; win[wi].maxf = frame; }
			if (same) win[wi].same++; else win[wi].diff++;
			win[wi].dcalls += dcalls - d_prev;
			/* every counted frame, qsr_f<N>.raw, for image checks */
			save_frame(frame);
		} else {
			warm_insn += d;
		}
	} else {
		state_insn += d;
		nstate++;
	}
	if (cur_c && cur_d) {
		s31gl_ctx *c = ctx[cur_c];
		unsigned long long t1 = rdinstret();
		s31gl_frame_end(c);
		/* two colour buffers in turn (GLX, 200 kB or less) */
		drw[cur_d].cur ^= 1;
		bind();
		t_prev = t1;
	} else {
		t_prev = rdinstret();
	}
	d_prev = dcalls;
	if (is_counted(frame + 1) || (prof_load && !first_full))
		q_on();
}

static void on_unhandled(const char *n)
{
	printf("qsr: UNHANDLED %s in the trace\n", n);
}

static void read_env(void)
{
	FILE *f = fopen("qr.env", "r");
	char line[256];
	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		char *e = strchr(line, '=');
		size_t n = strlen(line);
		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = 0;
		if (!e || line[0] == '#')
			continue;
		*e = 0;
		setenv(line, e + 1, 1);
		printf("qsr: env %s=%s\n", line, e + 1);
	}
	fclose(f);
}

#ifndef QR_LABEL
#define QR_LABEL "qsreplay"
#endif

int main(void)
{
	FILE *f = fopen("qs.gltr", "rb");
	long n;
	void *buf;
	struct rp_trace t;
	struct rp_platform pl = { newctx, delctx, on_ctx, on_swap, on_unhandled };
	unsigned long long t0;
	if (!f) {
		printf("qsr: cannot open qs.gltr\n");
		return 2;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = __real_malloc((size_t)n + 16);
	if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
		printf("qsr: cannot read qs.gltr (%ld bytes)\n", n);
		return 2;
	}
	fclose(f);
	read_env();
	if (rp_load(&t, buf, (size_t)n, NULL)) {
		printf("qsr: the trace does not load\n");
		return 2;
	}
	(void)started;
	/* which frames are counted (for q_on/q_off) */
	for (const uint32_t *w = t.w; w < t.w + t.nw;) {
		uint32_t h = *w, nw = h >> TR_ID_BITS;
		if (nw == TR_NW_EXT)
			nw = w[1];
		if ((h & TR_ID_MASK) == TR_SWAP && (w[2] & TRS_COUNT) && w[1] < MAXF)
			counted_f[w[1] >> 3] |= (unsigned char)(1u << (w[1] & 7));
		w += nw;
	}
	prof_load = getenv("QR_PROF_LOAD") != NULL;
	if (prof_load) memset(counted_f, 0, sizeof counted_f);
	t0 = rdinstret();
	t_prev = t0;
	d_prev = dcalls;
	if (prof_load) q_on();
	rp_run(&t, &pl);
	for (int i = 0; i < MAXW; i++) {
		if (!win[i].n)
			continue;
		printf("qsr %s window %d frames %u-%u: %.4f Minsn/frame min %.4f max %.4f (frame %u), "
		       "%.0f dcalls/frame, hashes %u/%u same as live, tex %lu B heap %lu B at start\n",
		       QR_LABEL, i, (unsigned)win[i].a, (unsigned)win[i].b, win[i].sum / (double)win[i].n / 1e6,
		       win[i].min / 1e6, win[i].max / 1e6, (unsigned)win[i].maxf,
		       win[i].dcalls / (double)win[i].n, (unsigned)win[i].same, (unsigned)win[i].n,
		       win[i].tex_at_start, win[i].heap_at_start);
	}
	printf("qsr %s load: %.2f Minsn before frame %u (%u state-only frames in all: %.2f Minsn), "
	       "warm-up %.2f Minsn\n", QR_LABEL, load_insn / 1e6, (unsigned)first_full, (unsigned)nstate,
	       state_insn / 1e6, warm_insn / 1e6);
	printf("qsr %s heap: texture %lu B, other %lu B after the load phase; at the end texture %lu B, "
	       "other %lu B; peak %lu B, %lu allocations\n", QR_LABEL, tex_at_first, heap_at_first,
	       live[1], live[0], peak_total, nallocs);
	printf("qsr %s peak: texture %lu B, other %lu B, in frame %u\n", QR_LABEL, peak_tex, peak_other,
	       (unsigned)peak_frame);
	if (s31_census_dump) s31_census_dump();
	printf("qsr %s done: %u frames, %u full: %u same as live, %u differ\n", QR_LABEL,
	       (unsigned)t.frames, (unsigned)t.full, (unsigned)nsame, (unsigned)ndiff);
	return 0;
}
