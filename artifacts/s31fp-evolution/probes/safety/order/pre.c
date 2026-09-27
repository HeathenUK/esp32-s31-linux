#define _GNU_SOURCE
#include <link.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
static int cb(struct dl_phdr_info *i, size_t n, void *d){
  fprintf(stderr,"  [pre] obj '%s' adds=%llu subs=%llu\n", i->dlpi_name, i->dlpi_adds, i->dlpi_subs); return 0; }
__attribute__((constructor)) static void c(void){
  write(2,"CTOR pre\n",9);
  dl_iterate_phdr(cb,0);
}
