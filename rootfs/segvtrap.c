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
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

static char altstack[16384];

static void report(int sig, siginfo_t *si, void *ucp)
{
	ucontext_t *uc = ucp;
	char buf[256], m[2048];
	int n, fd;
	ssize_t r;

	n = snprintf(buf, sizeof(buf),
		     "SEGVTRAP sig %d code %d addr %p pc %08lx ra %08lx sp %08lx\n",
		     sig, si->si_code, si->si_addr,
		     uc->uc_mcontext.__gregs[0], uc->uc_mcontext.__gregs[1],
		     uc->uc_mcontext.__gregs[2]);
	write(2, buf, n);
	fd = open("/proc/self/maps", O_RDONLY);
	if (fd >= 0) {
		while ((r = read(fd, m, sizeof(m))) > 0)
			write(2, m, r);
		close(fd);
	}
	write(2, "SEGVTRAP end\n", 13);
	signal(sig, SIG_DFL);
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
}
