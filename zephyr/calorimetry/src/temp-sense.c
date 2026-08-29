/*
 * temp-sense.c - TMP117 chamber temperatures over I2C.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

#include "temp-sense.h"

LOG_MODULE_REGISTER(temp_sense, LOG_LEVEL_INF);

/* After this many consecutive bus failures the channel is declared dead and
 * excluded from its zone; the supervisor then decides whether the point is
 * still valid.  Recovering from a transient glitch is worth a few retries;
 * pretending a disconnected sensor is fine is not. */
#define TEMP_DEAD_AFTER 5

struct temp_ch {
	const struct device *dev;
	const struct device *bus;   /* for i2c_recover_bus() on this leg only */
	const char *name;
	struct meas m;
	float offset_k;
	uint8_t fail_count;
};

/*
 * The channel table.  This - and the aliases it names - is the ONLY place the
 * physical sensor set appears.  Adding guard faces means adding rows here and
 * nodes in the overlay.
 */
static struct temp_ch channels[TEMP_CH_COUNT] = {
	[TEMP_CH_INNER] = { .dev = DEVICE_DT_GET(DT_ALIAS(cal_t_inner)),
			    .bus = DEVICE_DT_GET(DT_BUS(DT_ALIAS(cal_t_inner))),
			    .name = "inner" },
	[TEMP_CH_GUARD] = { .dev = DEVICE_DT_GET(DT_ALIAS(cal_t_guard)),
			    .bus = DEVICE_DT_GET(DT_BUS(DT_ALIAS(cal_t_guard))),
			    .name = "guard" },
};

static K_MUTEX_DEFINE(temp_lock);

int temp_sense_init(void)
{
	int missing = 0;

	for (int i = 0; i < TEMP_CH_COUNT; i++) {
		if (!device_is_ready(channels[i].dev)) {
			LOG_ERR("TMP117 '%s' not ready", channels[i].name);
			missing++;
			continue;
		}
		LOG_INF("TMP117 '%s' bound to %s", channels[i].name,
			channels[i].dev->name);
	}

	return missing ? -ENODEV : 0;
}

/** One sensor, one tick.  Never blocks longer than the driver's own timeout. */
static void read_one(struct temp_ch *c)
{
	struct sensor_value val;
	int rc;

	if (!device_is_ready(c->dev)) {
		c->m.flags = MEAS_DEAD;
		return;
	}

	/* sensor_sample_fetch() latches the device's registers; the driver
	 * does the I2C transaction here, not in sensor_channel_get(). */
	rc = sensor_sample_fetch(c->dev);
	if (rc == 0) {
		rc = sensor_channel_get(c->dev, SENSOR_CHAN_AMBIENT_TEMP, &val);
	}

	if (rc != 0) {
		c->fail_count++;
		/* A failed read does NOT update t_ms.  That is deliberate: the
		 * staleness check downstream is what turns a silently frozen
		 * bus into a fault instead of a plausible constant the guard
		 * integrator happily accumulates against. */
		c->m.flags &= (uint8_t)~MEAS_VALID;
		if (c->fail_count >= TEMP_DEAD_AFTER) {
			if (!(c->m.flags & MEAS_DEAD)) {
				LOG_ERR("TMP117 '%s' dead after %u failures",
					c->name, c->fail_count);
			}
			c->m.flags |= MEAS_DEAD;
			/* Try to unwedge a slave that is holding SDA low.  If
			 * this works the channel comes back on the next tick.
			 * Recover THIS channel's bus: once the carrier board
			 * puts the sensors behind a TCA9548A they will not all
			 * share one leg, and recovering the wrong one would be
			 * a bug that only appears on real hardware. */
			if (device_is_ready(c->bus)) {
				(void)i2c_recover_bus(c->bus);
			}
		}
		return;
	}

	if (c->fail_count > 0) {
		c->m.flags |= MEAS_RETRIED;
		c->fail_count = 0;
	}

	c->m.v = (float)sensor_value_to_double(&val) - c->offset_k;
	c->m.t_ms = k_uptime_get();
	c->m.flags = (c->m.flags & MEAS_RETRIED) | MEAS_VALID;
}

void temp_sense_read_all(void)
{
	k_mutex_lock(&temp_lock, K_FOREVER);
	for (int i = 0; i < TEMP_CH_COUNT; i++) {
		read_one(&channels[i]);
	}
	k_mutex_unlock(&temp_lock);
}

struct meas temp_sense_get(enum temp_channel ch)
{
	struct meas out = { 0 };

	if (ch < 0 || ch >= TEMP_CH_COUNT) {
		return out;
	}

	k_mutex_lock(&temp_lock, K_FOREVER);
	out = channels[ch].m;
	k_mutex_unlock(&temp_lock);

	return out;
}

void temp_sense_set_offset(enum temp_channel ch, float offset_k)
{
	if (ch < 0 || ch >= TEMP_CH_COUNT) {
		return;
	}
	k_mutex_lock(&temp_lock, K_FOREVER);
	channels[ch].offset_k = offset_k;
	k_mutex_unlock(&temp_lock);
	LOG_INF("'%s' offset = %+.4f K", channels[ch].name, (double)offset_k);
}

float temp_sense_get_offset(enum temp_channel ch)
{
	if (ch < 0 || ch >= TEMP_CH_COUNT) {
		return 0.0f;
	}
	return channels[ch].offset_k;
}

const char *temp_sense_name(enum temp_channel ch)
{
	if (ch < 0 || ch >= TEMP_CH_COUNT) {
		return "?";
	}
	return channels[ch].name;
}

bool temp_sense_all_ok(int64_t now_ms)
{
	for (int i = 0; i < TEMP_CH_COUNT; i++) {
		if (!meas_ok(&channels[i].m, now_ms)) {
			return false;
		}
	}
	return true;
}
