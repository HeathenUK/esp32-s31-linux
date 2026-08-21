// SPDX-License-Identifier: GPL-2.0
/* Long-lived process for inspecting how its own text is mapped. */
#include <stdio.h>
#include <unistd.h>
int main(void)
{
	printf("xipsleep pid %d\n", (int)getpid());
	fflush(stdout);
	for (;;)
		sleep(3600);
	return 0;
}
