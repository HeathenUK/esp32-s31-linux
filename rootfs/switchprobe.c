/* Open "default" (our s31route ioplug) and keep writing while the sink file
 * changes underneath - exactly Doom's path, minus SDL. Prints what the
 * write returns so a mid-stream reopen failure shows its errno. */
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
int main(int argc, char **argv) {
	int secs = argc>1?atoi(argv[1]):12;
	if(argc>2){ if(chdir(argv[2])==0) printf("chdir %s ok\n",argv[2]); }
	snd_pcm_t *pcm; static short b[1024*2]; int i,e; float ph=0;
	time_t end;
	for(i=0;i<1024;i++){b[2*i]=b[2*i+1]=(short)(sinf(ph)*8000);ph+=2*3.14159f*440/22050;}
	if((e=snd_pcm_open(&pcm,"default",SND_PCM_STREAM_PLAYBACK,0))<0){printf("open: %s\n",snd_strerror(e));return 1;}
	snd_pcm_set_params(pcm,SND_PCM_FORMAT_S16_LE,SND_PCM_ACCESS_RW_INTERLEAVED,2,22050,1,100000);
	end=time(NULL)+secs;
	while(time(NULL)<end){
		snd_pcm_sframes_t n=snd_pcm_writei(pcm,b,1024);
		if(n<0){printf("t=%ld write: %s\n",(long)(time(NULL)),snd_strerror((int)n));fflush(stdout);
			if((e=snd_pcm_recover(pcm,(int)n,1))<0){printf("recover: %s\n",snd_strerror(e));return 1;}}
	}
	printf("done ok\n");return 0;
}
