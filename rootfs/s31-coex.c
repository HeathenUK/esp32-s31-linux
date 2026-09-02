/*
 * s31-coex - poke the radio coexistence scheduler on hart0 from Linux.
 *
 *   s31-coex get                 scheme period and interval now
 *   s31-coex prefer  0|1|2       wifi | bt | balance
 *   s31-coex bt-set   0xNN       ESP_COEX_BT_ST_* bits (A2DP_STREAMING=0x10, PAUSED=0x20)
 *   s31-coex bt-clear 0xNN
 *   s31-coex interval N          coex_schm_interval_set
 *   s31-coex period   N          coex_schm_flexible_period_set
 *   s31-coex wifi-set/wifi-clear 0xNN
 *
 * Exists so that a coexistence hypothesis costs an echo, not an eight
 * minute loader reflash and a wedge. Talks to /dev/esps0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "s31_hosted_sram.h"

#define S31_HOSTED_IOC_COEX _IOWR('S', 0x37, struct s31_hosted_coex_msg)

int main(int argc, char **argv)
{
	struct s31_hosted_coex_msg m = { 0 };
	const char *what = argc > 1 ? argv[1] : "get";
	int fd;

	if (!strcmp(what, "get")) m.op = S31_HOSTED_COEX_GET;
	else if (!strcmp(what, "prefer")) m.op = S31_HOSTED_COEX_PREFER;
	else if (!strcmp(what, "bt-set")) m.op = S31_HOSTED_COEX_BT_SET;
	else if (!strcmp(what, "bt-clear")) m.op = S31_HOSTED_COEX_BT_CLEAR;
	else if (!strcmp(what, "interval")) m.op = S31_HOSTED_COEX_INTERVAL;
	else if (!strcmp(what, "period")) m.op = S31_HOSTED_COEX_FLEX_PERIOD;
	else if (!strcmp(what, "wifi-set")) m.op = S31_HOSTED_COEX_WIFI_SET;
	else if (!strcmp(what, "wifi-clear")) m.op = S31_HOSTED_COEX_WIFI_CLEAR;
	else { fprintf(stderr, "usage: s31-coex get|prefer|bt-set|bt-clear|interval|period|wifi-set|wifi-clear [arg]\n"); return 2; }
	if (argc > 2)
		m.arg = (unsigned)strtoul(argv[2], NULL, 0);
	fd = open("/dev/esps0", O_RDWR);
	if (fd < 0) { perror("/dev/esps0"); return 1; }
	if (ioctl(fd, S31_HOSTED_IOC_COEX, &m) < 0) { perror("ioctl COEX"); return 1; }
	printf("%s: status=%d result=%u arg=%u\n", what, (int)m.status, m.result, m.arg);
	return m.status ? 1 : 0;
}
