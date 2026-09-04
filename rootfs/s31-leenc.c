/*
 * s31-leenc - known-answer test of the controller's LE AES engine.
 *
 * LE link encryption derives the session key as SK = e(LTK, SKD) inside the
 * controller. If that AES is wrong, every encrypted packet fails its MIC
 * while the host's own key arithmetic looks perfect - which is exactly the
 * symptom on this board. HCI_LE_Encrypt (0x2017) exposes that same engine,
 * so the Bluetooth spec's own test vector (Core v5, Vol 3, Part H, D.1)
 * settles it without needing any peer device.
 *
 * HCI carries keys and data least-significant octet first; the spec quotes
 * them most-significant first, so both are byte-reversed on the way in and
 * the answer is reversed on the way back.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_HCI 1
#define HCI_CHANNEL_RAW 0
#define SOL_HCI 0
#define HCI_FILTER 2
#define HCI_COMMAND_PKT 0x01
#define HCI_EVENT_PKT 0x04
#define EVT_CMD_COMPLETE 0x0e

struct sockaddr_hci {
	unsigned short hci_family;
	unsigned short hci_dev;
	unsigned short hci_channel;
};
struct hci_filter {
	unsigned int type_mask;
	unsigned int event_mask[2];
	unsigned short opcode;
};

static void rev(unsigned char *d, const unsigned char *s, int n)
{
	int i;

	for (i = 0; i < n; i++)
		d[i] = s[n - 1 - i];
}

static int hex(const char *h, unsigned char *out, int n)
{
	int i;

	for (i = 0; i < n; i++)
		if (sscanf(h + 2 * i, "%2hhx", &out[i]) != 1)
			return -1;
	return 0;
}

int main(int argc, char **argv)
{
	struct sockaddr_hci sa = { .hci_family = AF_BLUETOOTH, .hci_dev = 0,
				   .hci_channel = HCI_CHANNEL_RAW };
	struct hci_filter flt;
	unsigned char key_be[16], pt_be[16], want_be[16];
	unsigned char cmd[4 + 32], buf[260];
	int fd, i;
	struct pollfd p;

	/* Core v5 Vol 3 Part H D.1, or override: s31-leenc <key> <pt> <exp> */
	hex(argc > 1 ? argv[1] : "4C68384139F574D836BCF34E9DFB01BF", key_be, 16);
	hex(argc > 2 ? argv[2] : "0213243546576879ACBDCEDFE0F10213", pt_be, 16);
	hex(argc > 3 ? argv[3] : "99AD1B5226A37E3E058E3B8E27C2C666", want_be, 16);

	fd = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
	if (fd < 0) {
		perror("socket");
		return 1;
	}
	memset(&flt, 0, sizeof(flt));
	flt.type_mask = 1u << HCI_EVENT_PKT;
	flt.event_mask[0] = 0xffffffff;
	flt.event_mask[1] = 0xffffffff;
	if (setsockopt(fd, SOL_HCI, HCI_FILTER, &flt, sizeof(flt)) < 0)
		perror("filter (continuing)");
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("bind hci0");
		return 1;
	}

	cmd[0] = HCI_COMMAND_PKT;
	cmd[1] = 0x17; cmd[2] = 0x20;		/* HCI_LE_Encrypt */
	cmd[3] = 32;
	rev(cmd + 4, key_be, 16);
	rev(cmd + 20, pt_be, 16);
	if (write(fd, cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd)) {
		perror("write HCI_LE_Encrypt");
		return 1;
	}

	p.fd = fd; p.events = POLLIN;
	for (i = 0; i < 40; i++) {
		ssize_t n;

		if (poll(&p, 1, 250) <= 0)
			continue;
		n = read(fd, buf, sizeof(buf));
		if (n < 7 || buf[0] != HCI_EVENT_PKT || buf[1] != EVT_CMD_COMPLETE)
			continue;
		if (buf[4] != 0x17 || buf[5] != 0x20)
			continue;
		if (buf[6] != 0x00) {
			printf("LE_Encrypt returned status 0x%02x\n", buf[6]);
			return 2;
		}
		if (n < 7 + 16) {
			printf("short reply (%d bytes)\n", (int)n);
			return 2;
		}
		{
			unsigned char got_be[16];

			rev(got_be, buf + 7, 16);
			printf("key       %.*s\n", 32, argc > 1 ? argv[1] : "4C68384139F574D836BCF34E9DFB01BF");
			printf("expected  ");
			for (int j = 0; j < 16; j++) printf("%02X", want_be[j]);
			printf("\ncontroller ");
			for (int j = 0; j < 16; j++) printf("%02X", got_be[j]);
			printf("\nVERDICT: %s\n",
			       memcmp(got_be, want_be, 16) == 0 ?
			       "PASS - the controller's AES is correct" :
			       "FAIL - the controller's AES is WRONG");
			return memcmp(got_be, want_be, 16) == 0 ? 0 : 3;
		}
	}
	printf("no Command Complete for LE_Encrypt in 10 s\n");
	return 1;
}
