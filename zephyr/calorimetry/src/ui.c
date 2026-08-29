/*
 * ui.c - LVGL front panel: RUN / CONTROL / NET.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ui.h"

LOG_MODULE_REGISTER(ui, LOG_LEVEL_INF);

#if defined(CONFIG_LVGL)

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/pwm.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

#include "system.h"
#include "model_gen.h"
#include "control-system.h"
#include "calibration.h"

/* ------------------------------------------------------------- palette --- */
/*
 * A lab instrument that runs for hours in a bright room: dark ground so the
 * numbers carry, one accent per meaning, and nothing decorative.  Every pair
 * below clears 4.5:1 against its background.
 */
#define C_BG      lv_color_hex(0x0E1116)
#define C_SURFACE lv_color_hex(0x171C23)
#define C_LINE    lv_color_hex(0x2A313A)
#define C_TEXT    lv_color_hex(0xE6EDF3)
#define C_MUTED   lv_color_hex(0x8B949E)
#define C_OK      lv_color_hex(0x3FB950)
#define C_WARN    lv_color_hex(0xD29922)
#define C_BAD     lv_color_hex(0xF85149)
#define C_ACCENT  lv_color_hex(0x58A6FF)

#define UI_REFRESH_MS 250   /* 4 Hz redraw of a 1 Hz signal: cheap, and it
			     * keeps button feedback feeling immediate */

/* --------------------------------------------------------------- state --- */

static const struct device *disp_dev;
#if DT_HAS_ALIAS(pwm_led0)
static const struct pwm_dt_spec backlight = PWM_DT_SPEC_GET(DT_ALIAS(pwm_led0));
#endif

static int sleep_timeout_s = 120;    /* the default the operator asked for */
static bool asleep;

static struct {
	lv_obj_t *tabs;
	/* RUN */
	lv_obj_t *lbl_state, *lbl_power, *lbl_se, *lbl_gate_null;
	lv_obj_t *lbl_gate_steady, *lbl_temps, *lbl_dt, *lbl_fault;
	lv_obj_t *bar_hin, *bar_hgrd, *bar_fan, *bar_pump;
	lv_obj_t *lbl_hin, *lbl_hgrd, *lbl_fan, *lbl_pump;
	/* CONTROL */
	lv_obj_t *lbl_tss, *lbl_dtw, *btn_run;
	lv_obj_t *lbl_btn_run;
	/* NET */
	lv_obj_t *ta_ssid, *ta_pass, *kb, *qr, *lbl_net;
} ui;

static char net_ssid[33];
static char net_ip[40] = "not connected";
static bool net_up;

/* ----------------------------------------------------------- helpers ----- */

static lv_obj_t *row_label(lv_obj_t *parent, const char *text,
			   const lv_font_t *font, lv_color_t colour)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_label_set_text(l, text);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, colour, 0);

	return l;
}

/** A labelled progress bar: "GUARD  42 %" with the bar underneath. */
static void make_bar(lv_obj_t *parent, const char *name, lv_color_t colour,
		     lv_obj_t **bar_out, lv_obj_t **lbl_out)
{
	lv_obj_t *row = lv_obj_create(parent);

	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), 26);
	lv_obj_set_style_pad_all(row, 0, 0);

	lv_obj_t *name_lbl = row_label(row, name, &lv_font_montserrat_14, C_MUTED);

	lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

	*lbl_out = row_label(row, "0 %", &lv_font_montserrat_14, C_TEXT);
	lv_obj_align(*lbl_out, LV_ALIGN_TOP_RIGHT, 0, 0);

	lv_obj_t *bar = lv_bar_create(row);

	lv_obj_set_size(bar, LV_PCT(100), 6);
	lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_bar_set_range(bar, 0, 100);
	lv_obj_set_style_bg_color(bar, C_LINE, LV_PART_MAIN);
	lv_obj_set_style_bg_color(bar, colour, LV_PART_INDICATOR);
	*bar_out = bar;
}

static void set_bar(lv_obj_t *bar, lv_obj_t *lbl, float frac)
{
	const int pct = (int)(cal_clampf(frac, 0.0f, 1.0f) * 100.0f + 0.5f);

	lv_bar_set_value(bar, pct, LV_ANIM_OFF);
	lv_label_set_text_fmt(lbl, "%d %%", pct);
}

/* --------------------------------------------------------- command glue -- */

static void submit(enum cal_cmd_type t, float arg)
{
	const struct cal_cmd c = { .type = t, .arg = arg, .origin = "panel" };

	(void)sys_cmd_submit(&c);
}

static void ev_run_stop(lv_event_t *e)
{
	ARG_UNUSED(e);

	/* One button, two meanings, decided by the state machine's own view of
	 * the world rather than by a flag this file keeps. */
	if (sys_state() == CAL_ST_IDLE) {
		submit(CAL_CMD_START, 0.0f);
	} else {
		submit(CAL_CMD_STOP, 0.0f);
	}
}

