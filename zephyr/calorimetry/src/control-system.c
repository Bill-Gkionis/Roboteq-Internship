/*
 * control-system.c - the C port of the Python model.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * No Zephyr headers.  See control-system.h for why that matters.
 */

#include <math.h>
#include <string.h>

#include "control-system.h"

/* ======================================================== water properties */

float cal_rho_w(float t_c)
{
	return CAL_RHO_A + CAL_RHO_B * t_c + CAL_RHO_C * t_c * t_c;
}

float cal_cp_w(float t_c)
{
	return CAL_CP_A + CAL_CP_B * t_c + CAL_CP_C * t_c * t_c;
}

/* ================================================================ the meter */

float cal_meter_power(float v_dot_ml_min, float t_in, float t_out,
		      float dt_tare, float p_aux, float *mcp_out)
{
	/* mL/min -> m^3/s */
	const float v_dot = v_dot_ml_min * 1.0e-6f / 60.0f;

	/* Density is evaluated at the METER's temperature, not the mean,
	 * because the turbine measures the volume that physically passed
	 * through it.  This is why the pump and the flow meter belong on the
	 * cold leg: the inlet probe is then already measuring the right
	 * temperature and no third probe is needed.
	 */
	const float rho = cal_rho_w(t_in);

	/* Specific heat is evaluated at the mean of the two ends, because that
	 * is the temperature the heat was actually absorbed at.
	 */
	const float t_mean = 0.5f * (t_in + t_out);
	const float cp = cal_cp_w(t_mean);

	const float mdot = rho * v_dot;          /* kg/s */
	const float mcp = mdot * cp;             /* W/K  */
	const float dt = (t_out - t_in) - dt_tare;

	if (mcp_out) {
		*mcp_out = mcp;
	}

	/* The -P_aux term is one matrix entry in the model (D_u = -1), and
	 * deleting it turns the P_aux DC gain from 0 into approximately 1:
	 * every watt of preheat would be reported as DUT loss.
	 */
	return mcp * dt - p_aux;
}

float cal_flow_for_point(float p_w, float dt_set, float t_mean)
{
	const float cp = cal_cp_w(t_mean);
	const float rho = cal_rho_w(t_mean);

	if (dt_set < 0.1f) {
		dt_set = 0.1f;
	}
	/* mdot = P / (cp * dT)  ->  V_dot [mL/min] = mdot/rho * 6e7 */
	const float mdot = p_w / (cp * dt_set);

	return cal_clampf(mdot / rho * 6.0e7f, 0.0f, CAL_FLOW_MAX_ML);
}

const struct cal_range *cal_range_for(float p_w)
{
	for (int i = 0; i < CAL_RANGE_COUNT; i++) {
		if (p_w <= cal_ranges[i].p_max) {
			return &cal_ranges[i];
		}
	}
	return &cal_ranges[CAL_RANGE_COUNT - 1];
}

float cal_tau_dom(float mcp)
{
	const float tau_advect = (mcp > 0.1f) ? (CAL_C_I + CAL_C_W) / mcp
					      : 1.0e6f;

	return (tau_advect > CAL_TAU_E) ? tau_advect : CAL_TAU_E;
}

/* ================================================================= PI loops */

float cal_clampf(float x, float lo, float hi)
{
	if (x < lo) {
		return lo;
	}
	if (x > hi) {
		return hi;
	}
	return x;
}

void cal_pi_init(struct cal_pi *pi, float kp, float ki, float u_min, float u_max)
{
	memset(pi, 0, sizeof(*pi));
	pi->kp = kp;
	pi->ki = ki;
	/* Back-calculation time constant.  kp/ki equals the plant's own time
	 * constant for a lambda-tuned PI (297 s for the guard), which is the
	 * textbook choice and the smoothest recovery from saturation. */
	pi->tt = (ki > 0.0f) ? (kp / ki) : 1.0f;
	pi->u_min = u_min;
	pi->u_max = u_max;
}

void cal_pi_preload(struct cal_pi *pi, float u0)
{
	pi->i = cal_clampf(u0, pi->u_min, pi->u_max);
	pi->u = pi->i;
	pi->u_raw = pi->i;
	pi->sat = false;
	pi->sat_s = 0.0f;
}

