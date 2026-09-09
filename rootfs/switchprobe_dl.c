/* switchprobe, but libasound loaded the way SDL 1.2 loads it: dlopen(RTLD_NOW)
 * and dlsym, nothing linked. If a sink switch fails here and not in
 * switchprobe, the difference is library scoping, not our plugin. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
typedef void snd_pcm_t;
static int (*p_open)(snd_pcm_t **, const char *, int, int);
static int (*p_set_params)(snd_pcm_t *, int, int, unsigned, unsigned, int, unsigned);
static long (*p_writei)(snd_pcm_t *, const void *, unsigned long);
static int (*p_recover)(snd_pcm_t *, int, int);
static const char *(*p_strerror)(int);
int main(int argc, char **argv) {
	int secs = argc>1?atoi(argv[1]):10;
	void *h = dlopen("libasound.so.2", RTLD_NOW);
	snd_pcm_t *pcm; static short b[1024*2]; int i,e; float ph=0; time_t end;
	if(!h){printf("dlopen: %s\n",dlerror());return 1;}
	p_open=dlsym(h,"snd_pcm_open"); p_set_params=dlsym(h,"snd_pcm_set_params");
	p_writei=dlsym(h,"snd_pcm_writei"); p_recover=dlsym(h,"snd_pcm_recover"); p_strerror=dlsym(h,"snd_strerror");
	for(i=0;i<1024;i++){b[2*i]=b[2*i+1]=(short)(sinf(ph)*8000);ph+=2*3.14159f*440/22050;}
	if((e=p_open(&pcm,"default",0,0))<0){printf("open: %s\n",p_strerror(e));return 1;}
	/* S16_LE=2, RW_INTERLEAVED=3 */
	if((e=p_set_params(pcm,2,3,2,22050,1,100000))<0){printf("set_params: %s\n",p_strerror(e));return 1;}
	end=time(NULL)+secs;
	while(time(NULL)<end){
		long n=p_writei(pcm,b,1024);
		if(n<0){printf("write: %s\n",p_strerror((int)n));fflush(stdout);
			if((e=p_recover(pcm,(int)n,1))<0){printf("recover: %s\n",p_strerror(e));return 1;}}
	}
	printf("done ok\n");return 0;
}
