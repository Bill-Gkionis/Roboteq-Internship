/*
 * system.c - supervisor state machine, safety thread, POST, faults.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/sys/atomic.h>

#include "system.h"
#include "model_gen.h"
#include "control-system.h"
#include "temp-sense.h"
#include "power-sense.h"
#include "water-loop.h"
#include "heaters.h"
#include "fans.h"
#include "calibration.h"
#include "iot.h"

LOG_MODULE_REGISTER(sys, LOG_LEVEL_INF);

/* ===================================================== the shared cluster == */

static struct cal_snapshot snap;
static K_MUTEX_DEFINE(snap_lock);

void sys_snapshot_get(struct cal_snapshot *out)
{
	k_mutex_lock(&snap_lock, K_FOREVER);
	*out = snap;
	k_mutex_unlock(&snap_lock);
}

void sys_snapshot_set(const struct cal_snapshot *in)
{
	k_mutex_lock(&snap_lock, K_FOREVER);
	snap = *in;
	k_mutex_unlock(&snap_lock);
}

/* ============================================================== faults ==== */

static atomic_t fault_word;

struct fault_name { uint32_t bit; const char *name; };

static const struct fault_name fault_names[] = {
	{ FAULT_ALERT_PIN,      "OVERCURRENT (ALERT)" },
	{ FAULT_OVERTEMP,       "OVER TEMPERATURE" },
	{ FAULT_SENSOR_STALE,   "SENSOR STALE" },
	{ FAULT_SENSOR_DEAD,    "SENSOR DEAD" },
	{ FAULT_CTRL_STALLED,   "CONTROL STALLED" },
	{ FAULT_NO_FLOW,        "NO FLOW" },
	{ FAULT_POST_FAILED,    "SELF TEST FAILED" },
	{ FAULT_INTEGRATOR_PIN, "GUARD SATURATED" },
	{ FAULT_FLOW_MISMATCH,  "FLOW MISMATCH" },
	{ FAULT_NULL_BREACH,    "NULL GATE BREACH" },
	{ FAULT_FAN_HEALTH,     "FAN STOPPED" },
	{ FAULT_BOARD_OVERTEMP, "BOARD OVER TEMPERATURE" },
};

void sys_fault_raise(uint32_t bits, const char *why)
{
	const uint32_t before = (uint32_t)atomic_or(&fault_word, bits);
	const uint32_t added = bits & ~before;

	if (added == 0U) {
		return;   /* already latched - do not spam the log */
	}

	LOG_ERR("FAULT 0x%08x: %s", added, why ? why : "");

	/* A tier-1 fault means outputs off, now, without waiting for the
	 * control thread's next tick. */
	if (added & FAULT_TIER1_MASK) {
		heaters_all_off();
		water_loop_stop();
	}
}

uint32_t sys_faults(void)
{
	return (uint32_t)atomic_get(&fault_word);
}

void sys_fault_clear(void)
{
	atomic_clear(&fault_word);
	LOG_WRN("faults cleared by operator");
}

const char *sys_fault_name(uint32_t faults)
{
	for (size_t i = 0; i < ARRAY_SIZE(fault_names); i++) {
		if (faults & fault_names[i].bit) {
			return fault_names[i].name;
		}
	}
	return "";
}

/* ================================================== hardware ALERT input == */

/* Optional: a carrier board may not have a pin to spare for it, in which case
 * the software backstop in the safety thread is the only over-current check
 * and says so at boot. */
static const struct gpio_dt_spec alert_pin =
	GPIO_DT_SPEC_GET_OR(DT_NODELABEL(cal_io), alert_gpios, {0});
static struct gpio_callback alert_cb;

static void alert_isr(const struct device *port, struct gpio_callback *cb,
		      uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	/* Deliberately minimal: the INA226 already made the decision in
	 * hardware.  All this does is latch it and kill the outputs. */
	sys_fault_raise(FAULT_ALERT_PIN, "INA226 over-current");
}

/* ========================================================= physical button */

/*
 * On a HEADLESS build the DevKitC's BOOT button is the rig's only physical
 * control, and STOP is the right thing to put on it: it is the one command
 * that is always safe, always allowed, and the one an operator reaches for
 * without reading a screen.
 *
 * Not compiled when there is a panel: on the LCD dev kit the same button is
 * LVGL's tab-cycling key, and stopping the run every time someone changes
 * screen would be a memorable bug.
 */
