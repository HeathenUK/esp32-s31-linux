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

	/*
	 * loglevel <tag> <level> - quieten hart0, which shares the 1 Mbps
	 * console with Linux. "s31-coex loglevel wifi 2" drops the Wi-Fi block
	 * from INFO to WARN and takes the ADDBA chatter with it.
	 */
	if (!strcmp(what, "loglevel")) {
		static const char *const tags[] = {
			"all", "wifi", "coexist", "pm", "hosted",
		};
		const char *tn = argc > 2 ? argv[2] : "wifi";
		unsigned lv = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 0) : 2;
		unsigned ti;

		for (ti = 0; ti < sizeof(tags) / sizeof(tags[0]); ti++)
			if (!strcmp(tn, tags[ti]))
				break;
		if (ti == sizeof(tags) / sizeof(tags[0]) || lv > 5) {
			fprintf(stderr, "usage: s31-coex loglevel "
				"all|wifi|coexist|pm|hosted 0..5\n"
				"  0 none 1 error 2 warn 3 info 4 debug 5 verbose\n");
			return 2;
		}
		if (lv > 3)
			fprintf(stderr, "note: the loader is built with "
				"CONFIG_LOG_MAXIMUM_LEVEL=3, so DEBUG and\n"
				"      VERBOSE are compiled out - this will "
				"not produce more output.\n");
		m.op = S31_HOSTED_COEX_LOGLEVEL;
		m.arg = (ti << 8) | lv;
	}
	else if (!strcmp(what, "get")) m.op = S31_HOSTED_COEX_GET;
	else if (!strcmp(what, "prefer")) m.op = S31_HOSTED_COEX_PREFER;
	else if (!strcmp(what, "bt-set")) m.op = S31_HOSTED_COEX_BT_SET;
	else if (!strcmp(what, "bt-clear")) m.op = S31_HOSTED_COEX_BT_CLEAR;
	else if (!strcmp(what, "interval")) m.op = S31_HOSTED_COEX_INTERVAL;
	else if (!strcmp(what, "period")) m.op = S31_HOSTED_COEX_FLEX_PERIOD;
	else if (!strcmp(what, "wifi-set")) m.op = S31_HOSTED_COEX_WIFI_SET;
	else if (!strcmp(what, "wifi-clear")) m.op = S31_HOSTED_COEX_WIFI_CLEAR;
	else { fprintf(stderr, "usage: s31-coex get|prefer|bt-set|bt-clear|interval|period|wifi-set|wifi-clear [arg]\n"); return 2; }
	/*
	 * loglevel builds its own arg from TWO words (tag, level), so it must
	 * not be re-parsed here - argv[2] is "wifi", strtoul() makes that 0,
	 * and the op silently sets level 0 on tag 0 instead. It reported
	 * status=OK while doing the wrong thing, which is the worst kind of
	 * bug and was only caught by checking the echoed arg.
	 */
	if (argc > 2 && strcmp(what, "loglevel"))
		m.arg = (unsigned)strtoul(argv[2], NULL, 0);
	fd = open("/dev/esps0", O_RDWR);
	if (fd < 0) { perror("/dev/esps0"); return 1; }
	if (ioctl(fd, S31_HOSTED_IOC_COEX, &m) < 0) { perror("ioctl COEX"); return 1; }
	printf("%s: status=%d result=%u arg=%u\n", what, (int)m.status, m.result, m.arg);
	return m.status ? 1 : 0;
}
