/*
 * s31-bt - the board's one Bluetooth daemon: pairing agent, device list,
 * control socket for the desktop, and the A2DP source that s31-a2dp was.
 *
 * WHY ONE PROCESS. bluetoothd gives us discovery, pairing, HID and the
 * Media API over D-Bus, and every one of those calls blocks for seconds
 * (Pair, Connect). lvdesk is single-threaded and is the compositor, so it
 * must never make them. This daemon owns the D-Bus connection and talks to
 * lvdesk over a control socket in the shape of wpa_supplicant's: one
 * command per line, replies and events pushed on the same connection, so
 * it joins lvdesk's poll set like the Wi-Fi socket does. libdbus and
 * libsbc only - no glib (BlueALSA's spun at 92% of the core), no threads.
 *
 * Protocol (one line each; '#' introduces a reply/event line below):
 *
 *   list                  # DEV <addr> paired=0|1 conn=0|1 trusted=0|1
 *                         #     kind=audio|keyboard|mouse|gamepad|other
 *                         #     rssi=<n> "<name>"   (one per device), then OK
 *   scan on|off           # SCAN on|off ; DEV lines follow as devices appear
 *   pair <addr>           # PAIRING <addr>; then PASSKEY <addr> <6 digits> |
 *                         #   CONFIRM <addr> <6 digits> | PIN <addr> <pin>;
 *                         #   then PAIRED <addr> | FAIL <addr> <reason>
 *   confirm <addr> yes|no
 *   connect|disconnect|forget <addr>   # CONNECTED/DISCONNECTED/FORGOT or FAIL
 *   power on|off          # STATE line follows
 *   status                # STATE powered=0|1 scanning=0|1 connected=<addr>|-
 *   play <file.wav>|stop  # PLAY <addr> | STOPPED | FAIL ...
 *
 * Client mode: `s31-bt cmd <line...>` sends one line and prints what comes
 * back for a few seconds; `s31-bt monitor` prints events until killed.
 */
#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <sbc/sbc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include "s31_hosted_sram.h"

#define CTL_PATH	"/var/run/s31-bt.ctl"
#define AGENT_PATH	"/s31/agent"
#define EP_PATH		"/s31/a2dp/source"
#define A2DP_SOURCE_UUID "0000110A-0000-1000-8000-00805F9B34FB"
#define A2DP_SINK_UUID	 "0000110b-0000-1000-8000-00805f9b34fb"
#define HID_UUID	 "00001124-0000-1000-8000-00805f9b34fb"
#define HOG_UUID	 "00001812-0000-1000-8000-00805f9b34fb"

#define MAXDEV		32
#define MAXCLI		6

static DBusConnection *conn;
static char adapter[64] = "/org/bluez/hci0";
static int powered, discovering;
static int scan_stop_at;			/* seconds, 0 = not armed */

/* ------------------------------------------------------------ devices */

struct dev {
	char path[80];
	char addr[18];
	char name[48];
	unsigned int class;
	int paired, trusted, connected, rssi;
	int audio, hid;
	int used;
};
static struct dev devs[MAXDEV];

static uint64_t now_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000ull + t.tv_nsec / 1000;
}

static uint64_t cpu_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
	return (uint64_t)t.tv_sec * 1000000ull + t.tv_nsec / 1000;
}

static struct dev *dev_by_path(const char *path)
{
	int i;

	for (i = 0; i < MAXDEV; i++)
		if (devs[i].used && !strcmp(devs[i].path, path))
			return &devs[i];
	return NULL;
}

static struct dev *dev_by_addr(const char *addr)
{
	int i;

	for (i = 0; i < MAXDEV; i++)
		if (devs[i].used && !strcasecmp(devs[i].addr, addr))
			return &devs[i];
	return NULL;
}

static struct dev *dev_get(const char *path)
{
	struct dev *d = dev_by_path(path);
	int i;

	if (d)
		return d;
	for (i = 0; i < MAXDEV; i++)
		if (!devs[i].used) {
			d = &devs[i];
			memset(d, 0, sizeof(*d));
			d->used = 1;
			d->rssi = 0;
			snprintf(d->path, sizeof(d->path), "%s", path);
			return d;
		}
	return NULL;
}

/*
 * What is it? The class of device is the quick answer for classic devices
 * (major 5 = peripheral, minor bits say keyboard/pointer; major 4 = audio),
 * the UUID list the reliable one (HID 0x1124 classic, HoG 0x1812 BLE,
 * A2DP sink 0x110b). A device that is neither is still listed as "other"
 * once paired, and hidden from scan results.
 */
static const char *dev_kind(const struct dev *d)
{
	unsigned int major = (d->class >> 8) & 0x1f;
	unsigned int minor = (d->class >> 2) & 0x3f;

	if (d->audio)
		return "audio";
	if (major == 5 || d->hid) {
		if (minor & 0x10)
			return "keyboard";
		if (minor & 0x20)
			return "mouse";
		if ((minor & 0x0f) == 0x02 || (minor & 0x0f) == 0x01)
			return "gamepad";
		return d->hid ? "keyboard" : "other";
	}
	if (major == 4)
		return "audio";
	return "other";
}

static int dev_interesting(const struct dev *d)
{
	return d->paired || d->audio || d->hid ||
	       ((d->class >> 8) & 0x1f) == 4 || ((d->class >> 8) & 0x1f) == 5;
}

/* ------------------------------------------------------------ clients */

struct cli {
	int fd;
	char buf[256];
	int len;
	int monitor;	/* wants events */
};
static struct cli clis[MAXCLI];
static int ctl_fd = -1;

static void cli_send(struct cli *c, const char *fmt, ...)
{
	char line[320];
	va_list ap;
	int n;

	if (c->fd < 0)
		return;
	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof(line) - 2)
		n = sizeof(line) - 2;
	line[n++] = '\n';
	if (write(c->fd, line, (size_t)n) < 0) {
		close(c->fd);
		c->fd = -1;
	}
}

static void event(const char *fmt, ...)
{
	char line[320];
	va_list ap;
	int i, n;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof(line) - 2)
		n = sizeof(line) - 2;
	fprintf(stderr, "s31-bt: %s\n", line);
	line[n++] = '\n';
	for (i = 0; i < MAXCLI; i++)
		if (clis[i].fd >= 0 && clis[i].monitor)
			if (write(clis[i].fd, line, (size_t)n) < 0) {
				close(clis[i].fd);
				clis[i].fd = -1;
			}
}

static void dev_line(struct cli *c, const struct dev *d)
{
	char line[200];

	snprintf(line, sizeof(line),
		 "DEV %s paired=%d conn=%d trusted=%d kind=%s rssi=%d \"%s\"",
		 d->addr, d->paired, d->connected, d->trusted, dev_kind(d),
		 d->rssi, d->name[0] ? d->name : d->addr);
	if (c)
		cli_send(c, "%s", line);
	else
		event("%s", line);
}

