#define _GNU_SOURCE
#include <link.h>
#include <stdio.h>
#include <dlfcn.h>
static int cb(struct dl_phdr_info *i, size_t n, void *d){
  for (int k=0;k<i->dlpi_phnum;k++) if (i->dlpi_phdr[k].p_type==PT_LOAD && (i->dlpi_phdr[k].p_flags&PF_X))
    fprintf(stderr,"obj '%s' bias=%#lx Xseg vaddr=%#lx memsz=%#lx adds=%llu\n", i->dlpi_name, (unsigned long)i->dlpi_addr,(unsigned long)i->dlpi_phdr[k].p_vaddr,(unsigned long)i->dlpi_phdr[k].p_memsz, i->dlpi_adds);
  return 0; }
__attribute__((constructor)) static void c(void){
  int *d = dlsym(RTLD_DEFAULT,"dep_inited");
  fprintf(stderr,"preload ctor: dep_inited=%d\n", d?*d:-1);
  dl_iterate_phdr(cb,0);
}
