/*
 * Is a 40-byte socket call really ~350 us, or is that an accounting artefact?
 *
 * xshim's XSHIM_PROF attributes ~386 us of CLOCK_THREAD_CPUTIME_ID to a
 * recvmsg of 40 bytes, and the project has written that off as structural. But
 * this kernel has TICK_CPU_ACCOUNTING at HZ=100: task utime/stime advance only
 * in account_process_tick(), in whole 10 ms jiffies, charged to whatever
 * happened to be running when the tick landed. Timing ONE syscall with a clock
 * that moves in 10 ms steps cannot produce a trustworthy microsecond figure,
 * and the bias runs toward whatever the timer interrupt most often catches.
 *
 * So measure the same work two ways:
 *   - a batch of N calls bracketed ONCE in CLOCK_MONOTONIC (nanosecond
 *     resolution, no per-call overhead, no quantisation)
 *   - the same calls each timed with CLOCK_THREAD_CPUTIME_ID and summed, which
 *     is what xshim does today
 * If those disagree by a lot, every per-call figure in current-state.md is
 * suspect and ~9% of the machine may be attackable after all - or may not
 * exist.
 *
 * Arms are chosen to separate the syscall from the wakeup:
 *   self   socketpair write+read in ONE process - af_unix code, no context
 *          switch, no scheduler involvement
 *   pair   two processes ping-ponging - adds the switch and the wakeup
 *   read   read() vs recvmsg-with-control on the same fd, same bytes
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define N 2000
#define MSG 40

static uint64_t mono_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

static uint64_t cpu_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

int main(void)
{
	int sv[2];
	char buf[MSG];
	uint64_t t0, t1, cpu_sum = 0;
	int i;

	memset(buf, 0x5a, sizeof buf);

	/* ---- arm 1: af_unix code with no switch and no wakeup ---- */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { perror("sp"); return 1; }

	for (i = 0; i < 50; i++) { write(sv[0], buf, MSG); read(sv[1], buf, MSG); }

	t0 = mono_ns();
	for (i = 0; i < N; i++) {
		write(sv[0], buf, MSG);
		read(sv[1], buf, MSG);
	}
	t1 = mono_ns();
	printf("self  batch  CLOCK_MONOTONIC : %6llu ns per write+read pair\n",
	       (unsigned long long)((t1 - t0) / N));

	/* Same calls, each timed the way xshim times them. */
	cpu_sum = 0;
	for (i = 0; i < N; i++) {
		uint64_t a = cpu_ns();
		write(sv[0], buf, MSG);
		read(sv[1], buf, MSG);
		cpu_sum += cpu_ns() - a;
	}
	printf("self  per-call THREAD_CPUTIME: %6llu ns per write+read pair\n",
	       (unsigned long long)(cpu_sum / N));

	/* And the batch measured in thread CPU time, which removes the
	 * per-call sampling but keeps the accounting source. */
	{
		uint64_t c0 = cpu_ns();
		for (i = 0; i < N; i++) {
			write(sv[0], buf, MSG);
			read(sv[1], buf, MSG);
		}
		printf("self  batch  THREAD_CPUTIME : %6llu ns per write+read pair\n",
		       (unsigned long long)((cpu_ns() - c0) / N));
	}
	close(sv[0]); close(sv[1]);

	/* ---- arm 2: recvmsg with a control buffer, as xshim actually does ---- */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return 1;
	{
		struct msghdr m;
		struct iovec io;
		union { struct cmsghdr al; char b[CMSG_SPACE(sizeof(int) * 8)]; } cm;

		for (i = 0; i < 50; i++) { write(sv[0], buf, MSG); read(sv[1], buf, MSG); }
		t0 = mono_ns();
		for (i = 0; i < N; i++) {
			write(sv[0], buf, MSG);
			io.iov_base = buf; io.iov_len = MSG;
			memset(&m, 0, sizeof m);
			m.msg_iov = &io; m.msg_iovlen = 1;
			m.msg_control = cm.b; m.msg_controllen = sizeof cm.b;
			recvmsg(sv[1], &m, 0);
		}
		t1 = mono_ns();
		printf("recvmsg+control batch       : %6llu ns per write+recvmsg\n",
		       (unsigned long long)((t1 - t0) / N));
	}
	close(sv[0]); close(sv[1]);

	/* ---- arm 3: two processes, so the switch and wakeup are included ---- */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return 1;
	{
		pid_t pid = fork();

		if (pid == 0) {
			close(sv[0]);
			for (i = 0; i < N + 50; i++) {
				if (read(sv[1], buf, MSG) != MSG) break;
				write(sv[1], buf, MSG);
			}
			_exit(0);
		}
		close(sv[1]);
		for (i = 0; i < 50; i++) { write(sv[0], buf, MSG); read(sv[0], buf, MSG); }
		t0 = mono_ns();
		for (i = 0; i < N; i++) {
			write(sv[0], buf, MSG);
			read(sv[0], buf, MSG);
		}
		t1 = mono_ns();
		printf("pair  round trip + switch   : %6llu ns per round trip\n",
		       (unsigned long long)((t1 - t0) / N));
		close(sv[0]);
		waitpid(pid, NULL, 0);
	}
	return 0;
}