/* ------------------------------------------------------------ D-Bus helpers */

/* Walk a{sv} of org.bluez.Device1 properties into a dev. Returns 1 if changed. */
static int dev_props(struct dev *d, DBusMessageIter *dict)
{
	int changed = 0;

	while (dbus_message_iter_get_arg_type(dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter kv, var;
		const char *key = NULL;
		int t;

		dbus_message_iter_recurse(dict, &kv);
		dbus_message_iter_get_basic(&kv, &key);
		dbus_message_iter_next(&kv);
		dbus_message_iter_recurse(&kv, &var);
		t = dbus_message_iter_get_arg_type(&var);
		if (!key) {
			dbus_message_iter_next(dict);
			continue;
		}
		if (!strcmp(key, "Address") && t == DBUS_TYPE_STRING) {
			const char *s;

			dbus_message_iter_get_basic(&var, &s);
			snprintf(d->addr, sizeof(d->addr), "%s", s);
		} else if ((!strcmp(key, "Alias") || (!strcmp(key, "Name") && !d->name[0])) &&
			   t == DBUS_TYPE_STRING) {
			const char *s;

			dbus_message_iter_get_basic(&var, &s);
			if (strcmp(d->name, s)) {
				snprintf(d->name, sizeof(d->name), "%s", s);
				changed = 1;
			}
		} else if (!strcmp(key, "Class") && t == DBUS_TYPE_UINT32) {
			dbus_uint32_t v;

			dbus_message_iter_get_basic(&var, &v);
			if (d->class != v) {
				d->class = v;
				changed = 1;
			}
		} else if (!strcmp(key, "RSSI") && t == DBUS_TYPE_INT16) {
			dbus_int16_t v;

			dbus_message_iter_get_basic(&var, &v);
			d->rssi = v;
		} else if (t == DBUS_TYPE_BOOLEAN) {
			dbus_bool_t v;
			int *slot = NULL;

			dbus_message_iter_get_basic(&var, &v);
			if (!strcmp(key, "Paired"))
				slot = &d->paired;
			else if (!strcmp(key, "Trusted"))
				slot = &d->trusted;
			else if (!strcmp(key, "Connected"))
				slot = &d->connected;
			if (slot && *slot != (int)v) {
				*slot = v;
				changed = 1;
			}
		} else if (!strcmp(key, "UUIDs") && t == DBUS_TYPE_ARRAY) {
			DBusMessageIter arr;

			dbus_message_iter_recurse(&var, &arr);
			while (dbus_message_iter_get_arg_type(&arr) ==
			       DBUS_TYPE_STRING) {
				const char *u;

				dbus_message_iter_get_basic(&arr, &u);
				if (!strcasecmp(u, A2DP_SINK_UUID))
					d->audio = 1;
				if (!strcasecmp(u, HID_UUID) ||
				    !strcasecmp(u, HOG_UUID))
					d->hid = 1;
				dbus_message_iter_next(&arr);
			}
		}
		dbus_message_iter_next(dict);
	}
	return changed;
}

static int adapter_props(DBusMessageIter *dict)
{
	int changed = 0;

	while (dbus_message_iter_get_arg_type(dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter kv, var;
		const char *key = NULL;

		dbus_message_iter_recurse(dict, &kv);
		dbus_message_iter_get_basic(&kv, &key);
		dbus_message_iter_next(&kv);
		dbus_message_iter_recurse(&kv, &var);
		if (key && dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_BOOLEAN) {
			dbus_bool_t v;

			dbus_message_iter_get_basic(&var, &v);
			if (!strcmp(key, "Powered") && powered != (int)v) {
				powered = v;
				changed = 1;
			} else if (!strcmp(key, "Discovering") &&
				   discovering != (int)v) {
				discovering = v;
				changed = 1;
			}
		}
		dbus_message_iter_next(dict);
	}
	return changed;
}

static void state_line(struct cli *c)
{
	const char *conn_addr = "-";
	int i;

	for (i = 0; i < MAXDEV; i++)
		if (devs[i].used && devs[i].connected) {
			conn_addr = devs[i].addr;
			break;
		}
	if (c)
		cli_send(c, "STATE powered=%d scanning=%d connected=%s",
			 powered, discovering, conn_addr);
	else
		event("STATE powered=%d scanning=%d connected=%s",
		      powered, discovering, conn_addr);
}

/* One object's interface dict: a{sa{sv}}. */
static void object_ifaces(const char *path, DBusMessageIter *ifaces, int announce)
{
	while (dbus_message_iter_get_arg_type(ifaces) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter kv, props;
		const char *iname = NULL;

		dbus_message_iter_recurse(ifaces, &kv);
		dbus_message_iter_get_basic(&kv, &iname);
		dbus_message_iter_next(&kv);
		if (iname && dbus_message_iter_get_arg_type(&kv) == DBUS_TYPE_ARRAY) {
			dbus_message_iter_recurse(&kv, &props);
			if (!strcmp(iname, "org.bluez.Device1")) {
				struct dev *d = dev_get(path);

				if (d) {
					dev_props(d, &props);
					if (announce && dev_interesting(d))
						dev_line(NULL, d);
				}
			} else if (!strcmp(iname, "org.bluez.Adapter1")) {
				snprintf(adapter, sizeof(adapter), "%s", path);
				adapter_props(&props);
			} else if (!strcmp(iname, "org.bluez.MediaTransport1")) {
				extern void transport_seen(const char *path);

				transport_seen(path);
			}
		}
		dbus_message_iter_next(ifaces);
	}
}

static void load_objects(void)
{
	DBusMessage *m, *r;
	DBusMessageIter it, objs;
	DBusError err;

	m = dbus_message_new_method_call("org.bluez", "/",
					 "org.freedesktop.DBus.ObjectManager",
					 "GetManagedObjects");
	if (!m)
		return;
	dbus_error_init(&err);
	r = dbus_connection_send_with_reply_and_block(conn, m, 5000, &err);
	dbus_message_unref(m);
	if (!r) {
		fprintf(stderr, "s31-bt: GetManagedObjects: %s\n",
			err.message ? err.message : "?");
		dbus_error_free(&err);
		return;
	}
	if (dbus_message_iter_init(r, &it) &&
	    dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
		dbus_message_iter_recurse(&it, &objs);
		while (dbus_message_iter_get_arg_type(&objs) == DBUS_TYPE_DICT_ENTRY) {
			DBusMessageIter ent, ifaces;
			const char *path = NULL;

			dbus_message_iter_recurse(&objs, &ent);
			dbus_message_iter_get_basic(&ent, &path);
			dbus_message_iter_next(&ent);
			if (path && dbus_message_iter_get_arg_type(&ent) == DBUS_TYPE_ARRAY) {
				dbus_message_iter_recurse(&ent, &ifaces);
				object_ifaces(path, &ifaces, 0);
			}
			dbus_message_iter_next(&objs);
		}
	}
	dbus_message_unref(r);
}

/* Set a boolean property on an org.bluez object. */
static int set_bool(const char *path, const char *iface, const char *prop, int val)
{
	DBusMessage *m, *r;
	DBusMessageIter it, var;
	DBusError err;
	dbus_bool_t v = val;
	int ok;

	m = dbus_message_new_method_call("org.bluez", path,
					 "org.freedesktop.DBus.Properties", "Set");
	if (!m)
		return -1;
	dbus_message_iter_init_append(m, &it);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &prop);
	dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "b", &var);
	dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &v);
	dbus_message_iter_close_container(&it, &var);
	dbus_error_init(&err);
	r = dbus_connection_send_with_reply_and_block(conn, m, 5000, &err);
	dbus_message_unref(m);
	ok = r != NULL;
	if (!r) {
		fprintf(stderr, "s31-bt: Set %s.%s: %s\n", iface, prop,
			err.message ? err.message : "?");
		dbus_error_free(&err);
	} else {
		dbus_message_unref(r);
	}
	return ok ? 0 : -1;
}

