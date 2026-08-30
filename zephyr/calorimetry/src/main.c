/*
 * main.c - boot order and the 1 Hz control tick.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * THE SHAPE OF THIS FILE
 * ----------------------
 * Everything hard lives in a module; this file is the sequence.  Read it top
 * to bottom and you have the whole firmware:
 *
 *   main()      bind hardware, load calibration, run the POST, then loop
 *   tick()      acquire -> meter -> interlock -> control -> actuate ->
 *               supervise -> publish
 *
 * THREADS
 * -------
 * Three, plus whatever LVGL and the network stack start for themselves:
 *
 *   safety   prio 2, 10 Hz, in system.c.  NEVER touches a bus.
 *   ctrl     this file, 1 Hz.  The only writer of actuators and of the
 *            snapshot.
 *   tlm/ui   prio 10+, opportunistic.  Panel and web.  Must never block ctrl.
 *
 * The architecture note proposes a fourth thread (`acq`) at 10 Hz.  It is
 * merged into `ctrl` here, deliberately: `acq` existed to run a 10 Hz A/B chop
 * across a MULTIPLEXED RTD front end, and this build has one converter per
 * probe, so there is nothing to chop.  With that gone the entire bus workload
 * is ~5 ms inside a 1000 ms tick and a separate thread would buy only extra
 * race conditions.  Sensor skew costs 0.34 mW per second of it, so a single
 * shared tick with +-100 ms of internal spread is free.  If the carrier board
 * arrives with a muxed front end, split `acq` back out - the module boundaries
 * already allow it.
 */

#include <math.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include "types.h"
#include "model_gen.h"
#include "system.h"
#include "control-system.h"
#include "temp-sense.h"
#include "power-sense.h"
#include "water-loop.h"
#include "fans.h"
#include "heaters.h"
#include "calibration.h"
#include "ui.h"
#include "iot.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#ifndef APP_VERSION_STR
#define APP_VERSION_STR "0.0.0-unset"
#endif

/* ------------------------------------------------------- control context -- */

static struct cal_pi guard_pi;    /* the accuracy-critical loop            */
static struct cal_pi reject_pi;   /* inlet temperature; buys schedule only */
static struct cal_pi inner_pi;    /* chamber Tss, for "test at 55 degC"    */
static struct cal_gates gates;
static struct cal_observer observer;

static struct cal_snapshot s;

/* The reject loop's target inlet temperature.  A [knob]: it only has to be
 * achievable (i.e. above ambient) and stable.  Its value cannot bias the
 * reading - the water setpoint has exactly zero DC gain to the error. */
static float t_inlet_set = 30.0f;

/* Guard feedforward, slew limited.  Ramped, never stepped: at 150 W a
 * feedforward STEP is itself a disturbance on the leak path and pushes 1 %
 * settling from 9.1 to 13.2 minutes.  (The DUT load is the opposite case - it
 * must be a step, because the observer needs the disturbance constant.) */
static float guard_ff;
#define GUARD_FF_SLEW_W_PER_S 0.05f

static void controllers_init(void)
{
	/* --- guard PI: the gains straight out of the model, in W/K -------- */
	cal_pi_init(&guard_pi, CAL_KP_G, CAL_KI_G, 0.0f, CAL_PE_MAX);

	/* --- reject PI ----------------------------------------------------
	 * Kp = C_r / lambda_r is flow independent; Ki depends on the rail's
	 * time constant C_r/(UA_rej + mcp) and is refreshed per range below.
	 * The output is COOLING watts, mapped to reject fan duty.
	 */
	cal_pi_init(&reject_pi, CAL_KP_R,
		    CAL_KP_R * (CAL_UA_REJ + 15.0f) / CAL_C_R,
		    0.0f, CAL_REJECT_FULL_W);

	/* --- inner chamber PI ---------------------------------------------
	 * The inner node seen from its heater is a first-order lag:
	 *   K = 1/UA_rad = 0.033 K/W,  tau = C_i/UA_rad = 21.7 s
	 * lambda-tuned the same way as the guard, with lambda = 60 s.
	 * These watts sit inside B1 and are metered, so the model gives them
	 * exactly zero DC gain to the reading: the chamber setpoint cannot
	 * bias the answer, only the schedule.
	 */
	const float tau_i = CAL_C_I / CAL_UA_RAD;
	const float k_i = 1.0f / CAL_UA_RAD;
	const float lambda_i = 60.0f;
	const float kp_i = tau_i / (k_i * lambda_i);

	cal_pi_init(&inner_pi, kp_i, kp_i / tau_i, 0.0f, 100.0f);

	cal_gates_init(&gates);
	cal_observer_init(&observer);
}