#if defined(CONFIG_INPUT) && !defined(CONFIG_LVGL)
#include <zephyr/input/input.h>

static void button_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY || evt->code != INPUT_KEY_0 ||
	    evt->value == 0) {
		return;   /* release, or some other key */
	}

	const struct cal_cmd c = {
		.type = CAL_CMD_STOP,
		.origin = "button",
	};

	(void)sys_cmd_submit(&c);
}

INPUT_CALLBACK_DEFINE(NULL, button_cb, NULL);
#endif

/* ================================================================ watchdog */

#if DT_HAS_ALIAS(watchdog0)
static const struct device *const wdt_dev = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int wdt_channel = -1;
#endif

static void watchdog_start(void)
{
#if DT_HAS_ALIAS(watchdog0)
	if (!device_is_ready(wdt_dev)) {
		LOG_WRN("watchdog not ready - running without it");
		return;
	}

	struct wdt_timeout_cfg cfg = {
		.flags = WDT_FLAG_RESET_SOC,
		/* Generous: the safety thread runs at 10 Hz, so 3 s is 30
		 * missed cycles.  The point is to catch a hang, not to police
		 * jitter. */
		.window.min = 0U,
		.window.max = 3000U,
	};

	wdt_channel = wdt_install_timeout(wdt_dev, &cfg);
	if (wdt_channel < 0) {
		LOG_WRN("wdt_install_timeout failed (%d)", wdt_channel);
		return;
	}

	if (wdt_setup(wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG) != 0) {
		LOG_WRN("wdt_setup failed - running without the watchdog");
		wdt_channel = -1;
		return;
	}

	LOG_INF("watchdog armed, 3 s window");
#endif
}

static void watchdog_feed(void)
{
#if DT_HAS_ALIAS(watchdog0)
	if (wdt_channel >= 0) {
		(void)wdt_feed(wdt_dev, wdt_channel);
	}
#endif
}

/* ============================================================ heartbeat === */

static atomic_t ctrl_beat;

void sys_ctrl_heartbeat(void)
{
	atomic_set(&ctrl_beat, (atomic_val_t)k_uptime_get_32());
}

/* ============================================================== interlock = */

static atomic_t interlock_ok;

bool sys_interlock_eval(const struct cal_snapshot *s)
{
	const int64_t now = k_uptime_get();
	bool ok = true;
	const char *why = NULL;

	if (sys_faults() != 0U) {
		ok = false;
		why = "latched fault";
	} else if (!meas_ok(&s->t_inner, now) || !meas_ok(&s->t_guard, now)) {
		/* THE CLAUSE PEOPLE LEAVE OUT.  A hung bus reports the last
		 * value forever; without this the guard PI integrates against a
		 * plausible constant while the real chamber climbs. */
		ok = false;
		why = "chamber sensor stale";
	} else if (s->t_inner.v > CAL_T_INNER_MAX ||
		   s->t_guard.v > CAL_T_GUARD_MAX) {
		ok = false;
		why = "node over temperature";
	} else if (!meas_ok(&s->flow, now) || s->flow.v < CAL_FLOW_MIN_ML) {
		ok = false;
		why = "flow below minimum";
	}

	if (!ok && atomic_get(&interlock_ok)) {
		LOG_WRN("heater interlock OPENED: %s", why);
	}

	atomic_set(&interlock_ok, ok ? 1 : 0);
	heaters_set_enable(ok);

	return ok;
}

/* ========================================================= safety thread == */

#define SAFETY_STACK 1536
#define SAFETY_PRIO  2      /* highest application priority */