/*
 * Asynchronous method call with a completion callback. Pair and Connect
 * take seconds; the loop must keep serving the socket and the stream.
 */
struct pending {
	char addr[18];
	char what[12];		/* pair / connect / disconnect */
};

static void method_done(DBusPendingCall *pc, void *user)
{
	struct pending *p = user;
	DBusMessage *r = dbus_pending_call_steal_reply(pc);
	struct dev *d = dev_by_addr(p->addr);

	if (!r) {
		event("FAIL %s %s no-reply", p->addr, p->what);
	} else if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		const char *name = dbus_message_get_error_name(r);
		const char *reason = name ? strrchr(name, '.') : NULL;

		event("FAIL %s %s %s", p->addr, p->what,
		      reason ? reason + 1 : (name ? name : "?"));
	} else if (!strcmp(p->what, "pair")) {
		/* paired: trust it so it may reconnect on its own, then connect */
		if (d) {
			set_bool(d->path, "org.bluez.Device1", "Trusted", 1);
			d->trusted = 1;
			d->paired = 1;
		}
		event("PAIRED %s", p->addr);
		if (d) {
			extern void dev_call(struct dev *d, const char *what);

			dev_call(d, "connect");
		}
	} else if (!strcmp(p->what, "connect")) {
		event("CONNECTED %s", p->addr);
	} else if (!strcmp(p->what, "disconnect")) {
		event("DISCONNECTED %s", p->addr);
	}
	if (r)
		dbus_message_unref(r);
	dbus_pending_call_unref(pc);
	free(p);
}

void dev_call(struct dev *d, const char *what)
{
	DBusMessage *m;
	DBusPendingCall *pc = NULL;
	struct pending *p;
	const char *member = !strcmp(what, "pair") ? "Pair" :
			     !strcmp(what, "connect") ? "Connect" : "Disconnect";

	m = dbus_message_new_method_call("org.bluez", d->path,
					 "org.bluez.Device1", member);
	if (!m)
		return;
	p = calloc(1, sizeof(*p));
	snprintf(p->addr, sizeof(p->addr), "%s", d->addr);
	snprintf(p->what, sizeof(p->what), "%s", what);
	if (!dbus_connection_send_with_reply(conn, m, &pc, 90000) || !pc) {
		event("FAIL %s %s send", d->addr, what);
		free(p);
	} else {
		dbus_pending_call_set_notify(pc, method_done, p, NULL);
	}
	dbus_message_unref(m);
}

static void adapter_call(const char *member)
{
	DBusMessage *m, *r;
	DBusError err;

	m = dbus_message_new_method_call("org.bluez", adapter,
					 "org.bluez.Adapter1", member);
	if (!m)
		return;
	dbus_error_init(&err);
	r = dbus_connection_send_with_reply_and_block(conn, m, 5000, &err);
	dbus_message_unref(m);
	if (!r) {
		event("FAIL adapter %s %s", member,
		      err.message ? err.message : "?");
		dbus_error_free(&err);
		return;
	}
	dbus_message_unref(r);
}

/*
 * Which bearer to discover on. This board's LE pairing dies in the kernel's
 * SMP ("security requested but not available", recorded 2026-09-01 and
 * again 2026-09-04), and every device we care about - headphones, the
 * 8BitDo keyboard - is dual-mode and pairs fine over BR/EDR, which is how
 * the runbook did it in bluetoothctl (menu scan -> transport bredr). So
 * discovery is BR/EDR by default; bluez then knows the device on that
 * bearer only and Pair goes the classic route. "scan on le|auto" is there
 * for when SMP is fixed.
 */
static void set_discovery_transport(const char *transport)
{
	DBusMessage *m, *r;
	DBusMessageIter it, dict, e, var;
	DBusError err;
	const char *key = "Transport";

	m = dbus_message_new_method_call("org.bluez", adapter,
					 "org.bluez.Adapter1", "SetDiscoveryFilter");
	if (!m)
		return;
	dbus_message_iter_init_append(m, &it);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
	dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
	dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "s", &var);
	dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &transport);
	dbus_message_iter_close_container(&e, &var);
	dbus_message_iter_close_container(&dict, &e);
	dbus_message_iter_close_container(&it, &dict);
	dbus_error_init(&err);
	r = dbus_connection_send_with_reply_and_block(conn, m, 5000, &err);
	dbus_message_unref(m);
	if (!r) {
		event("FAIL adapter SetDiscoveryFilter %s", err.message ? err.message : "?");
		dbus_error_free(&err);
		return;
	}
	dbus_message_unref(r);
}

static void remove_device(struct dev *d)
{
	DBusMessage *m, *r;
	DBusError err;
	const char *path = d->path;

	m = dbus_message_new_method_call("org.bluez", adapter,
					 "org.bluez.Adapter1", "RemoveDevice");
	if (!m)
		return;
	dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &path,
				 DBUS_TYPE_INVALID);
	dbus_error_init(&err);
	r = dbus_connection_send_with_reply_and_block(conn, m, 5000, &err);
	dbus_message_unref(m);
	if (!r) {
		event("FAIL %s forget %s", d->addr,
		      err.message ? err.message : "?");
		dbus_error_free(&err);
		return;
	}
	dbus_message_unref(r);
	event("FORGOT %s", d->addr);
	d->used = 0;
}

/* ------------------------------------------------------------ agent */

