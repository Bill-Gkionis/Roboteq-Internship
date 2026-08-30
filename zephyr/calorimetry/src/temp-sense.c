/*
 * temp-sense.c - TMP117 chamber temperatures, grouped into zones.
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

/* After this many consecutive bus failures a sensor is declared dead and
 * excluded from its zone's mean.  A few retries ride out a glitch; pretending
 * a disconnected sensor is fine is how a wrong answer looks right. */
#define TEMP_DEAD_AFTER 5

/* Ceiling on sensors per zone.  The guard array is expected to reach 8-12
 * across several faces; raise this and the overlay, nothing else. */
#define ZONE_MAX_SENSORS 8

struct zone_sensor {
	const struct device *dev;
	const struct device *bus;   /* this sensor's own mux leg / bus */
	float offset_k;
	float last_raw;
	uint8_t fail_count;
	uint8_t flags;              /* MEAS_VALID | MEAS_DEAD */
};

struct zone {
	const char *name;
	struct zone_sensor s[ZONE_MAX_SENSORS];
	uint8_t n;
	struct meas mean;
};

/*
 * Build a zone straight out of its devicetree node: the sensor list, and each
 * sensor's bus, come from the `sensors` phandle array.  Adding a probe is a
 * phandle in the overlay - this file does not change.
 */
#define ZONE_DEV(node_id, prop, idx) \
	{ .dev = DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx)), \
	  .bus = DEVICE_DT_GET(DT_BUS(DT_PHANDLE_BY_IDX(node_id, prop, idx))) },

#define ZONE_FROM_DT(node_id)                                                  \
	{                                                                      \
		.name = DT_PROP(node_id, zone_name),                           \
		.n = DT_PROP_LEN(node_id, sensors),                            \
		.s = { DT_FOREACH_PROP_ELEM(node_id, sensors, ZONE_DEV) },     \
	}

static struct zone zones[TEMP_CH_COUNT] = {
	[TEMP_CH_INNER] = ZONE_FROM_DT(DT_ALIAS(cal_zone_inner)),
	[TEMP_CH_GUARD] = ZONE_FROM_DT(DT_ALIAS(cal_zone_guard)),
};

BUILD_ASSERT(DT_PROP_LEN(DT_ALIAS(cal_zone_inner), sensors) <= ZONE_MAX_SENSORS,
	     "inner zone has more sensors than ZONE_MAX_SENSORS");
BUILD_ASSERT(DT_PROP_LEN(DT_ALIAS(cal_zone_guard), sensors) <= ZONE_MAX_SENSORS,
	     "guard zone has more sensors than ZONE_MAX_SENSORS");

/* The heatsink sensor is optional: the dev kit has no MOSFETs to watch. */
#if DT_HAS_ALIAS(cal_t_board)
static const struct device *const board_dev = DEVICE_DT_GET(DT_ALIAS(cal_t_board));
#else
static const struct device *const board_dev;
#endif
static struct meas board_meas;

static K_MUTEX_DEFINE(temp_lock);

int temp_sense_init(void)
{
	int missing = 0;

	for (int z = 0; z < TEMP_CH_COUNT; z++) {
		for (int i = 0; i < zones[z].n; i++) {
			if (!device_is_ready(zones[z].s[i].dev)) {
				LOG_ERR("zone '%s' sensor %d not ready",
					zones[z].name, i);
				zones[z].s[i].flags = MEAS_DEAD;
				missing++;
				continue;
			}
			LOG_INF("zone '%s' sensor %d = %s", zones[z].name, i,
				zones[z].s[i].dev->name);
		}
	}

	if (board_dev != NULL && device_is_ready(board_dev)) {
		LOG_INF("board temperature sensor = %s", board_dev->name);
	} else if (board_dev != NULL) {
		LOG_WRN("board temperature sensor not ready");
		missing++;
	}

	return missing ? -ENODEV : 0;
}

/** One sensor, one tick.  Never blocks longer than the driver's own timeout. */
static void read_one(struct zone_sensor *s, const char *zone, int idx)
{
	struct sensor_value val;
	int rc;

	if (s->flags & MEAS_DEAD) {
		return;
	}

	/* sensor_sample_fetch() is where the I2C transaction happens; on the
	 * carrier board it also walks through the TCA9548A leg. */
	rc = sensor_sample_fetch(s->dev);
	if (rc == 0) {
		rc = sensor_channel_get(s->dev, SENSOR_CHAN_AMBIENT_TEMP, &val);
	}

	if (rc != 0) {
		s->fail_count++;
		s->flags &= (uint8_t)~MEAS_VALID;
		if (s->fail_count >= TEMP_DEAD_AFTER) {
			LOG_ERR("zone '%s' sensor %d dead after %u failures",
				zone, idx, s->fail_count);
			s->flags |= MEAS_DEAD;
			/* Unwedge a slave holding SDA low, on THIS sensor's own
			 * leg.  Behind the mux the legs are separate buses, and
			 * recovering the wrong one would be a bug that only
			 * appears on real hardware. */
			if (device_is_ready(s->bus)) {
				(void)i2c_recover_bus(s->bus);
			}
		}
		return;
	}

	if (s->fail_count > 0) {
		s->flags |= MEAS_RETRIED;
		s->fail_count = 0;
	}

	s->last_raw = (float)sensor_value_to_double(&val);
	s->flags |= MEAS_VALID;
}