static void safety_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	struct cal_snapshot s;

	while (1) {
		/*
		 * NOTHING IN THIS LOOP TOUCHES A BUS.  Only the cached
		 * snapshot, the fault word and (via the ISR above) a GPIO.
		 */
		sys_snapshot_get(&s);

		const int64_t now = k_uptime_get();

		/* --- over-temperature, on cached values ------------------- */
		if ((s.t_inner.flags & MEAS_VALID) &&
		    s.t_inner.v > CAL_T_INNER_MAX) {
			sys_fault_raise(FAULT_OVERTEMP, "inner chamber");
		}
		if ((s.t_guard.flags & MEAS_VALID) &&
		    s.t_guard.v > CAL_T_GUARD_MAX) {
			sys_fault_raise(FAULT_OVERTEMP, "guard gap");
		}
		if ((s.t_water_out.flags & MEAS_VALID) &&
		    s.t_water_out.v > CAL_T_WATER_MAX) {
			sys_fault_raise(FAULT_OVERTEMP, "water outlet");
		}
		if ((s.t_board.flags & MEAS_VALID) &&
		    s.t_board.v > CAL_T_BOARD_MAX) {
			sys_fault_raise(FAULT_BOARD_OVERTEMP,
					"MOSFET heatsink");
		}

		/*
		 * Software over-current backstop.  The hardware ALERT is the
		 * fast line; this is the one that still exists on a board that
		 * did not route it, and the one that covers the INA3221 rails,
		 * which have no ALERT pin at all.  It reads only CACHED values,
		 * so it stays bus-free.
		 */
		{
			const enum power_rail oc = power_sense_overcurrent();

			if (oc != PWR_COUNT) {
				sys_fault_raise(FAULT_ALERT_PIN,
						power_sense_name(oc));
			}
		}

		/* --- staleness -------------------------------------------- */
		if (s.tick > CAL_STALE_TICKS && heaters_enabled()) {
			if (!meas_ok(&s.t_inner, now) ||
			    !meas_ok(&s.t_guard, now)) {
				sys_fault_raise(FAULT_SENSOR_STALE,
						"chamber sensor froze while heating");
			}
		}

		/* --- control-thread liveness ------------------------------ */
		const uint32_t beat = (uint32_t)atomic_get(&ctrl_beat);
		const uint32_t age = k_uptime_get_32() - beat;

		if (beat != 0U && age > (CAL_STALE_TICKS * CAL_TICK_MS)) {
			sys_fault_raise(FAULT_CTRL_STALLED, "no tick");
		} else {
			/* The watchdog is fed ONLY when the control thread has
			 * checked in.  A hung ctrl therefore resets the board. */
			watchdog_feed();
		}

		k_msleep(CAL_SAFETY_TICK_MS);
	}
}

K_THREAD_DEFINE(safety_tid, SAFETY_STACK, safety_thread, NULL, NULL, NULL,
		SAFETY_PRIO, 0, 0);

/* ========================================================= command queue == */

K_MSGQ_DEFINE(cmd_q, sizeof(struct cal_cmd), 8, 4);

/*
 * STOP gets a flag of its own as well as a queue slot.  Peeking the head of the
 * queue would miss a STOP that arrived behind three other commands, and "stop"
 * is the one request that must never wait its turn.
 */
static atomic_t stop_requested;

int sys_cmd_submit(const struct cal_cmd *cmd)
{
	if (cmd == NULL || cmd->type == CAL_CMD_NONE) {
		return -EINVAL;
	}

	/* Refusals that belong to the door, not to the caller. */
	if (cmd->type != CAL_CMD_CLEAR_FAULT && cmd->type != CAL_CMD_STOP &&
	    sys_faults() != 0U) {
		LOG_WRN("command %d from %s refused: fault latched",
			(int)cmd->type, cmd->origin ? cmd->origin : "?");
		return -EACCES;
	}

	LOG_INF("cmd %d (arg %.2f) from %s", (int)cmd->type, (double)cmd->arg,
		cmd->origin ? cmd->origin : "?");

	if (cmd->type == CAL_CMD_STOP) {
		atomic_set(&stop_requested, 1);
		return 0;
	}

	return k_msgq_put(&cmd_q, cmd, K_NO_WAIT);
}

bool sys_cmd_pop(struct cal_cmd *out)
{
	return k_msgq_get(&cmd_q, out, K_NO_WAIT) == 0;
}

/* ============================================================ POST ======== */

/*
 * The power-on self test.  Every row is "energise a group, wait out the
 * inrush, check the steady current against an expectation".  The blanking
 * column is not optional: fan and heater inrush will trip a naive threshold.
 */
struct post_step {
	const char *what;
	int blank_ms;
	int (*action)(void);
	int (*check)(void);
};

static int post_none(void) { return 0; }

