/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <stdio.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

#include "hosted_sram.h"
#include "s31_hosted_sram.h"
#include "slave_wifi_std.h"
#include "s31_wifi_config.h"

static const char *TAG = "s31_wifi_cfg";
static const char *NVS_NAMESPACE = "s31wifi";
static const uint8_t INVALID_SLOT = 0xff;
/* Bound the scan report so one sweep cannot monopolise the control ring. */
#define S31_WIFI_SCAN_MAX_RECORDS 24

static struct s31_hosted_wifi_slot s_slots[S31_HOSTED_WIFI_SLOT_COUNT];
static struct s31_hosted_wifi_state s_state;
static esp_timer_handle_t s_retry_timer;
static uint8_t s_retry_slot;
static uint8_t s_attempt_slot = INVALID_SLOT;
static bool s_initialized;

static esp_err_t wifi_ensure_started(void);
static void send_response(uint8_t type, uint8_t slot, esp_err_t status,
			  const void *data, size_t len);
static void report_scan(void);
static void send_paced(uint8_t type, esp_err_t status, const void *data,
		       size_t len);
static void send_link_event(bool connected, uint8_t reason,
			    const uint8_t *bssid, const uint8_t *ssid,
			    uint8_t ssid_len, uint8_t channel);
static bool station_request(const struct s31_hosted_wifi_msg *msg);

static uint32_t hosted_generation(void)
{
	return *(volatile uint32_t *)(S31_HOSTED_SRAM_BASE +
					 offsetof(struct s31_hosted_control, generation));
}

static esp_err_t nvs_load(void)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (err != ESP_OK)
		return err;

	memset(s_slots, 0, sizeof(s_slots));
	memset(&s_state, 0, sizeof(s_state));
	s_state.enabled = 1;
	s_state.scan_interval_sec = 30;
	s_state.active_slot = 0;
	s_state.connected_slot = INVALID_SLOT;
	for (uint8_t i = 0; i < S31_HOSTED_WIFI_SLOT_COUNT; i++) {
		char key[8];
		snprintf(key, sizeof(key), "slot%u", i);
		size_t len = sizeof(s_slots[i]);
		if (nvs_get_blob(handle, key, &s_slots[i], &len) != ESP_OK ||
		    len != sizeof(s_slots[i]))
			memset(&s_slots[i], 0, sizeof(s_slots[i]));
	}
	{
		size_t len = sizeof(s_state);
		if (nvs_get_blob(handle, "state", &s_state, &len) != ESP_OK ||
		    len != sizeof(s_state)) {
			s_state.enabled = 1;
			s_state.auto_connect = 0;
			s_state.scan_interval_sec = 30;
			s_state.active_slot = 0;
			s_state.connected_slot = INVALID_SLOT;
		}
	}
	nvs_close(handle);
	if (s_state.scan_interval_sec == 0)
		s_state.scan_interval_sec = 30;
	if (s_state.active_slot >= S31_HOSTED_WIFI_SLOT_COUNT)
		s_state.active_slot = 0;
	s_state.connected_slot = INVALID_SLOT;
	return ESP_OK;
}

static esp_err_t nvs_save_blob(const char *key, const void *data, size_t len)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (err != ESP_OK)
		return err;
	err = nvs_set_blob(handle, key, data, len);
	if (err == ESP_OK)
		err = nvs_commit(handle);
	nvs_close(handle);
	return err;
}

static bool slot_valid(uint8_t slot)
{
	return slot < S31_HOSTED_WIFI_SLOT_COUNT &&
	       s_slots[slot].valid && s_slots[slot].ssid_len &&
	       s_slots[slot].ssid_len <= S31_HOSTED_WIFI_SSID_MAX &&
	       s_slots[slot].password_len <= S31_HOSTED_WIFI_PASSWORD_MAX;
}

static int choose_slot(void)
{
	uint8_t active = s_state.active_slot;
	if (slot_valid(active))
		return active;
	for (uint8_t i = 0; i < S31_HOSTED_WIFI_SLOT_COUNT; i++)
		if (slot_valid(i))
			return i;
	return -1;
}

