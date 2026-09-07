/* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0 */
/*
 * ESP32-S31 in-package ESP-Hosted transport.
 *
 * Both harts access this layout at the same HP SRAM physical address.  Linux
 * maps it as device memory.  ESP-IDF accesses can remain in the shared L1
 * D-cache, so hart0 maintains lines when exchanging ownership with Linux.
 * Producer and consumer words remain 64-byte separated so each side owns a
 * distinct synchronization line and the ABI stays safe if a cached alias is
 * introduced later.
 */
#ifndef S31_HOSTED_SRAM_H
#define S31_HOSTED_SRAM_H

#include "s31_memory_layout.h"

#ifdef __KERNEL__
#include <linux/types.h>
typedef u8 s31_u8;
typedef u16 s31_u16;
typedef u32 s31_u32;
typedef u64 s31_u64;
typedef s8 s31_s8;
#else
#include <stdint.h>
typedef uint8_t s31_u8;
typedef uint16_t s31_u16;
typedef uint32_t s31_u32;
typedef uint64_t s31_u64;
typedef int8_t s31_s8;
#endif

#define S31_HOSTED_SRAM_BASE		S31_HP_SHARED_BASE
#define S31_HOSTED_CTRL_SIZE		0x00001000U
#define S31_HOSTED_SLOT_SIZE		1600U
#define S31_HOSTED_SLOT_COUNT		8U
#define S31_HOSTED_SLOT_DATA_SIZE	(S31_HOSTED_SLOT_SIZE - 8U)
#define S31_HOSTED_H0_TO_H1_OFFSET	S31_HOSTED_CTRL_SIZE
#define S31_HOSTED_H1_TO_H0_OFFSET	\
	(S31_HOSTED_H0_TO_H1_OFFSET + S31_HOSTED_SLOT_COUNT * S31_HOSTED_SLOT_SIZE)

#define S31_HOSTED_MAGIC		0x53334846U /* "FH3S" little endian */
/*
 * Bumped to 2 for the cfg80211 station operations (scan/connect/disconnect,
 * link events and station info). Both harts validate this at probe, so the
 * loader and the kernel must be flashed together when it changes.
 */
#define S31_HOSTED_ABI_VERSION		2U

#define S31_HOSTED_H0_READY		(1U << 0)
#define S31_HOSTED_H1_READY		(1U << 1)

#define S31_HOSTED_FEAT_WIFI		(1U << 0)
#define S31_HOSTED_FEAT_BT		(1U << 1)
#define S31_HOSTED_FEAT_COEX		(1U << 2)

/* Values match esp_hosted_fg/common/include/adapter.h. */
enum s31_hosted_if_type {
	S31_HOSTED_STA_IF = 0,
	S31_HOSTED_AP_IF,
	S31_HOSTED_SERIAL_IF,
	S31_HOSTED_HCI_IF,
	S31_HOSTED_PRIV_IF,
	S31_HOSTED_TEST_IF,
	S31_HOSTED_MAX_IF,
};

