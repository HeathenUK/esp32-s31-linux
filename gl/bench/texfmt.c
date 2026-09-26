/*
 * texfmt.c - RGB565 + A8 plane against ARGB4444 for alpha textures (plan
 * F3: "measure both"), under the bench's qemu instruction counter. Each arm
 * is the general path's GL_MODULATE-of-RGBA texenv (zpipe.c ze_mod_rgba):
 * fetch the texel, expand it to 8 bits a channel, multiply the fragment
 * colour, for ZP_CHUNK = 32 fragments at a time, over a 256x256 texture
 * sampled at pseudo-random indices. Same compiler, flags and multiply as
 * zpipe.c. Prints instructions per fragment for each arm. s31, MIT.
 */
#include <stdio.h>
#include <stdlib.h>

#define N 32
#define MUL8(x, y) (((x) * ((y) + 1)) >> 8)

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

typedef struct {
	unsigned char r[N], g[N], b[N], a[N];
	unsigned int idx[N];
} Frag;

/* RGB565 plane + A8 plane (what texture.c stores) */
__attribute__((noinline)) void mod_565a8(const unsigned short *tex,
	const unsigned char *al, Frag *f)
{
	int i, tr, tg, tb, ta;
	for (i = 0; i < N; i++) {
		unsigned int t = tex[f->idx[i]];
		tr = ((t >> 8) & 0xf8) | (t >> 13);
		tg = ((t >> 3) & 0xfc) | ((t >> 9) & 3);
		tb = ((t << 3) & 0xf8) | ((t >> 2) & 7);
		ta = al[f->idx[i]];
		f->r[i] = (unsigned char)MUL8(f->r[i], tr);
		f->g[i] = (unsigned char)MUL8(f->g[i], tg);
		f->b[i] = (unsigned char)MUL8(f->b[i], tb);
		f->a[i] = (unsigned char)MUL8(f->a[i], ta);
	}
}

/* ARGB4444, one 16-bit plane */
__attribute__((noinline)) void mod_4444(const unsigned short *tex, Frag *f)
{
	int i, tr, tg, tb, ta;
	for (i = 0; i < N; i++) {
		unsigned int t = tex[f->idx[i]];
		tr = ((t >> 8) & 0x0f) * 17;
		tg = ((t >> 4) & 0x0f) * 17;
		tb = (t & 0x0f) * 17;
		ta = (t >> 12) * 17;
		f->r[i] = (unsigned char)MUL8(f->r[i], tr);
		f->g[i] = (unsigned char)MUL8(f->g[i], tg);
		f->b[i] = (unsigned char)MUL8(f->b[i], tb);
		f->a[i] = (unsigned char)MUL8(f->a[i], ta);
	}
}

int main(void)
{
	static unsigned short t565[65536], t4444[65536];
	static unsigned char a8[65536];
	Frag f;
	unsigned int seed = 12345, sum = 0;
	unsigned long long t0, c1 = 0, c2 = 0;
	int k, i;

	for (i = 0; i < 65536; i++) {
		seed = seed * 1103515245u + 12345u;
		t565[i] = (unsigned short)(seed >> 8);
		t4444[i] = (unsigned short)(seed >> 12);
		a8[i] = (unsigned char)(seed >> 20);
	}
	for (k = 0; k < 2000; k++) {
		for (i = 0; i < N; i++) {
			seed = seed * 1103515245u + 12345u;
			f.idx[i] = seed >> 16;
			f.r[i] = f.g[i] = f.b[i] = f.a[i] = (unsigned char)(seed >> 3);
		}
		t0 = rdinstret(); mod_565a8(t565, a8, &f); c1 += rdinstret() - t0;
		sum += f.r[3] + f.a[7];
		t0 = rdinstret(); mod_4444(t4444, &f); c2 += rdinstret() - t0;
		sum += f.r[3] + f.a[7];
	}
	printf("texfmt: RGB565+A8 %.2f insn/fragment, ARGB4444 %.2f insn/fragment (check %u)\n",
	       c1 / (2000.0 * N), c2 / (2000.0 * N), sum);
	return 0;
}
