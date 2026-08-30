/*
 * fans.c - three PWM fan groups, one code path.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "fans.h"
#include "power-sense.h"
#include "control-system.h"    /* cal_clampf */

LOG_MODULE_REGISTER(fans, LOG_LEVEL_INF);

/* A PC fan's tach is two pulses per revolution. */
#define TACH_PULSES_PER_REV 2.0f

/* Below this commanded duty a stopped fan proves nothing, so health is not
 * evaluated. */
#define FAN_HEALTH_MIN_DUTY 0.20f

/* How far the rail current may fall from its POST signature before the group
 * is called unhealthy.  One stopped fan in a chain of three is a ~33 % drop,
 * so 20 % catches it with margin against fan-to-fan spread. */
#define FAN_CURRENT_DROP 0.20f

struct fan_grp {
	struct pwm_dt_spec pwm;
	struct gpio_dt_spec tach;      /* .port == NULL when not wired */
	struct gpio_callback tach_cb;
	atomic_t edges;
	const char *name;
	float duty;
	float rpm;
	enum power_rail rail;          /* PWR_COUNT when not metered */
	float signature_a;             /* learned at POST, 0 = unknown */
};

/* The tach lines are optional in the binding: the bench dev kit has neither
 * fans nor free pins.  GPIO_DT_SPEC_GET_OR() gives an all-zero spec when the
 * property is absent, and gpio_is_ready_dt() then reports it as unusable,
 * which is exactly the "no tach on this board" path. */
#define TACH_OR_NONE(prop) \
	GPIO_DT_SPEC_GET_OR(DT_NODELABEL(cal_io), prop, {0})

/*
 * PWM_DT_SPEC_GET() carries the channel, the period and the flags out of the
 * overlay, so the frequency (25 kHz, the Intel 4-wire fan spec) is devicetree
 * data and not a magic number in this file.
 */
static struct fan_grp groups[FAN_COUNT] = {
	[FAN_INNER]  = { .pwm = PWM_DT_SPEC_GET(DT_NODELABEL(pwm_fan_inner)),
			 .tach = TACH_OR_NONE(fan_inner_tach_gpios),
			 .rail = PWR_FAN_INNER,
			 .name = "inner" },
	[FAN_GUARD]  = { .pwm = PWM_DT_SPEC_GET(DT_NODELABEL(pwm_fan_guard)),
			 .tach = TACH_OR_NONE(fan_guard_tach_gpios),
			 .rail = PWR_FAN_GUARD,
			 .name = "guard" },
	[FAN_REJECT] = { .pwm = PWM_DT_SPEC_GET(DT_NODELABEL(pwm_fan_reject)),
			 .tach = TACH_OR_NONE(fan_reject_tach_gpios),
			 .rail = PWR_COUNT,      /* not metered - outside B1 */
			 .name = "reject" },
};

static bool inner_frozen;

/*
 * One handler for all three lines.  A tach edge is the cheapest possible
 * interrupt - increment and return - so there is no work item and no lock:
 * the counter is atomic and the reader swaps it to zero.
 */
static void tach_isr(const struct device *port, struct gpio_callback *cb,
		     uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	struct fan_grp *g = CONTAINER_OF(cb, struct fan_grp, tach_cb);

	atomic_inc(&g->edges);
}

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
		groups[i].rpm = -1.0f;

		if (!gpio_is_ready_dt(&groups[i].tach)) {
			LOG_INF("fan '%s': no tach line on this board",
				groups[i].name);
			continue;
		}

		(void)gpio_pin_configure_dt(&groups[i].tach, GPIO_INPUT);
		(void)gpio_pin_interrupt_configure_dt(&groups[i].tach,
						      GPIO_INT_EDGE_TO_ACTIVE);
		gpio_init_callback(&groups[i].tach_cb, tach_isr,
				   BIT(groups[i].tach.pin));
		(void)gpio_add_callback(groups[i].tach.port, &groups[i].tach_cb);
		LOG_INF("fan '%s': tach armed", groups[i].name);
	}

	inner_frozen = false;

	return missing ? -ENODEV : 0;
}

void fans_service(float dt)
{
	if (dt <= 0.0f) {
		return;
	}

	for (int i = 0; i < FAN_COUNT; i++) {
		if (!gpio_is_ready_dt(&groups[i].tach)) {
			continue;
		}

		/* Swap-to-zero: every edge counted since the last tick belongs
		 * to exactly this interval, with no window to lose one in. */
		const int32_t edges = atomic_set(&groups[i].edges, 0);

		groups[i].rpm = ((float)edges / TACH_PULSES_PER_REV) / dt * 60.0f;
	}
}

float fans_rpm(enum fan_group g)
{
	return (g >= 0 && g < FAN_COUNT) ? groups[g].rpm : -1.0f;
}

bool fans_has_tach(enum fan_group g)
{
	return (g >= 0 && g < FAN_COUNT) && gpio_is_ready_dt(&groups[g].tach);
}

void fans_learn_signature(enum fan_group g, float amps)
{
	if (g >= 0 && g < FAN_COUNT) {
		groups[g].signature_a = amps;
		LOG_INF("fan '%s' current signature %.3f A at %.0f %% duty",
			groups[g].name, (double)amps,
			(double)(groups[g].duty * 100.0f));
	}
}

enum fan_group fans_unhealthy(void)
{
	for (int i = 0; i < FAN_COUNT; i++) {
		struct fan_grp *g = &groups[i];

		if (g->duty < FAN_HEALTH_MIN_DUTY) {
			continue;   /* not commanded hard enough to judge */
		}

		/* Signal 1: the chain is not turning at all. */
		if (gpio_is_ready_dt(&g->tach) && g->rpm >= 0.0f &&
		    g->rpm < 60.0f) {
			LOG_WRN("fan '%s' commanded %.0f %% but reads %.0f rpm",
				g->name, (double)(g->duty * 100.0f),
				(double)g->rpm);
			return (enum fan_group)i;
		}

		/*
		 * Signal 2: the rail current has collapsed.  This is the only
		 * one that can see ONE stopped fan in a PST chain, because the
		 * chain's single tach line keeps reporting from fan #1.
		 */
		if (g->rail < PWR_COUNT && g->signature_a > 0.01f) {
			const float now_a = power_sense_current(g->rail);

			if (now_a < g->signature_a * (1.0f - FAN_CURRENT_DROP)) {
				LOG_WRN("fan '%s' rail at %.3f A against a "
					"%.3f A signature - a fan has stopped",
					g->name, (double)now_a,
					(double)g->signature_a);
				return (enum fan_group)i;
			}
		}
	}

	return FAN_COUNT;
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