float cal_pi_step(struct cal_pi *pi, float e, float ff, float dt)
{
	if (dt <= 0.0f) {
		return pi->u;
	}

	/* The integrator is kept in ACTUATOR UNITS (watts), not in kelvin
	 * seconds.  Precision is identical either way because the ki cancels
	 * out of the increment-to-value ratio, but the clamp then lives in the
	 * same [0, P_max] box as the heater and is trivially readable.
	 */
	const float u_raw = pi->kp * e + pi->i + ff;
	const float u = cal_clampf(u_raw, pi->u_min, pi->u_max);

	/* Back-calculation anti-windup: the correction term is exactly zero
	 * whenever the output is not clipped, so there is no branch to get
	 * wrong and no discontinuity at the limit.
	 */
	pi->i += pi->ki * e * dt + (u - u_raw) * (dt / pi->tt);

	/* The integrator can never usefully exceed the actuator's own range,
	 * and the floor is 0 rather than -u_max because HEATERS CANNOT COOL:
	 * a negative demand saturates hard and the integrator must not wind
	 * down forever chasing it.
	 */
	pi->i = cal_clampf(pi->i, pi->u_min, pi->u_max);

	pi->sat = (u != u_raw);
	pi->sat_s = pi->sat ? (pi->sat_s + dt) : 0.0f;
	pi->u_raw = u_raw;
	pi->u = u;

	return u;
}

/* ============================================== guard loop feedforward ==== */

float cal_guard_ff(float t_water_in, float dt_set, float p_hat)
{
	/* G_out * (the gap temperature the guard will have to hold).  With
	 * alpha = 0 the last term is the full P/UA_rad air-above-water offset.
	 */
	const float theta_i = t_water_in + dt_set +
			      (1.0f - CAL_ALPHA) * p_hat / CAL_UA_RAD;

	return CAL_G_OUT * theta_i;
}

float cal_slew(float current, float target, float rate_per_s, float dt)
{
	const float step = rate_per_s * dt;
	const float d = target - current;

	if (d > step) {
		return current + step;
	}
	if (d < -step) {
		return current - step;
	}
	return target;
}

/* ========================================================== slope estimate */

void cal_slope_init(struct cal_slope *s, int decim_s, int n)
{
	memset(s, 0, sizeof(*s));
	s->decim_s = (decim_s > 0) ? decim_s : 1;
	s->n = (n > CAL_SLOPE_MAX_N) ? CAL_SLOPE_MAX_N : n;
	if (s->n < 3) {
		s->n = 3;
	}
}

void cal_slope_reset(struct cal_slope *s)
{
	s->acc = 0.0f;
	s->acc_n = 0;
	s->count = 0;
	s->head = 0;
	s->slope = 0.0f;
	s->ready = false;
}

float cal_slope_push(struct cal_slope *s, float value, float dt)
{
	(void)dt;   /* samples are taken one per tick; decimation is by count */

	s->acc += value;
	s->acc_n++;

	if (s->acc_n < s->decim_s) {
		return s->slope;
	}

	/* One decimated sample: the mean over decim_s ticks.  Pre-averaging is
	 * what makes the decimation anti-aliased rather than a naive skip. */
	s->ring[s->head] = s->acc / (float)s->acc_n;
	s->head = (s->head + 1) % s->n;
	s->acc = 0.0f;
	s->acc_n = 0;
	if (s->count < s->n) {
		s->count++;
	}
	s->ready = (s->count >= s->n);

	if (s->count < 3) {
		return s->slope;
	}

	/*
	 * Least-squares slope over evenly spaced samples.  For k = 0..N-1 the
	 * abscissa sums are closed form, so only one pass over the ring is
	 * needed:
	 *     slope = sum((k - k_bar) * y_k) / (N (N^2 - 1) / 12)
	 * and the spacing decim_s converts it from per-sample to per-second.
	 */
	const int n = s->count;
	const float k_bar = 0.5f * (float)(n - 1);
	float num = 0.0f;
	/* oldest sample first */
	int idx = (s->head - n + s->n * 2) % s->n;

	for (int k = 0; k < n; k++) {
		num += ((float)k - k_bar) * s->ring[idx];
		idx = (idx + 1) % s->n;
	}

	const float den = (float)n * ((float)n * (float)n - 1.0f) / 12.0f;

	s->slope = (num / den) / (float)s->decim_s;

	return s->slope;
}

