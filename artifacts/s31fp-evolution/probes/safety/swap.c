#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <link.h>
#include <dlfcn.h>

static int cb(struct dl_phdr_info *i, size_t s, void *d){ printf("obj %s adds=%llu\n", i->dlpi_name, (unsigned long long)i->dlpi_adds); return 0; }
int main(void){
  dl_iterate_phdr(cb,0);
  uintptr_t f;
  void *h=dlopen("./libl.so",RTLD_NOW); int(*rf)(int)=dlsym(h,"lf");
  f=(uintptr_t)rf; uintptr_t pg=f&~4095u;
  printf("before %d\n", rf(5));
  uint8_t *n=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE,-1,0);
  memcpy(n,(void*)pg,4096);
  /* patch lf to: li a0,42; ret  (c.li a0,42 not valid >31; use addi a0,x0,42 = 0x02a00513, ret=0x8082) */
  uint16_t *p=(uint16_t*)(n+(f-pg)); uint32_t i0=0x02a00513; p[0]=i0; p[1]=i0>>16; p[2]=0x8082;
  if(mprotect(n,4096,PROT_READ|PROT_EXEC)) {perror("mprotect");return 1;}
  void *r=mremap(n,4096,4096,MREMAP_MAYMOVE|MREMAP_FIXED,(void*)pg);
  printf("mremap %p want %p\n", r,(void*)pg);
  __builtin___clear_cache((char*)pg,(char*)pg+4096);
  printf("after %d\n", rf(5));
  dl_iterate_phdr(cb,0);
  return 0;
}
