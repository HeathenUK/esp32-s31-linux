/* Calibrate the syscall floor before believing any socket number. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <fcntl.h>
static uint64_t ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
	return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
#define N 20000
int main(void){
	uint64_t t0,t1; int i; volatile long r=0; int fd;
	for(i=0;i<1000;i++) r+=syscall(SYS_getpid);
	t0=ns(); for(i=0;i<N;i++) r+=syscall(SYS_getpid); t1=ns();
	printf("getpid (raw syscall)      : %6llu ns\n",(unsigned long long)((t1-t0)/N));
	t0=ns(); for(i=0;i<N;i++) r+=getpid(); t1=ns();
	printf("getpid (libc, may cache)  : %6llu ns\n",(unsigned long long)((t1-t0)/N));
	fd=open("/dev/null",O_WRONLY);
	t0=ns(); for(i=0;i<N;i++) r+=write(fd,"x",1); t1=ns();
	printf("write(/dev/null,1)        : %6llu ns\n",(unsigned long long)((t1-t0)/N));
	t0=ns(); for(i=0;i<N;i++){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);r+=t.tv_nsec;} t1=ns();
	printf("clock_gettime MONOTONIC   : %6llu ns\n",(unsigned long long)((t1-t0)/N));
	t0=ns(); for(i=0;i<N;i++){struct timespec t;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);r+=t.tv_nsec;} t1=ns();
	printf("clock_gettime THREAD_CPU  : %6llu ns\n",(unsigned long long)((t1-t0)/N));
	(void)r; return 0;
}
