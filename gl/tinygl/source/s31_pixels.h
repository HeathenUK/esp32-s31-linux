/* s31_pixels.h - client pixel unpacking; see s31_pixels.c. s31, MIT. */
#ifndef S31_PIXELS_H
#define S31_PIXELS_H

typedef struct {
  const unsigned char *base;   /* first pixel after SKIP_ROWS/SKIP_PIXELS */
  int pitch;                   /* bytes between rows (ROW_LENGTH, ALIGNMENT) */
  int group;                   /* bytes per pixel */
  int elem;                    /* bytes per component */
  int format, type, swap;
  int width, height;
} S31Unpack;

int s31_unpack_setup(GLContext *c, S31Unpack *u, int width, int height,
                     int format, int type, const void *pixels);
void s31_unpack_pixel(const S31Unpack *u, int x, int y, unsigned char *rgba);

#endif
