/*
 * ztriangle_gen.c - the general triangle filler (plan F3-F6). s31, MIT.
 *
 * Which pixels a triangle covers and the depth of each are decided by
 * ztri.h, the code TinyGL's fillers (tier 1, ztriangle.h) use too, so a
 * triangle generates the same fragments with the same depth whichever path
 * draws it (GL 1.3 Appendix A rule 2; review P1/G1): a multipass render
 * whose first pass is tier 1 and whose second is blended, alpha-tested or
 * GL_EQUAL lands on exactly the first pass's pixels. The rule is GL's: a
 * pixel is covered when its centre is inside the triangle, and a centre on
 * an edge belongs to the triangle to its right/below (top-left), so a mesh
 * covers each pixel once and a translucent quad's diagonal is not blended
 * twice.
 *
 * The other attributes are plane equations about the reference pixel of
 * ztri.h, evaluated at each span's first pixel centre and stepped along it;
 * texture coordinates are divided by w (perspective-correct, GL 3.8)
 * through the same 8-pixel subdivision as TinyGL's filler (zpipe.c), from
 * the same s/w, t/w, 1/w planes, so tier 1 and this path pick the same
 * texel. Each span goes to the stage list chosen at glBegin (zpipe.h,
 * raster.c). Float only.
 */
#include "zgl.h"
#include "zpipe.h"
#include "ztri.h"
#include "s31_thr.h"

/* the filler, for a batch without (mt 0) or with (mt 1, phase 5 O1)
   texture unit 1: mt 0 is exactly the phase 4 filler (every unit 1 part
   below is dead code in it) */
/* phase 6 (s31_thr.h): everything after the set-up - the texel level
   choice, the planes and the spans - for the rows own[y] == me (band 1;
   own NULL: every row). The application's thread and the worker both run
   one out-of-line banded copy (ZB_fillBodyGeneral), so a pixel is computed
   by the same instructions whichever thread owns its row; the unbanded
   copy inlined in the filler is S31GL_THREADS=0's code as it was (a row
   test cost +0.3-0.5% on the QuakeSpasm replay), and gl/tests/
   run-qsr-threads.sh holds the two copies to the same bits */
