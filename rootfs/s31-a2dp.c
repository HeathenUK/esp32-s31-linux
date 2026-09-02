/*
 * s31-a2dp - a minimal A2DP source, in place of BlueALSA.
 *
 * WHY THIS EXISTS. BlueALSA is a fine general-purpose daemon - source and
 * sink, HFP, several codecs - and we use one corner of it: send stereo PCM
 * to a pair of headphones. On this board it also busy-waits at ~92% of the
 * single core with nothing connected, in glib's thread-pool spawner, which
 * starved the whole desktop (system idle 0%, input lag 170-411 ms; with it
 * stopped, 65% idle and 15 ms). bluez itself is NOT the problem -
 * bluetoothd measured 0% CPU and gives us HID, pairing and SDP natively -
 * so only the audio daemon is replaced.
 *
 * Nor can this move to hart0: the hosted Bluetooth is a VHCI transport
 * where hart0 is the controller and Linux is the host, and ESP-IDF's A2DP
 * lives in Bluedroid, a second host stack. One controller takes one host.
 *
 * WHAT IT DOES. Registers an A2DP *source* endpoint on bluez's Media API,
 * lets bluez negotiate SBC with the headphones, takes the transport file
 * descriptor, and writes RTP-framed SBC to it. libdbus and libsbc only -
 * no glib, no threads, one poll loop.
 *
 * usage: s31-a2dp <file.wav|->        44.1 kHz, 16-bit, stereo
 */
#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <sbc/sbc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include "s31_hosted_sram.h"
#include <sys/mman.h>

#define EP_PATH		"/s31/a2dp/source"
#define A2DP_SOURCE_UUID "0000110A-0000-1000-8000-00805F9B34FB"

/*
 * The SBC capability blob, exactly as A2DP defines it. The bitfield order
 * follows bluez's a2dp-codecs.h on little-endian: the low nibble of the
 * first byte is the channel mode. Getting this backwards makes bluez
 * negotiate a configuration the headphones then reject, which surfaces as
 * a transport that never becomes "active" rather than as any error.
 */
struct sbc_caps {
	unsigned char chan_mode:4;
	unsigned char freq:4;
	unsigned char alloc:2;
	unsigned char subbands:2;
	unsigned char block_len:4;
	unsigned char min_bitpool;
	unsigned char max_bitpool;
} __attribute__((packed));

#define FREQ_44100	(1 << 1)	/* bit order per the spec's table */
#define FREQ_48000	(1 << 0)
#define CHAN_JOINT	(1 << 0)
#define CHAN_STEREO	(1 << 1)
#define CHAN_DUAL	(1 << 2)
#define CHAN_MONO	(1 << 3)
#define BLOCK_16	(1 << 0)
#define BLOCK_12	(1 << 1)
#define BLOCK_8		(1 << 2)
#define BLOCK_4		(1 << 3)
#define SUB_8		(1 << 0)
#define SUB_4		(1 << 1)
#define ALLOC_LOUDNESS	(1 << 0)
#define ALLOC_SNR	(1 << 1)

static char transport_path[256];
static int transport_ready;
static struct sbc_caps chosen;

/*
 * Tell hart0's coexistence scheduler that A2DP is streaming, which is what
 * ESP-IDF's own A2DP source does through esp_coex_status_bit_set(). With
 * the Bluetooth host on Linux nobody else can. Best effort: a kernel
 * without the knob just means no hint.
 */
#define S31_HOSTED_IOC_COEX _IOWR('S', 0x37, struct s31_hosted_coex_msg)
static void coex_hint(int streaming)
{
	struct s31_hosted_coex_msg m = {
		.op = streaming ? S31_HOSTED_COEX_BT_SET : S31_HOSTED_COEX_BT_CLEAR,
		.arg = 0x10,			/* ESP_COEX_BT_ST_A2DP_STREAMING */
	};
	int fd = open("/dev/esps0", O_RDWR);

	if (fd < 0)
		return;
	if (ioctl(fd, S31_HOSTED_IOC_COEX, &m) == 0)
		fprintf(stderr, "s31-a2dp: coex A2DP_STREAMING %s (status %d)\n",
			streaming ? "set" : "cleared", (int)m.status);
	close(fd);
}

static uint64_t cpu_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
	return (uint64_t)t.tv_sec * 1000000ull + t.tv_nsec / 1000;
}

static uint64_t now_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000ull + t.tv_nsec / 1000;
}

