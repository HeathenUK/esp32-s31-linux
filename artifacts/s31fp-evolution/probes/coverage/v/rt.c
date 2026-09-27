#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
double s31_floor(double),s31_ceil(double),s31_trunc(double),s31_sqrt(double);
static uint64_t s=0x9E3779B97F4A7C15ULL; static uint64_t rnd(void){s^=s<<13;s^=s>>7;s^=s<<17;return s;}
static inline void setfr(unsigned rm, unsigned fl){ __asm__ volatile("fsrm %0; fsflags %1"::"r"(rm),"r"(fl)); }
static inline unsigned getfl(void){unsigned f; __asm__ volatile("frflags %0":"=r"(f)); return f;}
static inline void clrfr(void){ __asm__ volatile("fsrm x0; fsflags x0"); }
typedef double (*d1)(double);
static double rdd(int k){ uint64_t b=rnd(); double x;
  switch(k&7){ case 0: break;
   case 1: b=(b&0x800fffffffffffffULL)|((uint64_t)(1023-30+(rnd()%60))<<52); break;
   case 2: b=(b&0x800fffffffffffffULL)|((uint64_t)(1023-3+(rnd()%60))<<52); break;
   case 3: b=(b&0x800fffffffffffffULL)|((uint64_t)(1023+(rnd()%56))<<52); b&=~((1ULL<<(rnd()%53))-1); break; /* near-integers */
   case 4: b&=0x800fffffffffffffULL; b>>= rnd()%52; break; /* subnormals */
   case 5: { uint64_t q=rnd()>>38; if(rnd()&1) q&=0xffff; double d=(double)q; if(rnd()&1){ d=d*d; } memcpy(&b,&d,8); if(rnd()&1) b+=(rnd()%3)-1; } break; /* perfect squares, ±1ulp */
   case 6: { static const uint64_t E[]={0,0x8000000000000000ULL,0x7ff0000000000000ULL,0xfff0000000000000ULL,0x7ff8000000000000ULL,0x7ff4000000000001ULL,0xfff8000000000123ULL,1,0x3ff0000000000000ULL,0xbff0000000000000ULL,0x4330000000000000ULL,0x432fffffffffffffULL,0x7fefffffffffffffULL,0x0010000000000000ULL,0x3fe0000000000000ULL,0xbfe0000000000000ULL,0x000fffffffffffffULL,0x8000000000000001ULL,0x4320000000000000ULL,0xc32fffffffffffffULL}; b=E[rnd()%20]; if(rnd()&1) b+= (rnd()%5)-2; } break;
   case 7: b=(b&0x800fffffffffffffULL)|((uint64_t)(1023+(rnd()%53))<<52); b&=~((1ULL<<(rnd()%53))-1); b|=1ULL<<(rnd()%53); break; }
  memcpy(&x,&b,8); return x;}
int main(int c,char**v){
  FILE*f=fopen(v[1],"rb"); unsigned char*M=mmap(0,0xab000,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  fread(M,1,0xa8dac+0x520,f); fclose(f);
  struct { const char*n; unsigned off; d1 r; } T[]={{"floor",0x2f9be,s31_floor},{"ceil",0x2be6c,s31_ceil},{"trunc",0x3e50a,s31_trunc},{"sqrt",0x3b9f4,s31_sqrt}};
  long n=atol(v[2]);
  for(int t=0;t<4;t++){ long bad=0,badf=0,nxc=0; int shown=0; d1 img=(d1)(M+T[t].off);
    for(long i=0;i<n;i++){ unsigned rm=rnd()%5, pre=(rnd()&3)?0:(rnd()&31); double x=rdd(rnd()); double a,b; unsigned fa,fb;
      if(v[3]&&t==3) x = x<0?-x:x; /* optional */
      setfr(rm,pre); a=img(x); fa=getfl(); clrfr(); setfr(rm,pre); b=T[t].r(x); fb=getfl(); clrfr();
      uint64_t ua,ub,ux; memcpy(&ua,&a,8); memcpy(&ub,&b,8); memcpy(&ux,&x,8);
      int mb=ua!=ub, mf=fa!=fb; bad+=mb; badf+=mf; nxc+=(fa&1)&&!(pre&1);
      if((mb||mf)&&shown<5){shown++; printf("  MIS %s rm=%u pre=%x x=%016llx img=%016llx/%x rep=%016llx/%x\n",T[t].n,rm,pre,(unsigned long long)ux,(unsigned long long)ua,fa,(unsigned long long)ub,fb);} }
    printf("%-6s n=%ld bitmismatch=%ld flagmismatch=%ld (NX raised by img in %ld)\n",T[t].n,n,bad,badf,nxc); }
  return 0;}
