#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
uint64_t s31_udivdi3(uint64_t,uint64_t),s31_umoddi3(uint64_t,uint64_t);
int64_t s31_divdi3(int64_t,int64_t),s31_moddi3(int64_t,int64_t);
extern uint64_t __udivdi3(uint64_t,uint64_t),__umoddi3(uint64_t,uint64_t);
extern int64_t __divdi3(int64_t,int64_t),__moddi3(int64_t,int64_t);
static uint64_t s=1234567; static uint64_t rnd(void){s^=s<<13;s^=s>>7;s^=s<<17;return s;}
int main(int c,char**v){ long n=atol(v[1]); long bad=0;
 uint64_t E[]={0,1,2,1000,1000000,0xffffffffULL,0x100000000ULL,~0ULL,0x8000000000000000ULL,0xfffffffffffffULL,999,0x100000001ULL};
 int ne=sizeof(E)/8;
 for(long i=0;i<n;i++){ uint64_t a,b; int k=rnd()%5;
   if(k==0){a=E[rnd()%ne];b=E[rnd()%ne];} else if(k==1){a=rnd()&0xfffffffffffULL;b=1000+(rnd()%2?999000:0);} else if(k==2){a=rnd();b=(rnd()>>32)|1;} else {a=rnd();b=rnd();}
   if(!b)b=1;
   if(s31_udivdi3(a,b)!=__udivdi3(a,b)){bad++; if(bad<4)printf("udiv %llx/%llx\n",(unsigned long long)a,(unsigned long long)b);}
   if(s31_umoddi3(a,b)!=__umoddi3(a,b))bad++;
   if(s31_divdi3(a,b)!=__divdi3(a,b))bad++;
   if(s31_moddi3(a,b)!=__moddi3(a,b))bad++;
 }
 printf("bad=%ld of %ld\n",bad,n); return 0;}
