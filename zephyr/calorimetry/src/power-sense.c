/*
 * power-sense.c - INA226 / INA3221 rail metering and the over-current trips.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "power-sense.h"

LOG_MODULE_REGISTER(power_sense, LOG_LEVEL_INF);

/* INA226 register map - only the two the standard driver does not expose. */
#define INA226_REG_MASK_ENABLE 0x06
#define INA226_REG_ALERT_LIMIT 0x07
#define INA226_MASK_SOL        BIT(15)  /* Shunt Over-voltage -> ALERT      */
#define INA226_MASK_LATCH      BIT(0)   /* latch until read: a trip STAYS   */
#define INA226_SHUNT_LSB_UV    2.5f     /* shunt voltage LSB, microvolts    */

/*
 * The INA3221 driver picks which of its three channels channel_get() returns
 * through a private attribute.  The symbol lives in the driver's own header
 * (drivers/sensor/ti/ina3221/ina3221.h) rather than a public one, so it is
 * mirrored here.  Its VALUE is public API - SENSOR_ATTR_PRIV_START is - and a
 * BUILD_ASSERT would not help because there is nothing public to compare
 * against.  Worth an upstream patch promoting it to
 * include/zephyr/drivers/sensor/ina3221.h.
 */
#define SENSOR_ATTR_INA3221_SELECTED_CHANNEL \
	((enum sensor_attribute)(SENSOR_ATTR_PRIV_START + 1))

enum rail_kind { RAIL_INA226, RAIL_INA3221 };

struct rail {
	const struct device *dev;
	struct i2c_dt_spec i2c;      /* INA226 only: the raw register writes */
	enum rail_kind kind;
	uint8_t channel;             /* INA3221 only: 1..3                   */
	const char *name;
	bool inside_b1;
	bool has_alert;
	uint32_t rshunt_uohm;
	float limit_a;               /* software backstop, 0 = disabled      */
	struct meas power;
	float current_a;
	float voltage_v;
	uint8_t fail_count;
};

#define INA226_RAIL(alias, nm, b1)                                             \
	{                                                                      \
		.dev = DEVICE_DT_GET(DT_ALIAS(alias)),                         \
		.i2c = I2C_DT_SPEC_GET(DT_ALIAS(alias)),                       \
		.kind = RAIL_INA226,                                           \
		.name = nm,                                                    \
		.inside_b1 = b1,                                               \
		.has_alert = DT_NODE_HAS_PROP(DT_ALIAS(alias), alert_gpios),   \
		.rshunt_uohm = DT_PROP(DT_ALIAS(alias), rshunt_micro_ohms),    \
	}

#define INA3221_RAIL(ch, nm, b1)                                               \
	{                                                                      \
		.dev = DEVICE_DT_GET(DT_ALIAS(cal_ina_aux)),                   \
		.kind = RAIL_INA3221,                                          \
		.channel = ch,                                                 \
		.name = nm,                                                    \
		.inside_b1 = b1,                                               \
	}

static struct rail rails[PWR_COUNT] = {
	[PWR_HEAT_INNER] = INA226_RAIL(cal_ina_hin,  "heat_inner", true),
	[PWR_HEAT_GUARD] = INA226_RAIL(cal_ina_hgrd, "heat_guard", false),
	[PWR_FAN_INNER]  = INA3221_RAIL(1, "fan_inner", true),
	[PWR_FAN_GUARD]  = INA3221_RAIL(2, "fan_guard", false),
	[PWR_PUMP]       = INA3221_RAIL(3, "pump",      false),
};

static K_MUTEX_DEFINE(power_lock);

int power_sense_init(void)
{
	int missing = 0;

	for (int i = 0; i < PWR_COUNT; i++) {
		if (!device_is_ready(rails[i].dev)) {
			LOG_ERR("rail '%s' device not ready", rails[i].name);
			missing++;
			continue;
		}
		if (rails[i].kind == RAIL_INA226) {
			LOG_INF("rail '%s': INA226, %s B1%s", rails[i].name,
				rails[i].inside_b1 ? "INSIDE" : "outside",
				rails[i].has_alert ? ", ALERT routed" : "");
		} else {
			LOG_INF("rail '%s': INA3221 ch%u, %s B1",
				rails[i].name, rails[i].channel,
				rails[i].inside_b1 ? "INSIDE" : "outside");
		}
	}

	return missing ? -ENODEV : 0;
}

/** Point the INA3221 at one of its three channels before reading it. */
static int select_channel(const struct rail *r)
{
	const struct sensor_value v = { .val1 = r->channel, .val2 = 0 };

	return sensor_attr_set(r->dev, SENSOR_CHAN_ALL,
			       SENSOR_ATTR_INA3221_SELECTED_CHANNEL, &v);
}

static void read_one(struct rail *r, bool already_fetched)
{
	struct sensor_value v;

	if (!device_is_ready(r->dev)) {
		r->power.flags = MEAS_DEAD;
		return;
	}

	if (!already_fetched && sensor_sample_fetch(r->dev) != 0) {
		r->fail_count++;
		r->power.flags &= (uint8_t)~MEAS_VALID;
		return;
	}

	if (r->kind == RAIL_INA3221 && select_channel(r) != 0) {
		r->power.flags &= (uint8_t)~MEAS_VALID;
		return;
	}

	r->fail_count = 0;

	if (sensor_channel_get(r->dev, SENSOR_CHAN_POWER, &v) == 0) {
		r->power.v = (float)sensor_value_to_double(&v);
		r->power.t_ms = k_uptime_get();
		r->power.flags = MEAS_VALID;
	}
	if (sensor_channel_get(r->dev, SENSOR_CHAN_CURRENT, &v) == 0) {
		r->current_a = (float)sensor_value_to_double(&v);
	}
	if (sensor_channel_get(r->dev, SENSOR_CHAN_VOLTAGE, &v) == 0) {
		r->voltage_v = (float)sensor_value_to_double(&v);
	}
}