static esp_err_t wifi_ensure_started(void)
{
	wifi_mode_t mode;
	esp_err_t err = esp_wifi_get_mode(&mode);
	if (err == ESP_ERR_WIFI_NOT_INIT) {
		wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
		err = esp_wifi_init(&config);
		if (err != ESP_OK && err != ESP_ERR_WIFI_STATE)
			return err;
		(void)esp_hosted_register_wifi_event_handlers();
		err = esp_wifi_set_mode(WIFI_MODE_STA);
		if (err != ESP_OK && err != ESP_ERR_WIFI_STATE)
			return err;
	} else if (err != ESP_OK) {
		return err;
	} else if (mode == WIFI_MODE_NULL) {
		err = esp_wifi_set_mode(WIFI_MODE_STA);
		if (err != ESP_OK)
			return err;
	}
	err = esp_wifi_start();
	return err == ESP_ERR_WIFI_STATE ? ESP_OK : err;
}

static esp_err_t connect_slot(uint8_t slot)
{
	wifi_config_t config = { 0 };
	if (!slot_valid(slot))
		return ESP_ERR_INVALID_ARG;
	memcpy(config.sta.ssid, s_slots[slot].ssid,
	       s_slots[slot].ssid_len);
	memcpy(config.sta.password, s_slots[slot].password,
	       s_slots[slot].password_len);
	config.sta.ssid[s_slots[slot].ssid_len] = 0;
	config.sta.password[s_slots[slot].password_len] = 0;
	if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK)
		return ESP_FAIL;
	s_attempt_slot = slot;
	return esp_wifi_connect();
}

static void retry_timer_cb(void *arg)
{
	uint8_t slot;
	(void)arg;
	if (!s_state.enabled || !s_state.auto_connect)
		return;
	for (uint8_t i = 0; i < S31_HOSTED_WIFI_SLOT_COUNT; i++) {
		slot = (uint8_t)((s_retry_slot + i) % S31_HOSTED_WIFI_SLOT_COUNT);
		if (slot_valid(slot)) {
			s_retry_slot = (uint8_t)((slot + 1) %
						 S31_HOSTED_WIFI_SLOT_COUNT);
			(void)connect_slot(slot);
			break;
		}
	}
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
				       int32_t event_id, void *event_data)
{
	(void)arg;
	if (base != WIFI_EVENT)
		return;
	if (event_id == WIFI_EVENT_SCAN_DONE) {
		report_scan();
	} else if (event_id == WIFI_EVENT_STA_START) {
		if (s_state.enabled && s_state.auto_connect) {
			int slot = choose_slot();
			if (slot >= 0) {
				s_retry_slot = (uint8_t)((slot + 1) %
						       S31_HOSTED_WIFI_SLOT_COUNT);
				(void)connect_slot((uint8_t)slot);
			}
		}
	} else if (event_id == WIFI_EVENT_STA_CONNECTED) {
		const wifi_event_sta_connected_t *ev = event_data;

		s_state.connected_slot = s_attempt_slot;
		if (s_retry_timer)
			(void)esp_timer_stop(s_retry_timer);
		if (ev)
			send_link_event(true, 0, ev->bssid, ev->ssid,
					ev->ssid_len, ev->channel);
	} else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
		const wifi_event_sta_disconnected_t *ev = event_data;

		s_state.connected_slot = INVALID_SLOT;
		send_link_event(false, ev ? ev->reason : 0,
				ev ? ev->bssid : NULL, ev ? ev->ssid : NULL,
				ev ? ev->ssid_len : 0, 0);
		if (s_state.enabled && s_state.auto_connect && s_retry_timer) {
			(void)esp_timer_stop(s_retry_timer);
			(void)esp_timer_start_once(s_retry_timer,
				(uint64_t)s_state.scan_interval_sec * 1000000ULL);
		}
	}
}

