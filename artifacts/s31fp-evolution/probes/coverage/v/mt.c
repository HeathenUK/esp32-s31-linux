/* Compare toolchain libc.so (Espressif build, raw image) libm bodies against
   upstream musl 1.2.5 rebuilt (QEMU sysroot, libgcc helpers): bits + fflags,
   all 5 frm modes, random preset flags. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <sys/mman.h>
static uint64_t s=0x9E3779B97F4A7C15ULL; static uint64_t rnd(void){s^=s<<13;s^=s>>7;s^=s<<17;return s;}
static inline void setfr(unsigned rm, unsigned fl){ __asm__ volatile("fsrm %0; fsflags %1"::"r"(rm),"r"(fl)); }
static inline unsigned getfl(void){unsigned f; __asm__ volatile("frflags %0":"=r"(f)); return f;}
static inline void clrfr(void){ __asm__ volatile("fsrm x0; fsflags x0"); }
typedef double (*d1)(double); typedef double (*d2)(double,double); typedef void (*dsc)(double,double*,double*);
typedef float (*f1)(float); typedef float (*f2)(float,float); typedef void (*fsc)(float,float*,float*);
static unsigned char *M;
static double rdd(int k){ uint64_t b=rnd(); double x;
  switch(k&7){ case 0: break; /* any bits */
   case 1: b=(b&0x800fffffffffffffULL)|((uint64_t)(1023-30+(rnd()%60))<<52); break;
   case 2: b=(b&0x800fffffffffffffULL)|((uint64_t)(1023-3+(rnd()%8))<<52); break;
   case 3: b=(b&0x800fffffffffffffULL)|((uint64_t)(1023+15+(rnd()%40))<<52); break; /* big: rem_pio2 */
   case 4: b=(b&0x800fffffffffffffULL)|((uint64_t)(1023+40+(rnd()%980))<<52); break; /* huge: large */
   case 5: b=(b&0x800fffffffffffffULL)|((uint64_t)(rnd()%40)<<52); break; /* tiny/subnormal */
   case 6: { static const uint64_t E[]={0,0x8000000000000000ULL,0x7ff0000000000000ULL,0xfff0000000000000ULL,0x7ff8000000000000ULL,0x7ff4000000000001ULL,0xfff8000000000123ULL,1,0x3ff0000000000000ULL,0xbff0000000000000ULL,0x4330000000000000ULL,0x432fffffffffffffULL,0x7fefffffffffffffULL,0x0010000000000000ULL,0x3fe0000000000000ULL,0x400921fb54442d18ULL}; b=E[rnd()%16]; if(rnd()&1) b+= (rnd()%5)-2; } break;
   case 7: b=(b&0x800fffffffffffffULL)|((uint64_t)(1023+(rnd()%12))<<52); b&=~0xfffffULL; break; }
  memcpy(&x,&b,8); return x;}