void power_sense_read_all(void)
{
	k_mutex_lock(&power_lock, K_FOREVER);

	/*
	 * The INA3221's sample_fetch() triggers one conversion and fills all
	 * three channels, so it is fetched exactly once here and then read
	 * three times with the channel selector moved between reads.  Fetching
	 * per rail would triple the conversion wait for no new data.
	 */
	bool aux_fetched = false;

	for (int i = 0; i < PWR_COUNT; i++) {
		if (rails[i].kind == RAIL_INA3221 && !aux_fetched) {
			if (device_is_ready(rails[i].dev) &&
			    sensor_sample_fetch(rails[i].dev) == 0) {
				aux_fetched = true;
			}
		}
		read_one(&rails[i],
			 rails[i].kind == RAIL_INA3221 && aux_fetched);
	}

	k_mutex_unlock(&power_lock);
}

struct meas power_sense_get(enum power_rail rail)
{
	struct meas out = { 0 };

	if (rail < 0 || rail >= PWR_COUNT) {
		return out;
	}
	k_mutex_lock(&power_lock, K_FOREVER);
	out = rails[rail].power;
	k_mutex_unlock(&power_lock);

	return out;
}

float power_sense_current(enum power_rail rail)
{
	return (rail >= 0 && rail < PWR_COUNT) ? rails[rail].current_a : 0.0f;
}

float power_sense_voltage(enum power_rail rail)
{
	return (rail >= 0 && rail < PWR_COUNT) ? rails[rail].voltage_v : 0.0f;
}

float power_sense_p_aux(void)
{
	float sum = 0.0f;
	const int64_t now = k_uptime_get();

	k_mutex_lock(&power_lock, K_FOREVER);
	for (int i = 0; i < PWR_COUNT; i++) {
		if (rails[i].inside_b1 && meas_ok(&rails[i].power, now)) {
			sum += rails[i].power.v;
		}
	}
	k_mutex_unlock(&power_lock);

	return sum;
}

int power_sense_arm_alert(enum power_rail rail, float trip_amps)
{
	if (rail < 0 || rail >= PWR_COUNT) {
		return -EINVAL;
	}

	struct rail *r = &rails[rail];

	/* The software backstop is armed either way - the hardware trip is the
	 * faster line, not the only one. */
	power_sense_set_limit(rail, trip_amps);

	if (r->kind != RAIL_INA226) {
		return -ENOTSUP;   /* the INA3221 rails carry no ALERT here */
	}
	if (!r->has_alert) {
		LOG_WRN("'%s': no ALERT line on this board - over-current "
			"falls back to the software check (one tick slower)",
			r->name);
		return -ENOTSUP;
	}
	if (!i2c_is_ready_dt(&r->i2c)) {
		return -ENODEV;
	}

	/* V_shunt = I * R.  Convert to the INA226's 2.5 uV shunt LSB. */
	const float v_shunt_uv = trip_amps * (float)r->rshunt_uohm;
	const int32_t limit = (int32_t)(v_shunt_uv / INA226_SHUNT_LSB_UV);

	if (limit <= 0 || limit > INT16_MAX) {
		LOG_ERR("'%s': %.2f A is outside the shunt's range",
			r->name, (double)trip_amps);
		return -ERANGE;
	}

	uint8_t buf[3];
	int rc;

	buf[0] = INA226_REG_ALERT_LIMIT;
	sys_put_be16((uint16_t)limit, &buf[1]);
	rc = i2c_write_dt(&r->i2c, buf, sizeof(buf));
	if (rc) {
		return rc;
	}

	buf[0] = INA226_REG_MASK_ENABLE;
	sys_put_be16(INA226_MASK_SOL | INA226_MASK_LATCH, &buf[1]);
	rc = i2c_write_dt(&r->i2c, buf, sizeof(buf));
	if (rc) {
		return rc;
	}

	LOG_INF("'%s' hardware ALERT armed at %.2f A (limit code %d)",
		r->name, (double)trip_amps, limit);

	return 0;
}

void power_sense_set_limit(enum power_rail rail, float amps)
{
	if (rail >= 0 && rail < PWR_COUNT) {
		rails[rail].limit_a = amps;
	}
}

enum power_rail power_sense_overcurrent(void)
{
	const int64_t now = k_uptime_get();

	for (int i = 0; i < PWR_COUNT; i++) {
		if (rails[i].limit_a <= 0.0f) {
			continue;
		}
		/* Only a FRESH reading may trip: a stale one is caught by the
		 * staleness check instead, and tripping on it would turn every
		 * bus hiccup into an over-current fault. */
		if (meas_ok(&rails[i].power, now) &&
		    rails[i].current_a > rails[i].limit_a) {
			return (enum power_rail)i;
		}
	}

	return PWR_COUNT;
}

const char *power_sense_name(enum power_rail rail)
{
	return (rail >= 0 && rail < PWR_COUNT) ? rails[rail].name : "?";
}

bool power_sense_inside_b1(enum power_rail rail)
{
	return (rail >= 0 && rail < PWR_COUNT) ? rails[rail].inside_b1 : false;
}