/*
 * The pairing agent, capability DisplayYesNo: we have a screen, and the
 * user has whatever is being paired. Keyboards get DisplayPasskey (the
 * six digits go on the panel, the user types them on the keyboard);
 * headphones and mice get RequestConfirmation, answered by the desktop's
 * Yes/No or, if nobody answers in 30 s, rejected.
 */
static DBusMessage *confirm_msg;	/* the RequestConfirmation awaiting an answer */
static char confirm_addr[18];
static uint64_t confirm_at;

static const char *addr_of_path(const char *path)
{
	struct dev *d = dev_by_path(path);
	static char tmp[18];
	const char *p;

	if (d)
		return d->addr;
	/* .../dev_AA_BB_CC_DD_EE_FF */
	p = strrchr(path, '/');
	if (p && strlen(p) >= 22 && !strncmp(p, "/dev_", 5)) {
		int i;

		for (i = 0; i < 17; i++)
			tmp[i] = p[5 + i] == '_' ? ':' : p[5 + i];
		tmp[17] = 0;
		return tmp;
	}
	return "?";
}

static void confirm_finish(int yes)
{
	DBusMessage *reply;

	if (!confirm_msg)
		return;
	reply = yes ? dbus_message_new_method_return(confirm_msg) :
		      dbus_message_new_error(confirm_msg,
					     "org.bluez.Error.Rejected",
					     "rejected");
	dbus_connection_send(conn, reply, NULL);
	dbus_message_unref(reply);
	dbus_message_unref(confirm_msg);
	confirm_msg = NULL;
}

static DBusHandlerResult agent_message(DBusConnection *c, DBusMessage *msg,
				       void *user)
{
	const char *iface = dbus_message_get_interface(msg);
	const char *member = dbus_message_get_member(msg);
	DBusMessage *reply;

	(void)user;
	if (!iface || !member)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	if (!strcmp(iface, "org.freedesktop.DBus.Introspectable")) {
		static const char *xml =
			"<node><interface name='org.bluez.Agent1'>"
			"<method name='Release'/>"
			"<method name='RequestPinCode'><arg type='o' direction='in'/><arg type='s' direction='out'/></method>"
			"<method name='DisplayPinCode'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
			"<method name='RequestPasskey'><arg type='o' direction='in'/><arg type='u' direction='out'/></method>"
			"<method name='DisplayPasskey'><arg type='o' direction='in'/><arg type='u' direction='in'/><arg type='q' direction='in'/></method>"
			"<method name='RequestConfirmation'><arg type='o' direction='in'/><arg type='u' direction='in'/></method>"
			"<method name='RequestAuthorization'><arg type='o' direction='in'/></method>"
			"<method name='AuthorizeService'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
			"<method name='Cancel'/>"
			"</interface></node>";

		reply = dbus_message_new_method_return(msg);
		dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml,
					 DBUS_TYPE_INVALID);
		dbus_connection_send(c, reply, NULL);
		dbus_message_unref(reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (strcmp(iface, "org.bluez.Agent1"))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (!strcmp(member, "RequestPinCode")) {
		const char *path = NULL, *pin = "0000";

		dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
				      DBUS_TYPE_INVALID);
		event("PIN %s %s", path ? addr_of_path(path) : "?", pin);
		reply = dbus_message_new_method_return(msg);
		dbus_message_append_args(reply, DBUS_TYPE_STRING, &pin,
					 DBUS_TYPE_INVALID);
	} else if (!strcmp(member, "DisplayPinCode")) {
		const char *path = NULL, *pin = NULL;

		dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
				      DBUS_TYPE_STRING, &pin, DBUS_TYPE_INVALID);
		event("PIN %s %s", path ? addr_of_path(path) : "?", pin ? pin : "?");
		reply = dbus_message_new_method_return(msg);
	} else if (!strcmp(member, "RequestPasskey")) {
		/* keyboard-only capability would ask this; we display instead */
		const char *path = NULL;
		dbus_uint32_t key = 0;

		dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
				      DBUS_TYPE_INVALID);
		event("PASSKEY %s 000000 entered=0", path ? addr_of_path(path) : "?");
		reply = dbus_message_new_method_return(msg);
		dbus_message_append_args(reply, DBUS_TYPE_UINT32, &key,
					 DBUS_TYPE_INVALID);
	} else if (!strcmp(member, "DisplayPasskey")) {
		const char *path = NULL;
		dbus_uint32_t key = 0;
		dbus_uint16_t entered = 0;

		dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
				      DBUS_TYPE_UINT32, &key,
				      DBUS_TYPE_UINT16, &entered, DBUS_TYPE_INVALID);
		event("PASSKEY %s %06u entered=%u", path ? addr_of_path(path) : "?",
		      (unsigned)key, (unsigned)entered);
		reply = dbus_message_new_method_return(msg);
	} else if (!strcmp(member, "RequestConfirmation")) {
		const char *path = NULL;
		dbus_uint32_t key = 0;

		dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
				      DBUS_TYPE_UINT32, &key, DBUS_TYPE_INVALID);
		confirm_finish(0);		/* only one at a time */
		confirm_msg = dbus_message_ref(msg);
		confirm_at = now_us();
		snprintf(confirm_addr, sizeof(confirm_addr), "%s",
			 path ? addr_of_path(path) : "?");
		event("CONFIRM %s %06u", confirm_addr, (unsigned)key);
		return DBUS_HANDLER_RESULT_HANDLED;	/* answered later */
	} else if (!strcmp(member, "RequestAuthorization") ||
		   !strcmp(member, "AuthorizeService")) {
		/* a paired device coming back: always welcome */
		reply = dbus_message_new_method_return(msg);
	} else if (!strcmp(member, "Cancel")) {
		if (confirm_msg) {
			event("FAIL %s pair cancelled", confirm_addr);
			dbus_message_unref(confirm_msg);
			confirm_msg = NULL;
		}
		reply = dbus_message_new_method_return(msg);
	} else if (!strcmp(member, "Release")) {
		reply = dbus_message_new_method_return(msg);
	} else {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	dbus_connection_send(c, reply, NULL);
	dbus_message_unref(reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static int register_agent(void)
{
	DBusMessage *m, *r;
	DBusError err;
	const char *path = AGENT_PATH, *cap = "DisplayYesNo";

	m = dbus_message_new_method_call("org.bluez", "/org/bluez",
					 "org.bluez.AgentManager1", "RegisterAgent");
	dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &path,
				 DBUS_TYPE_STRING, &cap, DBUS_TYPE_INVALID);
	dbus_error_init(&err);
	r = dbus_connection_send_with_reply_and_block(conn, m, 5000, &err);
	dbus_message_unref(m);
	if (!r) {
		fprintf(stderr, "s31-bt: RegisterAgent: %s\n",
			err.message ? err.message : "?");
		dbus_error_free(&err);
		return -1;
	}
	dbus_message_unref(r);
	m = dbus_message_new_method_call("org.bluez", "/org/bluez",
					 "org.bluez.AgentManager1",
					 "RequestDefaultAgent");
	dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &path,
				 DBUS_TYPE_INVALID);
	r = dbus_connection_send_with_reply_and_block(conn, m, 5000, &err);
	dbus_message_unref(m);
	if (!r) {
		fprintf(stderr, "s31-bt: RequestDefaultAgent: %s\n",
			err.message ? err.message : "?");
		dbus_error_free(&err);
		return -1;
	}
	dbus_message_unref(r);
	return 0;
}