enum s31_hosted_control_type {
	S31_HOSTED_CTRL_PING = 1,
	S31_HOSTED_CTRL_PONG,
	S31_HOSTED_CTRL_LINK,
	S31_HOSTED_CTRL_RADIO_READY,
	/* OpenSBI asks hart0 to enter IDF's chip-wide deep-sleep path. */
	S31_HOSTED_CTRL_POWER_OFF,
	/* OpenSBI asks hart0 to execute IDF's complete restart sequence. */
	S31_HOSTED_CTRL_RESTART,
	/* Linux and FreeRTOS compare independent clocks over one interval. */
	S31_HOSTED_CTRL_CLOCK_START,
	S31_HOSTED_CTRL_CLOCK_STOP,
	/* Linux requests/retrieves FreeRTOS HP-SRAM heap statistics. */
	S31_HOSTED_CTRL_MEM_STATS_REQUEST,
	S31_HOSTED_CTRL_MEM_STATS_RESPONSE,
	S31_HOSTED_CTRL_WIFI_SLOT_SET,
	S31_HOSTED_CTRL_WIFI_SLOT_GET,
	S31_HOSTED_CTRL_WIFI_STATE_SET,
	S31_HOSTED_CTRL_WIFI_STATE_GET,
	S31_HOSTED_CTRL_WIFI_SLOT_SET_RESPONSE,
	S31_HOSTED_CTRL_WIFI_SLOT_GET_RESPONSE,
	S31_HOSTED_CTRL_WIFI_STATE_SET_RESPONSE,
	S31_HOSTED_CTRL_WIFI_STATE_GET_RESPONSE,
	/* Linux asks the IDF-owned PM policy to raise the CPU frequency floor. */
	S31_HOSTED_CTRL_CPU_FREQ_SET,
	S31_HOSTED_CTRL_CPU_FREQ_SET_RESPONSE,
	/*
	 * cfg80211 station operations. Linux drives association directly
	 * rather than relying on hart0's stored slot profiles, so that
	 * wpa_supplicant and nl80211 see a normal FullMAC device. All of
	 * these carry a struct s31_hosted_wifi_msg over the private
	 * interface; SCAN_RESULT, SCAN_DONE and LINK_EVENT are asynchronous
	 * hart0 -> Linux notifications with no matching request.
	 */
	S31_HOSTED_CTRL_WIFI_SCAN,
	S31_HOSTED_CTRL_WIFI_SCAN_RESPONSE,
	S31_HOSTED_CTRL_WIFI_SCAN_RESULT,
	S31_HOSTED_CTRL_WIFI_SCAN_DONE,
	S31_HOSTED_CTRL_WIFI_CONNECT,
	S31_HOSTED_CTRL_WIFI_CONNECT_RESPONSE,
	S31_HOSTED_CTRL_WIFI_DISCONNECT,
	S31_HOSTED_CTRL_WIFI_DISCONNECT_RESPONSE,
	S31_HOSTED_CTRL_WIFI_LINK_EVENT,
	S31_HOSTED_CTRL_WIFI_STATION_INFO,
	S31_HOSTED_CTRL_WIFI_STATION_INFO_RESPONSE,
	/*
	 * Radio coexistence knobs, applied on hart0. Bluetooth's host stack is
	 * on Linux, so hart0 cannot otherwise learn that A2DP is streaming, and
	 * every coexistence hypothesis used to cost a loader reflash.
	 */
	S31_HOSTED_CTRL_COEX_SET,
	S31_HOSTED_CTRL_COEX_SET_RESPONSE,
};

enum s31_hosted_link_state {
	S31_HOSTED_LINK_DOWN = 0,
	S31_HOSTED_LINK_UP = 1,
};

/*
 * Wire header from ESP-Hosted-FG.  SRAM slots contain this header followed by
 * the payload at @offset.  Both harts are RV32 little-endian.
 */
struct s31_esp_payload_header {
	s31_u8 if_type:4;
	s31_u8 if_num:4;
	s31_u8 flags;
	s31_u16 len;
	s31_u16 offset;
	s31_u16 checksum;
	s31_u16 seq_num;
	s31_u8 reserved2;
	union {
		s31_u8 reserved3;
		s31_u8 hci_pkt_type;
		s31_u8 priv_pkt_type;
	};
} __attribute__((packed));

struct s31_hosted_control_msg {
	s31_u8 type;
	s31_u8 value;
	s31_u16 length;
	s31_u32 generation;
	s31_u8 data[16];
} __attribute__((packed));

/* CLOCK_START/STOP data payload.  All fields use little-endian wire order. */
struct s31_hosted_clock_stamp {
	s31_u32 cookie;
	s31_u32 duration_sec;
	s31_u64 freertos_us;
} __attribute__((packed));

/* CPU_FREQ_SET data payload. target_mhz is the PM floor requested by Linux;
 * actual_mhz is the current FreeRTOS/ESP-PM-selected frequency. */
