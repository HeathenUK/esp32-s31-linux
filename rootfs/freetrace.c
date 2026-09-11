/* Who freed what, last: a ring of free() calls with their return
 * addresses, dumped on SIGSEGV/SIGABRT together with pc/ra.
 *
 * prboom dies at exit inside __libc_free/get_meta (bad or double free);
 * the kernel oops line names libc but not the caller. This records the
 * last N free() callers so the crashing one is identifiable. Only calls
 * through the public `free` symbol are seen - musl's internal frees use
 * a hidden alias - so if the ring's last entry is not the crash, the
 * bad free came from inside libc (fclose, dlclose, ...).
 *
 * Build: ./docker/build.sh 'cd /src && sh rootfs/build-freetrace.sh'
 * Use:   LD_PRELOAD=/root/doom/freetrace.so FREETRACE_LOG=/root/ft.txt prboom ...
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ucontext.h>

#define RING 128
static struct { void *p; void *ra; unsigned seq; } ring[RING];
static unsigned seq;
static void (*real_free)(void *);
static int inited;
static void (*app_handler[65])(int);
static void init(void);

static void dump_addr(int fd, const char *tag, void *a)
{
	Dl_info di;
	char buf[256];
	if (a && dladdr(a, &di) && di.dli_fname)
		snprintf(buf, sizeof buf, "%s %p = %s+0x%lx (%s+0x%lx)\n", tag, a,
			 di.dli_fname, (unsigned long)((char *)a - (char *)di.dli_fbase),
			 di.dli_sname ? di.dli_sname : "?",
			 di.dli_saddr ? (unsigned long)((char *)a - (char *)di.dli_saddr) : 0);
	else
		snprintf(buf, sizeof buf, "%s %p = ?\n", tag, a);
	write(fd, buf, strlen(buf));
}

static void on_sig(int sig, siginfo_t *si, void *uc_)
{
	ucontext_t *uc = uc_;
	const char *log = getenv("FREETRACE_LOG");
	int fd = log ? open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644) : 2;
	char buf[128];
	unsigned i, n;

	if (fd < 0)
		fd = 2;
	snprintf(buf, sizeof buf, "freetrace: signal %d addr %p seq %u\n", sig,
		 si ? si->si_addr : NULL, seq);
	write(fd, buf, strlen(buf));
#ifdef __riscv
	dump_addr(fd, "pc", (void *)uc->uc_mcontext.__gregs[0]);
	dump_addr(fd, "ra", (void *)uc->uc_mcontext.__gregs[1]);
#endif
	n = seq < RING ? seq : RING;
	for (i = 0; i < n; i++) {
		unsigned k = (seq - 1 - i) % RING;
		snprintf(buf, sizeof buf, "free[-%u] seq=%u ptr=%p ", i, ring[k].seq, ring[k].p);
		write(fd, buf, strlen(buf));
		dump_addr(fd, "from", ring[k].ra);
	}
	if (fd != 2)
		close(fd);
	if (app_handler[sig]) {
		app_handler[sig](sig);
		return;
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

/* prboom installs its own SIGSEGV handler (I_SignalHandler) after we do,
 * which would replace ours. Keep ours in front and chain to theirs. */
typedef void (*sighandler_fn)(int);
sighandler_fn signal(int sig, sighandler_fn h)
{
	static sighandler_fn (*real)(int, sighandler_fn);
	sighandler_fn old;
	if (!real)
		real = dlsym(RTLD_NEXT, "signal");
	if (!inited)
		init();
	if (sig == SIGSEGV || sig == SIGBUS || sig == SIGABRT) {
		old = app_handler[sig];
		app_handler[sig] = (h == SIG_DFL || h == SIG_IGN) ? NULL : h;
		return old ? old : SIG_DFL;
	}
	return real(sig, h);
}

static void init(void)
{
	struct sigaction sa;
	real_free = dlsym(RTLD_NEXT, "free");
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = on_sig;
	sa.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	inited = 1;
}

void free(void *p)
{
	unsigned k;
	if (!inited)
		init();
	if (!p)
		return;
	k = seq++ % RING;
	ring[k].p = p;
	ring[k].ra = __builtin_return_address(0);
	ring[k].seq = seq - 1;
	real_free(p);
}