/* ------------------------------------------------------------ A2DP source */

struct sbc_caps {
	unsigned char chan_mode:4;
	unsigned char freq:4;
	unsigned char alloc:2;
	unsigned char subbands:2;
	unsigned char block_len:4;
	unsigned char min_bitpool;
	unsigned char max_bitpool;
} __attribute__((packed));

#define FREQ_44100	(1 << 1)
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
static struct sbc_caps chosen;

/* streaming state */
static struct {
	int active;
	int fd, wmtu;
	FILE *in;
	sbc_t sbc;
	unsigned char *pcm, *pkt;
	size_t codesize, framelen;
	int max_frames;
	uint64_t t0, sent_us, report_at;
	unsigned int seq, ts;
	long late_max, stalls;
	uint64_t t_enc, c_enc, t_wr;
	char inbuf[256 * 1024];
	char want[256];			/* file requested while no transport yet */
	uint64_t want_at;
} st;

#define S31_HOSTED_IOC_COEX _IOWR('S', 0x37, struct s31_hosted_coex_msg)
static void coex_hint(int streaming)
{
	struct s31_hosted_coex_msg m = {
		.op = streaming ? S31_HOSTED_COEX_BT_SET : S31_HOSTED_COEX_BT_CLEAR,
		.arg = 0x10,
	};
	int fd = open("/dev/esps0", O_RDWR);

	if (fd < 0)
		return;
	ioctl(fd, S31_HOSTED_IOC_COEX, &m);
	close(fd);
}

void transport_seen(const char *path)
{
	snprintf(transport_path, sizeof(transport_path), "%s", path);
	fprintf(stderr, "s31-bt: transport %s\n", path);
}

static int pick(unsigned int have, const int *order, int n)
{
	int i;

	for (i = 0; i < n; i++)
		if (have & order[i])
			return order[i];
	return 0;
}

static DBusHandlerResult ep_message(DBusConnection *c, DBusMessage *msg,
				    void *user)
{
	DBusMessage *reply = NULL;
	const char *iface = dbus_message_get_interface(msg);
	const char *member = dbus_message_get_member(msg);

	(void)user;
	if (!iface || !member)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	if (!strcmp(iface, "org.freedesktop.DBus.Introspectable")) {
		static const char *xml =
			"<node><interface name='org.bluez.MediaEndpoint1'>"
			"<method name='SetConfiguration'><arg type='o' direction='in'/><arg type='a{sv}' direction='in'/></method>"
			"<method name='SelectConfiguration'><arg type='ay' direction='in'/><arg type='ay' direction='out'/></method>"
			"<method name='ClearConfiguration'><arg type='o' direction='in'/></method>"
			"<method name='Release'/></interface></node>";

		reply = dbus_message_new_method_return(msg);
		dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml,
					 DBUS_TYPE_INVALID);
		dbus_connection_send(c, reply, NULL);
		dbus_message_unref(reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (strcmp(iface, "org.bluez.MediaEndpoint1"))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (!strcmp(member, "SelectConfiguration")) {
		unsigned char *caps = NULL;
		int n = 0;
		struct sbc_caps *peer, out;
		static const int freqs[] = { FREQ_44100, FREQ_48000 };
		static const int chans[] = { CHAN_JOINT, CHAN_STEREO, CHAN_DUAL, CHAN_MONO };
		static const int blocks[] = { BLOCK_16, BLOCK_12, BLOCK_8, BLOCK_4 };
		static const int subs[] = { SUB_8, SUB_4 };
		static const int allocs[] = { ALLOC_LOUDNESS, ALLOC_SNR };
		unsigned char *outp = (unsigned char *)&out;

		dbus_message_get_args(msg, NULL, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
				      &caps, &n, DBUS_TYPE_INVALID);
		if (n < (int)sizeof(struct sbc_caps)) {
			reply = dbus_message_new_error(msg,
				"org.bluez.Error.InvalidArguments", "short caps");
			dbus_connection_send(c, reply, NULL);
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
		reply = dbus_message_new_method_return(msg);
		dbus_message_append_args(reply, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
					 &outp, (int)sizeof(out), DBUS_TYPE_INVALID);
	} else if (!strcmp(member, "SetConfiguration")) {
		const char *path = NULL;
		DBusMessageIter it, dict;

		dbus_message_iter_init(msg, &it);
		if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH)
			dbus_message_iter_get_basic(&it, &path);
		if (path)
			transport_seen(path);
		if (dbus_message_iter_next(&it) &&
		    dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
			dbus_message_iter_recurse(&it, &dict);
			while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
				DBusMessageIter kv, var, arr;
				const char *key = NULL;

				dbus_message_iter_recurse(&dict, &kv);
				dbus_message_iter_get_basic(&kv, &key);
				dbus_message_iter_next(&kv);
				dbus_message_iter_recurse(&kv, &var);
				if (key && !strcmp(key, "Configuration") &&
				    dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_ARRAY) {
					unsigned char *cc = NULL;
					int cn = 0;

					dbus_message_iter_recurse(&var, &arr);
					dbus_message_iter_get_fixed_array(&arr, &cc, &cn);
					if (cn >= (int)sizeof(chosen))
						memcpy(&chosen, cc, sizeof(chosen));
				}
				dbus_message_iter_next(&dict);
			}
		}
		reply = dbus_message_new_method_return(msg);
	} else if (!strcmp(member, "ClearConfiguration") || !strcmp(member, "Release")) {
		transport_path[0] = 0;
		reply = dbus_message_new_method_return(msg);
	} else {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	dbus_connection_send(c, reply, NULL);
	dbus_message_unref(reply);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static int register_endpoint(void)
{
	DBusMessage *m, *r;
	DBusMessageIter it, dict, e, var, arr;
	DBusError err;
	struct sbc_caps caps;
	const char *uuid = A2DP_SOURCE_UUID;
	const char *k_uuid = "UUID", *k_codec = "Codec", *k_caps = "Capabilities";
	unsigned char codec = 0;
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

	m = dbus_message_new_method_call("org.bluez", adapter, "org.bluez.Media1",
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
		fprintf(stderr, "s31-bt: RegisterEndpoint: %s\n",
			err.message ? err.message : "?");
		dbus_error_free(&err);
		return -1;
	}
	dbus_message_unref(r);
	return 0;
}

static void transport_config(void)
{
	DBusMessage *m, *r;
	DBusMessageIter it, var, arr;
	DBusError err;
	const char *iface = "org.bluez.MediaTransport1", *prop = "Configuration";
	unsigned char *c = NULL;
	int n = 0;

	m = dbus_message_new_method_call("org.bluez", transport_path,
					 "org.freedesktop.DBus.Properties", "Get");
	if (!m)
		return;
	dbus_message_append_args(m, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING,
				 &prop, DBUS_TYPE_INVALID);
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
			if (n >= (int)sizeof(chosen))
				memcpy(&chosen, c, sizeof(chosen));
		}
	}
	dbus_message_unref(r);
}

