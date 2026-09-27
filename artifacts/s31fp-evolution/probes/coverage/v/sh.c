#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
typedef void (*dsc)(double,double*,double*); typedef double (*d1)(double); typedef float (*f1)(float);
volatile double A,B; volatile float FA; double X[64]; float F[64];
int main(int c,char**v){ FILE*f=fopen(v[1],"rb"); unsigned char*M=mmap(0,0xab000,7,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); fread(M,1,0xa8dac+0x520,f); fclose(f);
 fprintf(stderr,"BASE %p\n",(void*)M); int m=atoi(v[2]); long n=atol(v[3]); for(int i=0;i<64;i++){X[i]=(i-32)*0.311+0.07; F[i]=X[i];}
 dsc sc=(dsc)(M+0x3c638); d1 sn=(d1)(M+0x3c55e); f1 snf=(f1)(M+0x3cd00);
 for(long i=0;i<n;i++){ double x=X[i&63]; float ff=F[i&63]; double a,b;
   if(m==0){A=x;} else if(m==1){sc(x,&a,&b);A=a;B=b;} else if(m==2){A=sn(x);} else if(m==3){FA=snf(ff);} }
 return 0;}