esp_err_t s31_wifi_config_init(void)
{
	esp_timer_create_args_t timer_args = {
		.callback = retry_timer_cb,
		.name = "s31_wifi_retry",
	};
	if (s_initialized)
		return ESP_OK;
	if (nvs_load() != ESP_OK)
		return ESP_FAIL;
	if (esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
				       wifi_event_handler, NULL) != ESP_OK)
		return ESP_FAIL;
	if (esp_timer_create(&timer_args, &s_retry_timer) != ESP_OK)
		return ESP_FAIL;
	s_initialized = true;
	ESP_LOGI(TAG, "Wi-Fi config service ready (3 NVS slots, interval=%us)",
		 (unsigned)s_state.scan_interval_sec);
	return ESP_OK;
}

esp_err_t s31_wifi_config_autostart(void)
{
	int slot;
	if (!s_initialized || !s_state.enabled || !s_state.auto_connect)
		return ESP_OK;
	if (wifi_ensure_started() != ESP_OK)
		return ESP_FAIL;
	slot = choose_slot();
	if (slot >= 0)
		s_retry_slot = (uint8_t)((slot + 1) % S31_HOSTED_WIFI_SLOT_COUNT);
	return slot < 0 ? ESP_ERR_NOT_FOUND : connect_slot((uint8_t)slot);
}

static void send_response(uint8_t type, uint8_t slot, esp_err_t status,
			  const void *data, size_t len)
{
	struct s31_hosted_wifi_msg response = {
		.type = type,
		.slot = slot,
		.length = len,
		.generation = hosted_generation(),
		.status = (uint32_t)status,
	};
	if (len > sizeof(response.data))
		len = sizeof(response.data);
	if (data && len)
		memcpy(response.data, data, len);
	(void)s31_hosted_sram_send_control(&response, sizeof(response));
}



/*
 * The control ring is shallow: s31_hosted_sram_send_control() queues at most
 * S31_PENDING_CONTROL_DEPTH messages and then drops them. A scan sweep emits
 * one message per BSS, which overruns it easily, and a lost SCAN_DONE leaves
 * the host waiting forever. Pace the sends instead. This runs on the esp_event
 * task, not the transport RX task that drains the ring, so sleeping here lets
 * the ring catch up rather than deadlocking it.
 */
static void send_paced(uint8_t type, esp_err_t status, const void *data,
		       size_t len)
{
	struct s31_hosted_wifi_msg response = {
		.type = type,
		.length = len,
		.generation = hosted_generation(),
		.status = (uint32_t)status,
	};

	if (len > sizeof(response.data))
		len = sizeof(response.data);
	if (data && len)
		memcpy(response.data, data, len);

	for (int retry = 0; retry < 200; retry++) {
		if (s31_hosted_sram_send_control(&response,
						 sizeof(response)) == 0)
			return;
		vTaskDelay(pdMS_TO_TICKS(2));
	}
	ESP_LOGW(TAG, "dropped Wi-Fi message type %u after retries", type);
}

/* ------------------------- cfg80211 station ops ------------------------- */

/*
 * Linux drives the radio through nl80211/cfg80211 rather than the stored
 * profiles above. These requests bypass the slot machinery entirely.
 */
static bool s_scan_in_progress;

static uint8_t auth_mode_to_wire(wifi_auth_mode_t mode)
{
	switch (mode) {
	case WIFI_AUTH_OPEN:		return S31_HOSTED_WIFI_AUTH_OPEN;
	case WIFI_AUTH_WEP:		return S31_HOSTED_WIFI_AUTH_WEP;
	case WIFI_AUTH_WPA_PSK:		return S31_HOSTED_WIFI_AUTH_WPA_PSK;
	case WIFI_AUTH_WPA2_PSK:	return S31_HOSTED_WIFI_AUTH_WPA2_PSK;
	case WIFI_AUTH_WPA_WPA2_PSK:	return S31_HOSTED_WIFI_AUTH_WPA_WPA2_PSK;
	case WIFI_AUTH_WPA3_PSK:	return S31_HOSTED_WIFI_AUTH_WPA3_PSK;
	case WIFI_AUTH_WPA2_WPA3_PSK:	return S31_HOSTED_WIFI_AUTH_WPA2_WPA3_PSK;
	case WIFI_AUTH_WAPI_PSK:	return S31_HOSTED_WIFI_AUTH_WAPI_PSK;
	default:			return S31_HOSTED_WIFI_AUTH_UNKNOWN;
	}
}