/* --------------------------------------------------------------- the tick - */

static void acquire(float dt)
{
	const int64_t now = k_uptime_get();

	/* One shared tick.  Nothing here needs a synchronisation fabric: a 1 s
	 * skew between the flow channel and the dT channel is worth 0.34 mW,
	 * and it would take ~5 minutes of skew to reach the 0.1 W promotion
	 * threshold. */
	temp_sense_read_all();
	water_loop_read(dt);
	power_sense_read_all();

	s.t_ms = now;
	s.tick++;

	s.t_inner = temp_sense_get(TEMP_CH_INNER);
	s.t_guard = temp_sense_get(TEMP_CH_GUARD);
	s.t_water_in = water_loop_t_in();
	s.t_water_out = water_loop_t_out();
	s.flow = water_loop_flow();

	s.p_heat_inner = power_sense_get(PWR_HEAT_INNER);
	s.p_heat_guard = power_sense_get(PWR_HEAT_GUARD);
	s.p_fan_inner = power_sense_get(PWR_FAN_INNER);
	s.p_fan_guard = power_sense_get(PWR_FAN_GUARD);
	s.p_pump = power_sense_get(PWR_PUMP);
	s.t_board = temp_sense_board();

	/* Tach edges accumulated over this interval become RPM here. */
	fans_service(dt);
	for (int i = 0; i < FAN_COUNT; i++) {
		s.rpm_fan[i] = fans_rpm((enum fan_group)i);
	}
}

static void meter(void)
{
	/* Everything metered INSIDE boundary B1.  Every watt in here that is
	 * not subtracted would be reported as DUT loss one-for-one. */
	s.p_aux = power_sense_p_aux();

	float mcp = 0.0f;

	s.p_meas = cal_meter_power(s.flow.v, s.t_water_in.v, s.t_water_out.v,
				   water_loop_get_tare(), s.p_aux, &mcp);
	s.mcp = mcp;
	s.mdot = (mcp > 0.0f) ? mcp / cal_cp_w(0.5f * (s.t_water_in.v +
						       s.t_water_out.v))
			      : 0.0f;
	s.dt_water = (s.t_water_out.v - s.t_water_in.v) - water_loop_get_tare();
	s.e_null = s.t_inner.v - s.t_guard.v;
}

