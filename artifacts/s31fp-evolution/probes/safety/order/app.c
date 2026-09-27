#define _GNU_SOURCE
#include <link.h>
#include <dlfcn.h>
#include <stdio.h>
int fA(void); int fC(void);
static unsigned long long g; static int nobj;
static int cb(struct dl_phdr_info *i, size_t n, void *d){ g=i->dlpi_adds; nobj++; if(d) fprintf(stderr,"    obj '%s' adds=%llu\n", i->dlpi_name, i->dlpi_adds); return 0; }
static void snap(const char *w, int v){ nobj=0; dl_iterate_phdr(cb,(void*)(long)v); fprintf(stderr,"%-34s adds=%llu nobj=%d\n", w, g, nobj); }
int main(){
  fprintf(stderr,"main fA+fC=%d\n", fA()+fC());
  snap("start",1);
  void *h=dlopen("./libD.so",RTLD_NOW); snap("dlopen libD (new, +deps E,F)",1);
  dlopen("./libD.so",RTLD_NOW); snap("dlopen libD again (already loaded)",0);
  dlopen("./libA.so",RTLD_NOW); snap("dlopen libA (DT_NEEDED, loaded)",0);
  void *x=dlopen("./libD.so",RTLD_NOW|RTLD_NOLOAD); snap("dlopen libD RTLD_NOLOAD",0);
  x=dlopen("./nonexist.so",RTLD_NOW); snap(x?"??":"dlopen nonexistent (fails)",0);
  x=dlopen(0,RTLD_NOW); snap("dlopen(NULL)",0);
  dlclose(h); snap("dlclose libD",0);
  return 0;
}
