/* Fault reporter control: without HOLD the application's handler exits 99;
 * with HOLD the reporter must emit the real PC/maps and die of SIGSEGV. */
#include <signal.h>
#include <unistd.h>
static void replaced(int sig) { (void)sig; _exit(99); }
int main(void)
{
	signal(SIGSEGV, replaced);
	*(volatile int *)0 = 1;
	return 1;
}
