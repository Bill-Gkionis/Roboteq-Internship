/*
 * iot.c - Wi-Fi association, HTTP dashboard, WebSocket telemetry.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "iot.h"

LOG_MODULE_REGISTER(iot, LOG_LEVEL_INF);

#if defined(CONFIG_HTTP_SERVER)

#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#if defined(CONFIG_NET_L2_WIFI_MGMT)
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>
#endif
#include <zephyr/net/socket.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#include <stdio.h>
#include <string.h>

#include "system.h"
#include "control-system.h"
#include "model_gen.h"
#include "water-loop.h"
#include "ui.h"

/* The dashboard, gzipped at build time from src/webapp/index.html.  Editing
 * the page therefore needs no C change and costs only its compressed size. */
static const uint8_t index_html_gz[] = {
#include "index.html.gz.inc"
};

/* ------------------------------------------------------------ credentials */

static struct {
	char ssid[33];
	char psk[65];
	char ip[NET_IPV4_ADDR_LEN];
	char token[9];
	bool connected;
} net;

static int net_settings_set(const char *name, size_t len,
			    settings_read_cb read_cb, void *cb_arg)
{
	if (settings_name_steq(name, "ssid", NULL) && len < sizeof(net.ssid)) {
		return read_cb(cb_arg, net.ssid, len) > 0 ? 0 : -EINVAL;
	}
	if (settings_name_steq(name, "psk", NULL) && len < sizeof(net.psk)) {
		return read_cb(cb_arg, net.psk, len) > 0 ? 0 : -EINVAL;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(iot, "net", NULL, net_settings_set, NULL, NULL);

/* ------------------------------------------------------ association ------ */

#if defined(CONFIG_NET_L2_WIFI_MGMT)
static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;

static void on_wifi_event(struct net_mgmt_event_callback *cb, uint64_t event,
			  struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	switch (event) {
	case NET_EVENT_WIFI_CONNECT_RESULT:
		LOG_INF("Wi-Fi associated with '%s'", net.ssid);
		break;
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		LOG_WRN("Wi-Fi disconnected");
		net.connected = false;
		(void)strcpy(net.ip, "");
		ui_set_net_status(net.ssid, NULL, false);
		break;
	default:
		break;
	}
}

static void on_ipv4_event(struct net_mgmt_event_callback *cb, uint64_t event,
			  struct net_if *iface)
{
	ARG_UNUSED(cb);

	if (event != NET_EVENT_IPV4_ADDR_ADD) {
		return;
	}

	struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;

	if (ipv4 == NULL) {
		return;
	}

	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		if (ipv4->unicast[i].ipv4.addr_type != NET_ADDR_DHCP) {
			continue;
		}

		(void)net_addr_ntop(AF_INET,
				    &ipv4->unicast[i].ipv4.address.in_addr,
				    net.ip, sizeof(net.ip));
		net.connected = true;
		LOG_INF("dashboard at http://%s/   control token %s",
			net.ip, net.token);
		ui_set_net_status(net.ssid, net.ip, true);
		break;
	}
}

static int associate(void)
{
	struct net_if *iface = net_if_get_first_wifi();

	if (iface == NULL) {
		LOG_ERR("no Wi-Fi interface");
		return -ENODEV;
	}

	if (net.ssid[0] == '\0') {
		LOG_WRN("no SSID stored - use the NET screen or "
			"'cal wifi <ssid> <psk>' from the shell");
		return -ENOENT;
	}

	struct wifi_connect_req_params p = {
		.ssid = (const uint8_t *)net.ssid,
		.ssid_length = strlen(net.ssid),
		.psk = (const uint8_t *)net.psk,
		.psk_length = strlen(net.psk),
		.security = (net.psk[0] != '\0') ? WIFI_SECURITY_TYPE_PSK
						 : WIFI_SECURITY_TYPE_NONE,
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
		.mfp = WIFI_MFP_OPTIONAL,
	};

	LOG_INF("associating with '%s'", net.ssid);

	return net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &p, sizeof(p));
}

#else /* no radio in this build */

/*
 * Wi-Fi left out of the build (see wifi.conf).  The HTTP server, the
 * JSON serialiser and the WebSocket push above are all still compiled and
 * linked - only the association step is missing, so the dashboard becomes
 * reachable the moment a radio exists.
 */
static int associate(void)
{
	LOG_WRN("built without Wi-Fi - add -DEXTRA_CONF_FILE=wifi.conf");
	return -ENOTSUP;
}

#endif /* CONFIG_NET_L2_WIFI_MGMT */

/* --------------------------------------------------------- serialisation - */