static int transport_acquire(void)
{
	DBusMessage *m, *r;
	DBusError err;
	int fd = -1;
	dbus_uint16_t rmtu = 0, wmtu = 0;

	if (!transport_path[0])
		return -1;
	m = dbus_message_new_method_call("org.bluez", transport_path,
					 "org.bluez.MediaTransport1", "Acquire");
	if (!m)
		return -1;
	dbus_error_init(&err);
	r = dbus_connection_send_with_reply_and_block(conn, m, 10000, &err);
	dbus_message_unref(m);
	if (!r) {
		fprintf(stderr, "s31-bt: Acquire: %s\n", err.message ? err.message : "?");
		dbus_error_free(&err);
		return -1;
	}
	if (!dbus_message_get_args(r, &err, DBUS_TYPE_UNIX_FD, &fd,
				   DBUS_TYPE_UINT16, &rmtu, DBUS_TYPE_UINT16, &wmtu,
				   DBUS_TYPE_INVALID)) {
		dbus_error_free(&err);
		dbus_message_unref(r);
		return -1;
	}
	dbus_message_unref(r);
	st.wmtu = wmtu;
	return fd;
}

static void transport_release(void)
{
	DBusMessage *m, *r;
	DBusError err;

	if (!transport_path[0])
		return;
	m = dbus_message_new_method_call("org.bluez", transport_path,
					 "org.bluez.MediaTransport1", "Release");
	if (!m)
		return;
	dbus_error_init(&err);
	r = dbus_connection_send_with_reply_and_block(conn, m, 5000, &err);
	dbus_message_unref(m);
	if (r)
		dbus_message_unref(r);
	else
		dbus_error_free(&err);
}

static int sbc_freq_hz(unsigned int f)
{
	return (f & FREQ_44100) ? 44100 : 48000;
}

static void stream_stop(const char *why)
{
	if (!st.active)
		return;
	coex_hint(0);
	sbc_finish(&st.sbc);
	close(st.fd);
	if (st.in)
		fclose(st.in);
	free(st.pcm);
	free(st.pkt);
	memset(&st, 0, sizeof(st));
	transport_release();
	event("STOPPED %s", why);
}

static int stream_start(const char *file)
{
	int fd;

	if (!transport_path[0])
		return -1;
	transport_config();
	fd = transport_acquire();
	if (fd < 0)
		return -1;
	st.in = fopen(file, "rb");
	if (!st.in) {
		close(fd);
		transport_release();
		return -1;
	}
	setvbuf(st.in, st.inbuf, _IOFBF, sizeof(st.inbuf));
	fseek(st.in, 44, SEEK_SET);
	st.fd = fd;
	sbc_init_a2dp(&st.sbc, 0L, &chosen, sizeof(chosen));
	st.sbc.endian = SBC_LE;
	st.codesize = sbc_get_codesize(&st.sbc);
	st.framelen = sbc_get_frame_length(&st.sbc);
	st.pcm = malloc(st.codesize);
	st.pkt = malloc(st.wmtu > 0 ? (size_t)st.wmtu : 1024);
	st.max_frames = 15;
	st.t0 = now_us();
	st.active = 1;
	coex_hint(1);
	event("PLAY %s", file);
	return 0;
}

/* One packet: returns the microseconds until the next one is due. */
static int stream_step(void)
{
	size_t off = 13, nframes = 0;
	ssize_t w;
	uint64_t tenc0 = now_us(), cenc0 = cpu_us();

	while (off + st.framelen <= (size_t)st.wmtu && nframes < (size_t)st.max_frames) {
		ssize_t enc, wrote = 0;
		size_t rd = fread(st.pcm, 1, st.codesize, st.in);

		if (rd < st.codesize)
			break;
		enc = sbc_encode(&st.sbc, st.pcm, st.codesize, st.pkt + off,
				 (size_t)st.wmtu - off, &wrote);
		if (enc <= 0 || wrote <= 0)
			break;
		off += (size_t)wrote;
		nframes++;
		st.ts += (unsigned int)(st.codesize / 4);
	}
	if (!nframes) {
		stream_stop("end");
		return 100000;
	}
	st.t_enc += now_us() - tenc0;
	st.c_enc += cpu_us() - cenc0;
	st.pkt[0] = 0x80; st.pkt[1] = 96;
	st.pkt[2] = (st.seq >> 8) & 0xff; st.pkt[3] = st.seq & 0xff;
	st.pkt[4] = (st.ts >> 24) & 0xff; st.pkt[5] = (st.ts >> 16) & 0xff;
	st.pkt[6] = (st.ts >> 8) & 0xff;  st.pkt[7] = st.ts & 0xff;
	st.pkt[8] = 0; st.pkt[9] = 0; st.pkt[10] = 0; st.pkt[11] = 1;
	st.pkt[12] = (unsigned char)nframes;
	st.seq++;
	{
		uint64_t tw0 = now_us();

		w = write(st.fd, st.pkt, off);
		st.t_wr += now_us() - tw0;
	}
	if (w < 0) {
		if (errno == EAGAIN) {
			st.stalls++;
			return 5000;
		}
		if (errno == EMSGSIZE && st.max_frames > 1) {
			st.max_frames /= 2;
			return 0;
		}
		stream_stop("write error");
		return 100000;
	}
	st.sent_us += (uint64_t)nframes * (st.codesize / 4) * 1000000ull /
		      (uint64_t)sbc_freq_hz(chosen.freq);
	{
		int64_t ahead = (int64_t)st.sent_us - (int64_t)(now_us() - st.t0);

		if (ahead < 0) {
			long late = (long)(-ahead / 1000);

			if (late > st.late_max)
				st.late_max = late;
		}
		if (now_us() - st.t0 > st.report_at) {
			st.report_at += 5000000ull;
			fprintf(stderr, "s31-bt: %us %u pkts late_max=%ld stalls=%ld "
				"| per pkt encode=%lluus(cpu %llu) write=%lluus\n",
				(unsigned)((now_us() - st.t0) / 1000000), st.seq,
				st.late_max, st.stalls,
				(unsigned long long)(st.seq ? st.t_enc / st.seq : 0),
				(unsigned long long)(st.seq ? st.c_enc / st.seq : 0),
				(unsigned long long)(st.seq ? st.t_wr / st.seq : 0));
			st.late_max = 0;
		}
		if (ahead > 20000)
			return (int)(ahead - 10000);
	}
	return 0;
}

