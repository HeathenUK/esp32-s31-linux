/*
 * hciinfo - print the controller's ACL geometry. No btmgmt/hciconfig ship
 * here, and this is the number that decides how many HCI packets one
 * 679-byte A2DP packet becomes on the hosted transport.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

int main(void)
{
	struct hci_dev_info di;
	int s = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);

	if (s < 0) { perror("hci socket"); return 1; }
	memset(&di, 0, sizeof(di));
	di.dev_id = 0;
	if (ioctl(s, HCIGETDEVINFO, &di) < 0) { perror("HCIGETDEVINFO"); return 1; }
	printf("features: %02x %02x %02x %02x %02x %02x %02x %02x  (LE=%d BR/EDR-not=%d SimulLE=%d)\n",
	       di.features[0], di.features[1], di.features[2], di.features[3],
	       di.features[4], di.features[5], di.features[6], di.features[7],
	       !!(di.features[4] & 0x40), !!(di.features[4] & 0x20), !!(di.features[4] & 0x80));
	printf("hci0 acl_mtu=%u acl_pkts=%u sco_mtu=%u sco_pkts=%u\n",
	       di.acl_mtu, di.acl_pkts, di.sco_mtu, di.sco_pkts);
	printf("stats: acl_tx=%u acl_rx=%u evt_rx=%u cmd_tx=%u err_rx=%u err_tx=%u\n",
	       di.stat.acl_tx, di.stat.acl_rx, di.stat.evt_rx, di.stat.cmd_tx,
	       di.stat.err_rx, di.stat.err_tx);
	return 0;
}