int iot_snapshot_json(char *buf, size_t buf_len)
{
	struct cal_snapshot s;

	sys_snapshot_get(&s);

	/*
	 * Hand-rolled rather than JSON_OBJ_DESCR: the schema is a flat list of
	 * floats and the encoder library cannot format them without a float
	 * descriptor anyway.  snprintk is bounded and this runs on the
	 * opportunistic thread, so the cost is irrelevant.
	 *
	 * Raw AND derived are both sent, deliberately: if every input is
	 * logged, the whole control stack can be re-run offline against a
	 * recorded session, which means a new gate or a retuned lambda can be
	 * evaluated without occupying the rig.
	 */
	return snprintk(buf, buf_len,
		"{\"t\":%lld,\"tick\":%u,\"state\":\"%s\",\"faults\":%u,"
		"\"fault_name\":\"%s\","
		"\"p_meas\":%.3f,\"p_hat\":%.3f,\"p_aux\":%.3f,"
		"\"t_in\":%.3f,\"t_gd\":%.3f,\"e\":%.4f,"
		"\"tw_in\":%.3f,\"tw_out\":%.3f,\"dtw\":%.4f,"
		"\"flow\":%.2f,\"flow_set\":%.2f,\"mcp\":%.3f,"
		"\"guard_i\":%.3f,\"guard_w\":%.3f,\"inner_w\":%.3f,"
		"\"duty_hin\":%.3f,\"duty_hgd\":%.3f,\"duty_fan\":%.3f,"
		"\"duty_rej\":%.3f,\"pump\":%.3f,"
		"\"gate_null\":%s,\"gate_steady\":%s,"
		"\"slope\":%.6e,\"slope_gate\":%.6e,"
		"\"tss\":%.2f,\"dtw_set\":%.2f,"
		"\"settle_s\":%d,\"dwell_s\":%d,\"dwell_target\":%d,"
		"\"armed\":%s,\"tare\":%.4f,"
		"\"last_w\":%.3f,\"last_se\":%.3f,\"last_ok\":%s,"
		"\"params\":\"%s\",\"fw\":\"%s\"}",
		s.t_ms, s.tick, cal_state_name(s.state), s.faults,
		sys_fault_name(s.faults),
		(double)s.p_meas, (double)s.p_hat, (double)s.p_aux,
		(double)s.t_inner.v, (double)s.t_guard.v, (double)s.e_null,
		(double)s.t_water_in.v, (double)s.t_water_out.v,
		(double)s.dt_water,
		(double)s.flow.v, (double)s.flow_set, (double)s.mcp,
		(double)s.guard_i, (double)s.guard_demand,
		(double)s.inner_demand,
		(double)s.duty_heat_inner, (double)s.duty_heat_guard,
		(double)s.duty_fan_inner, (double)s.duty_fan_reject,
		(double)s.pump_frac,
		s.gate_null ? "true" : "false",
		s.gate_steady ? "true" : "false",
		(double)s.p_slope, (double)s.p_slope_gate,
		(double)s.t_inner_set, (double)s.dt_water_set,
		s.settle_s, s.dwell_s, CAL_DWELL_S,
		s.heaters_enabled ? "true" : "false",
		(double)water_loop_get_tare(),
		(double)s.last_point_w, (double)s.last_point_se,
		s.last_point_valid ? "true" : "false",
		CAL_PARAMS_HASH, APP_VERSION_STR);
}

/* ------------------------------------------------------- HTTP resources -- */

static uint16_t http_port = 80;

HTTP_SERVICE_DEFINE(cal_http, NULL, &http_port, CONFIG_HTTP_SERVER_MAX_CLIENTS,
		    4, NULL, NULL, NULL);

static struct http_resource_detail_static index_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_STATIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_encoding = "gzip",
		.content_type = "text/html",
	},
	.static_data = index_html_gz,
	.static_data_len = sizeof(index_html_gz),
};

HTTP_RESOURCE_DEFINE(index_res, cal_http, "/", &index_detail);

/* --- GET /api/state ------------------------------------------------------ */

static char state_buf[1024];

static int state_handler(struct http_client_ctx *client,
			 enum http_transaction_status status,
			 const struct http_request_ctx *req,
			 struct http_response_ctx *rsp, void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(req);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	const int len = iot_snapshot_json(state_buf, sizeof(state_buf));

	rsp->body = (const uint8_t *)state_buf;
	rsp->body_len = (len > 0) ? (size_t)len : 0;
	rsp->final_chunk = true;

	return 0;
}

static struct http_resource_detail_dynamic state_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_type = "application/json",
	},
	.cb = state_handler,
	.user_data = NULL,
};

HTTP_RESOURCE_DEFINE(state_res, cal_http, "/api/state", &state_detail);

/* --- POST /api/cmd ------------------------------------------------------- */

