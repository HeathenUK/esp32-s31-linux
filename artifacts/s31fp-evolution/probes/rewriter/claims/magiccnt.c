#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
extern uint64_t magicdiv(uint64_t), ref(uint64_t,uint64_t); extern void setup(uint64_t);
static uint64_t s=88172645463325252ull; static uint64_t r64(void){s^=s<<13;s^=s>>7;s^=s<<17;return s;}
uint64_t in[4096];
int main(int argc,char**argv){ long n=atol(argv[1]); int mode=atoi(argv[2]); uint64_t K=strtoull(argv[3],0,0); int small=argc>4;
  setup(K); for(int i=0;i<4096;i++){ in[i]=r64(); if(small) in[i]>>=32; }
  uint64_t sink=0;
  if(mode==1) for(long i=0;i<n;i++) sink+=ref(in[i&4095],K);
  else if(mode==2) for(long i=0;i<n;i++) sink+=magicdiv(in[i&4095]);
  else for(long i=0;i<n;i++) sink+=in[i&4095];
  printf("%llu\n",(unsigned long long)sink); }
