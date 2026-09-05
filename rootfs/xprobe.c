/* Does the X11 path SDL uses actually work? dlopen + dlsym + connect,
 * with nothing of SDL in the way. */
#include <dlfcn.h>
#include <stdio.h>
int main(void)
{
	void *h = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
	void *(*open_dpy)(const char *);
	void *d;
	const char *miss[] = { "XOpenDisplay", "XCreateImage", "XPutImage",
			       "XCheckTypedEvent", "XMaskEvent", "XQueryKeymap",
			       "XKeysymToKeycode", "Xutf8LookupString",
			       "XGetWMHints", "XInstallColormap", NULL };
	int i;

	if (!h) { printf("PROBE dlopen libX11 FAILED: %s\n", dlerror()); return 1; }
	printf("PROBE dlopen libX11 ok\n");
	for (i = 0; miss[i]; i++)
		if (!dlsym(h, miss[i]))
			printf("PROBE dlsym MISSING %s\n", miss[i]);
	if (!dlopen("libXext.so.6", RTLD_NOW | RTLD_GLOBAL))
		printf("PROBE dlopen libXext FAILED: %s\n", dlerror());
	else
		printf("PROBE dlopen libXext ok\n");
	open_dpy = (void *(*)(const char *))dlsym(h, "XOpenDisplay");
	if (!open_dpy) { printf("PROBE no XOpenDisplay\n"); return 1; }
	d = open_dpy(NULL);
	printf("PROBE XOpenDisplay(NULL) -> %s\n", d ? "OK" : "NULL (cannot connect)");
	return 0;
}
