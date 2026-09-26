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
  unsigned short *pz1;
  PIXEL *pp1;
  int part, y_, ye_, xl_, dxl_, xr_, dxr_, x1, x2;
  const int *box_ = zb->pipe->box;

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

  if (!ztri_setup(&T_, p0->fx, p0->fy, p0->z, p1->fx, p1->fy, p1->z,
                  p2->fx, p2->fy, p2->z, box_))
    return;
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

    for (; y_ < ye_; y_++) {
      ZTRI_SPAN(xl_, xr_, x1, x2);
      x2--;                                   /* the last pixel */
      if (x2 >= x1) {
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
#ifndef DRAW_LINE
      /* generic draw line */
      {
          register PIXEL *pp;
          register int n;
#ifdef INTERP_Z
          register unsigned short *pz;
          register unsigned int z,zz;
#endif

          n=x2 - x1;
          pp=(PIXEL *)((char *)pp1 + x1 * PSZB);
#ifdef INTERP_Z
          pz=pz1+x1;
          z=z1;
#endif
          while (n>=3) {
              PUT_PIXEL(0);
              PUT_PIXEL(1);
              PUT_PIXEL(2);
              PUT_PIXEL(3);
#ifdef INTERP_Z
              pz+=4;
#endif
              pp=(PIXEL *)((char *)pp + 4 * PSZB);
              n-=4;
          }
          while (n>=0) {
              PUT_PIXEL(0);
#ifdef INTERP_Z
              pz+=1;
#endif
              pp=(PIXEL *)((char *)pp + PSZB);
              n-=1;
          }
      }
#else
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
    }
  }
}

#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_STZ

#undef DRAW_INIT
#undef DRAW_LINE
#undef PUT_PIXEL