static void control(float dt)
{
	const int64_t now = k_uptime_get();
	const bool sensors_ok = meas_ok(&s.t_inner, now) &&
				meas_ok(&s.t_guard, now);

	/* --- the observer, always running, never gating ------------------- */
	s.p_hat = cal_observer_step(&observer, s.t_inner.v, s.t_water_out.v,
				    s.p_meas, dt);

	/* --- the guard loop: THE accuracy ---------------------------------- */
	if (s.heaters_enabled && sensors_ok) {
		const float ff_target = cal_guard_ff(s.t_water_in.v,
						     s.dt_water_set,
						     s.p_hat);

		guard_ff = cal_slew(guard_ff, ff_target,
				    GUARD_FF_SLEW_W_PER_S, dt);

		s.guard_demand = cal_pi_step(&guard_pi, s.e_null, guard_ff, dt);
		heaters_set_power(HEATER_GUARD, s.guard_demand);

		/* The integrator IS the accuracy.  If it is pinned at a rail
		 * the guard cannot reach the null, which means the reading is
		 * biased by an unknown amount - a MEASUREMENT fault, not a
		 * comfort one. */
		if (guard_pi.sat_s > (float)CAL_INTEGRATOR_PIN_S) {
			sys_fault_raise(FAULT_INTEGRATOR_PIN,
					"guard demand pinned - the null cannot be reached");
		}
	} else {
		heaters_set_power(HEATER_GUARD, 0.0f);
		s.guard_demand = 0.0f;
	}
	s.guard_i = guard_pi.i;

	/* --- the inner chamber loop ---------------------------------------- */
	if (s.heaters_enabled && sensors_ok && s.t_inner_set > 0.0f) {
		const float e = s.t_inner_set - s.t_inner.v;

		s.inner_demand = cal_pi_step(&inner_pi, e, 0.0f, dt);
		heaters_set_power(HEATER_INNER, s.inner_demand);
	} else {
		s.inner_demand = 0.0f;
		heaters_set_power(HEATER_INNER, 0.0f);
	}

	/* --- the reject loop: schedule, not accuracy ----------------------- */
	if (meas_ok(&s.t_water_in, now)) {
		/* Ki tracks the rail's time constant, which moves with flow. */
		reject_pi.ki = CAL_KP_R * (CAL_UA_REJ + s.mcp) / CAL_C_R;

		const float e_cool = s.t_water_in.v - t_inlet_set;
		const float cool_w = cal_pi_step(&reject_pi, e_cool, 0.0f, dt);

		s.duty_fan_reject = CAL_REJECT_FAN_MIN +
				    (1.0f - CAL_REJECT_FAN_MIN) *
					    (cool_w / CAL_REJECT_FULL_W);
		s.duty_fan_reject = cal_clampf(s.duty_fan_reject,
					       CAL_REJECT_FAN_MIN, 1.0f);
		(void)fans_set(FAN_REJECT, s.duty_fan_reject);
	}

	/* --- actuators ------------------------------------------------------ */
	heaters_service(dt);
	water_loop_service(dt);

	s.duty_heat_inner = heaters_get_duty(HEATER_INNER);
	s.duty_heat_guard = heaters_get_duty(HEATER_GUARD);
	s.duty_fan_inner = fans_get(FAN_INNER);
	s.duty_fan_guard = fans_get(FAN_GUARD);
	s.pump_frac = water_loop_pump_fraction();
	s.flow_set = water_loop_get_flow_setpoint();

	/* --- the gates ------------------------------------------------------ */
	/*
	 * The steady threshold is P_thr / tau_dom, and tau_dom moves with flow,
	 * so a range change has to re-arm it.  A range change is a "discard and
	 * re-settle" event anyway, which makes it the natural moment to drop
	 * the slope history and reset the observer as well - both are
	 * describing a plant that no longer exists.
	 */
	static float last_mcp;

	if (s.mcp > 0.1f &&
	    (last_mcp <= 0.1f || fabsf(s.mcp - last_mcp) > 0.10f * last_mcp)) {
		cal_gates_set_range(&gates, s.mcp);
		cal_observer_reset(&observer);
		last_mcp = s.mcp;
		LOG_INF("range changed: mcp %.2f W/K, tau_dom %.0f s, "
			"steady gate %.3e W/s (%.2f W/h)",
			(double)s.mcp, (double)cal_tau_dom(s.mcp),
			(double)gates.slope_gate,
			(double)(gates.slope_gate * 3600.0f));
	}

	(void)cal_gates_eval(&gates, s.p_meas, s.e_null, dt);
	s.gate_null = gates.null_ok;
	s.gate_steady = gates.steady_ok;
	s.p_slope = gates.p_slope.slope;
	s.p_slope_gate = gates.slope_gate;

	/* --- fan health ------------------------------------------------------
	 * A stalled inner fan silently breaks the single-node assumption the
	 * whole model rests on, so it is a MEASUREMENT fault: the point is
	 * invalid, even though nothing is unsafe.
	 */
	if (s.tick > 5) {
		const enum fan_group bad = fans_unhealthy();

		if (bad != FAN_COUNT) {
			sys_fault_raise(FAULT_FAN_HEALTH, fans_name(bad));
		}
	}

	/* --- the flow witness ----------------------------------------------- */
	if (s.flow_set > 5.0f && meas_ok(&s.flow, now)) {
		const float rel = fabsf(s.flow.v - s.flow_set) / s.flow_set;

		/* A SILENT flow error is the worst failure this rig can have -
		 * it produces a plausible-looking wrong answer.  A slipped
		 * tube, an air lock or an occlusion all change the pump's
		 * displacement without changing anything else, so the
		 * disagreement between what was commanded and what the turbine
		 * sees is the only thing that makes it loud. */
		if (rel > 0.35f) {
			sys_fault_raise(FAULT_FLOW_MISMATCH,
					"commanded and measured flow disagree");
		}
	}
}

