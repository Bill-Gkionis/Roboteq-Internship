/*
 * power-sense.c - INA226 rail metering and the hardware over-current arm.
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

struct rail {
	const struct device *dev;
	struct i2c_dt_spec i2c;      /* for the two raw register writes */
	const char *name;
	bool inside_b1;
	uint32_t rshunt_uohm;
	struct meas power;
	float current_a;
	float voltage_v;
	uint8_t fail_count;
};

#define RAIL_ENTRY(alias, nm, b1)                                              \
	{                                                                      \
		.dev = DEVICE_DT_GET(DT_ALIAS(alias)),                         \
		.i2c = I2C_DT_SPEC_GET(DT_ALIAS(alias)),                       \
		.name = nm,                                                    \
		.inside_b1 = b1,                                               \
		.rshunt_uohm = DT_PROP(DT_ALIAS(alias), rshunt_micro_ohms),    \
	}

static struct rail rails[PWR_COUNT] = {
	[PWR_HEAT_INNER] = RAIL_ENTRY(cal_ina_hin,  "heat_inner", true),
	[PWR_HEAT_GUARD] = RAIL_ENTRY(cal_ina_hgrd, "heat_guard", false),
	[PWR_FAN_INNER]  = RAIL_ENTRY(cal_ina_fan,  "fan_inner",  true),
	[PWR_PUMP]       = RAIL_ENTRY(cal_ina_pump, "pump",       false),
};

static K_MUTEX_DEFINE(power_lock);

int power_sense_init(void)
{
	int missing = 0;

	for (int i = 0; i < PWR_COUNT; i++) {
		if (!device_is_ready(rails[i].dev)) {
			LOG_ERR("INA226 '%s' not ready", rails[i].name);
			missing++;
			continue;
		}
		LOG_INF("INA226 '%s' bound (%s B1, Rshunt %u uOhm)",
			rails[i].name, rails[i].inside_b1 ? "INSIDE" : "outside",
			rails[i].rshunt_uohm);
	}

	return missing ? -ENODEV : 0;
}

static void read_one(struct rail *r)
{
	struct sensor_value v;

	if (!device_is_ready(r->dev)) {
		r->power.flags = MEAS_DEAD;
		return;
	}

	if (sensor_sample_fetch(r->dev) != 0) {
		r->fail_count++;
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
	for (int i = 0; i < PWR_COUNT; i++) {
		read_one(&rails[i]);
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

	LOG_INF("'%s' ALERT armed at %.2f A (limit code %d)",
		r->name, (double)trip_amps, limit);

	return 0;
}

const char *power_sense_name(enum power_rail rail)
{
	return (rail >= 0 && rail < PWR_COUNT) ? rails[rail].name : "?";
}

bool power_sense_inside_b1(enum power_rail rail)
{
	return (rail >= 0 && rail < PWR_COUNT) ? rails[rail].inside_b1 : false;
}