/* COEX_SET payload: exactly the 16 data bytes of a control message. */
enum s31_hosted_coex_op {
	S31_HOSTED_COEX_GET = 0,		/* result = scheme period, arg = interval */
	S31_HOSTED_COEX_PREFER = 1,		/* arg: 0 wifi, 1 bt, 2 balance */
	S31_HOSTED_COEX_BT_SET = 2,		/* arg: ESP_COEX_BT_ST_* bits */
	S31_HOSTED_COEX_BT_CLEAR = 3,
	S31_HOSTED_COEX_INTERVAL = 4,		/* arg: coex scheme interval */
	S31_HOSTED_COEX_FLEX_PERIOD = 5,	/* arg: flexible period */
	S31_HOSTED_COEX_WIFI_SET = 6,
	S31_HOSTED_COEX_WIFI_CLEAR = 7,
	/*
	 * Not coexistence at all - it rides this message because this is the
	 * one runtime channel Linux already has to hart0, and adding a whole
	 * control type for two integers is not worth a protocol revision.
	 *
	 * arg: (tag << 8) | level, where level is an ESP_LOG_* value
	 * (0 NONE, 1 ERROR, 2 WARN, 3 INFO, 4 DEBUG, 5 VERBOSE) and tag is
	 * S31_HOSTED_LOGTAG_*. Tags are enumerated rather than sent as
	 * strings so the payload stays fixed-size.
	 *
	 * WHY: hart0 logs at INFO and shares the 1 Mbps console with Linux.
	 * The Wi-Fi block alone emits an ADDBA line per stream setup, and a
	 * console recording of a Doom run came back containing NOTHING BUT
	 * that chatter - the spam is loud enough to be the only thing in a
	 * capture taken to diagnose a crash.
	 */
	S31_HOSTED_COEX_LOGLEVEL = 8,	/* arg: (tag << 8) | level */
};

enum s31_hosted_logtag {
	S31_HOSTED_LOGTAG_ALL = 0,	/* the "*" default level */
	S31_HOSTED_LOGTAG_WIFI = 1,	/* "wifi" - the ADDBA chatter */
	S31_HOSTED_LOGTAG_COEX = 2,	/* "coexist" */
	S31_HOSTED_LOGTAG_PM = 3,	/* "pm" */
	S31_HOSTED_LOGTAG_HOSTED = 4,	/* our own transport */
};

struct s31_hosted_coex_msg {
	s31_u32 op;
	s31_u32 arg;
	s31_u32 status;		/* esp_err_t, 0 on success */
	s31_u32 result;
} __attribute__((packed));

struct s31_hosted_cpu_freq_msg {
	s31_u32 target_mhz;
	s31_u32 actual_mhz;
	s31_u32 status;
	s31_u32 reserved;
} __attribute__((packed));

/* MEM_STATS_RESPONSE payload. All fields use little-endian wire order. */
struct s31_hosted_mem_stats {
	s31_u32 total_bytes;
	s31_u32 free_bytes;
	s31_u32 minimum_free_bytes;
	s31_u32 largest_free_block;
} __attribute__((packed));

#define S31_HOSTED_WIFI_SLOT_COUNT	3U
#define S31_HOSTED_WIFI_SSID_MAX	32U
#define S31_HOSTED_WIFI_PASSWORD_MAX	64U
/* Sized for the largest payload below, struct s31_hosted_wifi_connect. */
#define S31_HOSTED_WIFI_MSG_DATA_SIZE	128U

/* Wire-stable authentication modes, mapped from/to ESP-IDF's wifi_auth_mode_t. */
enum s31_hosted_wifi_auth {
	S31_HOSTED_WIFI_AUTH_OPEN = 0,
	S31_HOSTED_WIFI_AUTH_WEP,
	S31_HOSTED_WIFI_AUTH_WPA_PSK,
	S31_HOSTED_WIFI_AUTH_WPA2_PSK,
	S31_HOSTED_WIFI_AUTH_WPA_WPA2_PSK,
	S31_HOSTED_WIFI_AUTH_WPA3_PSK,
	S31_HOSTED_WIFI_AUTH_WPA2_WPA3_PSK,
	S31_HOSTED_WIFI_AUTH_WAPI_PSK,
	S31_HOSTED_WIFI_AUTH_ENTERPRISE,
	S31_HOSTED_WIFI_AUTH_UNKNOWN = 0xff,
};

/* s31_hosted_wifi_connect.flags */
#define S31_HOSTED_WIFI_CONNECT_F_BSSID	0x01U

/* Persistent station profile. Empty/invalid profiles are skipped. */
struct s31_hosted_wifi_slot {
	s31_u8 valid;
	s31_u8 priority;
	s31_u8 ssid_len;
	s31_u8 password_len;
	s31_u8 ssid[S31_HOSTED_WIFI_SSID_MAX];
	s31_u8 password[S31_HOSTED_WIFI_PASSWORD_MAX];
} __attribute__((packed));

