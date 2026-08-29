/*
 * water-loop.c - pump, RTD dT pair, turbine flow meter.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include "water-loop.h"
#include "control-system.h"

LOG_MODULE_REGISTER(water_loop, LOG_LEVEL_INF);

/* ------------------------------------------------------------- devices ---- */

static const struct device *const rtd_in =
	DEVICE_DT_GET(DT_ALIAS(cal_rtd_in));
static const struct device *const rtd_out =
	DEVICE_DT_GET(DT_ALIAS(cal_rtd_out));
static const struct device *const flow_cnt =
	DEVICE_DT_GET(DT_ALIAS(cal_flow));

static const struct pwm_dt_spec pump_step =
	PWM_DT_SPEC_GET(DT_NODELABEL(pwm_pump_step));
static const struct gpio_dt_spec pump_dir =
	GPIO_DT_SPEC_GET(DT_NODELABEL(cal_io), pump_dir_gpios);
static const struct gpio_dt_spec pump_en =
	GPIO_DT_SPEC_GET(DT_NODELABEL(cal_io), pump_enable_gpios);

/* ------------------------------------------------------------ defaults ---- */

/*
 * Both of these are [cal] values that belong in NVS after commissioning.  The
 * defaults below are design estimates and are wrong by design until the
 * gravimetric calibration replaces them - which is the point: a flow scale
 * error is the single largest line in the error budget (0.75 W at +0.5 %) and
 * no control loop can rescue it.  It is a bucket-and-scale problem.
 */
#define K_FACTOR_DEFAULT   6000.0f  /* pulses per litre, Biotech FCH-m [est] */

/*
 * JwardTech 304K: 0-1760 mL/min over 0.1-300 rpm => ~5.87 mL/rev.  With a
 * 200 step/rev motor at 16x microstepping that is 3200 steps/rev, so
 * 5.87/3200 = 1.83e-3 mL/step.
 */
#define ML_PER_STEP_DEFAULT  0.00183f

/* Outer flow trim: a slow integrator on the step rate.  Slow on purpose - the
 * turbine resolves only ~4 counts per second at the cold corner, so chasing
 * its per-tick noise would inject flow modulation into the meter. */
#define FLOW_TRIM_KI     0.02f      /* (1/s) per (mL/min) of error */
#define FLOW_TRIM_LIMIT  0.35f      /* +-35 % authority over the open loop */

/* ------------------------------------------------------------- state ------ */

static struct {
	struct meas t_in, t_out, flow;
	float k_factor;
	float ml_per_step;
	float tare_k;
	float setpoint_ml;
	float trim;             /* dimensionless multiplier around 1.0 */
	int32_t last_count;
	bool count_primed;
	bool running;
} wl = {
	.k_factor = K_FACTOR_DEFAULT,
	.ml_per_step = ML_PER_STEP_DEFAULT,
	.trim = 1.0f,
};

static K_MUTEX_DEFINE(wl_lock);

/* ------------------------------------------------------------- helpers ---- */

static void read_rtd(const struct device *dev, struct meas *m, const char *name)
{
	struct sensor_value v;
	int rc;

	if (!device_is_ready(dev)) {
		m->flags = MEAS_DEAD;
		return;
	}

	rc = sensor_sample_fetch(dev);
	if (rc == 0) {
		rc = sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &v);
	}

	if (rc != 0) {
		/* Leave t_ms alone so staleness detection can see it. */
		m->flags &= (uint8_t)~MEAS_VALID;
		LOG_WRN("RTD '%s' read failed (%d)", name, rc);
		return;
	}

	m->v = (float)sensor_value_to_double(&v);
	m->t_ms = k_uptime_get();
	m->flags = MEAS_VALID;
}

/** Turbine pulses -> mL/min, over the interval since the last call. */
static void read_flow(float dt)
{
	struct sensor_value v;

	if (!device_is_ready(flow_cnt) || dt <= 0.0f) {
		wl.flow.flags &= (uint8_t)~MEAS_VALID;
		return;
	}

	if (sensor_sample_fetch(flow_cnt) != 0 ||
	    sensor_channel_get(flow_cnt, SENSOR_CHAN_ENCODER_COUNT, &v) != 0) {
		wl.flow.flags &= (uint8_t)~MEAS_VALID;
		return;
	}

	const int32_t count = v.val1;

	if (!wl.count_primed) {
		wl.last_count = count;
		wl.count_primed = true;
		return;
	}

	const int32_t delta = count - wl.last_count;

	wl.last_count = count;

	/*
	 * pulses/s -> L/s -> mL/min.  At the cold corner this is ~4 counts in
	 * the window, i.e. +-25 % on a single tick; that is fine because the
	 * gate integrates the READING over tens of minutes, and because flow is
	 * frozen inside a measurement point by design.  It is emphatically NOT
	 * fine as a metrology claim - see the header.
	 */
	const float pulses_per_s = (float)delta / dt;

	wl.flow.v = pulses_per_s / wl.k_factor * 1000.0f * 60.0f;
	wl.flow.t_ms = k_uptime_get();
	wl.flow.flags = MEAS_VALID;
}

/** Write the STEP generator's frequency [Hz].  0 stops it. */
static void set_step_rate(float hz)
{
	if (hz < 1.0f) {
		(void)pwm_set_pulse_dt(&pump_step, 0);
		return;
	}

	/*
	 * The pulse train has to come from HARDWARE (LEDC here), never a
	 * software toggle: a software-generated step train jitters with every
	 * interrupt in the system, and the pump is the one actuator whose
	 * output is what the meter scales by.
	 *
	 * 50 % duty is what every STEP/DIR driver wants; only the edge counts.
	 */
	const uint32_t period_ns = (uint32_t)(1.0e9f / hz);

	(void)pwm_set_dt(&pump_step, period_ns, period_ns / 2U);
}