static int post_isothermal(void)
{
	/*
	 * Before anything is powered the rig genuinely IS isothermal.  That is
	 * a free and extremely strong reference: it catches a swapped sensor, a
	 * duplicated address, a mis-wired RTD leg or a dead bus, for the price
	 * of a short delay.  It is also the short form of the calibration soak,
	 * so it is written once and used twice.
	 */
	temp_sense_read_all();
	water_loop_read(1.0f);

	const int64_t now = k_uptime_get();
	float lo = 1.0e6f, hi = -1.0e6f;
	int n = 0;

	/* Every individual probe, not the zone means: a swapped or duplicated
	 * sensor inside one zone would average away into a plausible mean. */
	for (int z = 0; z < TEMP_CH_COUNT; z++) {
		for (int i = 0; i < temp_sense_sensor_count(z); i++) {
			float v;

			if (!temp_sense_raw((enum temp_channel)z, i, &v)) {
				LOG_ERR("POST: zone '%s' sensor %d did not read",
					temp_sense_name((enum temp_channel)z), i);
				return -EIO;
			}
			lo = MIN(lo, v);
			hi = MAX(hi, v);
			n++;
		}
	}

	if (n == 0) {
		LOG_ERR("POST: no chamber sensors at all");
		return -EIO;
	}

	struct meas wi = water_loop_t_in();
	struct meas wo = water_loop_t_out();

	if (meas_ok(&wi, now)) {
		lo = MIN(lo, wi.v);
		hi = MAX(hi, wi.v);
	}
	if (meas_ok(&wo, now)) {
		lo = MIN(lo, wo.v);
		hi = MAX(hi, wo.v);
	}

	LOG_INF("POST: isothermal spread %.3f K across %d probes "
		"(%.2f .. %.2f degC)", (double)(hi - lo), n,
		(double)lo, (double)hi);

	if ((hi - lo) > 2.0f) {
		LOG_ERR("POST: sensors disagree by more than 2 K - check wiring");
		return -EIO;
	}

	return 0;
}

static int post_fan_inner(void)  { return fans_set(FAN_INNER, 0.6f); }
static int post_fan_guard(void)  { return fans_set(FAN_GUARD, 0.6f); }
static int post_fan_reject(void) { return fans_set(FAN_REJECT, 0.6f); }

/**
 * Check one fan group: draw current, and turn.  Also learns the group's
 * current signature, which is the only signal that can later see ONE stopped
 * fan in a PST daisy chain.
 */
static int post_check_fan(enum fan_group g, enum power_rail rail)
{
	/* One second of running before judging: fans spin up slowly and the
	 * tach needs a window to accumulate edges in. */
	for (int i = 0; i < 5; i++) {
		k_msleep(200);
		fans_service(0.2f);
	}
	power_sense_read_all();

	const float i_a = (rail < PWR_COUNT) ? power_sense_current(rail) : -1.0f;
	const float rpm = fans_rpm(g);

	LOG_INF("POST: fan '%s' %.3f A, %.0f rpm", fans_name(g),
		(double)i_a, (double)rpm);

	if (rail < PWR_COUNT) {
		if (i_a < 0.05f) {
			LOG_ERR("POST: fan '%s' draws no current", fans_name(g));
			return -EIO;
		}
		fans_learn_signature(g, i_a);
	}

	if (fans_has_tach(g) && rpm < 60.0f) {
		LOG_ERR("POST: fan '%s' is not turning", fans_name(g));
		return -EIO;
	}

	return 0;
}

static int post_check_fan_inner(void)
{
	return post_check_fan(FAN_INNER, PWR_FAN_INNER);
}

static int post_check_fan_guard(void)
{
	return post_check_fan(FAN_GUARD, PWR_FAN_GUARD);
}

static int post_check_fan_reject(void)
{
	return post_check_fan(FAN_REJECT, PWR_COUNT);
}

/**
 * The stepper driver's version byte.  This one is worth having as its own POST
 * row because it is DEFINITIVE: a wrong value means the SPI link is wrong, and
 * every current setting written over it is then meaningless.
 */
static int post_check_stepper(void)
{
	const int ver = water_loop_pump_driver_version();

	if (ver < 0) {
		LOG_ERR("POST: stepper driver SPI failed (%d)", ver);
		return -EIO;
	}

	LOG_INF("POST: stepper driver version 0x%02x", ver);

	if (ver != 0x30) {
		LOG_ERR("POST: not a TMC5160 - check CS, MISO and SPI mode");
		return -EIO;
	}

	return 0;
}

static int post_pump(void)
{
	water_loop_set_flow(50.0f);
	return 0;
}

