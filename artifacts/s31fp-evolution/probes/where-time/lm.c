#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
volatile double sink; volatile float sinkf;
int main(int argc,char**argv){
  int which=atoi(argv[1]); int n=atoi(argv[2]);
  double x=0.37; float xf=0.37f; double s,c; float sf,cf;
  for(int i=0;i<n;i++){ double v=x+i*0.001; float vf=xf+i*0.001f;
    switch(which){
     case 0: break;
     case 1: sink=floor(v*3.7); break;
     case 2: sink=sqrt(v); break;
     case 3: sincos(v,&s,&c); sink=s+c; break;
     case 4: sinkf=sinf(vf); break;
     case 5: sincosf(vf,&sf,&cf); sinkf=sf+cf; break;
     case 6: sinkf=sqrtf(vf); break;
     case 7: sink=pow(v,1.3); break;
     case 8: sinkf=powf(vf,1.3f); break;
     case 9: sink=v*3.7; break;
     case 10: sink=v; break;
     case 11: sinkf=vf; break;
    }}
  return 0;}
