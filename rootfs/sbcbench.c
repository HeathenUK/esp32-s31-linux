/*
 * Where does SBC encoding actually spend its time?
 *
 * s31-a2dp costs 43% of a core while streaming. Before asking whether the
 * PPA, GDMA or the bitscrambler could take that work, find out what the
 * work IS: the analysis filterbank is multiply-accumulate, quantisation is
 * arithmetic per sample, and packing is bit shuffling. Only the last of
 * those is anything a bit-permutation engine could touch, so the split
 * decides the whole question.
 *
 * Sensitivity to bitpool is the tell. The filterbank does the same work
 * whatever the bitpool; quantisation and packing scale with it. Flat means
 * the cost is MACs and no permutation engine can help.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sbc/sbc.h>

static double cpu_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void run(const char *label, int freq, int mode, int bitpool,
		int subbands, int blocks, int seconds)
{
	sbc_t sbc;
	unsigned char *pcm, out[1024];
	size_t codesize;
	double t0, t1;
	long frames = 0, bytes = 0;
	int hz = freq == SBC_FREQ_44100 ? 44100 : 48000;
	long total, i;

	sbc_init(&sbc, 0L);
	sbc.frequency = freq;
	sbc.mode = mode;
	sbc.subbands = subbands;
	sbc.blocks = blocks;
	sbc.bitpool = bitpool;
	sbc.allocation = SBC_AM_LOUDNESS;
	sbc.endian = SBC_LE;

	codesize = sbc_get_codesize(&sbc);
	pcm = calloc(1, codesize);
	if (!pcm)
		return;
	/* A real signal, not silence: quantisation is data-dependent. */
	for (i = 0; i < (long)codesize / 2; i++)
		((short *)pcm)[i] = (short)((i * 37) % 20000 - 10000);

	total = (long)hz * seconds / ((long)codesize / 4);
	t0 = cpu_ms();
	for (i = 0; i < total; i++) {
		ssize_t wrote = 0;

		if (sbc_encode(&sbc, pcm, codesize, out, sizeof(out),
			       &wrote) <= 0)
			break;
		frames++;
		bytes += wrote;
	}
	t1 = cpu_ms();
	printf("%-28s %2d Hz-class bp=%-3d %d sub %2d blk : "
	       "%6.0f ms cpu for %d s audio = %5.1f%% of a core, "
	       "%ld frames, %ld bytes (%ld kbit/s)\n",
	       label, hz / 1000, bitpool, subbands, blocks,
	       t1 - t0, seconds, (t1 - t0) / (seconds * 1000.0) * 100.0,
	       frames, bytes, bytes * 8 / (seconds * 1000));
	free(pcm);
	sbc_finish(&sbc);
}

int main(void)
{
	int s = 5;

	printf("--- what we negotiate today ---\n");
	run("48k joint stereo bp43", SBC_FREQ_48000, SBC_MODE_JOINT_STEREO,
	    43, SBC_SB_8, SBC_BLK_16, s);
	printf("--- bitpool sensitivity (same everything else) ---\n");
	run("48k joint stereo bp32", SBC_FREQ_48000, SBC_MODE_JOINT_STEREO,
	    32, SBC_SB_8, SBC_BLK_16, s);
	run("48k joint stereo bp19", SBC_FREQ_48000, SBC_MODE_JOINT_STEREO,
	    19, SBC_SB_8, SBC_BLK_16, s);
	printf("--- channel mode ---\n");
	run("48k plain stereo  bp43", SBC_FREQ_48000, SBC_MODE_STEREO,
	    43, SBC_SB_8, SBC_BLK_16, s);
	run("48k dual channel  bp43", SBC_FREQ_48000, SBC_MODE_DUAL_CHANNEL,
	    43, SBC_SB_8, SBC_BLK_16, s);
	printf("--- rate and filterbank size ---\n");
	run("44.1k joint       bp43", SBC_FREQ_44100, SBC_MODE_JOINT_STEREO,
	    43, SBC_SB_8, SBC_BLK_16, s);
	run("48k joint 4 subbands", SBC_FREQ_48000, SBC_MODE_JOINT_STEREO,
	    43, SBC_SB_4, SBC_BLK_16, s);
	return 0;
}