static int post_check_pump(void)
{
	/* Give the turbine a couple of seconds of flow to produce pulses. */
	for (int i = 0; i < 4; i++) {
		k_msleep(500);
		water_loop_read(0.5f);
	}

	struct meas f = water_loop_flow();

	LOG_INF("POST: turbine reads %.1f mL/min at a 50 mL/min command",
		(double)f.v);

	water_loop_stop();

	if (!meas_ok(&f, k_uptime_get()) || f.v < 5.0f) {
		LOG_ERR("POST: no flow seen - air lock, slipped tube or dead sensor");
		return -EIO;
	}

	return 0;
}

static int post_heater_inner(void)
{
	heaters_set_enable(true);
	heaters_set_power(HEATER_INNER, 10.0f);
	for (int i = 0; i < 5; i++) {
		k_msleep(200);
		power_sense_read_all();
		heaters_service(0.2f);
	}
	return 0;
}

static int post_check_heater_inner(void)
{
	const float p = power_sense_get(PWR_HEAT_INNER).v;

	heaters_set_power(HEATER_INNER, 0.0f);
	heaters_set_enable(false);

	LOG_INF("POST: inner heater drew %.2f W at a 10 W demand", (double)p);

	if (p < 2.0f) {
		LOG_ERR("POST: inner heater drew nothing - gate, fuse or cutout");
		return -EIO;
	}

	return 0;
}

static const struct post_step post_steps[] = {
	{ "isothermal plausibility", 0,    post_none,         post_isothermal },
	{ "stepper driver link",     0,    post_none,         post_check_stepper },
	{ "inner fans",              300,  post_fan_inner,    post_check_fan_inner },
	{ "guard fans",              300,  post_fan_guard,    post_check_fan_guard },
	{ "reject fans",             300,  post_fan_reject,   post_check_fan_reject },
	{ "pump + turbine",          0,    post_pump,         post_check_pump },
	{ "inner heater",            0,    post_heater_inner, post_check_heater_inner },
};

int sys_post_run(void)
{
	int failures = 0;

	LOG_INF("---- POST ----");

	for (size_t i = 0; i < ARRAY_SIZE(post_steps); i++) {
		const struct post_step *st = &post_steps[i];
		int rc = st->action();

		if (st->blank_ms) {
			k_msleep(st->blank_ms);   /* ride out the inrush */
		}
		if (rc == 0) {
			rc = st->check();
		}

		LOG_INF("POST %u/%u  %-26s %s", (unsigned)(i + 1),
			(unsigned)ARRAY_SIZE(post_steps), st->what,
			rc == 0 ? "PASS" : "FAIL");

		if (rc != 0) {
			failures++;
		}
	}

	fans_all_off();
	heaters_all_off();

	if (failures) {
		sys_fault_raise(FAULT_POST_FAILED, "see the POST log above");
	}

	LOG_INF("---- POST %s (%d failed) ----",
		failures ? "FAILED" : "passed", failures);

	return failures ? -EIO : 0;
}

/* ========================================================== supervisor ==== */

/*
 * A state machine, not a controller.  Everything that can invalidate a
 * measurement point is a transition here rather than a matter of discipline.
 */
enum sup_idx {
	S_BOOT = 0, S_SELFTEST, S_IDLE, S_SET_RANGE, S_PREHEAT,
	S_SETTLE, S_DWELL, S_LOG, S_CAL, S_FAULT, S_COUNT
};

struct sup_obj {
	struct smf_ctx ctx;
	struct cal_snapshot *s;   /* the tick's snapshot, borrowed */
	float dt;
	int elapsed_s;
	struct cal_stat point;
};

static struct sup_obj sup;
static const struct smf_state sup_states[S_COUNT];
static enum cal_state sup_public_state = CAL_ST_BOOT;

static void goto_state(enum sup_idx i, enum cal_state pub)
{
	sup_public_state = pub;
	sup.elapsed_s = 0;
	smf_set_state(SMF_CTX(&sup), &sup_states[i]);
}

/* --- BOOT ---------------------------------------------------------------- */
static enum smf_state_result boot_run(void *o)
{
	ARG_UNUSED(o);
	goto_state(S_SELFTEST, CAL_ST_SELFTEST);

	return SMF_EVENT_HANDLED;
}

/* --- SELFTEST ------------------------------------------------------------ */
static enum smf_state_result selftest_run(void *o)
{
	ARG_UNUSED(o);
	/* POST is run once, synchronously, from main() before the loop starts;
	 * by the time the supervisor gets here the verdict is already in the
	 * fault word. */
	if (sys_faults() & FAULT_POST_FAILED) {
		goto_state(S_FAULT, CAL_ST_FAULT);
	} else {
		goto_state(S_IDLE, CAL_ST_IDLE);
	}

