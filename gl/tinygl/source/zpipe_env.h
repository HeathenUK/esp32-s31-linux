/* zpipe_env.h - the texture environment stages of zpipe.c (GL 1.3 table
   3.22), included twice: with T_RGB / T_A reading an RGB565 texel and its
   A8 plane (ZE(n) = ze_n), and (phase 5) reading an 8-bit texel's word in
   ZTexF.ftex32 (ZE(n) = ze8_n). s31, MIT. */
ZP_TEXENV(ZE(replace_rgb), T_RGB SET_C(tr, tg, tb))
ZP_TEXENV(ZE(replace_rgba), T_RGB T_A SET_C(tr, tg, tb) f->a[i] = (unsigned char)ta;)
ZP_TEXENV(ZE(mod_rgb), T_RGB
  SET_C(MUL8(f->r[i], tr), MUL8(f->g[i], tg), MUL8(f->b[i], tb)))
ZP_TEXENV(ZE(mod_rgba), T_RGB T_A
  SET_C(MUL8(f->r[i], tr), MUL8(f->g[i], tg), MUL8(f->b[i], tb))
  f->a[i] = (unsigned char)MUL8(f->a[i], ta);)
ZP_TEXENV(ZE(decal_rgba), T_RGB T_A
  SET_C(MIX8(f->r[i], 255 - ta, tr, ta), MIX8(f->g[i], 255 - ta, tg, ta),
        MIX8(f->b[i], 255 - ta, tb, ta)))
ZP_TEXENV(ZE(blend_rgb), T_RGB
  SET_C(BLENDK(f->r[i], tr, p->envc[0]), BLENDK(f->g[i], tg, p->envc[1]),
        BLENDK(f->b[i], tb, p->envc[2])))
ZP_TEXENV(ZE(blend_rgba), T_RGB T_A
  SET_C(BLENDK(f->r[i], tr, p->envc[0]), BLENDK(f->g[i], tg, p->envc[1]),
        BLENDK(f->b[i], tb, p->envc[2]))
  f->a[i] = (unsigned char)MUL8(f->a[i], ta);)
ZP_TEXENV(ZE(blend_i), T_RGB T_A
  SET_C(BLENDK(f->r[i], tr, p->envc[0]), BLENDK(f->g[i], tg, p->envc[1]),
        BLENDK(f->b[i], tb, p->envc[2]))
  f->a[i] = (unsigned char)BLENDK(f->a[i], ta, p->envc[3]);)
ZP_TEXENV(ZE(alpha_replace), T_A f->a[i] = (unsigned char)ta;)
ZP_TEXENV(ZE(alpha_mod), T_A f->a[i] = (unsigned char)MUL8(f->a[i], ta);)
ZP_TEXENV(ZE(add_rgb), T_RGB
  SET_C(clamp255(f->r[i] + tr), clamp255(f->g[i] + tg), clamp255(f->b[i] + tb)))
ZP_TEXENV(ZE(add_rgba), T_RGB T_A
  SET_C(clamp255(f->r[i] + tr), clamp255(f->g[i] + tg), clamp255(f->b[i] + tb))
  f->a[i] = (unsigned char)MUL8(f->a[i], ta);)
ZP_TEXENV(ZE(add_i), T_RGB T_A
  SET_C(clamp255(f->r[i] + tr), clamp255(f->g[i] + tg), clamp255(f->b[i] + tb))
  f->a[i] = (unsigned char)clamp255(f->a[i] + ta);)

/* phase 5: REPLACE of an RGBA texture stored without its A8 plane (every
   alpha 255: GLTexture.a1) */
ZP_TEXENV(ZE(replace_rgb1), T_RGB SET_C(tr, tg, tb) f->a[i] = 255;)