/* Persistent policy plus the current runtime slot reported by GET. */
struct s31_hosted_wifi_state {
	s31_u8 enabled;
	s31_u8 auto_connect;
	s31_u16 scan_interval_sec;
	s31_u8 active_slot;
	s31_u8 connected_slot;
	s31_u8 reserved[10];
} __attribute__((packed));

/* Private transport request/response. Data contains one slot or one state. */
struct s31_hosted_wifi_msg {
	s31_u8 type;
	s31_u8 slot;
	s31_u16 length;
	s31_u32 generation;
	s31_u32 status;
	s31_u8 data[S31_HOSTED_WIFI_MSG_DATA_SIZE];
} __attribute__((packed));

/*
 * One scan result, reported by S31_HOSTED_CTRL_WIFI_SCAN_RESULT. hart0 emits
 * one message per BSS as esp_wifi returns them, then a SCAN_DONE.
 */
struct s31_hosted_wifi_bss {
	s31_u8 bssid[6];
	s31_u16 capability;
	s31_u8 ssid_len;
	s31_u8 channel;
	s31_s8 rssi;
	s31_u8 auth_mode;		/* enum s31_hosted_wifi_auth */
	s31_u8 reserved[2];
	s31_u8 ssid[S31_HOSTED_WIFI_SSID_MAX];
} __attribute__((packed));

/*
 * S31_HOSTED_CTRL_WIFI_SCAN payload. An empty ssid requests the usual sweep;
 * a non-empty one asks for a directed probe, which is the only way a hidden
 * AP is ever found.
 */
struct s31_hosted_wifi_scan_req {
	s31_u8 ssid_len;
	s31_u8 channel;			/* 0 scans every channel */
	s31_u8 reserved[2];
	s31_u8 ssid[S31_HOSTED_WIFI_SSID_MAX];
} __attribute__((packed));

/* S31_HOSTED_CTRL_WIFI_CONNECT payload. */
struct s31_hosted_wifi_connect {
	s31_u8 ssid_len;
	s31_u8 password_len;
	s31_u8 channel;			/* 0 selects any channel */
	s31_u8 flags;			/* S31_HOSTED_WIFI_CONNECT_F_* */
	s31_u8 bssid[6];
	s31_u8 reserved[2];
	s31_u8 ssid[S31_HOSTED_WIFI_SSID_MAX];
	s31_u8 password[S31_HOSTED_WIFI_PASSWORD_MAX];
} __attribute__((packed));

/*
 * S31_HOSTED_CTRL_WIFI_LINK_EVENT payload. This carries the detail cfg80211
 * needs for cfg80211_connect_result()/cfg80211_disconnected(), which the
 * coarse S31_HOSTED_CTRL_LINK carrier notification cannot express.
 */
struct s31_hosted_wifi_link_event {
	s31_u8 connected;
	s31_u8 reason;			/* IEEE 802.11 reason code */
	s31_u8 channel;
	s31_u8 ssid_len;
	s31_u8 bssid[6];
	s31_s8 rssi;
	s31_u8 reserved;
	s31_u8 ssid[S31_HOSTED_WIFI_SSID_MAX];
} __attribute__((packed));

/* S31_HOSTED_CTRL_WIFI_STATION_INFO_RESPONSE payload. */
struct s31_hosted_wifi_station_info {
	s31_s8 rssi;
	s31_u8 channel;
	s31_u8 reserved[2];
	s31_u32 tx_rate_kbps;
	s31_u32 rx_rate_kbps;
} __attribute__((packed));

struct s31_hosted_wifi_slot_request {
	s31_u8 slot;
	s31_u8 reserved[3];
	struct s31_hosted_wifi_slot config;
};

struct s31_hosted_wifi_state_request {
	struct s31_hosted_wifi_state state;
};

/* Userspace argument for S31_HOSTED_IOC_CLOCK_TEST. */
struct s31_hosted_clock_test {
	s31_u32 duration_sec;
	s31_u32 cookie;
	s31_u64 linux_start_ns;
	s31_u64 linux_end_ns;
	s31_u64 freertos_start_us;
	s31_u64 freertos_end_us;
};