/* Pick one bit, highest preference first, from what the peer offers. */
static int pick(unsigned int have, const int *order, int n)
{
	int i;

	for (i = 0; i < n; i++)
		if (have & order[i])
			return order[i];
	return 0;
}

static DBusHandlerResult ep_message(DBusConnection *conn, DBusMessage *msg,
				    void *user)
{
	DBusMessage *reply = NULL;
	const char *iface = dbus_message_get_interface(msg);
	const char *member = dbus_message_get_member(msg);

	(void)user;
	if (!iface || !member)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (!strcmp(iface, "org.freedesktop.DBus.Introspectable") &&
	    !strcmp(member, "Introspect")) {
		static const char *xml =
			"<node><interface name='org.bluez.MediaEndpoint1'>"
			"<method name='SetConfiguration'>"
			"<arg type='o' direction='in'/>"
			"<arg type='a{sv}' direction='in'/></method>"
			"<method name='SelectConfiguration'>"
			"<arg type='ay' direction='in'/>"
			"<arg type='ay' direction='out'/></method>"
			"<method name='ClearConfiguration'>"
			"<arg type='o' direction='in'/></method>"
			"<method name='Release'/>"
			"</interface></node>";

		reply = dbus_message_new_method_return(msg);
		dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml,
					 DBUS_TYPE_INVALID);
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (strcmp(iface, "org.bluez.MediaEndpoint1"))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (!strcmp(member, "SelectConfiguration")) {
		/*
		 * bluez hands us the SINK's capabilities and asks what we
		 * want. Choose the highest-quality setting both ends allow;
		 * bitpool is capped at 53, which is what every A2DP source
		 * uses for 44.1 kHz joint stereo and what keeps a frame
		 * inside one L2CAP MTU.
		 */
		unsigned char *caps = NULL;
		int n = 0;
		struct sbc_caps *peer, out;
		static const int freqs[] = { FREQ_44100, FREQ_48000 };
		static const int chans[] = { CHAN_JOINT, CHAN_STEREO,
					     CHAN_DUAL, CHAN_MONO };
		static const int blocks[] = { BLOCK_16, BLOCK_12, BLOCK_8,
					      BLOCK_4 };
		static const int subs[] = { SUB_8, SUB_4 };
		static const int allocs[] = { ALLOC_LOUDNESS, ALLOC_SNR };
		unsigned char *outp = (unsigned char *)&out;

		dbus_message_get_args(msg, NULL, DBUS_TYPE_ARRAY,
				      DBUS_TYPE_BYTE, &caps, &n,
				      DBUS_TYPE_INVALID);
		if (n < (int)sizeof(struct sbc_caps)) {
			reply = dbus_message_new_error(msg,
				"org.bluez.Error.InvalidArguments",
				"short SBC capabilities");
			dbus_connection_send(conn, reply, NULL);
			dbus_message_unref(reply);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		peer = (struct sbc_caps *)caps;
		memset(&out, 0, sizeof(out));
		out.freq = pick(peer->freq, freqs, 2);
		out.chan_mode = pick(peer->chan_mode, chans, 4);
		out.block_len = pick(peer->block_len, blocks, 4);
		out.subbands = pick(peer->subbands, subs, 2);
		out.alloc = pick(peer->alloc, allocs, 2);
		out.min_bitpool = peer->min_bitpool < 2 ? 2 : peer->min_bitpool;
		out.max_bitpool = peer->max_bitpool > 53 ? 53 : peer->max_bitpool;
		chosen = out;
		fprintf(stderr, "s31-a2dp: chose freq=0x%x chan=0x%x "
			"block=0x%x sub=0x%x alloc=0x%x bitpool %u-%u\n",
			out.freq, out.chan_mode, out.block_len, out.subbands,
			out.alloc, out.min_bitpool, out.max_bitpool);
		reply = dbus_message_new_method_return(msg);
		dbus_message_append_args(reply, DBUS_TYPE_ARRAY,
					 DBUS_TYPE_BYTE, &outp,
					 (int)sizeof(out), DBUS_TYPE_INVALID);
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (!strcmp(member, "SetConfiguration")) {
		const char *path = NULL;
		DBusMessageIter it, dict;

		dbus_message_iter_init(msg, &it);
		if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH)
			dbus_message_iter_get_basic(&it, &path);
		if (path) {
			snprintf(transport_path, sizeof(transport_path),
				 "%s", path);
			transport_ready = 1;
			fprintf(stderr, "s31-a2dp: transport %s\n", path);
		}
		/* walk the dict for the negotiated Configuration blob */
		if (dbus_message_iter_next(&it) &&
		    dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
			dbus_message_iter_recurse(&it, &dict);
			while (dbus_message_iter_get_arg_type(&dict) ==
			       DBUS_TYPE_DICT_ENTRY) {
				DBusMessageIter kv, var, arr;
				const char *key = NULL;

				dbus_message_iter_recurse(&dict, &kv);
				dbus_message_iter_get_basic(&kv, &key);
				dbus_message_iter_next(&kv);
				dbus_message_iter_recurse(&kv, &var);
				if (key && !strcmp(key, "Configuration") &&
				    dbus_message_iter_get_arg_type(&var) ==
				    DBUS_TYPE_ARRAY) {
					unsigned char *c = NULL;
					int cn = 0;

					dbus_message_iter_recurse(&var, &arr);
					dbus_message_iter_get_fixed_array(&arr,
						&c, &cn);
					if (cn >= (int)sizeof(chosen))
						memcpy(&chosen, c,
						       sizeof(chosen));
				}
				dbus_message_iter_next(&dict);
			}
		}
		reply = dbus_message_new_method_return(msg);
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (!strcmp(member, "ClearConfiguration") || !strcmp(member, "Release")) {
		transport_ready = 0;
		transport_path[0] = 0;
		fprintf(stderr, "s31-a2dp: %s\n", member);
		reply = dbus_message_new_method_return(msg);
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int register_endpoint(DBusConnection *conn, const char *adapter)
{
	DBusMessage *m, *r;
	DBusMessageIter it, dict, e, var, arr;
	DBusError err;
	struct sbc_caps caps;
	const char *uuid = A2DP_SOURCE_UUID;
	const char *k_uuid = "UUID", *k_codec = "Codec", *k_caps = "Capabilities";
	unsigned char codec = 0;		/* SBC */
	unsigned char *capp = (unsigned char *)&caps;
	const char *path = EP_PATH;

	memset(&caps, 0, sizeof(caps));
	caps.freq = FREQ_44100 | FREQ_48000;
	caps.chan_mode = CHAN_MONO | CHAN_DUAL | CHAN_STEREO | CHAN_JOINT;
	caps.block_len = BLOCK_4 | BLOCK_8 | BLOCK_12 | BLOCK_16;
	caps.subbands = SUB_4 | SUB_8;
	caps.alloc = ALLOC_SNR | ALLOC_LOUDNESS;
	caps.min_bitpool = 2;
	caps.max_bitpool = 53;

	m = dbus_message_new_method_call("org.bluez", adapter,
					 "org.bluez.Media1",
					 "RegisterEndpoint");
	if (!m)
		return -1;
	dbus_message_iter_init_append(m, &it);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &path);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);

	dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
	dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k_uuid);
	dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "s", &var);
	dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &uuid);
	dbus_message_iter_close_container(&e, &var);
	dbus_message_iter_close_container(&dict, &e);

	dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
	dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k_codec);
	dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "y", &var);
	dbus_message_iter_append_basic(&var, DBUS_TYPE_BYTE, &codec);
	dbus_message_iter_close_container(&e, &var);
	dbus_message_iter_close_container(&dict, &e);

	dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
	dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k_caps);
	dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "ay", &var);
	dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "y", &arr);
	dbus_message_iter_append_fixed_array(&arr, DBUS_TYPE_BYTE, &capp,
					     (int)sizeof(caps));
	dbus_message_iter_close_container(&var, &arr);
	dbus_message_iter_close_container(&e, &var);
	dbus_message_iter_close_container(&dict, &e);

	dbus_message_iter_close_container(&it, &dict);

	dbus_error_init(&err);
	r = dbus_connection_send_with_reply_and_block(conn, m, 5000, &err);
	dbus_message_unref(m);
	if (!r) {
		fprintf(stderr, "s31-a2dp: RegisterEndpoint failed: %s\n",
			err.message ? err.message : "?");
		dbus_error_free(&err);
		return -1;
	}
	dbus_message_unref(r);
	return 0;
}

