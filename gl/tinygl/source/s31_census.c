/*
 * s31_census.c - phase 5 diagnostic: which general-path stage lists draw
 * how many pixels (gl/bench/qscensus.sh). Compiled to nothing unless
 * S31GL_CENSUS is defined (S31_BENCH_DEFS=-DS31GL_CENSUS: a bench build
 * only, never the shipped library). Every chunk the runners hand to a
 * stage list is counted under the list it actually ran - ZPipe.depth and
 * ZPipe.st, and under a fused filler also ZPipeX.gen_st (the per-triangle
 * texel choices are placed there) - while the replayer has the census on
 * (the counted frames). s31, MIT.
 */
#ifdef S31GL_CENSUS
#include <stdio.h>
#include <string.h>
#include "zgl.h"
#include "zpipe.h"

#define CN_MAX 512
#define CN_FN (2 + 2 * ZP_MAX_STAGES)
static struct {
  void *fn[CN_FN];
  int nfn, fused;
  unsigned long long chunks, px, alive;
} cn[CN_MAX];
static int ncn, cn_on;

void s31_census_set(int on) { cn_on = on; }

void zp_census_chunk(const ZPipe *p, int n, int alive)
{
  void *fn[CN_FN];
  int k = 0, i, j;
  const ZStageFn *st;
  if (!cn_on) return;
  fn[k++] = (void *)p->depth;
  for (st = p->st; *st && k < CN_FN; st++) fn[k++] = (void *)*st;
  if (p->x && p->x->fused) {
    fn[k++] = (void *)p->x->gen_depth;
    for (st = p->x->gen_st; *st && k < CN_FN; st++) fn[k++] = (void *)*st;
  }
  for (i = 0; i < ncn; i++) {
    if (cn[i].nfn != k || cn[i].fused != (p->x ? p->x->fused : 0)) continue;
    for (j = 0; j < k; j++) if (cn[i].fn[j] != fn[j]) break;
    if (j == k) break;
  }
  if (i == ncn) {
    if (ncn == CN_MAX) return;
    ncn++;
    cn[i].nfn = k;
    cn[i].fused = p->x ? p->x->fused : 0;
    memcpy(cn[i].fn, fn, sizeof(void *) * k);
  }
  cn[i].chunks++;
  cn[i].px += (unsigned long long)n;
  cn[i].alive += (unsigned long long)alive;
}

void s31_census_dump(void)
{
  int i, j;
  for (i = 0; i < ncn; i++) {
    printf("census %d fused %d chunks %llu px %llu alive %llu fns", i, cn[i].fused,
           cn[i].chunks, cn[i].px, cn[i].alive);
    for (j = 0; j < cn[i].nfn; j++) printf(" %lx", (unsigned long)cn[i].fn[j]);
    printf("\n");
  }
}
#endif