struct s31_hosted_ring_state {
	volatile s31_u32 producer;
	s31_u8 producer_pad[60];
	volatile s31_u32 consumer;
	s31_u8 consumer_pad[60];
	volatile s31_u32 drops;
	s31_u8 drops_pad[60];
} __attribute__((aligned(64)));

struct s31_hosted_control {
	volatile s31_u32 magic;
	volatile s31_u32 abi_version;
	volatile s31_u32 generation;
	volatile s31_u32 state;
	volatile s31_u32 features;
	volatile s31_u32 wifi_state;
	s31_u8 sta_mac[6];
	s31_u8 bt_mac[6];
	volatile s31_u32 h0_irq_count;
	volatile s31_u32 h0_rx_polls;
	volatile s31_u32 h0_seen_h1_producer;
	volatile s31_u32 h0_seen_h1_sequence;
	volatile s31_u32 h0_apm_status;
	volatile s31_u32 h0_h1_doorbell;
	s31_u8 header_pad[4];
	struct s31_hosted_ring_state h0_to_h1;
	struct s31_hosted_ring_state h1_to_h0;
};

struct s31_hosted_slot {
	volatile s31_u32 sequence;
	volatile s31_u16 length;
	volatile s31_u8 flags;
	volatile s31_u8 reserved;
	s31_u8 data[S31_HOSTED_SLOT_DATA_SIZE];
};

_Static_assert(sizeof(struct s31_esp_payload_header) == 12,
	       "ESP-Hosted payload header ABI changed");
_Static_assert(sizeof(struct s31_hosted_control_msg) == 24,
	       "hosted control message ABI changed");
_Static_assert(sizeof(struct s31_hosted_clock_stamp) == 16,
	       "hosted clock stamp ABI changed");
_Static_assert(sizeof(struct s31_hosted_cpu_freq_msg) == 16,
	       "hosted CPU frequency ABI changed");
_Static_assert(sizeof(struct s31_hosted_mem_stats) == 16,
	       "hosted memory statistics ABI changed");
_Static_assert(sizeof(struct s31_hosted_wifi_slot) == 100,
	       "hosted Wi-Fi slot ABI changed");
_Static_assert(sizeof(struct s31_hosted_wifi_state) == 16,
	       "hosted Wi-Fi state ABI changed");
_Static_assert(sizeof(struct s31_hosted_wifi_connect) <=
	       S31_HOSTED_WIFI_MSG_DATA_SIZE,
	       "Wi-Fi connect request does not fit the hosted message payload");
_Static_assert(sizeof(struct s31_hosted_wifi_scan_req) <=
	       S31_HOSTED_WIFI_MSG_DATA_SIZE,
	       "Wi-Fi scan request does not fit the hosted message payload");
_Static_assert(sizeof(struct s31_hosted_wifi_bss) <=
	       S31_HOSTED_WIFI_MSG_DATA_SIZE,
	       "Wi-Fi scan result does not fit the hosted message payload");
_Static_assert(sizeof(struct s31_hosted_wifi_link_event) <=
	       S31_HOSTED_WIFI_MSG_DATA_SIZE,
	       "Wi-Fi link event does not fit the hosted message payload");
_Static_assert(sizeof(struct s31_hosted_wifi_slot) <=
	       S31_HOSTED_WIFI_MSG_DATA_SIZE,
	       "Wi-Fi slot does not fit the hosted message payload");
_Static_assert(sizeof(struct s31_hosted_wifi_msg) == 140,
	       "hosted Wi-Fi message ABI changed");
_Static_assert(sizeof(struct s31_hosted_ring_state) == 192,
	       "hosted ring state must occupy three cache lines");
_Static_assert(sizeof(struct s31_hosted_control) == 448,
	       "hosted control block ABI changed");
_Static_assert(sizeof(struct s31_hosted_slot) == S31_HOSTED_SLOT_SIZE,
	       "hosted slot ABI changed");
_Static_assert(S31_HOSTED_H1_TO_H0_OFFSET +
	       S31_HOSTED_SLOT_COUNT * S31_HOSTED_SLOT_SIZE ==
	       S31_HOSTED_SRAM_SIZE,
	       "hosted SRAM layout does not fill its reservation");

#endif /* S31_HOSTED_SRAM_H */
