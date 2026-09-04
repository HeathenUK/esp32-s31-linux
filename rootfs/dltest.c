/* How long does merely LOADING the Qt stack take? dlopen does the mmap, the
 * relocations and - on musl, which has no lazy PLT - every symbol resolution,
 * but runs none of Qt's own initialisation. The difference between this and a
 * full start is Qt's constructors and QApplication setup. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int argc,char**argv){
	double a,b;
	void *h;
	const char *lib = argc>1?argv[1]:"/opt/qt5/lib/libQt5Widgets.so.5";
	a=now(); h=dlopen(lib, RTLD_NOW|RTLD_GLOBAL); b=now();
	printf("dlopen(%s) RTLD_NOW: %.2f s %s\n", lib, b-a, h?"ok":dlerror());
	return 0;
}