/*
 * Body is  {"cmd":"start","arg":0,"token":"deadbeef"}
 * Parsed by hand: three fields, and pulling in the JSON decoder for them would
 * cost more flash than the parser it replaced.
 */
static char cmd_buf[192];
static char cmd_reply[64];

static bool json_str(const char *body, const char *key, char *out, size_t n)
{
	char pattern[24];

	(void)snprintk(pattern, sizeof(pattern), "\"%s\"", key);

	const char *p = strstr(body, pattern);

	if (p == NULL) {
		return false;
	}
	p = strchr(p + strlen(pattern), '"');
	if (p == NULL) {
		return false;
	}
	p++;

	const char *end = strchr(p, '"');

	if (end == NULL || (size_t)(end - p) >= n) {
		return false;
	}

	(void)memcpy(out, p, (size_t)(end - p));
	out[end - p] = '\0';

	return true;
}

static float json_num(const char *body, const char *key, float dflt)
{
	char pattern[24];

	(void)snprintk(pattern, sizeof(pattern), "\"%s\"", key);

	const char *p = strstr(body, pattern);

	if (p == NULL) {
		return dflt;
	}
	p = strchr(p + strlen(pattern), ':');

	return (p != NULL) ? (float)atof(p + 1) : dflt;
}

static enum cal_cmd_type cmd_from_name(const char *n)
{
	if (!strcmp(n, "start"))  return CAL_CMD_START;
	if (!strcmp(n, "stop"))   return CAL_CMD_STOP;
	if (!strcmp(n, "tss"))    return CAL_CMD_SET_TSS;
	if (!strcmp(n, "dtw"))    return CAL_CMD_SET_DTW;
	if (!strcmp(n, "soak"))   return CAL_CMD_CAL_SOAK;
	if (!strcmp(n, "tare"))   return CAL_CMD_CAL_TARE;
	if (!strcmp(n, "subst"))  return CAL_CMD_CAL_SUBST;
	if (!strcmp(n, "clear"))  return CAL_CMD_CLEAR_FAULT;
	return CAL_CMD_NONE;
}

static int cmd_handler(struct http_client_ctx *client,
		       enum http_transaction_status status,
		       const struct http_request_ctx *req,
		       struct http_response_ctx *rsp, void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(user_data);
	static size_t used;

	if (status == HTTP_SERVER_REQUEST_DATA_MORE) {
		const size_t room = sizeof(cmd_buf) - 1 - used;
		const size_t n = MIN(req->data_len, room);

		(void)memcpy(cmd_buf + used, req->data, n);
		used += n;
		return 0;
	}

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		used = 0;
		return 0;
	}

	const size_t room = sizeof(cmd_buf) - 1 - used;
	const size_t n = MIN(req->data_len, room);

	(void)memcpy(cmd_buf + used, req->data, n);
	cmd_buf[used + n] = '\0';
	used = 0;

	char token[16] = { 0 };
	char name[16] = { 0 };
	int http_rc = 0;

	if (!json_str(cmd_buf, "token", token, sizeof(token)) ||
	    strcmp(token, net.token) != 0) {
		/* Refused at the door, before the command ever becomes a
		 * struct cal_cmd. */
		http_rc = -1;
		(void)strcpy(cmd_reply, "{\"ok\":false,\"err\":\"token\"}");
	} else if (!json_str(cmd_buf, "cmd", name, sizeof(name)) ||
		   cmd_from_name(name) == CAL_CMD_NONE) {
		http_rc = -1;
		(void)strcpy(cmd_reply, "{\"ok\":false,\"err\":\"cmd\"}");
	} else {
		const struct cal_cmd c = {
			.type = cmd_from_name(name),
			.arg = json_num(cmd_buf, "arg", 0.0f),
			.origin = "net",
		};
		const int rc = sys_cmd_submit(&c);

		(void)snprintk(cmd_reply, sizeof(cmd_reply),
			       "{\"ok\":%s,\"rc\":%d}",
			       rc == 0 ? "true" : "false", rc);
	}

	ARG_UNUSED(http_rc);

	rsp->body = (const uint8_t *)cmd_reply;
	rsp->body_len = strlen(cmd_reply);
	rsp->final_chunk = true;

	return 0;
}

static struct http_resource_detail_dynamic cmd_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_POST),
		.content_type = "application/json",
	},
	.cb = cmd_handler,
	.user_data = NULL,
};

HTTP_RESOURCE_DEFINE(cmd_res, cal_http, "/api/cmd", &cmd_detail);

#if defined(CONFIG_HTTP_SERVER_WEBSOCKET)
/* --- GET /ws  (WebSocket telemetry push) --------------------------------- */

#define WS_STACK 3072
#define WS_PRIO  10        /* opportunistic - it must never delay ctrl */