static inline __attribute__((always_inline))
void fill_body(ZBuffer *zb, const ZTri *T, const ZVtxG *a, const ZVtxG *b,
               const ZVtxG *c, const int mt, const int band,
               const unsigned char *own, int me)
{
  const ZPipe *p = zb->pipe;
  const ZVtxG *pv[3] = { a, b, c }, *v0, *v1, *v2;
  int need;
  float grx = 0, gry = 0, ggx = 0, ggy = 0, gbx = 0, gby = 0;
  float gax = 0, gay = 0, gfx = 0, gfy = 0;
  float gsx = 0, gsy = 0, gtx = 0, gty = 0, gqx = 0, gqy = 0;
  float g1x = 0, g1y = 0, g2x = 0, g2y = 0, g3x = 0, g3y = 0;
  float Rr = 0, Rg = 0, Rb = 0, Ra = 0, Rf = 0, Rs = 0, Rt = 0, Rq = 1.0f;
  float R1 = 0, R2 = 0, R3 = 0;
  float s0 = 0, s1 = 0, s2 = 0, t0 = 0, t1 = 0, t2 = 0;
  float gs1x = 0, gs1y = 0, gt1x = 0, gt1y = 0, Rs1 = 0, Rt1 = 0;
  int part, y, ye, xl, dxl, xr, dxr, x0, x1;
  unsigned int zy = 0;
  ZSpan sp;

  v0 = pv[T->o[0]]; v1 = pv[T->o[1]]; v2 = pv[T->o[2]];
  sp.run = mt ? p->x->run_mt : zp_run;
  /* phase 4 (s31_tfilter.c): this triangle's texture level(s) and filter,
     and whether its colour is perspective-corrected - one call when the
     batch has such a choice, which sets the stages and p->need */
  if (p->xact) {
    const ZPipeX *x = p->x;
    /* a triangle whose q hardly differ keeps the affine colour: one test
       here, not a call (zpx_qspread) */
    if ((p->xact & ZPX_TEX) || x->pc_cur || zpx_qspread(v0->q, v1->q, v2->q))
      if (zpx_tri((ZPipe *)p, T, v0, v1, v2)) sp.run = mt ? p->x->run_lod_mt : zp_run_lod;
  }
  need = p->need;

  /* only the planes the chosen stages read (ZPipe.need, gl_build_pipe),
     each referenced to the centre of pixel (px, py) */
#define PLANE(F, gx, gy, R) do { ZTRI_GRAD(T, v0->F, v1->F, v2->F, gx, gy); \
    R = v0->F + gx * T->ox + gy * T->oy; } while (0)
  if (need & ZP_N_RGBA) {
    PLANE(r, grx, gry, Rr);
    PLANE(g, ggx, ggy, Rg);
    PLANE(b, gbx, gby, Rb);
    PLANE(a, gax, gay, Ra);
  }
  if (need & ZP_N_Q) PLANE(q, gqx, gqy, Rq);
  if (need & ZP_N_F) PLANE(f, gfx, gfy, Rf);
  if (need & ZP_N_ST) {
    /* s/w and t/w exactly as tier 1 forms them (ztriangle.h): the int
       s, t, a whole number of GL_REPEAT periods added when one is negative
       so the int conversion floors, times q */
    int sa = v0->si, sb = v1->si, sc = v2->si, ta = v0->ti, tb = v1->ti, tc = v2->ti;
    int mn;
    if (!p->clamp_s) {
      mn = sa < sb ? sa : sb;
      if (sc < mn) mn = sc;
      if (mn < 0) {
        unsigned int k = (unsigned int)(-mn + zb->tex_speriod - 1) & ~(unsigned int)(zb->tex_speriod - 1);
        sa = (int)((unsigned int)sa + k); sb = (int)((unsigned int)sb + k);
        sc = (int)((unsigned int)sc + k);
      }
    }
    if (!p->clamp_t) {
      mn = ta < tb ? ta : tb;
      if (tc < mn) mn = tc;
      if (mn < 0) {
        unsigned int k = (unsigned int)(-mn + zb->tex_tperiod - 1) & ~(unsigned int)(zb->tex_tperiod - 1);
        ta = (int)((unsigned int)ta + k); tb = (int)((unsigned int)tb + k);
        tc = (int)((unsigned int)tc + k);
      }
    }
    s0 = (float)sa * v0->q; s1 = (float)sb * v1->q; s2 = (float)sc * v2->q;
    t0 = (float)ta * v0->q; t1 = (float)tb * v1->q; t2 = (float)tc * v2->q;
    ZTRI_GRAD(T, s0, s1, s2, gsx, gsy);
    ZTRI_GRAD(T, t0, t1, t2, gtx, gty);
    Rs = s0 + gsx * T->ox + gsy * T->oy;
    Rt = t0 + gtx * T->ox + gty * T->oy;
    /* phase 4 (review 4 R1): for the level per 8-pixel block */
    sp.dszdy = gsy; sp.dtzdy = gty; sp.dfzdy = gqy;
  }
  if (mt && (need & ZP_N_ST1)) {
    /* phase 5 O1: texture unit 1's s/w, t/w, formed as unit 0's are (its
       own fixed point and REPEAT periods, ZPipeX.g1) */
    const ZTexGeo *g = &p->x->g1;
    int sa = v0->si1, sb = v1->si1, sc = v2->si1, ta = v0->ti1, tb = v1->ti1, tc = v2->ti1;
    int mn;
    float u0, u1, u2, w0, w1, w2;
    if (!g->clamp_s) {
      mn = sa < sb ? sa : sb;
      if (sc < mn) mn = sc;
      if (mn < 0) {
        unsigned int k = (unsigned int)(-mn + g->speriod - 1) & ~(unsigned int)(g->speriod - 1);
        sa = (int)((unsigned int)sa + k); sb = (int)((unsigned int)sb + k);
        sc = (int)((unsigned int)sc + k);
      }
    }
    if (!g->clamp_t) {
      mn = ta < tb ? ta : tb;
      if (tc < mn) mn = tc;
      if (mn < 0) {
        unsigned int k = (unsigned int)(-mn + g->tperiod - 1) & ~(unsigned int)(g->tperiod - 1);
        ta = (int)((unsigned int)ta + k); tb = (int)((unsigned int)tb + k);
        tc = (int)((unsigned int)tc + k);
      }
    }
    u0 = (float)sa * v0->q; u1 = (float)sb * v1->q; u2 = (float)sc * v2->q;
    w0 = (float)ta * v0->q; w1 = (float)tb * v1->q; w2 = (float)tc * v2->q;
    ZTRI_GRAD(T, u0, u1, u2, gs1x, gs1y);
    ZTRI_GRAD(T, w0, w1, w2, gt1x, gt1y);
    Rs1 = u0 + gs1x * T->ox + gs1y * T->oy;
    Rt1 = w0 + gt1x * T->ox + gt1y * T->oy;
    sp.dszdy1 = gs1y; sp.dtzdy1 = gt1y; sp.dfzdy = gqy;
  }
  if (need & ZP_N_SPEC) {
    PLANE(sr, g1x, g1y, R1);
    PLANE(sg, g2x, g2y, R2);
    PLANE(sb, g3x, g3y, R3);
  }
  if (need & ZP_N_PC) {
    /* phase 4 F-PERSP: colour times 1/w, divided back per 8 pixels
       (zc_smooth_pc). The planes are kept in the span (pcr, pcgy, and
       the x steps), not in locals: only this triangle's spans read them */
#define PPLANE(F, k, gx) do { float a0_ = v0->F * v0->q, a1_ = v1->F * v1->q, \
      a2_ = v2->F * v2->q; ZTRI_GRAD(T, a0_, a1_, a2_, sp.gx, sp.pcgy[k]); \
      sp.pcr[k] = a0_ + sp.gx * T->ox + sp.pcgy[k] * T->oy; } while (0)
    PPLANE(r, 0, drqdx);
    PPLANE(g, 1, dgqdx);
    PPLANE(b, 2, dbqdx);
    PPLANE(a, 3, daqdx);
