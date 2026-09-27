/* build-then-swap prototype on the MAIN executable's text; syscall-counted under qemu -strace */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/auxv.h>
#include <elf.h>
#define PG 4096u
#define F(n) __attribute__((noinline,aligned(4096))) int f##n(int x){ return x+ n*10+1; }
F(0) F(1) F(2) F(3) F(4) F(5)
static int (*fs[6])(int)={f0,f1,f2,f3,f4,f5};
static uintptr_t seg_vaddr, seg_off, bias;
static void find_text(void){
  Elf32_Phdr *ph=(void*)getauxval(AT_PHDR); unsigned n=getauxval(AT_PHNUM);
  for(unsigned i=0;i<n;i++) if(ph[i].p_type==PT_PHDR) bias=(uintptr_t)ph-ph[i].p_vaddr;
  for(unsigned i=0;i<n;i++) if(ph[i].p_type==PT_LOAD&&(ph[i].p_flags&PF_X)){seg_vaddr=ph[i].p_vaddr;seg_off=ph[i].p_offset;}
}
static void patch_in(uint8_t *frame, uintptr_t page, int k){ /* make f_k return x+42+k*100 : addi a0,a0,imm ; ret */
  uintptr_t f=(uintptr_t)fs[k]; uint16_t *p=(uint16_t*)(frame+(f-page));
  int imm=42+k*100; uint32_t i0=0x00050513u|((uint32_t)imm<<20); p[0]=i0; p[1]=i0>>16; p[2]=0x8082; }
int main(int argc,char**argv){
  int bulk=argc>1&&argv[1][0]=='b', rb=argc>1&&argv[1][0]=='r';
  find_text();
  uintptr_t lo=(uintptr_t)f0&~(PG-1), hi=((uintptr_t)f5&~(PG-1))+PG; unsigned np=(hi-lo)/PG;
  int before[6],after[6],err=0; for(int k=0;k<6;k++) before[k]=fs[k](5);
  write(2,"--ENGINE--\n",11);
  if(bulk){
    uint8_t *fr=mmap(0,np*PG,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE,-1,0);
    memcpy(fr,(void*)lo,np*PG);
    for(int k=0;k<6;k++) patch_in(fr+(((uintptr_t)fs[k]&~(PG-1))-lo),(uintptr_t)fs[k]&~(PG-1),k);
    if(mprotect(fr,np*PG,PROT_READ|PROT_EXEC)) err|=1;
    if(mremap(fr,np*PG,np*PG,MREMAP_MAYMOVE|MREMAP_FIXED,(void*)lo)!=(void*)lo) err|=2;
  } else {
    uint8_t *fr=mmap(0,np*PG,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE,-1,0);
    for(int k=0;k<6;k++){ uintptr_t pg=(uintptr_t)fs[k]&~(PG-1); uint8_t *f=fr+k*PG;
      memcpy(f,(void*)pg,PG); patch_in(f,pg,k);
      if(mprotect(f,PG,PROT_READ|PROT_EXEC)) err|=1;
      if(mremap(f,PG,PG,MREMAP_MAYMOVE|MREMAP_FIXED,(void*)pg)!=(void*)pg) err|=2; }
  }
  __builtin___clear_cache((char*)lo,(char*)hi);
  write(2,"--END--\n",8);
  for(int k=0;k<6;k++) after[k]=fs[k](5);
  if(rb){ /* simulate destructive failure on page of f3, roll back from /proc/self/exe */
    uintptr_t pg=(uintptr_t)f3&~(PG-1); munmap((void*)pg,PG);
    int fd=open("/proc/self/exe",O_RDONLY); off_t off=(pg-bias-seg_vaddr)+seg_off;
    void *r=mmap((void*)pg,PG,PROT_READ|PROT_EXEC,MAP_PRIVATE|MAP_FIXED,fd,off); close(fd);
    printf("rollback mmap %s off=%#lx f3(5)=%d (orig %d)\n", r==(void*)pg?"ok":"FAIL",(long)off,f3(5),before[3]);
  }
  for(int k=0;k<6;k++) printf("f%d: before %d after %d want %d %s\n",k,before[k],after[k],5+42+k*100, after[k]==5+42+k*100?"OK":"BAD");
  printf("np=%u err=%d mode=%s\n",np,err,bulk?"bulk":"per-page");
  return err;
}