static int ws_sock = -1;
static char ws_buf[1024];
K_THREAD_STACK_DEFINE(ws_stack, WS_STACK);
static struct k_thread ws_thread_data;
static bool ws_running;

static void ws_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	const int sock = ws_sock;

	LOG_INF("WebSocket client attached");

	while (1) {
		const int len = iot_snapshot_json(ws_buf, sizeof(ws_buf));

		if (len <= 0) {
			break;
		}

		/* A plain send() on the upgraded socket: the HTTP server has
		 * already framed it as a WebSocket, so there is no websocket
		 * API to call here.  zsock_* rather than the POSIX aliases so
		 * this does not depend on CONFIG_POSIX_API staying on. */
		if (zsock_send(sock, ws_buf, (size_t)len, 0) < 0) {
			break;
		}

		/* Matched to the control tick.  There is no faster real signal
		 * in this instrument to show. */
		k_msleep(CAL_TICK_MS);
	}

	LOG_INF("WebSocket client gone");
	(void)zsock_close(sock);
	ws_sock = -1;
	ws_running = false;
}

static int ws_setup(int sock, struct http_request_ctx *req, void *user_data)
{
	ARG_UNUSED(req);
	ARG_UNUSED(user_data);

	if (ws_running) {
		return -EBUSY;   /* one streaming client is enough for a lab rig */
	}

	ws_sock = sock;
	ws_running = true;

	k_thread_create(&ws_thread_data, ws_stack, WS_STACK, ws_thread,
			NULL, NULL, NULL, WS_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&ws_thread_data, "ws");

	return 0;
}

static uint8_t ws_recv_buf[256];

static struct http_resource_detail_websocket ws_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_WEBSOCKET,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
	},
	.cb = ws_setup,
	.data_buffer = ws_recv_buf,
	.data_buffer_len = sizeof(ws_recv_buf),
	.user_data = NULL,
};

HTTP_RESOURCE_DEFINE(ws_res, cal_http, "/ws", &ws_detail);

#endif /* CONFIG_HTTP_SERVER_WEBSOCKET */

/* ------------------------------------------------------------------ API -- */

int iot_set_credentials(const char *ssid, const char *psk)
{
	if (ssid == NULL || ssid[0] == '\0') {
		return -EINVAL;
	}

	(void)strncpy(net.ssid, ssid, sizeof(net.ssid) - 1);
	net.ssid[sizeof(net.ssid) - 1] = '\0';

	(void)strncpy(net.psk, psk ? psk : "", sizeof(net.psk) - 1);
	net.psk[sizeof(net.psk) - 1] = '\0';

	(void)settings_save_one("net/ssid", net.ssid, strlen(net.ssid) + 1);
	(void)settings_save_one("net/psk", net.psk, strlen(net.psk) + 1);

	return associate();
}

int iot_init(void)
{
	/*
	 * A per-boot token rather than a stored password.  It is not
	 * cryptographic authentication and does not pretend to be - it is the
	 * "nobody else on this Wi-Fi accidentally or casually POSTs a setpoint"
	 * bar, which is the bar U3 (LAN only) actually sets.  It is shown on
	 * the panel, so physical access is what grants control access.
	 */
	(void)snprintk(net.token, sizeof(net.token), "%08x", sys_rand32_get());

#if defined(CONFIG_NET_L2_WIFI_MGMT)
	net_mgmt_init_event_callback(&wifi_cb, on_wifi_event,
				     NET_EVENT_WIFI_CONNECT_RESULT |
					     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);

	net_mgmt_init_event_callback(&ipv4_cb, on_ipv4_event,
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);
#endif

	/* Credentials were already loaded by calibration_init()'s
	 * settings_load(); this handler shares that pass. */
	LOG_INF("HTTP dashboard on port %u, control token %s",
		http_port, net.token);

	return associate();
}

bool iot_connected(void)      { return net.connected; }
const char *iot_ip(void)      { return net.ip; }
const char *iot_ssid(void)    { return net.ssid; }
const char *iot_token(void)   { return net.token; }

#else /* !CONFIG_HTTP_SERVER */

int iot_init(void) { return -ENOTSUP; }
int iot_set_credentials(const char *ssid, const char *psk)
{
	ARG_UNUSED(ssid); ARG_UNUSED(psk);
	return -ENOTSUP;
}
bool iot_connected(void)    { return false; }
const char *iot_ip(void)    { return ""; }
const char *iot_ssid(void)  { return ""; }
const char *iot_token(void) { return ""; }
int iot_snapshot_json(char *buf, size_t buf_len)
{
	ARG_UNUSED(buf); ARG_UNUSED(buf_len);
	return 0;
}

#endif /* CONFIG_HTTP_SERVER */