	return SMF_EVENT_HANDLED;
}

/* --- IDLE ---------------------------------------------------------------- */
static void idle_entry(void *o)
{
	ARG_UNUSED(o);
	heaters_all_off();
	water_loop_stop();
	fans_freeze_inner(false);
	fans_all_off();
}

static enum smf_state_result idle_run(void *o)
{
	struct sup_obj *s = o;
	struct cal_cmd cmd;

	while (sys_cmd_pop(&cmd)) {
		switch (cmd.type) {
		case CAL_CMD_START:
			goto_state(S_SET_RANGE, CAL_ST_SET_RANGE);
			return SMF_EVENT_HANDLED;
		case CAL_CMD_SET_TSS:
			s->s->t_inner_set = cmd.arg;
			break;
		case CAL_CMD_SET_DTW:
			s->s->dt_water_set = cmd.arg;
			break;
		case CAL_CMD_CAL_SOAK:
		case CAL_CMD_CAL_TARE:
		case CAL_CMD_CAL_SUBST:
			calibration_request(cmd.type, cmd.arg);
			goto_state(S_CAL, CAL_ST_CAL_SOAK);
			return SMF_EVENT_HANDLED;
		case CAL_CMD_CLEAR_FAULT:
			sys_fault_clear();
			break;
		default:
			break;
		}
	}

	return SMF_EVENT_HANDLED;
}

/* --- SET_RANGE ----------------------------------------------------------- */
static void set_range_entry(void *o)
{
	struct sup_obj *s = o;
	struct cal_snapshot *sn = s->s;

	/*
	 * Ranging needs an estimate of the load about to be measured, and the
	 * best one available is the previous point - the campaign sweeps a DUT
	 * whose loss moves gradually.  With no history, start in the middle of
	 * the 15-200 W envelope; the first point simply re-ranges once the
	 * reading is real.
	 *
	 * Getting it wrong is cheap: auto-ranging is FREE, because the water
	 * setpoint has exactly zero DC gain to the reading's error.  A bad
	 * range costs transient and schedule, never accuracy.
	 */
	const float p_expect = sn->last_point_valid ? sn->last_point_w : 100.0f;
	const struct cal_range *r = cal_range_for(p_expect);

	if (sn->dt_water_set <= 0.0f) {
		sn->dt_water_set = r->dt_set;
	}

	const float t_mean = (sn->t_water_in.flags & MEAS_VALID)
				     ? sn->t_water_in.v
				     : 25.0f;
	const float flow = cal_flow_for_point(p_expect, sn->dt_water_set,
					      t_mean);

	water_loop_set_flow(flow);
	water_loop_set_tare(calibration_tare_for_flow(flow));

	/* The inner fans enforce the lump model, so their duty is set once
	 * here and frozen for the whole campaign. */
	(void)fans_set(FAN_INNER, 0.7f);
	fans_freeze_inner(true);
	(void)fans_set(FAN_GUARD, 0.5f);
	(void)fans_set(FAN_REJECT, CAL_REJECT_FAN_MIN);

	LOG_INF("range set: dT %.1f K, flow %.1f mL/min",
		(double)sn->dt_water_set, (double)flow);
}

static enum smf_state_result set_range_run(void *o)
{
	struct sup_obj *s = o;

	/* Give the pump a few seconds to establish flow before asking the
	 * interlock to believe in it. */
	if (s->elapsed_s >= 10) {
		goto_state(S_PREHEAT, CAL_ST_PREHEAT);
	}

	return SMF_EVENT_HANDLED;
}

/* --- PREHEAT ------------------------------------------------------------- */
static enum smf_state_result preheat_run(void *o)
{
	struct sup_obj *s = o;
	struct cal_snapshot *sn = s->s;

	/* The chamber is driven to its setpoint with the inner (calibration)
	 * resistor.  Those watts are inside B1 and metered, so the model gives
	 * them exactly zero DC gain to the reading - the preheat cannot bias
	 * the answer, only the schedule. */
	if (sn->t_inner_set > 0.0f &&
	    (sn->t_inner.flags & MEAS_VALID) &&
	    sn->t_inner.v < (sn->t_inner_set - 0.5f)) {
		return SMF_EVENT_HANDLED;   /* still climbing */
	}

