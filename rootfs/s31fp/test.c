#include <stdio.h>
#include <stdint.h>
#include <string.h>
double s31_muldf3(double,double), s31_adddf3(double,double), s31_subdf3(double,double);
static uint64_t s=88172645463325252ULL; static uint64_t rnd(void){s^=s<<13;s^=s>>7;s^=s<<17;return s;}
static uint64_t bits(double d){uint64_t u;memcpy(&u,&d,8);return u;}
static double mk(void){ uint64_t r=rnd(); unsigned k=rnd()%16; uint64_t u=r;
 if(k==0)u=r&0x800FFFFFFFFFFFFFULL; else if(k==1)u=(r&0x8000000000000000ULL)|0x7FF0000000000000ULL; else if(k==2)u=r|0x7FF0000000000000ULL;
 else if(k==3)u=r&0x8000000000000000ULL; else if(k==4)u=(r&0x800FFFFFFFFFFFFFULL)|((uint64_t)(1023+(int)(rnd()%8)-4)<<52);
 else if(k==5)u=(r&0x8000000000000001ULL)|0x3FF0000000000000ULL; else if(k==6)u=(r&0x800FFFFFFFFFFFFFULL)|((uint64_t)(rnd()%64)<<52);
 else if(k==7)u=(r&0x800FFFFFFFFFFFFFULL)|((uint64_t)(2046-rnd()%64)<<52);
 double d; memcpy(&d,&u,8); return d; }
static int same(double a,double b){ if(a!=a&&b!=b) return 1; return bits(a)==bits(b); }
int main(void){ long bad=0,n=20000000; for(long i=0;i<n;i++){ volatile double a=mk(),b=mk();
  if(!same(s31_muldf3(a,b),a*b)){ if(bad++<5)printf("MUL %016llx %016llx got %016llx want %016llx\n",(unsigned long long)bits(a),(unsigned long long)bits(b),(unsigned long long)bits(s31_muldf3(a,b)),(unsigned long long)bits(a*b)); }
  if(!same(s31_adddf3(a,b),a+b)){ if(bad++<5)printf("ADD %016llx %016llx got %016llx want %016llx\n",(unsigned long long)bits(a),(unsigned long long)bits(b),(unsigned long long)bits(s31_adddf3(a,b)),(unsigned long long)bits(a+b)); }
  if(!same(s31_subdf3(a,b),a-b)){ if(bad++<5)printf("SUB %016llx %016llx got %016llx want %016llx\n",(unsigned long long)bits(a),(unsigned long long)bits(b),(unsigned long long)bits(s31_subdf3(a,b)),(unsigned long long)bits(a-b)); }
 } printf("tested %ld triples, %ld mismatches\n",n,bad); return bad!=0; }