/*
 * Report each BSS as its own message then finish with SCAN_DONE, so the kernel
 * feeds cfg80211 incrementally instead of buffering a whole sweep.
 */
static void report_scan(void)
{
	uint16_t count = 0;
	uint16_t wanted;
	wifi_ap_record_t *records;

	s_scan_in_progress = false;

	if (esp_wifi_scan_get_ap_num(&count) != ESP_OK || !count) {
		send_paced(S31_HOSTED_CTRL_WIFI_SCAN_DONE, ESP_OK, NULL, 0);
		return;
	}
	wanted = count > S31_WIFI_SCAN_MAX_RECORDS ? S31_WIFI_SCAN_MAX_RECORDS
						   : count;
	records = calloc(wanted, sizeof(*records));
	if (!records) {
		(void)esp_wifi_scan_get_ap_records(&count, NULL);
		send_paced(S31_HOSTED_CTRL_WIFI_SCAN_DONE, ESP_ERR_NO_MEM,
			   NULL, 0);
		return;
	}
	if (esp_wifi_scan_get_ap_records(&wanted, records) != ESP_OK) {
		free(records);
		send_paced(S31_HOSTED_CTRL_WIFI_SCAN_DONE, ESP_FAIL,
			   NULL, 0);
		return;
	}
	if (count > wanted)
		ESP_LOGW(TAG, "reporting %u of %u scan results", wanted, count);

	for (uint16_t i = 0; i < wanted; i++) {
		struct s31_hosted_wifi_bss bss = {
			.channel = records[i].primary,
			.rssi = records[i].rssi,
			.auth_mode = auth_mode_to_wire(records[i].authmode),
		};
		size_t ssid_len = strnlen((const char *)records[i].ssid,
					  sizeof(bss.ssid));

		memcpy(bss.bssid, records[i].bssid, sizeof(bss.bssid));
		memcpy(bss.ssid, records[i].ssid, ssid_len);
		bss.ssid_len = (uint8_t)ssid_len;
		send_paced(S31_HOSTED_CTRL_WIFI_SCAN_RESULT, ESP_OK,
			   &bss, sizeof(bss));
	}
	free(records);
	send_paced(S31_HOSTED_CTRL_WIFI_SCAN_DONE, ESP_OK, NULL, 0);
}

