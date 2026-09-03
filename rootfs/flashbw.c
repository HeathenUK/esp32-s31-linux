/*
 * flashbw - measure XIP flash read bandwidth from Linux.
 *
 * Maps a file that lives on the XIP cramfs (flash-backed, no page-cache
 * copy) and streams it. The first pass is cold; the ratio to a known
 * RAM copy discriminates DIO vs QIO vs frequency: at 80 MHz, DIO streams
 * ~20 MB/s and QIO ~40 MB/s, so the answer is unambiguous.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>

static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }

static unsigned sum(const unsigned char *p, size_t n){
	unsigned s=0; const unsigned *w=(const unsigned*)p; size_t i;
	for(i=0;i<n/4;i++) s+=w[i];
	return s;
}

int main(int argc,char**argv){
	const char *path = argc>1?argv[1]:"/bin/busybox";
	int fd=open(path,O_RDONLY); struct stat st;
	if(fd<0||fstat(fd,&st)){perror(path);return 1;}
	size_t n=st.st_size;
	unsigned char *m=mmap(NULL,n,PROT_READ,MAP_PRIVATE,fd,0);
	if(m==MAP_FAILED){perror("mmap");return 1;}
	unsigned char *ram=malloc(n); memcpy(ram,m,n);   /* warm RAM copy */
	double t0,t1; unsigned s;
	for(int pass=1;pass<=3;pass++){
		t0=now(); s=sum(m,n); t1=now();
		printf("  flash pass %d: %6.1f MB/s  (%zu bytes, sum %08x)\n",pass,n/1e6/(t1-t0),n,s);
	}
	t0=now(); s=sum(ram,n); t1=now();
	printf("  RAM (PSRAM)  : %6.1f MB/s  (sum %08x)\n",n/1e6/(t1-t0),s);
	return 0;
}