/*
 * Find a transport without being told about one.
 *
 * BlueZ 5.79 does not necessarily call SetConfiguration on our endpoint:
 * when the sink is already configured it exposes the stream under the
 * REMOTE SEP instead, at .../devXX/sepN/fdN, and an application is
 * expected to go and find it. Waiting for SetConfiguration alone left us
 * registered and idle while a perfectly good SBC transport sat there.
 *
 * So ask the object manager for anything implementing MediaTransport1.
 * The interface name appears in the reply as a plain string, so the tree
 * does not need decoding in full: walk to each object path, then check
 * its interface names.
 */
static int find_transport(DBusConnection *conn)
{
	DBusMessage *m, *r;
	DBusMessageIter it, objs;
	DBusError err;
	int found = 0;

	m = dbus_message_new_method_call("org.bluez", "/",
					 "org.freedesktop.DBus.ObjectManager",
					 "GetManagedObjects");
	if (!m)
		return 0;
	dbus_error_init(&err);
	r = dbus_connection_send_with_reply_and_block(conn, m, 5000, &err);
	dbus_message_unref(m);
	if (!r) {
		dbus_error_free(&err);
		return 0;
	}
	if (!dbus_message_iter_init(r, &it) ||
	    dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) {
		dbus_message_unref(r);
		return 0;
	}
	dbus_message_iter_recurse(&it, &objs);
	while (!found &&
	       dbus_message_iter_get_arg_type(&objs) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter ent, ifaces;
		const char *path = NULL;

		dbus_message_iter_recurse(&objs, &ent);
		dbus_message_iter_get_basic(&ent, &path);
		dbus_message_iter_next(&ent);
		if (path && dbus_message_iter_get_arg_type(&ent) ==
		    DBUS_TYPE_ARRAY) {
			dbus_message_iter_recurse(&ent, &ifaces);
			while (dbus_message_iter_get_arg_type(&ifaces) ==
			       DBUS_TYPE_DICT_ENTRY) {
				DBusMessageIter kv;
				const char *iname = NULL;

				dbus_message_iter_recurse(&ifaces, &kv);
				dbus_message_iter_get_basic(&kv, &iname);
				if (iname && !strcmp(iname,
						     "org.bluez.MediaTransport1")) {
					snprintf(transport_path,
						 sizeof(transport_path),
						 "%s", path);
					found = 1;
					break;
				}
				dbus_message_iter_next(&ifaces);
			}
		}
		dbus_message_iter_next(&objs);
	}
	dbus_message_unref(r);
	if (found)
		fprintf(stderr, "s31-a2dp: found transport %s\n",
			transport_path);
	return found;
}