	goto_state(S_SETTLE, CAL_ST_SETTLE);

	return SMF_EVENT_HANDLED;
}

/* --- SETTLE -------------------------------------------------------------- */
static void settle_entry(void *o)
{
	struct sup_obj *s = o;

	cal_stat_reset(&s->point);
}

static enum smf_state_result settle_run(void *o)
{
	struct sup_obj *s = o;
	struct cal_snapshot *sn = s->s;

	sn->settle_s = s->elapsed_s;

	/*
	 * A transient null-gate breach is NORMAL at and above 150 W: |e| peaks
	 * at 1.91 K against a 1.79 K gate because the reference moves 20x
	 * faster than the loop that has to follow it.  It costs schedule, not
	 * accuracy, and the gates already forbid logging while it lasts.  Only
	 * a breach that survives past the settling budget is a fault - get
	 * that distinction wrong and the first run faults out on physics.
	 */
	if (s->elapsed_s > CAL_SETTLE_BUDGET_S && !sn->gate_null) {
		sys_fault_raise(FAULT_NULL_BREACH,
				"null gate still open past the settling budget");
		goto_state(S_FAULT, CAL_ST_FAULT);
		return SMF_EVENT_HANDLED;
	}

	if (sn->gate_null && sn->gate_steady) {
		goto_state(S_DWELL, CAL_ST_DWELL);
	}

	return SMF_EVENT_HANDLED;
}

/* --- DWELL --------------------------------------------------------------- */
static void dwell_entry(void *o)
{
	struct sup_obj *s = o;

	cal_stat_reset(&s->point);
}

static enum smf_state_result dwell_run(void *o)
{
	struct sup_obj *s = o;
	struct cal_snapshot *sn = s->s;

	if (!sn->gate_null || !sn->gate_steady) {
		LOG_WRN("dwell interrupted - a gate went red, re-settling");
		goto_state(S_SETTLE, CAL_ST_SETTLE);
		return SMF_EVENT_HANDLED;
	}

	cal_stat_push(&s->point, sn->p_meas);
	sn->dwell_s = s->elapsed_s;

	if (s->elapsed_s >= CAL_DWELL_S) {
		goto_state(S_LOG, CAL_ST_LOG);
	}

	return SMF_EVENT_HANDLED;
}

/* --- LOG ----------------------------------------------------------------- */
static void log_entry(void *o)
{
	struct sup_obj *s = o;
	struct cal_snapshot *sn = s->s;

	sn->last_point_w = cal_stat_mean(&s->point);
	sn->last_point_se = cal_stat_stderr(&s->point);
	sn->last_point_valid = true;

	/*
	 * A point without its uncertainty is not a measurement.  Everything
	 * needed to defend this number later goes on one line: the value, its
	 * standard error, how hard the guard was working, what the flow and
	 * tare were, and the hash of the parameter set the firmware was built
	 * from.
	 */
	LOG_INF("POINT  P = %.3f +- %.3f W  |  |e| %.3f K  slope %.2e W/s  "
		"flow %.1f mL/min  tare %+.4f K  P_aux %.2f W  params %s  fw %s",
		(double)sn->last_point_w, (double)sn->last_point_se,
		(double)sn->e_null, (double)sn->p_slope,
		(double)sn->flow.v, (double)water_loop_get_tare(),
		(double)sn->p_aux, CAL_PARAMS_HASH, APP_VERSION_STR);
}

static enum smf_state_result log_run(void *o)
{
	ARG_UNUSED(o);
	goto_state(S_IDLE, CAL_ST_IDLE);

	return SMF_EVENT_HANDLED;
}

/* --- CAL ----------------------------------------------------------------- */
static enum smf_state_result cal_run(void *o)
{
	struct sup_obj *s = o;

	if (calibration_service(s->s, s->dt)) {
		goto_state(S_IDLE, CAL_ST_IDLE);
	}

	return SMF_EVENT_HANDLED;
}

/* --- FAULT --------------------------------------------------------------- */
static void fault_entry(void *o)
{
	ARG_UNUSED(o);
	heaters_all_off();
	water_loop_stop();
	fans_all_off();
	LOG_ERR("FAULT state entered: %s", sys_fault_name(sys_faults()));
}

static enum smf_state_result fault_run(void *o)
{
	ARG_UNUSED(o);

	struct cal_cmd cmd;