/* ==================================================================== gates */

void cal_gates_init(struct cal_gates *g)
{
	memset(g, 0, sizeof(*g));
	/* 10 s pre-average, 180 samples = a 30 minute sliding window, 720 B. */
	cal_slope_init(&g->p_slope, 10, 180);
	cal_gates_set_range(g, 15.0f);
}

void cal_gates_set_range(struct cal_gates *g, float mcp)
{
	g->slope_gate = CAL_P_THR / cal_tau_dom(mcp);
	cal_slope_reset(&g->p_slope);
	g->both_green_s = 0;
	g->null_ok = false;
	g->steady_ok = false;
}

bool cal_gates_eval(struct cal_gates *g, float p_meas, float e_null, float dt)
{
	const float slope = cal_slope_push(&g->p_slope, p_meas, dt);

	/* Gate the READING, never a temperature.  The chamber air is dominated
	 * by 15 s modes and settles long before the reading does - gating on
	 * it hands you an early, wrong, confident number.
	 */
	g->steady_ok = g->p_slope.ready && (fabsf(slope) < g->slope_gate);
	g->null_ok = (fabsf(e_null) < CAL_NULL_GATE_K);

	if (g->null_ok && g->steady_ok) {
		g->both_green_s += (int)(dt + 0.5f);
	} else {
		g->both_green_s = 0;
	}

	return g->both_green_s >= CAL_DWELL_S;
}

/* ================================================= disturbance observer === */

void cal_observer_init(struct cal_observer *o)
{
	memset(o, 0, sizeof(*o));
	/* 1 s pre-average, 120 samples = a 2 minute least-squares window.  Long
	 * enough to keep differentiation noise near 0.06 W, short enough to
	 * track the transient the estimate exists to read. */
	cal_slope_init(&o->theta_slope, 1, 120);
	o->c_tot = CAL_C_I + CAL_C_W;
	o->trusted = false;
}

void cal_observer_reset(struct cal_observer *o)
{
	cal_slope_reset(&o->theta_slope);
	o->p_hat = 0.0f;
}

float cal_observer_step(struct cal_observer *o, float t_inner, float t_water,
			float p_meas, float dt)
{
	/* Capacitance-weighted mean of the two nodes: this is the temperature
	 * whose derivative multiplies (C_i + C_w) in the energy balance. */
	const float theta_bar =
		(CAL_C_I * t_inner + CAL_C_W * t_water) / (CAL_C_I + CAL_C_W);

	const float d_theta = cal_slope_push(&o->theta_slope, theta_bar, dt);

	/* Until the window has filled, the slope estimate is not trustworthy;
	 * fall back to the raw reading, which is never wrong, only slow. */
	o->p_hat = o->theta_slope.ready ? (p_meas + o->c_tot * d_theta)
					: p_meas;

	return o->p_hat;
}

/* ============================================================== statistics */

void cal_stat_reset(struct cal_stat *s)
{
	s->sum = 0.0;
	s->sum_sq = 0.0;
	s->n = 0;
}

void cal_stat_push(struct cal_stat *s, float x)
{
	s->sum += (double)x;
	s->sum_sq += (double)x * (double)x;
	s->n++;
}

float cal_stat_mean(const struct cal_stat *s)
{
	return (s->n > 0) ? (float)(s->sum / (double)s->n) : 0.0f;
}

float cal_stat_stderr(const struct cal_stat *s)
{
	if (s->n < 2) {
		return 0.0f;
	}

	const double mean = s->sum / (double)s->n;
	double var = (s->sum_sq / (double)s->n) - mean * mean;

	if (var < 0.0) {
		var = 0.0;   /* catastrophic cancellation guard */
	}

	/* Sample variance (Bessel), then the standard error of the mean. */
	var *= (double)s->n / (double)(s->n - 1);

	return (float)sqrt(var / (double)s->n);
}