/* Acquire the transport: returns the fd, or -1. */
static int transport_acquire(DBusConnection *conn, int *write_mtu)
{
	DBusMessage *m, *r;
	DBusError err;
	int fd = -1;
	dbus_uint16_t rmtu = 0, wmtu = 0;

	if (!transport_path[0])
		return -1;	/* cleared under us - an empty path aborts libdbus */
	m = dbus_message_new_method_call("org.bluez", transport_path,
					 "org.bluez.MediaTransport1",
					 "Acquire");
	if (!m)
		return -1;
	dbus_error_init(&err);
	r = dbus_connection_send_with_reply_and_block(conn, m, 10000, &err);
	dbus_message_unref(m);
	if (!r) {
		fprintf(stderr, "s31-a2dp: Acquire failed: %s\n",
			err.message ? err.message : "?");
		dbus_error_free(&err);
		return -1;
	}
	if (!dbus_message_get_args(r, &err, DBUS_TYPE_UNIX_FD, &fd,
				   DBUS_TYPE_UINT16, &rmtu,
				   DBUS_TYPE_UINT16, &wmtu,
				   DBUS_TYPE_INVALID)) {
		fprintf(stderr, "s31-a2dp: Acquire reply: %s\n",
			err.message ? err.message : "?");
		dbus_error_free(&err);
		dbus_message_unref(r);
		return -1;
	}
	dbus_message_unref(r);
	*write_mtu = wmtu;
	return fd;
}

/*
 * Read the negotiated SBC blob off a transport we found ourselves.
 * MediaTransport1.Configuration is the same four bytes SetConfiguration
 * would have handed us.
 */