void temp_sense_read_all(void)
{
	const int64_t now = k_uptime_get();

	k_mutex_lock(&temp_lock, K_FOREVER);

	for (int z = 0; z < TEMP_CH_COUNT; z++) {
		struct zone *zn = &zones[z];
		float sum = 0.0f;
		int n = 0;

		for (int i = 0; i < zn->n; i++) {
			read_one(&zn->s[i], zn->name, i);
			if (zn->s[i].flags & MEAS_VALID) {
				sum += zn->s[i].last_raw - zn->s[i].offset_k;
				n++;
			}
		}

		if (n > 0) {
			zn->mean.v = sum / (float)n;
			zn->mean.t_ms = now;
			/*
			 * The zone stays VALID on a partial set - a dead probe
			 * degrades the mean, it does not invalidate the zone.
			 * Whether the POINT is still valid on a degraded set is
			 * the supervisor's call, not this module's.
			 */
			zn->mean.flags = MEAS_VALID |
					 ((n < zn->n) ? MEAS_RETRIED : 0);
		} else {
			/* Deliberately does NOT touch t_ms: the staleness check
			 * downstream is what turns a silently frozen bus into a
			 * fault instead of a plausible constant the guard
			 * integrator happily accumulates against. */
			zn->mean.flags &= (uint8_t)~MEAS_VALID;
		}
	}

	if (board_dev != NULL && device_is_ready(board_dev)) {
		struct sensor_value v;

		if (sensor_sample_fetch(board_dev) == 0 &&
		    sensor_channel_get(board_dev, SENSOR_CHAN_AMBIENT_TEMP,
				       &v) == 0) {
			board_meas.v = (float)sensor_value_to_double(&v);
			board_meas.t_ms = now;
			board_meas.flags = MEAS_VALID;
		} else {
			board_meas.flags &= (uint8_t)~MEAS_VALID;
		}
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
	out = zones[ch].mean;
	k_mutex_unlock(&temp_lock);

	return out;
}

int temp_sense_sensor_count(enum temp_channel ch)
{
	return (ch >= 0 && ch < TEMP_CH_COUNT) ? zones[ch].n : 0;
}

bool temp_sense_raw(enum temp_channel ch, int idx, float *out)
{
	if (ch < 0 || ch >= TEMP_CH_COUNT || idx < 0 || idx >= zones[ch].n) {
		return false;
	}

	bool ok;

	k_mutex_lock(&temp_lock, K_FOREVER);
	ok = (zones[ch].s[idx].flags & MEAS_VALID) != 0;
	if (ok && out) {
		*out = zones[ch].s[idx].last_raw;
	}
	k_mutex_unlock(&temp_lock);

	return ok;
}

void temp_sense_set_offset(enum temp_channel ch, int idx, float offset_k)
{
	if (ch < 0 || ch >= TEMP_CH_COUNT || idx < 0 || idx >= zones[ch].n) {
		return;
	}

	k_mutex_lock(&temp_lock, K_FOREVER);
	zones[ch].s[idx].offset_k = offset_k;
	k_mutex_unlock(&temp_lock);

	LOG_INF("'%s'[%d] offset = %+.4f K", zones[ch].name, idx,
		(double)offset_k);
}

float temp_sense_get_offset(enum temp_channel ch, int idx)
{
	if (ch < 0 || ch >= TEMP_CH_COUNT || idx < 0 || idx >= zones[ch].n) {
		return 0.0f;
	}
	return zones[ch].s[idx].offset_k;
}

const char *temp_sense_name(enum temp_channel ch)
{
	return (ch >= 0 && ch < TEMP_CH_COUNT) ? zones[ch].name : "?";
}

bool temp_sense_all_ok(int64_t now_ms)
{
	for (int z = 0; z < TEMP_CH_COUNT; z++) {
		if (!meas_ok(&zones[z].mean, now_ms)) {
			return false;
		}
	}
	return true;
}

struct meas temp_sense_board(void)
{
	struct meas out;

	k_mutex_lock(&temp_lock, K_FOREVER);
	out = board_meas;
	k_mutex_unlock(&temp_lock);

	return out;
}

bool temp_sense_has_board(void)
{
	return board_dev != NULL;
}
