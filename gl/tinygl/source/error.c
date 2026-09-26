#include <stdarg.h>
#include <string.h>
#include "zgl.h"

/*
 * s31: TinyGL used to exit(1) here. A GL library must never take the
 * application down, so this now prints the message once per distinct
 * format string and returns; every caller was changed to return safely
 * after it (and to record a GL error where the spec has one).
 */
#define WARN_SLOTS 256
static const char *warned[WARN_SLOTS];
static int nwarned;

static int first_time(const char *key)
{
  int i;
  /* s31: callers pass string literals and some sit on hot paths (every
     glBegin, every glEnable), so settle the common case - the same literal
     again - by pointer before any strcmp (review finding R3) */
  for (i = 0; i < nwarned; i++)
    if (warned[i] == key) return 0;
  for (i = 0; i < nwarned; i++)
    if (strcmp(warned[i], key) == 0) return 0;
  if (nwarned >= WARN_SLOTS) return 0;   /* full: stay quiet, never spam */
  warned[nwarned++] = key;
  return 1;
}

void gl_fatal_error(char *format, ...)
{
  va_list ap;

  if (!first_time(format)) return;
  va_start(ap,format);
  fprintf(stderr,"libGL: ");
  vfprintf(stderr,format,ap);
  fprintf(stderr,"\n");
  va_end(ap);
}

/* "libGL: unimplemented <what>", once per distinct string (string literals) */
void gl_warn_once(const char *what)
{
  if (!first_time(what)) return;
  fprintf(stderr, "libGL: unimplemented %s\n", what);
}

void gl_note_once(const char *what)
{
  if (!first_time(what)) return;
  fprintf(stderr, "libGL: approximated %s\n", what);
}

void tgl_warn_once(const char *what)
{
  gl_warn_once(what);
}
