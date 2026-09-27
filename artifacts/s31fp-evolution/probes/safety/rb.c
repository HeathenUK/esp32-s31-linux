#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <link.h>
#include <dlfcn.h>
static uintptr_t segoff, segva; static const char *nm;
static int cb(struct dl_phdr_info *i, size_t s, void *d){
  if(!strstr(i->dlpi_name,"libl.so")) return 0;
  for(int k=0;k<i->dlpi_phnum;k++){const ElfW(Phdr)*p=&i->dlpi_phdr[k];
    if(p->p_type==PT_LOAD && (p->p_flags&PF_X)){segoff=p->p_offset; segva=i->dlpi_addr+p->p_vaddr; nm=i->dlpi_name;}}
  return 0;}
#include <string.h>
int main(void){
  void *h=dlopen("./libl.so",RTLD_NOW); int(*rf)(int)=dlsym(h,"lf");
  dl_iterate_phdr(cb,0);
  uintptr_t pg=(uintptr_t)rf&~4095u;
  printf("seg va %#lx off %#lx fn page %#lx before %d\n",(long)segva,(long)segoff,(long)pg,rf(5));
  munmap((void*)pg,4096);             /* simulate destructive MREMAP_FIXED failure */
  int fd=open(nm,O_RDONLY);
  void *r=mmap((void*)pg,4096,PROT_READ|PROT_EXEC,MAP_PRIVATE|MAP_FIXED,fd,(pg-segva)+segoff);
  close(fd);
  printf("restore %p ok=%d after %d\n",r,r==(void*)pg,rf(5));
  return 0;}