/* ------------------------------------------------------------ signals */

static DBusHandlerResult signal_filter(DBusConnection *c, DBusMessage *msg,
				       void *user)
{
	DBusMessageIter it;

	(void)c; (void)user;
	if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_SIGNAL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (dbus_message_is_signal(msg, "org.freedesktop.DBus.ObjectManager",
				   "InterfacesAdded")) {
		const char *path = NULL;

		if (dbus_message_iter_init(msg, &it) &&
		    dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH) {
			DBusMessageIter ifaces;

			dbus_message_iter_get_basic(&it, &path);
			dbus_message_iter_next(&it);
			if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
				dbus_message_iter_recurse(&it, &ifaces);
				object_ifaces(path, &ifaces, 1);
			}
		}
	} else if (dbus_message_is_signal(msg, "org.freedesktop.DBus.ObjectManager",
					  "InterfacesRemoved")) {
		const char *path = NULL;

		if (dbus_message_iter_init(msg, &it) &&
		    dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH) {
			DBusMessageIter arr;

			dbus_message_iter_get_basic(&it, &path);
			dbus_message_iter_next(&it);
			dbus_message_iter_recurse(&it, &arr);
			while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
				const char *iname;
				struct dev *d;

				dbus_message_iter_get_basic(&arr, &iname);
				if (!strcmp(iname, "org.bluez.Device1") &&
				    (d = dev_by_path(path))) {
					event("GONE %s", d->addr);
					d->used = 0;
				} else if (!strcmp(iname, "org.bluez.MediaTransport1") &&
					   !strcmp(transport_path, path)) {
					transport_path[0] = 0;
					if (st.active)
						stream_stop("transport gone");
				}
				dbus_message_iter_next(&arr);
			}
		}
	} else if (dbus_message_is_signal(msg, "org.freedesktop.DBus.Properties",
					  "PropertiesChanged")) {
		const char *path = dbus_message_get_path(msg);
		const char *iname = NULL;

		if (path && dbus_message_iter_init(msg, &it) &&
		    dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
			DBusMessageIter dict;

			dbus_message_iter_get_basic(&it, &iname);
			dbus_message_iter_next(&it);
			if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
				dbus_message_iter_recurse(&it, &dict);
				if (!strcmp(iname, "org.bluez.Device1")) {
					struct dev *d = dev_get(path);

					if (d && dev_props(d, &dict) && dev_interesting(d))
						dev_line(NULL, d);
				} else if (!strcmp(iname, "org.bluez.Adapter1")) {
					if (adapter_props(&dict))
						state_line(NULL);
				}
			}
		}
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ------------------------------------------------------------ commands */

static void cmd(struct cli *c, char *line)
{
	char *a = strtok(line, " \t\r\n");
	char *b = strtok(NULL, " \t\r\n");
	char *d3 = strtok(NULL, " \t\r\n");
	struct dev *d = NULL;
	int i;

	if (!a)
		return;
	if (b && strchr(b, ':'))
		d = dev_by_addr(b);
	if (!strcmp(a, "list")) {
		for (i = 0; i < MAXDEV; i++)
			if (devs[i].used && dev_interesting(&devs[i]))
				dev_line(c, &devs[i]);
		cli_send(c, "OK");
	} else if (!strcmp(a, "status")) {
		state_line(c);
		cli_send(c, "OK");
	} else if (!strcmp(a, "monitor")) {
		c->monitor = 1;
		cli_send(c, "OK");
	} else if (!strcmp(a, "scan")) {
		int on = b && !strcmp(b, "on");

		if (on && !discovering) {
			/* forget stale discovery results before a new scan */
			for (i = 0; i < MAXDEV; i++)
				if (devs[i].used && !devs[i].paired)
					devs[i].rssi = 0;
			set_discovery_transport(d3 && (!strcmp(d3, "le") ||
						       !strcmp(d3, "auto")) ?
						d3 : "bredr");
			adapter_call("StartDiscovery");
			scan_stop_at = (int)(now_us() / 1000000) + 45;
		} else if (!on && discovering) {
			adapter_call("StopDiscovery");
			scan_stop_at = 0;
		}
		cli_send(c, "OK");
	} else if (!strcmp(a, "pair") || !strcmp(a, "connect") ||
		   !strcmp(a, "disconnect")) {
		if (!d) {
			cli_send(c, "ERR unknown device");
			return;
		}
		if (!strcmp(a, "pair"))
			event("PAIRING %s", d->addr);
		dev_call(d, a);
		cli_send(c, "OK");
	} else if (!strcmp(a, "forget")) {
		if (!d) {
			cli_send(c, "ERR unknown device");
			return;
		}
		remove_device(d);
		cli_send(c, "OK");
	} else if (!strcmp(a, "confirm")) {
		if (!confirm_msg) {
			cli_send(c, "ERR nothing to confirm");
			return;
		}
		confirm_finish(d3 && !strcmp(d3, "yes"));
		cli_send(c, "OK");
	} else if (!strcmp(a, "power")) {
		set_bool(adapter, "org.bluez.Adapter1", "Powered",
			 b && !strcmp(b, "on"));
		cli_send(c, "OK");
	} else if (!strcmp(a, "play")) {
		if (!b) {
			cli_send(c, "ERR file?");
			return;
		}
		if (st.active)
			stream_stop("replaced");
		snprintf(st.want, sizeof(st.want), "%s", b);
		st.want_at = now_us();
		cli_send(c, "OK");
	} else if (!strcmp(a, "stop")) {
		st.want[0] = 0;
		stream_stop("stop");
		cli_send(c, "OK");
	} else {
		cli_send(c, "ERR unknown command");
	}
}

static void ctl_accept(void)
{
	int fd = accept(ctl_fd, NULL, NULL);
	int i;

	if (fd < 0)
		return;
	for (i = 0; i < MAXCLI; i++)
		if (clis[i].fd < 0) {
			clis[i].fd = fd;
			clis[i].len = 0;
			clis[i].monitor = 0;
			return;
		}
	close(fd);
}

