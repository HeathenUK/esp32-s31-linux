/*
 * segvtrap.so - LD_PRELOAD fault reporter: prints the faulting PC, ra, sp,
 * the bad address and /proc/self/maps at the instant a process dies of
 * SIGSEGV/SIGBUS/SIGILL/SIGFPE, then re-raises so it still dies normally.
 *
 * Why: the kernel's exception-trace printed nothing for sdlquake's crash
 * (2026-09-21) - SDL installs its own fault handlers, so the kernel does not
 * count the signal as unhandled - and a process that dies in 2.8 s leaves no
 * time to read /proc/<pid>/maps from outside. This reads it from inside.
 *
 * A measurement shim, like fpsonly.so: it changes nothing in the client.
 * Run with SDL_NOPARACHUTE=1 so SDL does not replace these handlers.
 *   SDL_NOPARACHUTE=1 LD_PRELOAD=/root/segvtrap.so ./sdlquake ...
 * Its own alternate stack, so a stack overflow is reported too.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

static char altstack[16384];
/* Some clients (including stock prboom 2.5.0, even with -devparm)
 * replace the constructor's handler. Opt-in diagnostic mode keeps it.
 * This runs at signal registration, never in a rendering/audio hot path. */
static int hold_faults;
static void (*requested[NSIG])(int);
static void (*(*next_signal)(int, void (*)(int)))(int);

static char *text(char *p, const char *s)
{
	while (*s) *p++ = *s++;
	return p;
}
static char *hex(char *p, unsigned long v)
{
	for (int i = 28; i >= 0; i -= 4) *p++ = "0123456789abcdef"[(v >> i) & 15];
	return p;
}

void (*signal(int sig, void (*handler)(int)))(int)
{
	if (hold_faults && (sig == SIGSEGV || sig == SIGBUS ||
			   sig == SIGILL || sig == SIGFPE)) {
		void (*old)(int) = requested[sig];
		requested[sig] = handler;
		return old;
	}
	if (!next_signal) next_signal = dlsym(RTLD_NEXT, "signal");
	return next_signal(sig, handler);
}

static void report(int sig, siginfo_t *si, void *ucp)
{
	ucontext_t *uc = ucp;
	char buf[256], m[2048];
	int fd;
	ssize_t r;
	/* No stdio/allocator/PIE string routines in a fault handler. */
	char *p = text(buf, "SEGVTRAP sig ");
	p = hex(p, sig); p = text(p, " code "); p = hex(p, si->si_code);
	p = text(p, " addr "); p = hex(p, (unsigned long)si->si_addr);
	p = text(p, " pc "); p = hex(p, uc->uc_mcontext.__gregs[0]);
	p = text(p, " ra "); p = hex(p, uc->uc_mcontext.__gregs[1]);
	p = text(p, " sp "); p = hex(p, uc->uc_mcontext.__gregs[2]);
	*p++ = '\n'; write(2, buf, p - buf);
	fd = open("/proc/self/maps", O_RDONLY);
	if (fd >= 0) {
		while ((r = read(fd, m, sizeof(m))) > 0)
			write(2, m, r);
		close(fd);
	}
	write(2, "SEGVTRAP end\n", 13);
	struct sigaction sa = { .sa_handler = SIG_DFL };
	sigemptyset(&sa.sa_mask);
	sigaction(sig, &sa, NULL);
	raise(sig);
}

__attribute__((constructor)) static void segvtrap_init(void)
{
	stack_t ss = { .ss_sp = altstack, .ss_size = sizeof(altstack) };
	struct sigaction sa;

	sigaltstack(&ss, NULL);
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = report;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);
	const char *hold = getenv("SEGVTRAP_HOLD");
	hold_faults = hold && !strcmp(hold, "1");
	if (hold_faults) {
		static const char msg[] = "SEGVTRAP holding fault handlers\n";
		write(2, msg, sizeof(msg) - 1);
	}
}
