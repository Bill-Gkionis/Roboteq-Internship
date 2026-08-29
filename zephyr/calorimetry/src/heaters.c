/*
 * heaters.c - PWM heaters with an inner power loop.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>

#include "heaters.h"
#include "power-sense.h"
#include "control-system.h"

LOG_MODULE_REGISTER(heaters, LOG_LEVEL_INF);

/* Nominal heater resistance at 12 V, used ONLY to seed the inner loop with a
 * sensible first guess so it does not have to integrate up from zero.  Both
 * barrel resistors are ~100 W at 12 V => R ~ 1.44 Ohm. */
#define HEATER_R_NOMINAL   1.44f
#define HEATER_V_NOMINAL  12.0f

/* Inner power loop gain.  A pure integrator in duty per (watt second): a
 * proportional term is unnecessary because the plant it controls (duty ->
 * electrical power) has no dynamics of its own worth the name, and an
 * integrator alone cannot leave a steady-state error. */
#define HEATER_INNER_KI   0.0015f   /* duty / (W s) */

struct heater {
	struct pwm_dt_spec pwm;
	enum power_rail rail;
	const char *name;
	float demand_w;    /* what was asked for            */
	float duty;        /* what the inner loop settled on */
	float p_meas_w;    /* what the rail actually drew    */
};

static struct heater heaters[HEATER_COUNT] = {
	[HEATER_INNER] = { .pwm = PWM_DT_SPEC_GET(DT_NODELABEL(pwm_heat_inner)),
			   .rail = PWR_HEAT_INNER, .name = "inner" },
	[HEATER_GUARD] = { .pwm = PWM_DT_SPEC_GET(DT_NODELABEL(pwm_heat_guard)),
			   .rail = PWR_HEAT_GUARD, .name = "guard" },
};

static bool enabled;

/** Duty that open-loop physics says should give `watts`.  The seed, not the
 *  answer: sqrt(P R)/V is the duty for a resistive load at a fixed rail. */
static float duty_feedforward(float watts)
{
	if (watts <= 0.0f) {
		return 0.0f;
	}
	return cal_clampf(watts * HEATER_R_NOMINAL /
				  (HEATER_V_NOMINAL * HEATER_V_NOMINAL),
			  0.0f, 1.0f);
}

static void write_duty(struct heater *h, float duty)
{
	duty = cal_clampf(duty, 0.0f, 1.0f);
	h->duty = duty;
	(void)pwm_set_pulse_dt(&h->pwm, (uint32_t)(duty * (float)h->pwm.period));
}

int heaters_init(void)
{
	int missing = 0;

	for (int i = 0; i < HEATER_COUNT; i++) {
		if (!pwm_is_ready_dt(&heaters[i].pwm)) {
			LOG_ERR("heater PWM '%s' not ready", heaters[i].name);
			missing++;
			continue;
		}
		write_duty(&heaters[i], 0.0f);
		heaters[i].demand_w = 0.0f;
	}

	/* Reset must mean HEATERS OFF, and that has to be a resistor, not a
	 * promise: Zephyr leaves GPIOs as floating inputs at reset, so every
	 * gate driver input needs an external 10k pull-down.  This line only
	 * covers the case where the firmware is running. */
	enabled = false;

	return missing ? -ENODEV : 0;
}

void heaters_set_power(enum heater_id h, float watts)
{
	if (h < 0 || h >= HEATER_COUNT) {
		return;
	}
	heaters[h].demand_w = (watts > 0.0f) ? watts : 0.0f;
}

float heaters_get_demand(enum heater_id h)
{
	return (h >= 0 && h < HEATER_COUNT) ? heaters[h].demand_w : 0.0f;
}

float heaters_get_duty(enum heater_id h)
{
	return (h >= 0 && h < HEATER_COUNT) ? heaters[h].duty : 0.0f;
}

void heaters_service(float dt)
{
	const int64_t now = k_uptime_get();

	for (int i = 0; i < HEATER_COUNT; i++) {
		struct heater *h = &heaters[i];

		if (!enabled || h->demand_w <= 0.0f) {
			write_duty(h, 0.0f);
			continue;
		}

		struct meas p = power_sense_get(h->rail);

		if (!meas_ok(&p, now)) {
			/*
			 * No trustworthy power reading, so the inner loop
			 * cannot close.  Fall back to the open-loop
			 * feedforward rather than freezing the last duty: a
			 * frozen duty against an unknown rail is exactly the
			 * "gain nobody wrote down" failure this module exists
			 * to prevent, and it is also how a heater stays on
			 * while the rig believes something else.
			 */
			write_duty(h, duty_feedforward(h->demand_w));
			continue;
		}

		h->p_meas_w = p.v;

		const float err_w = h->demand_w - p.v;

		write_duty(h, h->duty + HEATER_INNER_KI * err_w * dt);
	}
}

void heaters_set_enable(bool enable)
{
	if (enabled == enable) {
		return;
	}

	enabled = enable;
	LOG_INF("heater interlock %s", enable ? "CLOSED (armed)" : "OPEN (safe)");

	if (!enable) {
		heaters_all_off();
	}
}

bool heaters_enabled(void)
{
	return enabled;
}

void heaters_all_off(void)
{
	for (int i = 0; i < HEATER_COUNT; i++) {
		heaters[i].demand_w = 0.0f;
		write_duty(&heaters[i], 0.0f);
	}
}

const char *heaters_name(enum heater_id h)
{
	return (h >= 0 && h < HEATER_COUNT) ? heaters[h].name : "?";
}