#undef PPLANE
  }
#undef PLANE

  sp.drdx = (int)grx; sp.dgdx = (int)ggx; sp.dbdx = (int)gbx;
  sp.dadx = (int)gax;
  sp.dzdx = T->dzdx;
  sp.dszdx = gsx; sp.dtzdx = gtx; sp.dfqdx = gfx; sp.dfzdx = gqx;
  sp.dsrdx = (int)g1x; sp.dsgdx = (int)g2x; sp.dsbdx = (int)g3x;
  sp.z = 0; sp.r = sp.g = sp.b = sp.a = 0;
  sp.sz = sp.tz = sp.fq = 0.0f; sp.fz = 1.0f;
  sp.sr = sp.sg = sp.sb = 0;
  if (mt) {
    sp.dszdx1 = gs1x; sp.dtzdx1 = gt1x;
    sp.sz1 = sp.tz1 = 0.0f;
  }

  /* phase 6 tier 2: a filler that walks the triangle's rows itself (the
     same edges, span values and statements as the loop below, so the same
     bits) - one call a triangle instead of one a span */
  if (mt && !band && p->x->run_tri != NULL && sp.run == p->x->run_mt &&
      (need & (ZP_N_Q | ZP_N_ST | ZP_N_ST1)) == (ZP_N_Q | ZP_N_ST | ZP_N_ST1)) {
    const float pl[15] = { Rq, gqx, gqy, Rs, gsx, gsy, Rt, gtx, gty,
                           Rs1, gs1x, gs1y, Rt1, gt1x, gt1y };
    p->x->run_tri(zb, T, &sp, pl);
    return;
  }

  for (part = 0; part < 2; part++) {
    y = T->part[part].ya; ye = T->part[part].yb;
    if (y >= ye) continue;
    xl = T->part[part].xl; dxl = T->part[part].dxl;
    xr = T->part[part].xr; dxr = T->part[part].dxr;
    zy = T->zc + (unsigned int)T->dzdy * (unsigned int)y;
    for (; y < ye; y++, xl += dxl, xr += dxr, zy += (unsigned int)T->dzdy) {
      float fx, fy;
      /* phase 6: another thread's row (the edges step on regardless) */
      if (band && own != NULL && own[y] != me) continue;
      ZTRI_SPAN(xl, xr, x0, x1);
      if (x1 <= x0) continue;
      fx = (float)(x0 - T->px);
      fy = (float)(y - T->py);
      sp.pp = (PIXEL *)((char *)zb->pbuf + y * zb->linesize) + x0;
      sp.pz = zb->zbuf + y * zb->xsize + x0;
      sp.n = x1 - x0;
      /* tier 1's depth, to the bit */
      sp.z = zy + (unsigned int)T->dzdx * (unsigned int)x0;
      if (need & ZP_N_RGBA) {
        sp.r = (int)(Rr + grx * fx + gry * fy);
        sp.g = (int)(Rg + ggx * fx + ggy * fy);
        sp.b = (int)(Rb + gbx * fx + gby * fy);
        sp.a = (int)(Ra + gax * fx + gay * fy);
      }
      if (need & ZP_N_Q) sp.fz = Rq + gqx * fx + gqy * fy;
      if (need & ZP_N_F) sp.fq = Rf + gfx * fx + gfy * fy;
      if (need & ZP_N_ST) {
        sp.sz = Rs + gsx * fx + gsy * fy;
        sp.tz = Rt + gtx * fx + gty * fy;
      }
      if (mt && (need & ZP_N_ST1)) {
        sp.sz1 = Rs1 + gs1x * fx + gs1y * fy;
        sp.tz1 = Rt1 + gt1x * fx + gt1y * fy;
      }
      if (need & ZP_N_SPEC) {
        sp.sr = (int)(R1 + g1x * fx + g1y * fy);
        sp.sg = (int)(R2 + g2x * fx + g2y * fy);
        sp.sb = (int)(R3 + g3x * fx + g3y * fy);
      }
      if (need & ZP_N_PC) {
        sp.rq = sp.pcr[0] + sp.drqdx * fx + sp.pcgy[0] * fy;
        sp.gq = sp.pcr[1] + sp.dgqdx * fx + sp.pcgy[1] * fy;
        sp.bq = sp.pcr[2] + sp.dbqdx * fx + sp.pcgy[2] * fy;
        sp.aq = sp.pcr[3] + sp.daqdx * fx + sp.pcgy[3] * fy;
      }
      /* zp_run, or zp_run_lod for a triangle whose mipmap level is chosen
         per 8-pixel block (zpx_tri): one load from the span, not a test
         (a test, or the runner in ZPipe, kept one more register live
         across the span loop: +0.4% on untextured general-path frames) */
      sp.run(zb, &sp);
    }
  }
}

