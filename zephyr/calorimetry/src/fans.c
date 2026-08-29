/*
 * fans.c - three PWM fan groups, one code path.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>

#include "fans.h"
#include "control-system.h"    /* cal_clampf */

LOG_MODULE_REGISTER(fans, LOG_LEVEL_INF);

struct fan_grp {
	struct pwm_dt_spec pwm;
	const char *name;
	float duty;
};

/*
 * PWM_DT_SPEC_GET() carries the channel, the period and the flags out of the
 * overlay, so the frequency (25 kHz, the Intel 4-wire fan spec) is devicetree
 * data and not a magic number in this file.
 */
static struct fan_grp groups[FAN_COUNT] = {
	[FAN_INNER]  = { .pwm = PWM_DT_SPEC_GET(DT_NODELABEL(pwm_fan_inner)),
			 .name = "inner" },
	[FAN_GUARD]  = { .pwm = PWM_DT_SPEC_GET(DT_NODELABEL(pwm_fan_guard)),
			 .name = "guard" },
	[FAN_REJECT] = { .pwm = PWM_DT_SPEC_GET(DT_NODELABEL(pwm_fan_reject)),
			 .name = "reject" },
};

static bool inner_frozen;

int fans_init(void)
{
	int missing = 0;

	for (int i = 0; i < FAN_COUNT; i++) {
		if (!pwm_is_ready_dt(&groups[i].pwm)) {
			LOG_ERR("fan PWM '%s' not ready", groups[i].name);
			missing++;
			continue;
		}
		/* Start at zero.  Nothing in this rig should spin because the
		 * MCU booted; every actuator is commanded on deliberately. */
		(void)pwm_set_pulse_dt(&groups[i].pwm, 0);
		groups[i].duty = 0.0f;
	}

	inner_frozen = false;

	return missing ? -ENODEV : 0;
}

/** The one place a duty actually reaches hardware. */
static int apply(struct fan_grp *g, float duty)
{
	duty = cal_clampf(duty, 0.0f, 1.0f);

	/* pwm_set_pulse_dt() keeps the period from the devicetree and changes
	 * only the on-time, which is exactly what a duty change is. */
	const uint32_t pulse = (uint32_t)(duty * (float)g->pwm.period);
	const int rc = pwm_set_pulse_dt(&g->pwm, pulse);

	if (rc == 0) {
		g->duty = duty;
	}

	return rc;
}

int fans_set(enum fan_group g, float duty)
{
	if (g < 0 || g >= FAN_COUNT) {
		return -EINVAL;
	}

	if (g == FAN_INNER && inner_frozen) {
		LOG_WRN("inner fan duty is frozen for this campaign - refused");
		return -EPERM;
	}

	return apply(&groups[g], duty);
}

float fans_get(enum fan_group g)
{
	return (g >= 0 && g < FAN_COUNT) ? groups[g].duty : 0.0f;
}

void fans_freeze_inner(bool freeze)
{
	inner_frozen = freeze;
	LOG_INF("inner fans %s at %.0f %%",
		freeze ? "FROZEN" : "unfrozen",
		(double)(groups[FAN_INNER].duty * 100.0f));
}

bool fans_inner_frozen(void)
{
	return inner_frozen;
}

void fans_all_off(void)
{
	/* The freeze is a measurement-integrity rule, not a safety one, so the
	 * safety path bypasses it. */
	for (int i = 0; i < FAN_COUNT; i++) {
		(void)apply(&groups[i], 0.0f);
	}
}

const char *fans_name(enum fan_group g)
{
	return (g >= 0 && g < FAN_COUNT) ? groups[g].name : "?";
}