static void ev_tss(lv_event_t *e)
{
	struct cal_snapshot s;

	sys_snapshot_get(&s);
	submit(CAL_CMD_SET_TSS, s.t_inner_set + (float)(intptr_t)lv_event_get_user_data(e));
}

static void ev_dtw(lv_event_t *e)
{
	struct cal_snapshot s;

	sys_snapshot_get(&s);
	submit(CAL_CMD_SET_DTW, s.dt_water_set + (float)(intptr_t)lv_event_get_user_data(e));
}

static void ev_soak(lv_event_t *e)  { ARG_UNUSED(e); submit(CAL_CMD_CAL_SOAK, 0.0f); }
static void ev_tare(lv_event_t *e)  { ARG_UNUSED(e); submit(CAL_CMD_CAL_TARE, 0.0f); }
static void ev_clear(lv_event_t *e) { ARG_UNUSED(e); submit(CAL_CMD_CLEAR_FAULT, 0.0f); }

static void ev_focus_kb(lv_event_t *e)
{
	lv_obj_t *ta = lv_event_get_target(e);

	/* The on-screen keyboard only appears when a field is touched, because
	 * on a 172 px wide panel it covers everything else. */
	lv_keyboard_set_textarea(ui.kb, ta);
	lv_obj_remove_flag(ui.kb, LV_OBJ_FLAG_HIDDEN);
}

static void ev_kb_done(lv_event_t *e)
{
	ARG_UNUSED(e);
	lv_obj_add_flag(ui.kb, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------- screens --- */

static lv_obj_t *make_page(lv_obj_t *tabs, const char *title)
{
	lv_obj_t *page = lv_tabview_add_tab(tabs, title);

	lv_obj_set_style_bg_color(page, C_BG, 0);
	lv_obj_set_style_pad_all(page, 6, 0);
	lv_obj_set_style_pad_row(page, 4, 0);
	lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);

	return page;
}

static void build_run(lv_obj_t *tabs)
{
	lv_obj_t *p = make_page(tabs, "RUN");

	ui.lbl_state = row_label(p, "BOOT", &lv_font_montserrat_14, C_ACCENT);

	/* The reading, as big as the panel allows.  It is the one number the
	 * whole instrument exists to produce. */
	ui.lbl_power = row_label(p, "--.- W", &lv_font_montserrat_28, C_TEXT);
	ui.lbl_se = row_label(p, "+- --- W", &lv_font_montserrat_14, C_MUTED);

	ui.lbl_gate_null = row_label(p, "null  ---", &lv_font_montserrat_14, C_MUTED);
	ui.lbl_gate_steady = row_label(p, "steady ---", &lv_font_montserrat_14, C_MUTED);

	ui.lbl_temps = row_label(p, "in --.- C  gd --.- C", &lv_font_montserrat_14, C_TEXT);
	ui.lbl_dt = row_label(p, "dT --.--- K", &lv_font_montserrat_14, C_TEXT);

	make_bar(p, "heat in",  C_BAD,    &ui.bar_hin,  &ui.lbl_hin);
	make_bar(p, "heat gd",  C_WARN,   &ui.bar_hgrd, &ui.lbl_hgrd);
	make_bar(p, "fans",     C_ACCENT, &ui.bar_fan,  &ui.lbl_fan);
	make_bar(p, "pump",     C_OK,     &ui.bar_pump, &ui.lbl_pump);

	ui.lbl_fault = row_label(p, "", &lv_font_montserrat_14, C_BAD);
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
			     lv_event_cb_t cb, void *user, lv_color_t colour)
{
	lv_obj_t *b = lv_button_create(parent);

	lv_obj_set_size(b, LV_PCT(100), 34);
	lv_obj_set_style_bg_color(b, colour, 0);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);

	lv_obj_t *l = lv_label_create(b);

	lv_label_set_text(l, text);
	lv_obj_center(l);

	return b;
}

