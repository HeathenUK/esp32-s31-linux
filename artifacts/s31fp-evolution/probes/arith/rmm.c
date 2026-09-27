#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
static uint64_t bits(double d){uint64_t u;memcpy(&u,&d,8);return u;}
static double mk(uint64_t u){double d;memcpy(&d,&u,8);return d;}
static void setfrm(unsigned r){__asm__ volatile("fsrm %0"::"r"(r));}
static uint64_t rs=0x9e3779b97f4a7c15ull; static uint64_t rnd(void){rs^=rs<<13;rs^=rs>>7;rs^=rs<<17;return rs;}
volatile double a=1.0, b=0x1.8p-53, c=-1.0; typedef double(*fn)(double); volatile fn S=sqrt;
int main(void){
  for(unsigned r=0;r<5;r++){ setfrm(r); double s=a+b, t=c-b; setfrm(0); printf("frm=%u 1+1.5ulp/2=%016llx  -1-1.5ulp/2=%016llx\n",r,(unsigned long long)bits(s),(unsigned long long)bits(t)); }
  long n=1000000, eq_rtz=0, eq_rne=0;
  for(long i=0;i<n;i++){ uint64_t u=((rnd()%0x7fe + 1)<<52)|(rnd()&0xfffffffffffffull); double x=mk(u);
    setfrm(4); double m4=S(x); setfrm(1); double m1=S(x); setfrm(0); double m0=S(x);
    eq_rtz+= bits(m4)==bits(m1); eq_rne+= bits(m4)==bits(m0); }
  printf("musl sqrt RMM==RTZ %ld/%ld, RMM==RNE %ld/%ld\n",eq_rtz,n,eq_rne,n); return 0; }