	while (sys_cmd_pop(&cmd)) {
		if (cmd.type == CAL_CMD_CLEAR_FAULT) {
			sys_fault_clear();
			goto_state(S_IDLE, CAL_ST_IDLE);
			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_HANDLED;
}

static const struct smf_state sup_states[S_COUNT] = {
	[S_BOOT]      = SMF_CREATE_STATE(NULL, boot_run, NULL, NULL, NULL),
	[S_SELFTEST]  = SMF_CREATE_STATE(NULL, selftest_run, NULL, NULL, NULL),
	[S_IDLE]      = SMF_CREATE_STATE(idle_entry, idle_run, NULL, NULL, NULL),
	[S_SET_RANGE] = SMF_CREATE_STATE(set_range_entry, set_range_run, NULL,
					 NULL, NULL),
	[S_PREHEAT]   = SMF_CREATE_STATE(NULL, preheat_run, NULL, NULL, NULL),
	[S_SETTLE]    = SMF_CREATE_STATE(settle_entry, settle_run, NULL, NULL,
					 NULL),
	[S_DWELL]     = SMF_CREATE_STATE(dwell_entry, dwell_run, NULL, NULL,
					 NULL),
	[S_LOG]       = SMF_CREATE_STATE(log_entry, log_run, NULL, NULL, NULL),
	[S_CAL]       = SMF_CREATE_STATE(NULL, cal_run, NULL, NULL, NULL),
	[S_FAULT]     = SMF_CREATE_STATE(fault_entry, fault_run, NULL, NULL,
					 NULL),
};

void sys_supervisor_step(struct cal_snapshot *s, float dt)
{
	sup.s = s;
	sup.dt = dt;
	sup.elapsed_s += (int)(dt + 0.5f);

	/* A tier-1 fault drops the machine into FAULT from wherever it is. */
	if ((sys_faults() & FAULT_TIER1_MASK) &&
	    sup_public_state != CAL_ST_FAULT) {
		goto_state(S_FAULT, CAL_ST_FAULT);
	}

	/* STOP is honoured from every state, always, and never queues behind
	 * anything. */
	if (atomic_cas(&stop_requested, 1, 0) &&
	    sup_public_state != CAL_ST_FAULT) {
		k_msgq_purge(&cmd_q);
		goto_state(S_IDLE, CAL_ST_IDLE);
	}

	(void)smf_run_state(SMF_CTX(&sup));

	s->state = sup_public_state;
}

enum cal_state sys_state(void)
{
	return sup_public_state;
}

const char *cal_state_name(enum cal_state st)
{
	static const char *const names[CAL_ST_COUNT] = {
		[CAL_ST_BOOT]      = "BOOT",
		[CAL_ST_SELFTEST]  = "SELFTEST",
		[CAL_ST_IDLE]      = "IDLE",
		[CAL_ST_CAL_SOAK]  = "CAL SOAK",
		[CAL_ST_CAL_TARE]  = "CAL TARE",
		[CAL_ST_CAL_SUBST] = "CAL SUBST",
		[CAL_ST_SET_RANGE] = "SET RANGE",
		[CAL_ST_PREHEAT]   = "PREHEAT",
		[CAL_ST_SETTLE]    = "SETTLE",
		[CAL_ST_DWELL]     = "DWELL",
		[CAL_ST_LOG]       = "LOG",
		[CAL_ST_FAULT]     = "FAULT",
	};

	if (st < 0 || st >= CAL_ST_COUNT || names[st] == NULL) {
		return "?";
	}
	return names[st];
}

/* =============================================================== init ===== */

int sys_init(void)
{
	if (gpio_is_ready_dt(&alert_pin)) {
		(void)gpio_pin_configure_dt(&alert_pin, GPIO_INPUT);
		(void)gpio_pin_interrupt_configure_dt(&alert_pin,
						      GPIO_INT_EDGE_TO_ACTIVE);
		gpio_init_callback(&alert_cb, alert_isr, BIT(alert_pin.pin));
		(void)gpio_add_callback(alert_pin.port, &alert_cb);
		LOG_INF("hardware ALERT input armed (tier 1 over-current)");
	} else {
		LOG_WRN("no ALERT line on this board - over-current is caught "
			"by the software backstop instead, one tick slower");
	}

	watchdog_start();

	smf_set_initial(SMF_CTX(&sup), &sup_states[S_BOOT]);
	sup_public_state = CAL_ST_BOOT;

	return 0;
}
