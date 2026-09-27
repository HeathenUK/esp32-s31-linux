#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
typedef double (*dd)(double,double);
extern double __muldf3(double,double), __adddf3(double,double), __divdf3(double,double);
static uint64_t s=88172645463325252ULL; static uint64_t rnd(void){s^=s<<13;s^=s>>7;s^=s<<17;return s;}
static double rd(void){ double x; uint64_t b=rnd(); b=(b&0x800fffffffffffffULL)|((uint64_t)(1023-20+(rnd()%40))<<52); memcpy(&x,&b,8); return x;}
static inline void setfcsr(unsigned v){__asm__ volatile("csrw fcsr,%0"::"r"(v));}
static inline unsigned getfcsr(void){unsigned v;__asm__ volatile("csrr %0,fcsr":"=r"(v));return v;}
int main(int c,char**v){
  FILE*f=fopen(v[1],"rb"); unsigned sz=0xa7eb8; unsigned char*m=mmap(0,sz+4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); fread(m,1,sz,f); fclose(f);
  dd L[3]={(dd)(m+0x695d0),(dd)(m+0x67a44),(dd)(m+0x688b4)}; dd G[3]={__muldf3,__adddf3,__divdf3};
  int n=atoi(v[2]); const char*nm[3]={"mul","add","div"};
  for(int op=0;op<3;op++) for(unsigned rm=0;rm<5;rm++){ long badv=0,badf=0;
    for(int i=0;i<n;i++){ double a=rd(),b=rd(); unsigned pre=rnd()&0x1f;
      setfcsr((rm<<5)|pre); double x=L[op](a,b); unsigned fx=getfcsr();
      setfcsr((rm<<5)|pre); double y=G[op](a,b); unsigned fy=getfcsr();
      badv+=memcmp(&x,&y,8)!=0; badf+=fx!=fy; }
    printf("%s frm=%u value_mismatch=%ld fcsr_mismatch=%ld of %d\n",nm[op],rm,badv,badf,n);}
  return 0;}