static void tick(float dt)
{
	acquire(dt);
	meter();

	s.faults = sys_faults();
	s.heaters_enabled = sys_interlock_eval(&s);

	control(dt);

	sys_supervisor_step(&s, dt);

	sys_snapshot_set(&s);
	sys_ctrl_heartbeat();
}

/* ------------------------------------------------------------- shell ------ */

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct cal_snapshot c;

	sys_snapshot_get(&c);

	shell_print(sh, "state      %s   faults 0x%08x %s",
		    cal_state_name(c.state), c.faults,
		    sys_fault_name(c.faults));
	shell_print(sh, "P_meas     %8.3f W    P_hat %8.3f W  P_aux %6.2f W",
		    (double)c.p_meas, (double)c.p_hat, (double)c.p_aux);
	shell_print(sh, "T inner    %8.3f C    guard %8.3f C  e = %+.3f K",
		    (double)c.t_inner.v, (double)c.t_guard.v, (double)c.e_null);
	shell_print(sh, "water      in %.3f C  out %.3f C  dT %+.4f K",
		    (double)c.t_water_in.v, (double)c.t_water_out.v,
		    (double)c.dt_water);
	shell_print(sh, "flow       %.1f mL/min (set %.1f)  mcp %.2f W/K",
		    (double)c.flow.v, (double)c.flow_set, (double)c.mcp);
	shell_print(sh, "guard      demand %.2f W  I %.2f W  duty %.1f %%",
		    (double)c.guard_demand, (double)c.guard_i,
		    (double)(c.duty_heat_guard * 100.0f));
	shell_print(sh, "gates      null %s  steady %s (slope %.3e / %.3e W/s)",
		    c.gate_null ? "GREEN" : "red",
		    c.gate_steady ? "GREEN" : "red",
		    (double)c.p_slope, (double)c.p_slope_gate);
	shell_print(sh, "fans       in %.0f  gd %.0f  rej %.0f rpm  "
		    "(-1 = no tach line)",
		    (double)c.rpm_fan[0], (double)c.rpm_fan[1],
		    (double)c.rpm_fan[2]);
	if (c.t_board.flags & MEAS_VALID) {
		shell_print(sh, "board      %.1f C (MOSFET heatsink)",
			    (double)c.t_board.v);
	}
	shell_print(sh, "heaters    %s", c.heaters_enabled ? "ARMED" : "safe");

	return 0;
}

static int submit(const struct shell *sh, enum cal_cmd_type t, float arg)
{
	const struct cal_cmd c = { .type = t, .arg = arg, .origin = "shell" };
	const int rc = sys_cmd_submit(&c);

	if (rc) {
		shell_error(sh, "refused (%d)", rc);
	}

	return rc;
}

static int cmd_start(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	return submit(sh, CAL_CMD_START, 0.0f);
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	return submit(sh, CAL_CMD_STOP, 0.0f);
}

static int cmd_tss(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: cal tss <degC>");
		return -EINVAL;
	}
	return submit(sh, CAL_CMD_SET_TSS, (float)atof(argv[1]));
}

static int cmd_dtw(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: cal dtw <K>");
		return -EINVAL;
	}
	return submit(sh, CAL_CMD_SET_DTW, (float)atof(argv[1]));
}

static int cmd_soak(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	return submit(sh, CAL_CMD_CAL_SOAK, 0.0f);
}

static int cmd_tare(const struct shell *sh, size_t argc, char **argv)
{
	return submit(sh, CAL_CMD_CAL_TARE,
		      argc > 1 ? (float)atof(argv[1]) : 0.0f);
}

static int cmd_subst(const struct shell *sh, size_t argc, char **argv)
{
	return submit(sh, CAL_CMD_CAL_SUBST,
		      argc > 1 ? (float)atof(argv[1]) : 100.0f);
}

static int cmd_clear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	return submit(sh, CAL_CMD_CLEAR_FAULT, 0.0f);
}

static int cmd_inlet(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "inlet setpoint %.2f C", (double)t_inlet_set);
		return 0;
	}
	t_inlet_set = (float)atof(argv[1]);
	return 0;
}