static void ctl_read(struct cli *c)
{
	ssize_t n = read(c->fd, c->buf + c->len, sizeof(c->buf) - 1 - c->len);
	char *nl;

	if (n <= 0) {
		close(c->fd);
		c->fd = -1;
		return;
	}
	c->len += (int)n;
	c->buf[c->len] = 0;
	while ((nl = strchr(c->buf, '\n'))) {
		*nl = 0;
		cmd(c, c->buf);
		if (c->fd < 0)
			return;
		c->len -= (int)(nl + 1 - c->buf);
		memmove(c->buf, nl + 1, (size_t)c->len + 1);
	}
	if (c->len >= (int)sizeof(c->buf) - 1)
		c->len = 0;
}

/* ------------------------------------------------------------ client mode */

static int client(int argc, char **argv)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM, 0), i, quiet = 0;
	char line[512] = "";
	struct pollfd p;

	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", CTL_PATH);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("s31-bt: connect");
		return 1;
	}
	if (!strcmp(argv[1], "monitor"))
		snprintf(line, sizeof(line), "monitor\n");
	else
		for (i = 2; i < argc; i++) {
			strncat(line, argv[i], sizeof(line) - strlen(line) - 2);
			strncat(line, i + 1 < argc ? " " : "\n",
				sizeof(line) - strlen(line) - 1);
		}
	if (write(fd, line, strlen(line)) < 0)
		return 1;
	p.fd = fd;
	p.events = POLLIN;
	for (;;) {
		char buf[512];
		ssize_t n;
		int t = !strcmp(argv[1], "monitor") ? -1 :
			(argc > 2 && !strcmp(argv[2], "list")) ? 500 : 3000;

		if (poll(&p, 1, t) <= 0) {
			if (++quiet > 0)
				break;
			continue;
		}
		n = read(fd, buf, sizeof(buf) - 1);
		if (n <= 0)
			break;
		buf[n] = 0;
		fputs(buf, stdout);
		fflush(stdout);
		if (strstr(buf, "\nOK\n") || !strncmp(buf, "OK\n", 3) ||
		    strstr(buf, "\nERR ") || !strncmp(buf, "ERR ", 4))
			if (strcmp(argv[1], "monitor") &&
			    !(argc > 2 && !strcmp(argv[2], "pair")))
				break;
	}
	close(fd);
	return 0;
}

/* ------------------------------------------------------------ main */

int main(int argc, char **argv)
{
	DBusError err;
	DBusObjectPathVTable agent_vt = { .message_function = agent_message };
	DBusObjectPathVTable ep_vt = { .message_function = ep_message };
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int dbus_fd = -1, i;

	if (argc > 1 && (!strcmp(argv[1], "cmd") || !strcmp(argv[1], "monitor")))
		return client(argc, argv);

	signal(SIGPIPE, SIG_IGN);
	for (i = 0; i < MAXCLI; i++)
		clis[i].fd = -1;
	{
		/* Woken promptly, never running long: a real-time job on one core. */
		struct sched_param sp = { .sched_priority = 5 };

		syscall(SYS_sched_setscheduler, 0, SCHED_FIFO, &sp);
		mlockall(MCL_CURRENT | MCL_FUTURE);
	}

	dbus_error_init(&err);
	conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
	if (!conn) {
		fprintf(stderr, "s31-bt: system bus: %s\n", err.message ? err.message : "?");
		return 1;
	}
	dbus_connection_set_exit_on_disconnect(conn, FALSE);
	dbus_bus_add_match(conn, "type='signal',sender='org.bluez',"
			   "interface='org.freedesktop.DBus.ObjectManager'", NULL);
	dbus_bus_add_match(conn, "type='signal',sender='org.bluez',"
			   "interface='org.freedesktop.DBus.Properties',"
			   "member='PropertiesChanged'", NULL);
	dbus_connection_add_filter(conn, signal_filter, NULL, NULL);
	dbus_connection_register_object_path(conn, AGENT_PATH, &agent_vt, NULL);
	dbus_connection_register_object_path(conn, EP_PATH, &ep_vt, NULL);

	load_objects();
	if (!powered)
		set_bool(adapter, "org.bluez.Adapter1", "Powered", 1);
	if (register_agent())
		fprintf(stderr, "s31-bt: no agent - pairing prompts will fail\n");
	if (register_endpoint())
		fprintf(stderr, "s31-bt: no A2DP endpoint - audio sinks will not stream\n");
	dbus_connection_get_unix_fd(conn, &dbus_fd);

	unlink(CTL_PATH);
	ctl_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", CTL_PATH);
	if (bind(ctl_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(ctl_fd, 4) < 0) {
		perror("s31-bt: control socket");
		return 1;
	}
	fprintf(stderr, "s31-bt: up on %s, adapter %s powered=%d, %d devices known\n",
		CTL_PATH, adapter, powered,
		({ int n = 0; for (i = 0; i < MAXDEV; i++) n += devs[i].used; n; }));
	state_line(NULL);

	for (;;) {
		struct pollfd p[2 + MAXCLI];
		int n = 0, timeout = 200, r;

		p[n].fd = dbus_fd; p[n++].events = POLLIN;
		p[n].fd = ctl_fd; p[n++].events = POLLIN;
		for (i = 0; i < MAXCLI; i++)
			if (clis[i].fd >= 0) {
				p[n].fd = clis[i].fd;
				p[n++].events = POLLIN;
			}
		if (st.active) {
			int wait = stream_step();

			timeout = wait / 1000 < timeout ? wait / 1000 : timeout;
		}
		r = poll(p, n, timeout);
		if (r > 0) {
			int k = 2;

			if (p[0].revents)
				dbus_connection_read_write(conn, 0);
			if (p[1].revents)
				ctl_accept();
			for (i = 0; i < MAXCLI; i++)
				if (clis[i].fd >= 0 && k < n) {
					if (p[k].revents)
						ctl_read(&clis[i]);
					k++;
				}
		}
		/* dispatch whatever arrived, and drive pending replies */
		dbus_connection_read_write(conn, 0);
		while (dbus_connection_dispatch(conn) == DBUS_DISPATCH_DATA_REMAINS)
			;
		/* housekeeping */
		if (confirm_msg && now_us() - confirm_at > 30000000ull) {
			event("FAIL %s pair confirmation timed out", confirm_addr);
			confirm_finish(0);
		}
		if (scan_stop_at && (int)(now_us() / 1000000) >= scan_stop_at) {
			scan_stop_at = 0;
			if (discovering)
				adapter_call("StopDiscovery");
		}
		if (st.want[0] && !st.active) {
			/* a play request waits for the transport, at most 30 s */
			if (transport_path[0]) {
				if (stream_start(st.want) < 0)
					event("FAIL play %s", st.want);
				st.want[0] = 0;
			} else if (now_us() - st.want_at > 30000000ull) {
				event("FAIL play no transport");
				st.want[0] = 0;
			}
		}
	}
	return 0;
}
