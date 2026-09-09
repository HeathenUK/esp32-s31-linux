/* Open one sink, keep it, open the other in the SAME process. */
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <unistd.h>
static int op(const char *n, snd_pcm_t **p, int cfg) {
	int e;
	if (cfg) snd_config_update_free_global();
	e = snd_pcm_open(p, n, SND_PCM_STREAM_PLAYBACK, 0);
	printf("open %-16s cfg=%d -> %s\n", n, cfg, e ? snd_strerror(e) : "OK");
	fflush(stdout);
	return e;
}
int main(int argc, char **argv) {
	snd_pcm_t *a=0,*b=0;
	const char *first = argc>1?argv[1]:"plug:'hw:1,0'";
	const char *second = argc>2?argv[2]:"plug:'hw:0,0'";
	op(first,&a,0);
	sleep(1);
	op(second,&b,0);          /* no config free */
	if (b){snd_pcm_close(b);b=0;}
	op(second,&b,1);          /* with config free */
	return 0;
}