static void build_control(lv_obj_t *tabs)
{
	lv_obj_t *p = make_page(tabs, "CTRL");

	ui.lbl_tss = row_label(p, "Tss  --.- C", &lv_font_montserrat_20, C_TEXT);

	lv_obj_t *rowa = lv_obj_create(p);

	lv_obj_remove_style_all(rowa);
	lv_obj_set_size(rowa, LV_PCT(100), 34);
	lv_obj_set_flex_flow(rowa, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(rowa, 6, 0);
	make_button(rowa, "-1", ev_tss, (void *)(intptr_t)-1, C_SURFACE);
	make_button(rowa, "+1", ev_tss, (void *)(intptr_t)1, C_SURFACE);

	ui.lbl_dtw = row_label(p, "dT set --.- K", &lv_font_montserrat_20, C_TEXT);

	lv_obj_t *rowb = lv_obj_create(p);

	lv_obj_remove_style_all(rowb);
	lv_obj_set_size(rowb, LV_PCT(100), 34);
	lv_obj_set_flex_flow(rowb, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(rowb, 6, 0);
	make_button(rowb, "-1", ev_dtw, (void *)(intptr_t)-1, C_SURFACE);
	make_button(rowb, "+1", ev_dtw, (void *)(intptr_t)1, C_SURFACE);

	ui.btn_run = make_button(p, "START", ev_run_stop, NULL, C_OK);
	ui.lbl_btn_run = lv_obj_get_child(ui.btn_run, 0);

	make_button(p, "SOAK cal", ev_soak, NULL, C_SURFACE);
	make_button(p, "TARE cal", ev_tare, NULL, C_SURFACE);
	make_button(p, "clear fault", ev_clear, NULL, C_SURFACE);
}

static void build_net(lv_obj_t *tabs)
{
	lv_obj_t *p = make_page(tabs, "NET");

	ui.ta_ssid = lv_textarea_create(p);
	lv_textarea_set_one_line(ui.ta_ssid, true);
	lv_textarea_set_placeholder_text(ui.ta_ssid, "SSID");
	lv_obj_set_width(ui.ta_ssid, LV_PCT(100));
	lv_obj_add_event_cb(ui.ta_ssid, ev_focus_kb, LV_EVENT_FOCUSED, NULL);

	ui.ta_pass = lv_textarea_create(p);
	lv_textarea_set_one_line(ui.ta_pass, true);
	lv_textarea_set_password_mode(ui.ta_pass, true);
	lv_textarea_set_placeholder_text(ui.ta_pass, "password");
	lv_obj_set_width(ui.ta_pass, LV_PCT(100));
	lv_obj_add_event_cb(ui.ta_pass, ev_focus_kb, LV_EVENT_FOCUSED, NULL);

	/* Scanning the code opens the dashboard; the address underneath is for
	 * anyone typing it into a laptop. */
	ui.qr = lv_qrcode_create(p);
	lv_qrcode_set_size(ui.qr, 110);
	lv_qrcode_set_dark_color(ui.qr, lv_color_black());
	lv_qrcode_set_light_color(ui.qr, lv_color_white());
	lv_obj_set_style_border_width(ui.qr, 4, 0);
	lv_obj_set_style_border_color(ui.qr, lv_color_white(), 0);

	ui.lbl_net = row_label(p, "not connected", &lv_font_montserrat_14, C_MUTED);

	ui.kb = lv_keyboard_create(lv_screen_active());
	lv_obj_add_flag(ui.kb, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(ui.kb, ev_kb_done, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(ui.kb, ev_kb_done, LV_EVENT_CANCEL, NULL);
}

/* -------------------------------------------------------------- refresh -- */

static void backlight_set(bool on)
{
#if DT_HAS_ALIAS(pwm_led0)
	if (pwm_is_ready_dt(&backlight)) {
		(void)pwm_set_pulse_dt(&backlight,
				       on ? backlight.period : 0);
	}
#else
	ARG_UNUSED(on);
#endif
}

static void refresh(void)
{
	struct cal_snapshot s;

	sys_snapshot_get(&s);

	/* --- RUN ---------------------------------------------------------- */
	lv_label_set_text(ui.lbl_state, cal_state_name(s.state));

	lv_label_set_text_fmt(ui.lbl_power, "%.2f W", (double)s.p_meas);
	if (s.last_point_valid) {
		lv_label_set_text_fmt(ui.lbl_se, "last %.3f +- %.3f W",
				      (double)s.last_point_w,
				      (double)s.last_point_se);
	} else {
		lv_label_set_text_fmt(ui.lbl_se, "est %.2f W (observer)",
				      (double)s.p_hat);
	}

	lv_label_set_text_fmt(ui.lbl_gate_null, "null   %s  |e| %.3f K",
			      s.gate_null ? "GREEN" : "red", (double)s.e_null);
	lv_obj_set_style_text_color(ui.lbl_gate_null,
				    s.gate_null ? C_OK : C_MUTED, 0);

	lv_label_set_text_fmt(ui.lbl_gate_steady, "steady %s  %.1e W/s",
			      s.gate_steady ? "GREEN" : "red",
			      (double)s.p_slope);
	lv_obj_set_style_text_color(ui.lbl_gate_steady,
				    s.gate_steady ? C_OK : C_MUTED, 0);

	lv_label_set_text_fmt(ui.lbl_temps, "in %.2f  gd %.2f C",
			      (double)s.t_inner.v, (double)s.t_guard.v);
	lv_label_set_text_fmt(ui.lbl_dt, "dTw %+.3f K  %.0f mL/min",
			      (double)s.dt_water, (double)s.flow.v);

	set_bar(ui.bar_hin, ui.lbl_hin, s.duty_heat_inner);
	set_bar(ui.bar_hgrd, ui.lbl_hgrd, s.duty_heat_guard);
	set_bar(ui.bar_fan, ui.lbl_fan, s.duty_fan_inner);
	set_bar(ui.bar_pump, ui.lbl_pump, s.pump_frac);

	lv_label_set_text(ui.lbl_fault,
			  s.faults ? sys_fault_name(s.faults) : "");

	/* --- CONTROL ------------------------------------------------------- */
	lv_label_set_text_fmt(ui.lbl_tss, "Tss  %.1f C", (double)s.t_inner_set);
	lv_label_set_text_fmt(ui.lbl_dtw, "dT set %.1f K", (double)s.dt_water_set);

	const bool idle = (s.state == CAL_ST_IDLE);

	lv_label_set_text(ui.lbl_btn_run, idle ? "START" : "STOP");
	lv_obj_set_style_bg_color(ui.btn_run, idle ? C_OK : C_BAD, 0);

	/* --- NET ----------------------------------------------------------- */
	lv_label_set_text(ui.lbl_net, net_ip);

	/* --- sleep --------------------------------------------------------- */
	if (sleep_timeout_s > 0) {
		const uint32_t idle_ms = lv_display_get_inactive_time(NULL);
		const bool want_sleep =
			idle_ms > (uint32_t)(sleep_timeout_s * 1000);

		if (want_sleep != asleep) {
			asleep = want_sleep;
			backlight_set(!asleep);
		}
	}
}

/* ---------------------------------------------------------- LVGL thread -- */

#define UI_STACK 6144
#define UI_PRIO  10        /* opportunistic: must never delay the control tick */

static void ui_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	while (1) {
		refresh();
		/* lv_timer_handler() returns how long it is happy to sleep;
		 * capping it keeps the panel responsive without busy waiting. */
		const uint32_t next = lv_timer_handler();

		k_msleep(MIN(next, UI_REFRESH_MS));
	}
}

K_THREAD_STACK_DEFINE(ui_stack, UI_STACK);
static struct k_thread ui_thread_data;

int ui_init(void)
{
	disp_dev = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_display));

	if (disp_dev == NULL || !device_is_ready(disp_dev)) {
		LOG_WRN("no display - running headless");
		return -ENODEV;
	}

	ui.tabs = lv_tabview_create(lv_screen_active());
	lv_tabview_set_tab_bar_size(ui.tabs, 30);
	lv_obj_set_style_bg_color(lv_screen_active(), C_BG, 0);
	lv_obj_set_style_bg_color(ui.tabs, C_BG, 0);

	build_run(ui.tabs);
	build_control(ui.tabs);
	build_net(ui.tabs);

	ui_set_net_status(NULL, NULL, false);

	display_blanking_off(disp_dev);
	backlight_set(true);

	k_thread_create(&ui_thread_data, ui_stack, UI_STACK, ui_thread,
			NULL, NULL, NULL, UI_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&ui_thread_data, "ui");

	LOG_INF("panel up: 3 screens, sleep after %d s", sleep_timeout_s);

	return 0;
}

void ui_set_sleep_timeout(int seconds)
{
	sleep_timeout_s = seconds;
}

int ui_get_sleep_timeout(void)
{
	return sleep_timeout_s;
}

void ui_set_net_status(const char *ssid, const char *ip, bool connected)
{
	net_up = connected;

	if (ssid) {
		(void)strncpy(net_ssid, ssid, sizeof(net_ssid) - 1);
	}

	if (connected && ip) {
		(void)snprintf(net_ip, sizeof(net_ip), "http://%s/", ip);
		if (ui.qr) {
			(void)lv_qrcode_update(ui.qr, net_ip, strlen(net_ip));
		}
	} else {
		(void)strcpy(net_ip, "not connected");
		if (ui.qr) {
			/* An empty code would render as noise; show the panel's
			 * own name instead so the widget still reads as a QR. */
			(void)lv_qrcode_update(ui.qr, "calorimeter", 11);
		}
	}
}

#else /* !CONFIG_LVGL */

int  ui_init(void) { return -ENOTSUP; }
void ui_set_sleep_timeout(int seconds) { ARG_UNUSED(seconds); }
int  ui_get_sleep_timeout(void) { return 0; }
void ui_set_net_status(const char *ssid, const char *ip, bool connected)
{
	ARG_UNUSED(ssid); ARG_UNUSED(ip); ARG_UNUSED(connected);
}

#endif /* CONFIG_LVGL */