/* (each instance in its own object - ztriangle_genmt.c includes this file
   with ZTRI_GEN_MT - so the single-unit one is compiled, inlined and
   register-allocated exactly as before phase 5) */
#ifndef ZTRI_GEN_MT
#define FILL_BODY ZB_fillBodyGeneral
#define FILL_TRI ZB_fillTriangleGeneral
#define FILL_MT 0
#else
/* phase 5 O1 */
#define FILL_BODY ZB_fillBodyGeneralMT
#define FILL_TRI ZB_fillTriangleGeneralMT
#define FILL_MT 1
#endif

__attribute__((noinline))
void FILL_BODY(ZBuffer *zb, const ZTri *T, const ZVtxG *a, const ZVtxG *b,
               const ZVtxG *c, const unsigned char *own, int me)
{
  fill_body(zb, T, a, b, c, FILL_MT, 1, own, me);
}

/* the set-up, which writes shared state (the depth epoch, the dirty box),
   runs once, on the application's thread; with S31GL_THREADS the triangle
   is queued for the worker's rows (s31_thr.c) and this thread draws its
   own */
void FILL_TRI(ZBuffer *zb, const ZVtxG *a, const ZVtxG *b, const ZVtxG *c,
              int textured)
{
  const ZPipe *p = zb->pipe;
  const unsigned char *own;
  ZTri T;

  (void)textured;
  if (!ztri_setup(&T, a->x, a->y, a->z, b->x, b->y, b->z, c->x, c->y, c->z,
                  p))
    return;
  ztri_zepoch(&T, p, a->z, b->z, c->z);
  ztri_rows(&T, p);
  if (__builtin_expect(p->thr != NULL, 0)) {
    own = s31t_tri(zb, &T, a, b, c, FILL_MT);
    if (own == (const unsigned char *)1) return;      /* all the worker's */
    FILL_BODY(zb, &T, a, b, c, own, 0);
    return;
  }
  fill_body(zb, &T, a, b, c, FILL_MT, 0, NULL, 0);
}
