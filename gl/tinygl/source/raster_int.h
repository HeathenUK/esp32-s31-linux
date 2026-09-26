/* raster_int.h - helpers shared by raster.c and raster_sel.c. s31, MIT. */
#ifndef RASTER_INT_H
#define RASTER_INT_H
/* colour scale (ZB_POINT_*_MIN..MAX) -> 8.16, 0.5 rounded in */
#define KR (255.0f * 65536.0f / (float)(ZB_POINT_RED_MAX - ZB_POINT_RED_MIN))
#define KG (255.0f * 65536.0f / (float)(ZB_POINT_GREEN_MAX - ZB_POINT_GREEN_MIN))
#define KB (255.0f * 65536.0f / (float)(ZB_POINT_BLUE_MAX - ZB_POINT_BLUE_MIN))

static inline int c816(int v, int lo, float k)
{
  return (int)((float)(v - lo) * k) + (1 << (ZP_CSHIFT - 1));
}

static inline int c8(int v, int lo, float k)
{
  int x = c816(v, lo, k) >> ZP_CSHIFT;
  return x < 0 ? 0 : (x > 255 ? 255 : x);
}

static inline unsigned char f8(float v)
{
  return (unsigned char)(v <= 0.0f ? 0 : (v >= 1.0f ? 255 : (int)(v * 255.0f + 0.5f)));
}

/* raster_sel.c */
void gl_build_pipe(GLContext *c);
#endif