static void transport_config(DBusConnection *conn)
{
	DBusMessage *m, *r;
	DBusMessageIter it, var, arr;
	DBusError err;
	const char *iface = "org.bluez.MediaTransport1";
	const char *prop = "Configuration";
	unsigned char *c = NULL;
	int n = 0;

	m = dbus_message_new_method_call("org.bluez", transport_path,
					 "org.freedesktop.DBus.Properties",
					 "Get");
	if (!m)
		return;
	dbus_message_append_args(m, DBUS_TYPE_STRING, &iface,
				 DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
	dbus_error_init(&err);
	r = dbus_connection_send_with_reply_and_block(conn, m, 5000, &err);
	dbus_message_unref(m);
	if (!r) {
		dbus_error_free(&err);
		return;
	}
	if (dbus_message_iter_init(r, &it) &&
	    dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
		dbus_message_iter_recurse(&it, &var);
		if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_ARRAY) {
			dbus_message_iter_recurse(&var, &arr);
			dbus_message_iter_get_fixed_array(&arr, &c, &n);
			if (n >= (int)sizeof(chosen)) {
				memcpy(&chosen, c, sizeof(chosen));
				fprintf(stderr, "s31-a2dp: transport config "
					"freq=0x%x chan=0x%x bitpool %u-%u\n",
					chosen.freq, chosen.chan_mode,
					chosen.min_bitpool, chosen.max_bitpool);
			}
		}
	}
	dbus_message_unref(r);
}

static int sbc_freq_hz(unsigned int f)
{
	return (f & FREQ_44100) ? 44100 : 48000;
}