static void send_link_event(bool connected, uint8_t reason,
			    const uint8_t *bssid, const uint8_t *ssid,
			    uint8_t ssid_len, uint8_t channel)
{
	struct s31_hosted_wifi_link_event ev = {
		.connected = connected ? 1 : 0,
		.reason = reason,
		.channel = channel,
	};
	wifi_ap_record_t ap;

	if (bssid)
		memcpy(ev.bssid, bssid, sizeof(ev.bssid));
	if (ssid && ssid_len) {
		if (ssid_len > sizeof(ev.ssid))
			ssid_len = sizeof(ev.ssid);
		memcpy(ev.ssid, ssid, ssid_len);
		ev.ssid_len = ssid_len;
	}
	if (connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
		ev.rssi = ap.rssi;

	send_paced(S31_HOSTED_CTRL_WIFI_LINK_EVENT, ESP_OK, &ev,
		   sizeof(ev));
}

static esp_err_t station_scan(const struct s31_hosted_wifi_scan_req *req)
{
	wifi_scan_config_t cfg = { 0 };
	uint8_t ssid[S31_HOSTED_WIFI_SSID_MAX + 1] = { 0 };
	esp_err_t err;

	if (s_scan_in_progress)
		return ESP_ERR_INVALID_STATE;

	/*
	 * A hidden AP does not answer a broadcast probe, so when the host names
	 * an SSID, ask for a directed probe instead.
	 */
	if (req && req->ssid_len && req->ssid_len <= S31_HOSTED_WIFI_SSID_MAX) {
		memcpy(ssid, req->ssid, req->ssid_len);
		cfg.ssid = ssid;
	}
	if (req)
		cfg.channel = req->channel;
	err = wifi_ensure_started();
	if (err != ESP_OK)
		return err;
	/* Asynchronous: WIFI_EVENT_SCAN_DONE reports the results. */
	err = esp_wifi_scan_start(&cfg, false);
	if (err == ESP_OK)
		s_scan_in_progress = true;
	return err;
}

static esp_err_t station_connect(const struct s31_hosted_wifi_connect *req)
{
	wifi_config_t cfg = { 0 };
	esp_err_t err;

	if (!req->ssid_len || req->ssid_len > sizeof(cfg.sta.ssid) ||
	    req->password_len > sizeof(cfg.sta.password))
		return ESP_ERR_INVALID_ARG;

	err = wifi_ensure_started();
	if (err != ESP_OK)
		return err;

	memcpy(cfg.sta.ssid, req->ssid, req->ssid_len);
	if (req->password_len)
		memcpy(cfg.sta.password, req->password, req->password_len);
	if (req->flags & S31_HOSTED_WIFI_CONNECT_F_BSSID) {
		cfg.sta.bssid_set = true;
		memcpy(cfg.sta.bssid, req->bssid, sizeof(cfg.sta.bssid));
	}
	cfg.sta.channel = req->channel;
	cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
	/*
	 * Sweep every channel rather than stopping at the first match. A hidden
	 * AP does not appear in a passive sweep, so the directed probe has to be
	 * given the chance to find it wherever it is.
	 */
	cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

	/*
	 * Linux is now authoritative. Stop the stored-profile retry timer so it
	 * cannot reconnect underneath wpa_supplicant.
	 */
	s_state.auto_connect = 0;
	if (s_retry_timer)
		(void)esp_timer_stop(s_retry_timer);

	(void)esp_wifi_disconnect();
	err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
	if (err != ESP_OK)
		return err;
	s_attempt_slot = INVALID_SLOT;
	return esp_wifi_connect();
}

/* Returns true when the message was one of the station operations. */
static bool station_request(const struct s31_hosted_wifi_msg *msg)
{
	esp_err_t status;

	switch (msg->type) {
	case S31_HOSTED_CTRL_WIFI_SCAN: {
		struct s31_hosted_wifi_scan_req req;

		memcpy(&req, msg->data, sizeof(req));
		status = station_scan(&req);
		send_response(S31_HOSTED_CTRL_WIFI_SCAN_RESPONSE, 0, status,
			      NULL, 0);
		if (status != ESP_OK)
			send_response(S31_HOSTED_CTRL_WIFI_SCAN_DONE, 0, status,
				      NULL, 0);
		return true;
	}
	case S31_HOSTED_CTRL_WIFI_CONNECT: {
		struct s31_hosted_wifi_connect req;

		memcpy(&req, msg->data, sizeof(req));
		status = station_connect(&req);
		send_response(S31_HOSTED_CTRL_WIFI_CONNECT_RESPONSE, 0, status,
			      NULL, 0);
		return true;
	}
	case S31_HOSTED_CTRL_WIFI_DISCONNECT:
		s_state.auto_connect = 0;
		if (s_retry_timer)
			(void)esp_timer_stop(s_retry_timer);
		status = esp_wifi_disconnect();
		send_response(S31_HOSTED_CTRL_WIFI_DISCONNECT_RESPONSE, 0,
			      status, NULL, 0);
		return true;
	case S31_HOSTED_CTRL_WIFI_STATION_INFO: {
		struct s31_hosted_wifi_station_info info = { 0 };
		wifi_ap_record_t ap;

		status = esp_wifi_sta_get_ap_info(&ap);
		if (status == ESP_OK) {
			info.rssi = ap.rssi;
			info.channel = ap.primary;
		}
		send_response(S31_HOSTED_CTRL_WIFI_STATION_INFO_RESPONSE, 0,
			      status, &info, sizeof(info));
		return true;
	}
	default:
		return false;
	}
}

bool hosted_wifi_config_handler(const uint8_t *data, size_t len)
{
	const struct s31_hosted_wifi_msg *msg;
	esp_err_t status = ESP_OK;
	uint8_t response_type;

	if (!data || len < sizeof(*msg))
		return false;
	msg = (const void *)data;
	if (msg->generation != hosted_generation())
		return true;
	/*
	 * cfg80211 station operations are independent of the stored profiles
	 * below: Linux drives association directly through nl80211.
	 */
	if (station_request(msg))
		return true;
	if (!s_initialized)
		return true;

	switch (msg->type) {
	case S31_HOSTED_CTRL_WIFI_SLOT_SET:
		response_type = S31_HOSTED_CTRL_WIFI_SLOT_SET_RESPONSE;
		if (msg->slot >= S31_HOSTED_WIFI_SLOT_COUNT ||
		    msg->length != sizeof(struct s31_hosted_wifi_slot)) {
			status = ESP_ERR_INVALID_ARG;
			break;
		}
		memcpy(&s_slots[msg->slot], msg->data, sizeof(s_slots[0]));
		if (s_slots[msg->slot].ssid_len > S31_HOSTED_WIFI_SSID_MAX ||
		    s_slots[msg->slot].password_len > S31_HOSTED_WIFI_PASSWORD_MAX ||
		    (s_slots[msg->slot].valid && !s_slots[msg->slot].ssid_len)) {
			status = ESP_ERR_INVALID_ARG;
			break;
		}
		{
			char key[8];
			snprintf(key, sizeof(key), "slot%u", msg->slot);
			status = nvs_save_blob(key, &s_slots[msg->slot],
					       sizeof(s_slots[0]));
		}
		break;
	case S31_HOSTED_CTRL_WIFI_SLOT_GET:
		response_type = S31_HOSTED_CTRL_WIFI_SLOT_GET_RESPONSE;
		if (msg->slot >= S31_HOSTED_WIFI_SLOT_COUNT)
			status = ESP_ERR_INVALID_ARG;
		break;
	case S31_HOSTED_CTRL_WIFI_STATE_SET:
		response_type = S31_HOSTED_CTRL_WIFI_STATE_SET_RESPONSE;
		if (msg->length != sizeof(s_state)) {
			status = ESP_ERR_INVALID_ARG;
			break;
		}
		{
			struct s31_hosted_wifi_state requested;
			memcpy(&requested, msg->data, sizeof(requested));
			if (requested.active_slot >= S31_HOSTED_WIFI_SLOT_COUNT ||
			    requested.scan_interval_sec == 0) {
				status = ESP_ERR_INVALID_ARG;
				break;
			}
			s_state.enabled = !!requested.enabled;
			s_state.auto_connect = !!requested.auto_connect;
			s_state.scan_interval_sec = requested.scan_interval_sec;
			s_state.active_slot = requested.active_slot;
			s_retry_slot = (uint8_t)((requested.active_slot + 1) %
						 S31_HOSTED_WIFI_SLOT_COUNT);
			status = nvs_save_blob("state", &s_state, sizeof(s_state));
			if (status == ESP_OK && !s_state.enabled) {
				(void)esp_wifi_disconnect();
				(void)esp_wifi_stop();
			} else if (status == ESP_OK && s_state.enabled) {
				status = wifi_ensure_started();
			}
			if (status == ESP_OK && s_state.enabled && s_state.auto_connect) {
				int slot = choose_slot();
				if (slot >= 0)
					(void)connect_slot((uint8_t)slot);
			}
		}
		break;
	case S31_HOSTED_CTRL_WIFI_STATE_GET:
		response_type = S31_HOSTED_CTRL_WIFI_STATE_GET_RESPONSE;
		break;
	default:
		return false;
	}

	if (msg->type == S31_HOSTED_CTRL_WIFI_SLOT_GET && status == ESP_OK)
		send_response(response_type, msg->slot, status,
			      &s_slots[msg->slot], sizeof(s_slots[0]));
	else if ((msg->type == S31_HOSTED_CTRL_WIFI_STATE_GET ||
		  msg->type == S31_HOSTED_CTRL_WIFI_STATE_SET) && status == ESP_OK)
		send_response(response_type, msg->slot, status, &s_state,
			      sizeof(s_state));
	else if (msg->type == S31_HOSTED_CTRL_WIFI_SLOT_SET && status == ESP_OK)
		send_response(response_type, msg->slot, status,
			      &s_slots[msg->slot], sizeof(s_slots[0]));
	else
		send_response(response_type, msg->slot, status, NULL, 0);
	return true;
}