static float rdf(int k){ double d=rdd(k); float f; uint32_t u=(uint32_t)rnd(); if(k&1){memcpy(&f,&u,4);return f;} return (float)d; }
int main(int c,char**v){
  FILE*f=fopen(v[1],"rb"); M=mmap(0,0xab000,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  fread(M,1,0xa8dac+0x520,f); fclose(f);
  const char*fn=v[2]; unsigned off=strtoul(v[3],0,16); long n=atol(v[4]);
  long bad=0,badf=0; int shown=0;
  for(long i=0;i<n;i++){ unsigned rm=rnd()%5; unsigned pre=(rnd()&3)?0:(rnd()&31); int k=rnd();
    uint64_t ra=0,rb=0,ra2=0,rb2=0; unsigned fa,fb; double x=rdd(k), y=rdd(k>>3);
    float xf=rdf(k), yf=rdf(k>>3);
#define RUN(expr_img, expr_ref) do{ setfr(rm,pre); expr_img; fa=getfl(); clrfr(); setfr(rm,pre); expr_ref; fb=getfl(); clrfr(); }while(0)
    if(!strcmp(fn,"sin")){ double a,b; RUN(a=((d1)(M+off))(x), b=sin(x)); memcpy(&ra,&a,8);memcpy(&rb,&b,8);}
    else if(!strcmp(fn,"cos")){ double a,b; RUN(a=((d1)(M+off))(x), b=cos(x)); memcpy(&ra,&a,8);memcpy(&rb,&b,8);}
    else if(!strcmp(fn,"exp")){ double a,b; RUN(a=((d1)(M+off))(x), b=exp(x)); memcpy(&ra,&a,8);memcpy(&rb,&b,8);}
    else if(!strcmp(fn,"log")){ double a,b; RUN(a=((d1)(M+off))(x), b=log(x)); memcpy(&ra,&a,8);memcpy(&rb,&b,8);}
    else if(!strcmp(fn,"atan")){ double a,b; RUN(a=((d1)(M+off))(x), b=atan(x)); memcpy(&ra,&a,8);memcpy(&rb,&b,8);}
    else if(!strcmp(fn,"floor")){ double a,b; RUN(a=((d1)(M+off))(x), b=floor(x)); memcpy(&ra,&a,8);memcpy(&rb,&b,8);}
    else if(!strcmp(fn,"sqrt")){ double a,b; RUN(a=((d1)(M+off))(x), b=sqrt(x)); memcpy(&ra,&a,8);memcpy(&rb,&b,8);}
    else if(!strcmp(fn,"atan2")){ double a,b; RUN(a=((d2)(M+off))(x,y), b=atan2(x,y)); memcpy(&ra,&a,8);memcpy(&rb,&b,8);}
    else if(!strcmp(fn,"pow")){ double a,b; RUN(a=((d2)(M+off))(x,y), b=pow(x,y)); memcpy(&ra,&a,8);memcpy(&rb,&b,8);}
    else if(!strcmp(fn,"sincos")){ double a,b,a2,b2; RUN(((dsc)(M+off))(x,&a,&a2), sincos(x,&b,&b2)); memcpy(&ra,&a,8);memcpy(&rb,&b,8);memcpy(&ra2,&a2,8);memcpy(&rb2,&b2,8);}
    else if(!strcmp(fn,"sinf")){ float a,b; RUN(a=((f1)(M+off))(xf), b=sinf(xf)); memcpy(&ra,&a,4);memcpy(&rb,&b,4);}
    else if(!strcmp(fn,"cosf")){ float a,b; RUN(a=((f1)(M+off))(xf), b=cosf(xf)); memcpy(&ra,&a,4);memcpy(&rb,&b,4);}
    else if(!strcmp(fn,"powf")){ float a,b; RUN(a=((f2)(M+off))(xf,yf), b=powf(xf,yf)); memcpy(&ra,&a,4);memcpy(&rb,&b,4);}
    else if(!strcmp(fn,"sincosf")){ float a,b,a2,b2; RUN(((fsc)(M+off))(xf,&a,&a2), sincosf(xf,&b,&b2)); memcpy(&ra,&a,4);memcpy(&rb,&b,4);memcpy(&ra2,&a2,4);memcpy(&rb2,&b2,4);}
    else { fprintf(stderr,"?\n"); return 2; }
    int mb = ra!=rb || ra2!=rb2; int mf = fa!=fb;
    bad+=mb; badf+=mf;
    if((mb||mf) && shown<6){ shown++; uint64_t xb,yb; memcpy(&xb,&x,8); memcpy(&yb,&y,8);
      printf("  MISMATCH %s rm=%u pre=%x x=%016llx y=%016llx xf=%08x img=%016llx/%016llx fl=%x ref=%016llx/%016llx fl=%x\n",fn,rm,pre,
        (unsigned long long)xb,(unsigned long long)yb,*(unsigned*)&xf,(unsigned long long)ra,(unsigned long long)ra2,fa,(unsigned long long)rb,(unsigned long long)rb2,fb);}
  }
  printf("%-8s n=%ld bitmismatch=%ld flagmismatch=%ld\n",fn,n,bad,badf); return 0;}
