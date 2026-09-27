#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
typedef double (*dd)(double,double); typedef unsigned long long (*uu)(unsigned long long,unsigned long long);
extern double __muldf3(double,double), __adddf3(double,double), __divdf3(double,double);
extern unsigned long long __udivdi3(unsigned long long,unsigned long long);
static uint64_t s=88172645463325252ULL; static uint64_t rnd(void){s^=s<<13;s^=s>>7;s^=s<<17;return s;}
static double rd(void){ double x; uint64_t b=rnd(); b=(b&0x800fffffffffffffULL)|((uint64_t)(1023-20+(rnd()%40))<<52); memcpy(&x,&b,8); return x;}
int main(int c,char**v){
  FILE*f=fopen(v[1],"rb"); unsigned sz=0xa7eb8; unsigned char*m=mmap(0,sz+4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); fread(m,1,sz,f); fclose(f);
  dd lmul=(dd)(m+0x695d0), ladd=(dd)(m+0x67a44), ldiv=(dd)(m+0x688b4); uu ludiv=(uu)(m+0x6733c);
  int mode=atoi(v[2]), n=atoi(v[3]); volatile double sink; volatile unsigned long long us; long bad=0;
  double A[256],B[256]; for(int i=0;i<256;i++){A[i]=rd();B[i]=rd();}
  for(int i=0;i<n;i++){ double a=A[i&255],b=B[(i*7)&255]; unsigned long long ua=(unsigned long long)rnd()>>20;
    switch(mode){case 0: sink=a; break; case 1: sink=lmul(a,b); break; case 2: sink=__muldf3(a,b); break;
    case 3: sink=ladd(a,b); break; case 4: sink=__adddf3(a,b); break; case 5: sink=ldiv(a,b); break; case 6: sink=__divdf3(a,b); break;
    case 7: us=ludiv(ua,1000); break; case 8: us=__udivdi3(ua,1000); break; case 9: us=ua; break;
    case 10: { double x=lmul(a,b),y=__muldf3(a,b),p=ladd(a,b),q=__adddf3(a,b),r=ldiv(a,b),t=__divdf3(a,b);
       bad+= memcmp(&x,&y,8)!=0 || memcmp(&p,&q,8)!=0 || memcmp(&r,&t,8)!=0; } }
  }
  if(mode==10) printf("mismatches %ld of %d\n",bad,n); return 0;}
