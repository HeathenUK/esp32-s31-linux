/*
 * We draw a triangle with various interpolations
 *
 * s31: the scan conversion is GL's (ztri.h), shared with the general
 * filler so the two rasteriser paths generate the same fragments with the
 * same depth (review P1/G1): pixel centres, top-left rule, unsnapped window
 * coordinates, one integer depth plane per triangle. What each filler does
 * per pixel (PUT_PIXEL / DRAW_LINE in ztriangle.c) is TinyGL's, unchanged.
 * Every attribute starts each row at its value at the first pixel's centre:
 *   z, r, g, b  integer planes (ztri_iplane): c + dx x + dy y
 *   s/w, t/w, 1/w  float planes about the reference pixel (px, py)
 * so textures sample the texel under the pixel centre, as the general path
 * does (review G2; TinyGL's texture squeeze is gone). The row loop gives
 * DRAW_LINE x1 (first pixel), x2 (last pixel, inclusive), pp1 / pz1 (the
 * row) and z1, r1 g1 b1, sz1 tz1 q1 (the first pixel's values).
 */
/* ztriangle.c includes ztri.h and zpipe.h (this is inside a function) */
{
  ZTri T_;
  ZBufferPoint *pv_[3];
  unsigned short *pz1, *pze1_;
  PIXEL *pp1;
  int part, y_, ye_, xl_, dxl_, xr_, dxr_, x1, x2;

#ifdef INTERP_Z
  unsigned int zy_, z1;
  int dzdx, dzdy;
#endif
#ifdef INTERP_RGB
  unsigned int rc_, gc_, bc_, ry_, gy_, by_;
  int r1, g1, b1, drdx, drdy, dgdx, dgdy, dbdx, dbdy;
#endif
#ifdef INTERP_STZ
  float sz1, tz1, q1, dszdx, dszdy, dtzdx, dtzdy, dqdx, dqdy;
  float szr_, tzr_, qr_;
#endif
#ifdef INTERP_PRGB
  /* phase 4 F-PERSP: r/w, g/w, b/w (the fillers' colour units) and 1/w */
  float rq1, gq1, bq1, q1, drqdx, drqdy, dgqdx, dgqdy, dbqdx, dbqdy, dqdx, dqdy;
  float rqr_, gqr_, bqr_, qr_;
#endif

  if (!ztri_setup(&T_, p0->fx, p0->fy, p0->z, p1->fx, p1->fy, p1->z,
                  p2->fx, p2->fy, p2->z, zb->pipe))
    return;
  ztri_zepoch(&T_, zb->pipe, p0->z, p1->z, p2->z);
  ztri_rows(&T_, zb->pipe);
  pv_[0] = p0; pv_[1] = p1; pv_[2] = p2;

#ifdef INTERP_Z
  dzdx = T_.dzdx; dzdy = T_.dzdy;
#endif
#ifdef INTERP_RGB
  {
    const ZBufferPoint *a_ = pv_[T_.o[0]], *b_ = pv_[T_.o[1]], *c_ = pv_[T_.o[2]];
    ztri_iplane(&T_, a_->r, b_->r, c_->r, &rc_, &drdx, &drdy);
    ztri_iplane(&T_, a_->g, b_->g, c_->g, &gc_, &dgdx, &dgdy);
    ztri_iplane(&T_, a_->b, b_->b, c_->b, &bc_, &dbdx, &dbdy);
  }
#endif
#ifdef INTERP_STZ
  {
    /* s/w, t/w, 1/w (GL 3.8 perspective); REPEAT textures only reach
       tier 1, so a whole number of periods can be added to s or t: a
       triangle with a negative coordinate is moved to positive ones, where
       the int conversion of the fillers is floor, as GL's nearest texel is
       (tex_period: raster_sel.c) */
    const ZBufferPoint *a_ = pv_[T_.o[0]], *b_ = pv_[T_.o[1]], *c_ = pv_[T_.o[2]];
    int sa_ = a_->s, sb_ = b_->s, sc_ = c_->s, ta_ = a_->t, tb_ = b_->t, tc_ = c_->t;
    int mn_ = sa_ < sb_ ? sa_ : sb_;
    if (sc_ < mn_) mn_ = sc_;
    if (mn_ < 0) {
      unsigned int k_ = (unsigned int)(-mn_ + zb->tex_speriod - 1) & ~(unsigned int)(zb->tex_speriod - 1);
      sa_ = (int)((unsigned int)sa_ + k_); sb_ = (int)((unsigned int)sb_ + k_);
      sc_ = (int)((unsigned int)sc_ + k_);
    }
    mn_ = ta_ < tb_ ? ta_ : tb_;
    if (tc_ < mn_) mn_ = tc_;
    if (mn_ < 0) {
      unsigned int k_ = (unsigned int)(-mn_ + zb->tex_tperiod - 1) & ~(unsigned int)(zb->tex_tperiod - 1);
      ta_ = (int)((unsigned int)ta_ + k_); tb_ = (int)((unsigned int)tb_ + k_);
      tc_ = (int)((unsigned int)tc_ + k_);
    }
    {
      float s0_ = (float)sa_ * a_->q, s1_ = (float)sb_ * b_->q, s2_ = (float)sc_ * c_->q;
      float t0_ = (float)ta_ * a_->q, t1_ = (float)tb_ * b_->q, t2_ = (float)tc_ * c_->q;
      ZTRI_GRAD(&T_, s0_, s1_, s2_, dszdx, dszdy);
      ZTRI_GRAD(&T_, t0_, t1_, t2_, dtzdx, dtzdy);
      ZTRI_GRAD(&T_, a_->q, b_->q, c_->q, dqdx, dqdy);
      /* at the reference pixel's centre */
      szr_ = s0_ + dszdx * T_.ox + dszdy * T_.oy;
      tzr_ = t0_ + dtzdx * T_.ox + dtzdy * T_.oy;
      qr_ = a_->q + dqdx * T_.ox + dqdy * T_.oy;
    }
  }
#endif

#ifdef INTERP_PRGB
  {
    const ZBufferPoint *a_ = pv_[T_.o[0]], *b_ = pv_[T_.o[1]], *c_ = pv_[T_.o[2]];
    float r0_ = (float)a_->r * a_->q, r1_ = (float)b_->r * b_->q, r2_ = (float)c_->r * c_->q;
    float g0_ = (float)a_->g * a_->q, g1_ = (float)b_->g * b_->q, g2_ = (float)c_->g * c_->q;
    float b0_ = (float)a_->b * a_->q, b1_ = (float)b_->b * b_->q, b2_ = (float)c_->b * c_->q;
    ZTRI_GRAD(&T_, r0_, r1_, r2_, drqdx, drqdy);
    ZTRI_GRAD(&T_, g0_, g1_, g2_, dgqdx, dgqdy);
    ZTRI_GRAD(&T_, b0_, b1_, b2_, dbqdx, dbqdy);
    ZTRI_GRAD(&T_, a_->q, b_->q, c_->q, dqdx, dqdy);
    rqr_ = r0_ + drqdx * T_.ox + drqdy * T_.oy;
    gqr_ = g0_ + dgqdx * T_.ox + dgqdy * T_.oy;
    bqr_ = b0_ + dbqdx * T_.ox + dbqdy * T_.oy;
    qr_ = a_->q + dqdx * T_.ox + dqdy * T_.oy;
  }
#endif

  DRAW_INIT();

  for (part = 0; part < 2; part++) {
    y_ = T_.part[part].ya; ye_ = T_.part[part].yb;
    if (y_ >= ye_) continue;
    xl_ = T_.part[part].xl; dxl_ = T_.part[part].dxl;
    xr_ = T_.part[part].xr; dxr_ = T_.part[part].dxr;
    pp1 = (PIXEL *) ((char *) zb->pbuf + zb->linesize * y_);
    pz1 = zb->zbuf + y_ * zb->xsize;
#ifdef INTERP_Z
    zy_ = T_.zc + (unsigned int)dzdy * (unsigned int)y_;
#endif
#ifdef INTERP_RGB
    ry_ = rc_ + (unsigned int)drdy * (unsigned int)y_;
    gy_ = gc_ + (unsigned int)dgdy * (unsigned int)y_;
    by_ = bc_ + (unsigned int)dbdy * (unsigned int)y_;
#endif

    /* s31 (phase 3a): the rows end at a depth-row pointer, not a count (the
       textured filler still steps y_ for its planes) */
    pze1_ = pz1 + (ye_ - y_) * zb->xsize;
    for (; pz1 != pze1_; ) {
      /* s31 (phase 3a): x2 is one past the last pixel here, and the
         DRAW_LINE macros get it as the last pixel (x2 - 1) */
      ZTRI_SPAN(xl_, xr_, x1, x2);
      if (x2 > x1) {
#ifdef INTERP_Z
        z1 = zy_ + (unsigned int)dzdx * (unsigned int)x1;
#endif
#ifdef INTERP_RGB
        r1 = (int)(ry_ + (unsigned int)drdx * (unsigned int)x1);
        g1 = (int)(gy_ + (unsigned int)dgdx * (unsigned int)x1);
        b1 = (int)(by_ + (unsigned int)dbdx * (unsigned int)x1);
#endif
#ifdef INTERP_STZ
        {
          float fx_ = (float)(x1 - T_.px), fy_ = (float)(y_ - T_.py);
          sz1 = szr_ + dszdx * fx_ + dszdy * fy_;
          tz1 = tzr_ + dtzdx * fx_ + dtzdy * fy_;
          q1 = qr_ + dqdx * fx_ + dqdy * fy_;
        }
#endif
#ifdef INTERP_PRGB
        {
          float fx_ = (float)(x1 - T_.px), fy_ = (float)(y_ - T_.py);
          rq1 = rqr_ + drqdx * fx_ + drqdy * fy_;
          gq1 = gqr_ + dgqdx * fx_ + dgqdy * fy_;
          bq1 = bqr_ + dbqdx * fx_ + dbqdy * fy_;
          q1 = qr_ + dqdx * fx_ + dqdy * fy_;
        }
#endif
#ifndef DRAW_LINE
      /* generic draw line (the flat fillers). s31 (phase 3a): the spans
         are short (gears: 4.8 pixels), so what costs is the span, not the
         pixel: two pixels a turn, the odd one first, and the end is a
         pointer - no count, no remainder loop to set up. (Review 3a,
         measured: on 640-pixel spans - geo10 640x400, one full-screen
         quad - this costs +1.0% of the frame against TinyGL's four a
         turn; a four-a-turn loop for long spans is untried.) */
      {
          PIXEL *pp = (PIXEL *)((char *)pp1 + x1 * PSZB);
#ifdef INTERP_Z
          unsigned short *pz = pz1 + x1;
          unsigned int z = z1, zz;
#endif
          PIXEL *ppe = (PIXEL *)((char *)pp1 + x2 * PSZB);

          if ((x2 - x1) & 1) {
              PUT_PIXEL(0);
#ifdef INTERP_Z
              pz += 1;
#endif
              pp = (PIXEL *)((char *)pp + PSZB);
          }
          while (pp != ppe) {
              PUT_PIXEL(0);
              PUT_PIXEL(1);
#ifdef INTERP_Z
              pz += 2;
#endif
              pp = (PIXEL *)((char *)pp + 2 * PSZB);
          }
      }
#else
      x2--;                                   /* the last pixel */
      DRAW_LINE();
#endif
      }
      xl_ += dxl_; xr_ += dxr_;
#ifdef INTERP_Z
      zy_ += (unsigned int)dzdy;
#endif
#ifdef INTERP_RGB
      ry_ += (unsigned int)drdy; gy_ += (unsigned int)dgdy; by_ += (unsigned int)dbdy;
#endif
      pp1=(PIXEL *)((char *)pp1 + zb->linesize);
      pz1+=zb->xsize;
#if defined(INTERP_STZ) || defined(INTERP_PRGB)
      y_++;
#endif
    }
  }
}

#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_STZ
#undef INTERP_PRGB

#undef DRAW_INIT
#undef DRAW_LINE
#undef PUT_PIXEL
