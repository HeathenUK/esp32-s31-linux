/* TEST-ONLY: the two Xlib calls stock glxgears imports that xlite lacks
 * (XSetNormalHints, XSetStandardProperties), as no-ops, so glxgears can be
 * run over xlite on the host. The real fix belongs in xlite. */
int XSetNormalHints(void *d, unsigned long w, void *h) { (void)d; (void)w; (void)h; return 1; }
int XSetStandardProperties(void *d, unsigned long w, const char *a, const char *b,
			   unsigned long p, char **argv, int argc, void *h)
{ (void)d; (void)w; (void)a; (void)b; (void)p; (void)argv; (void)argc; (void)h; return 1; }