static int cmd_wifi(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "ssid '%s'  %s  %s", iot_ssid(),
			    iot_connected() ? iot_ip() : "(not connected)",
			    iot_token());
		return 0;
	}

	const int rc = iot_set_credentials(argv[1], argc > 2 ? argv[2] : "");

	if (rc) {
		shell_error(sh, "wifi: %d", rc);
	}

	return rc;
}

static int cmd_post(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	return sys_post_run();
}

SHELL_STATIC_SUBCMD_SET_CREATE(cal_cmds,
	SHELL_CMD(status, NULL, "Everything, once",              cmd_status),
	SHELL_CMD(start,  NULL, "Begin a measurement point",     cmd_start),
	SHELL_CMD(stop,   NULL, "Back to IDLE, outputs safe",    cmd_stop),
	SHELL_CMD(tss,    NULL, "<degC> chamber setpoint",       cmd_tss),
	SHELL_CMD(dtw,    NULL, "<K> water dT setpoint",         cmd_dtw),
	SHELL_CMD(inlet,  NULL, "<degC> reject inlet setpoint",  cmd_inlet),
	SHELL_CMD(soak,   NULL, "Isothermal offset calibration", cmd_soak),
	SHELL_CMD(tare,   NULL, "[mL/min] dT tare",              cmd_tare),
	SHELL_CMD(subst,  NULL, "[W] substitution check",        cmd_subst),
	SHELL_CMD(wifi,   NULL, "[ssid] [psk] show or set",      cmd_wifi),
	SHELL_CMD(post,   NULL, "Re-run the self test",          cmd_post),
	SHELL_CMD(clear,  NULL, "Clear latched faults",          cmd_clear),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(cal, &cal_cmds, "Calorimeter control", NULL);

/* ---------------------------------------------------------------- main ---- */

int main(void)
{
	LOG_INF("=====================================================");
	LOG_INF(" DJC calorimeter  fw %s  params %s", APP_VERSION_STR,
		CAL_PARAMS_HASH);
	LOG_INF(" tick %d ms   guard Kp %.3f W/K  Ki %.5f W/(K s)",
		CAL_TICK_MS, (double)CAL_KP_G, (double)CAL_KI_G);
	LOG_INF(" null gate |e| < %.3f K   accuracy target +-%.1f W",
		(double)CAL_NULL_GATE_K, (double)CAL_P_ACC);
	LOG_INF("=====================================================");

	/* Order matters: the drivers first, then the values that configure
	 * them, then the safety net, then anything that can talk to a human. */
	(void)temp_sense_init();
	(void)power_sense_init();
	(void)water_loop_init();
	(void)fans_init();
	(void)heaters_init();
	(void)calibration_init();
	(void)sys_init();

	controllers_init();

	s.t_inner_set = 0.0f;                 /* no chamber setpoint yet */
	s.dt_water_set = cal_ranges[1].dt_set;

	/*
	 * Arm the over-current trips.  Board/Design.md sizes the heater output
	 * at 9.8 A max, so 12 A clears it with inrush headroom while staying
	 * well under the IPP041N04N and the channel fuse.
	 *
	 * This arms the hardware ALERT where the board routes one, and the
	 * software backstop always - the two are lines of defence, not
	 * alternatives.
	 */
	(void)power_sense_arm_alert(PWR_HEAT_INNER, 12.0f);
	(void)power_sense_arm_alert(PWR_HEAT_GUARD, 12.0f);
	/* The INA3221 rails have no ALERT pin, so these are backstop only.
	 * The pump is bounded by its 50 W / 12 V input-power figure; the fan
	 * chains are nowhere near an ampere. */
	power_sense_set_limit(PWR_PUMP, 5.5f);
	power_sense_set_limit(PWR_FAN_INNER, 1.0f);
	power_sense_set_limit(PWR_FAN_GUARD, 1.0f);

	ui_init();
	iot_init();

	(void)sys_post_run();

	LOG_INF("entering the control loop");

	int64_t last = k_uptime_get();

	while (1) {
		k_msleep(CAL_TICK_MS);

		const int64_t now = k_uptime_get();
		/* The MEASURED interval, never the nominal 1.000 s.  Every
		 * integrator and every slope in the firmware is fed this. */
		const float dt = (float)(now - last) / 1000.0f;

		last = now;

		tick(dt);
	}

	return 0;
}