int main(int argc, char **argv)
{
	const char *adapter = "/org/bluez/hci0";
	const char *path = argc > 1 ? argv[1] : "-";
	DBusConnection *conn;
	DBusError err;
	DBusObjectPathVTable vt = { .message_function = ep_message };
	FILE *in;
	sbc_t sbc;
	int fd = -1, wmtu = 0;
	unsigned char *pcm, *pkt;
	size_t codesize, framelen;
	uint64_t t0, sent_us = 0, report_at = 0;
	unsigned int seq = 0, ts = 0;
	int max_frames = 15;
	/*
	 * Dropout accounting. "Solid A2DP while Wi-Fi is busy" is the actual
	 * requirement, so measure the thing that breaks: how far behind real
	 * time the writer falls. The sink plays from a small buffer; if we
	 * are late handing frames over, that is a gap the ear hears. late_ms
	 * is the worst lateness in the current window, stalls counts writes
	 * that could not go out immediately.
	 */
	long stalls = 0, late_max = 0, late_events = 0;
	/* per-packet cost split: is it the encode, or the write? */
	uint64_t t_enc = 0, t_wr = 0, wr_max = 0, t_rd = 0, c_enc = 0;
	int loop = getenv("S31_A2DP_LOOP") != NULL;

	/*
	 * Real-time priority, because this is a real-time job on a machine
	 * with ONE core and a desktop on it.
	 *
	 * Measured without it: 38.7 ms of WALL time per packet inside
	 * sbc_encode() against 0.23 ms per frame of actual codec work
	 * (rootfs/sbcbench.c) - i.e. almost all of it was this process being
	 * descheduled while lvdesk and its clients ran. The stream fell
	 * behind real time at about 2 s per 5 s and the sink crackled
	 * continuously. Wall time is not CPU time on this board; that
	 * confusion has now cost two separate investigations.
	 *
	 * The priority is deliberately low (5): the daemon sleeps between
	 * packets, so it needs to be woken PROMPTLY rather than to run a
	 * lot, and a high FIFO priority on one core would starve the
	 * desktop it is playing for. mlockall is cheap here - a few hundred
	 * KB - and stops a page fault landing mid-packet.
	 */
	{
		struct sched_param sp = { .sched_priority = 5 };

		/*
		 * The raw syscall, not sched_setscheduler(): musl implements
		 * that function as an unconditional ENOSYS stub (it objects to
		 * Linux's per-thread semantics), which is how this daemon ran
		 * an entire afternoon of "RT priority changed nothing" tests at
		 * nice 0. busybox chrt uses the syscall and works.
		 */
		if (syscall(SYS_sched_setscheduler, 0, SCHED_FIFO, &sp) != 0)
			perror("s31-a2dp: SCHED_FIFO (continuing)");
		if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
			perror("s31-a2dp: mlockall (continuing)");
	}

	dbus_error_init(&err);
	conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
	if (!conn) {
		fprintf(stderr, "s31-a2dp: system bus: %s\n",
			err.message ? err.message : "?");
		return 1;
	}
	if (!dbus_connection_register_object_path(conn, EP_PATH, &vt, NULL)) {
		fprintf(stderr, "s31-a2dp: cannot own %s\n", EP_PATH);
		return 1;
	}
	if (register_endpoint(conn, adapter))
		return 1;
	fprintf(stderr, "s31-a2dp: endpoint registered, waiting for a sink\n");

	/*
	 * Wait for bluez to configure us - but also go looking. A sink that
	 * is already streaming-capable when we start gets no SetConfiguration
	 * call at all; its transport is simply there.
	 */
	{
		int spins = 0;

		while (!transport_ready) {
			if (!dbus_connection_read_write_dispatch(conn, 200)) {
				fprintf(stderr, "s31-a2dp: bus closed\n");
				return 1;
			}
			if (++spins % 5 == 0 && find_transport(conn)) {
				transport_ready = 1;
				/*
				 * No SetConfiguration means no negotiated blob,
				 * so read it from the transport itself.
				 */
				transport_config(conn);
			}
		}
	}
	/*
	 * Acquire, patiently. A transport can be handed to us and then
	 * withdrawn while the sink finishes its own setup - seen as
	 * SetConfiguration on .../fd1, Acquire -EIO, then ClearConfiguration
	 * and a fresh transport a moment later - so this keeps pumping the
	 * bus and goes back to waiting whenever the path is cleared, rather
	 * than retrying a name that no longer exists.
	 */
	for (int i = 0; i < 120 && fd < 0; i++) {
		if (transport_ready && transport_path[0])
			fd = transport_acquire(conn, &wmtu);
		if (fd < 0) {
			dbus_connection_read_write_dispatch(conn, 250);
			if (!transport_ready && (i % 4) == 3)
				if (find_transport(conn)) {
					transport_ready = 1;
					transport_config(conn);
				}
		}
	}
	if (fd < 0) {
		fprintf(stderr, "s31-a2dp: no usable transport after 30 s\n");
		return 1;
	}
	fprintf(stderr, "s31-a2dp: transport fd=%d write_mtu=%d\n", fd, wmtu);
	coex_hint(1);

	sbc_init_a2dp(&sbc, 0L, &chosen, sizeof(chosen));
	sbc.endian = SBC_LE;
	codesize = sbc_get_codesize(&sbc);
	framelen = sbc_get_frame_length(&sbc);
	pcm = malloc(codesize);
	pkt = malloc(wmtu > 0 ? (size_t)wmtu : 1024);
	if (!pcm || !pkt)
		return 1;

	fprintf(stderr, "s31-a2dp: codesize=%zu framelen=%zu mtu=%d\n",
		codesize, framelen, wmtu);
	in = strcmp(path, "-") ? fopen(path, "rb") : stdin;
	if (!in) {
		perror("s31-a2dp: open");
		return 1;
	}
	/*
	 * A big input buffer, because this is a REAL-TIME path.
	 *
	 * One SBC frame is 512 bytes of PCM and stdio's default buffer is
	 * 4 KB, so every eighth frame went to the SD card - in the middle of
	 * the audio loop, at this board's ~3.2 ms per request and worse when
	 * the page cache is under pressure. Measured before the fix: 52 ms
	 * per packet in "encode", against 8 ms to write and ~2 ms of actual
	 * SBC work, so the stream ran at a third of real time and the sink
	 * crackled continuously. The codec was never the problem.
	 */
	{
		static char inbuf[256 * 1024];

		setvbuf(in, inbuf, _IOFBF, sizeof(inbuf));
	}
	if (in != stdin)
		fseek(in, 44, SEEK_SET);	/* skip a canonical WAV header */

	t0 = now_us();
	for (;;) {
		/*
		 * Fill one L2CAP packet with as many SBC frames as the MTU
		 * takes: 13 bytes of RTP plus SBC payload header, then whole
		 * frames. One write per packet - a write per FRAME would cost
		 * a socket syscall (1.3-5.8 ms here) every 128 samples.
		 */
		size_t off = 13, nframes = 0;
		ssize_t w;

		uint64_t tenc0 = now_us(), cenc0 = cpu_us();

		while (off + framelen <= (size_t)wmtu &&
		       nframes < (size_t)max_frames) {
			ssize_t enc, wrote = 0;
			uint64_t trd0 = now_us();
			size_t rd = fread(pcm, 1, codesize, in);

			t_rd += now_us() - trd0;
			if (rd < codesize)
				break;
			/*
			 * sbc_encode() returns the INPUT bytes consumed, not
			 * the size of what it wrote - that comes back in the
			 * last argument. Using the return value as the output
			 * length advances the packet by 512 bytes a frame
			 * instead of 77, so every write overruns the L2CAP
			 * MTU and the sink hears nothing at all.
			 */
			enc = sbc_encode(&sbc, pcm, codesize, pkt + off,
					 (size_t)wmtu - off, &wrote);
			if (enc <= 0 || wrote <= 0)
				break;
			off += (size_t)wrote;
			nframes++;
			ts += (unsigned int)(codesize / 4);	/* stereo s16 */
		}
		if (!nframes) {
			if (loop && in != stdin) {
				fseek(in, 44, SEEK_SET);
				continue;
			}
			break;
		}
		t_enc += now_us() - tenc0;
		c_enc += cpu_us() - cenc0;
		pkt[0] = 0x80;			/* RTP v2 */
		pkt[1] = 96;			/* dynamic payload type */
		pkt[2] = (seq >> 8) & 0xff; pkt[3] = seq & 0xff;
		pkt[4] = (ts >> 24) & 0xff; pkt[5] = (ts >> 16) & 0xff;
		pkt[6] = (ts >> 8) & 0xff;  pkt[7] = ts & 0xff;
		pkt[8] = 0; pkt[9] = 0; pkt[10] = 0; pkt[11] = 1;  /* SSRC */
		pkt[12] = (unsigned char)nframes;	/* SBC payload header */
		seq++;

		{
			uint64_t tw0 = now_us(), d;

			w = write(fd, pkt, off);
			d = now_us() - tw0;
			t_wr += d;
			if (d > wr_max)
				wr_max = d;
		}
		if (w < 0) {
			if (errno == EAGAIN) {
				struct pollfd p = { .fd = fd, .events = POLLOUT };

				stalls++;
				poll(&p, 1, 100);
				continue;
			}
			if (errno == EMSGSIZE && max_frames > 1) {
				/*
				 * The MTU bluez reports is not always what the
				 * socket will take. Rather than guess the
				 * overhead, halve the frames per packet and
				 * carry on - it converges in a couple of
				 * steps and costs one dropped packet.
				 */
				max_frames /= 2;
				fprintf(stderr, "s31-a2dp: EMSGSIZE at %zu "
					"bytes, %d frames/packet now\n",
					off, max_frames);
				continue;
			}
			perror("s31-a2dp: write");
			break;
		}
		/*
		 * Pace to real time. The sink has a small buffer; writing as
		 * fast as the encoder runs overruns it and the audio breaks
		 * up, and sleeping a fixed amount drifts. Track how much
		 * audio has been handed over and sleep only the surplus.
		 */
		sent_us += (uint64_t)nframes * (codesize / 4) * 1000000ull /
			   (uint64_t)sbc_freq_hz(chosen.freq);
		{
			int64_t ahead = (int64_t)sent_us -
					(int64_t)(now_us() - t0);

			if (ahead > 20000) {
				usleep((useconds_t)(ahead - 10000));
			} else if (ahead < 0) {
				/* behind real time: the sink is starving */
				long late = (long)(-ahead / 1000);

				if (late > late_max)
					late_max = late;
				if (late > 20)
					late_events++;
			}
			if (now_us() - t0 > report_at) {
				report_at += 5000000ull;
				fprintf(stderr, "s31-a2dp: %us %u pkts "
					"late_max=%ld ms stalls=%ld | per pkt: "
					"read=%lluus encode=%lluus(cpu %lluus) "
					"write=%lluus worst=%lluus\n",
					(unsigned)((now_us() - t0) / 1000000),
					seq, late_max, stalls,
					(unsigned long long)(seq ? t_rd / seq : 0),
					(unsigned long long)(seq ? (t_enc - t_rd) / seq : 0),
					(unsigned long long)(seq ? c_enc / seq : 0),
					(unsigned long long)(seq ? t_wr / seq : 0),
					(unsigned long long)wr_max);
				late_max = 0; wr_max = 0;
			}
		}
		dbus_connection_read_write_dispatch(conn, 0);
		if (!transport_ready) {
			fprintf(stderr, "s31-a2dp: transport went away\n");
			break;
		}
	}
	coex_hint(0);
	fprintf(stderr, "s31-a2dp: done, %u packets\n", seq);
	sbc_finish(&sbc);
	close(fd);
	if (in != stdin)
		fclose(in);
	return 0;
}
