#include <stdio.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e3+t.tv_nsec/1e6;}
int main(void){
	volatile double a=1.000001,b=0.999999,acc=1.0; volatile float fa=1.000001f,fb=0.999999f,facc=1.0f; volatile int ia=12345,ib=6789,iacc=1;
	int i,N=1000000; long t0,t1;
	t0=(long)now(); for(i=0;i<N;i++) acc=acc*a*b; t1=(long)now(); printf("double mul x2M: %ld ms -> %.0f ns each\n",t1-t0,(t1-t0)*1e6/(2.0*N));
	t0=(long)now(); for(i=0;i<N;i++) acc=acc+a-b; t1=(long)now(); printf("double add x2M: %ld ms -> %.0f ns each\n",t1-t0,(t1-t0)*1e6/(2.0*N));
	t0=(long)now(); for(i=0;i<N;i++) iacc=(int)(acc*1.5)+i; t1=(long)now(); printf("double mul+fix x1M: %ld ms\n",t1-t0);
	t0=(long)now(); for(i=0;i<N;i++) facc=facc*fa*fb; t1=(long)now(); printf("float mul x2M: %ld ms -> %.0f ns each\n",t1-t0,(t1-t0)*1e6/(2.0*N));
	t0=(long)now(); for(i=0;i<N;i++) iacc=iacc*ia+ib; t1=(long)now(); printf("int mul x1M: %ld ms\n",t1-t0);
	return 0;}