/* --------------------------------------------------------------- API ------ */

int water_loop_init(void)
{
	int missing = 0;

	if (!device_is_ready(rtd_in) || !device_is_ready(rtd_out)) {
		LOG_ERR("MAX31865 pair not ready");
		missing++;
	}
	if (!device_is_ready(flow_cnt)) {
		LOG_ERR("flow pulse counter not ready");
		missing++;
	}
	if (!pwm_is_ready_dt(&pump_step)) {
		LOG_ERR("pump STEP PWM not ready");
		missing++;
	}

	if (gpio_is_ready_dt(&pump_dir)) {
		/* Direction is fixed in this rig; set it once and leave it. */
		(void)gpio_pin_configure_dt(&pump_dir, GPIO_OUTPUT_ACTIVE);
	} else {
		missing++;
	}

	if (gpio_is_ready_dt(&pump_en)) {
		/* INACTIVE on an active-low enable means the driver is
		 * disabled, which is where it must start. */
		(void)gpio_pin_configure_dt(&pump_en, GPIO_OUTPUT_INACTIVE);
	} else {
		missing++;
	}

	set_step_rate(0.0f);

	return missing ? -ENODEV : 0;
}

void water_loop_read(float dt)
{
	k_mutex_lock(&wl_lock, K_FOREVER);
	read_rtd(rtd_in, &wl.t_in, "in");
	read_rtd(rtd_out, &wl.t_out, "out");
	read_flow(dt);
	k_mutex_unlock(&wl_lock);
}

struct meas water_loop_t_in(void)
{
	struct meas m;

	k_mutex_lock(&wl_lock, K_FOREVER);
	m = wl.t_in;
	k_mutex_unlock(&wl_lock);

	return m;
}

struct meas water_loop_t_out(void)
{
	struct meas m;

	k_mutex_lock(&wl_lock, K_FOREVER);
	m = wl.t_out;
	k_mutex_unlock(&wl_lock);

	return m;
}

struct meas water_loop_flow(void)
{
	struct meas m;

	k_mutex_lock(&wl_lock, K_FOREVER);
	m = wl.flow;
	k_mutex_unlock(&wl_lock);

	return m;
}

void water_loop_set_flow(float ml_per_min)
{
	ml_per_min = cal_clampf(ml_per_min, 0.0f, CAL_FLOW_MAX_ML);

	k_mutex_lock(&wl_lock, K_FOREVER);
	wl.setpoint_ml = ml_per_min;
	/* A range change is a "discard and re-settle" event anyway, so it is
	 * also the right moment to drop the accumulated trim. */
	wl.trim = 1.0f;
	k_mutex_unlock(&wl_lock);

	if (ml_per_min <= 0.0f) {
		water_loop_stop();
		return;
	}

	if (!wl.running && gpio_is_ready_dt(&pump_en)) {
		(void)gpio_pin_set_dt(&pump_en, 1);   /* assert ENn -> enabled */
		wl.running = true;
	}

	LOG_INF("flow setpoint %.1f mL/min", (double)ml_per_min);
}

float water_loop_get_flow_setpoint(void)
{
	return wl.setpoint_ml;
}

float water_loop_pump_fraction(void)
{
	return wl.setpoint_ml / CAL_FLOW_MAX_ML;
}

void water_loop_service(float dt)
{
	if (!wl.running || wl.setpoint_ml <= 0.0f) {
		set_step_rate(0.0f);
		return;
	}

	/* Open loop: the step rate the calibrated displacement says we need. */
	const float steps_per_s =
		wl.setpoint_ml / (wl.ml_per_step * 60.0f);

	/* Closed trim, against the turbine.  Only applied when the turbine
	 * reading is fresh and valid; otherwise the loop runs open, which is
	 * the safe direction (the flow is then whatever the calibration says,
	 * not whatever a dead sensor last claimed). */
	struct meas f = water_loop_flow();

	if (meas_ok(&f, k_uptime_get()) && f.v > 0.5f) {
		const float err = wl.setpoint_ml - f.v;

		wl.trim += FLOW_TRIM_KI * (err / wl.setpoint_ml) * dt;
		wl.trim = cal_clampf(wl.trim, 1.0f - FLOW_TRIM_LIMIT,
				     1.0f + FLOW_TRIM_LIMIT);
	}

	set_step_rate(steps_per_s * wl.trim);
}

void water_loop_stop(void)
{
	set_step_rate(0.0f);
	if (gpio_is_ready_dt(&pump_en)) {
		(void)gpio_pin_set_dt(&pump_en, 0);   /* de-assert -> disabled */
	}
	wl.running = false;
	wl.setpoint_ml = 0.0f;
}

void water_loop_set_k_factor(float pulses_per_litre)
{
	if (pulses_per_litre > 1.0f) {
		wl.k_factor = pulses_per_litre;
		LOG_INF("turbine K = %.1f pulses/L", (double)pulses_per_litre);
	}
}

float water_loop_get_k_factor(void)
{
	return wl.k_factor;
}

void water_loop_set_ml_per_step(float ml)
{
	if (ml > 0.0f) {
		wl.ml_per_step = ml;
	}
}

float water_loop_get_ml_per_step(void)
{
	return wl.ml_per_step;
}

void water_loop_set_tare(float tare_k)
{
	wl.tare_k = tare_k;
	LOG_INF("dT tare = %+.4f K", (double)tare_k);
}

float water_loop_get_tare(void)
{
	return wl.tare_k;
}

int32_t water_loop_pulse_count(void)
{
	return wl.last_count;
}
