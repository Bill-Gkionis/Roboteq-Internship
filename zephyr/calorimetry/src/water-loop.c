/*
 * water-loop.c - pump, RTD dT pair, turbine flow meter.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

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

/* ------------------------------------------------ the TMC5160, over SPI ---- */

#define PUMP_DRV_NODE DT_ALIAS(cal_pump_drv)

/* Mode 3 (CPOL | CPHA), MSB first, 8-bit words - what the TMC5160 requires.
 * The MAX31865s on the same bus declare mode 1 in their own driver; Zephyr
 * reprograms the controller per transfer, so the two coexist and CS gates the
 * deselected device through the CPOL change. */
static const struct spi_dt_spec pump_drv = SPI_DT_SPEC_GET(
	PUMP_DRV_NODE,
	SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB |
		SPI_MODE_CPOL | SPI_MODE_CPHA);

/* TMC5160 register map - only what is written here. */
#define TMC_GCONF         0x00
#define TMC_GSTAT         0x01
#define TMC_IOIN          0x04
#define TMC_GLOBALSCALER  0x0B
#define TMC_IHOLD_IRUN    0x10
#define TMC_TPOWERDOWN    0x11
#define TMC_CHOPCONF      0x6C
#define TMC_DRV_STATUS    0x6F

#define TMC_WRITE         0x80
#define TMC5160_VERSION   0x30   /* IOIN[31:24] on a genuine TMC5160 */

/* Full-scale sense voltage, datasheet "Motor current control". */
#define TMC_V_FS 0.325f

/*
 * A TMC datagram is 40 bits: one address byte, then four data bytes MSB first.
 * A read is two transfers - the first latches the address, the second returns
 * that register's contents while latching the next address.
 */
static int tmc_write(uint8_t reg, uint32_t val)
{
	uint8_t tx[5] = { (uint8_t)(reg | TMC_WRITE) };
	const struct spi_buf buf = { .buf = tx, .len = sizeof(tx) };
	const struct spi_buf_set set = { .buffers = &buf, .count = 1 };

	sys_put_be32(val, &tx[1]);

	return spi_write_dt(&pump_drv, &set);
}

static int tmc_read(uint8_t reg, uint32_t *val)
{
	uint8_t tx[5] = { (uint8_t)(reg & 0x7F) };
	uint8_t rx[5] = { 0 };
	const struct spi_buf txb = { .buf = tx, .len = sizeof(tx) };
	const struct spi_buf rxb = { .buf = rx, .len = sizeof(rx) };
	const struct spi_buf_set txs = { .buffers = &txb, .count = 1 };
	const struct spi_buf_set rxs = { .buffers = &rxb, .count = 1 };
	int rc;

	/* First transfer latches the address; its reply is the PREVIOUS
	 * register, which is why the same request is sent twice. */
	rc = spi_transceive_dt(&pump_drv, &txs, &rxs);
	if (rc) {
		return rc;
	}
	rc = spi_transceive_dt(&pump_drv, &txs, &rxs);
	if (rc) {
		return rc;
	}

	*val = sys_get_be32(&rx[1]);

	return 0;
}

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

/* ------------------------------------------ stepper driver configuration --- */

int water_loop_pump_driver_version(void)
{
	uint32_t ioin;
	const int rc = tmc_read(TMC_IOIN, &ioin);

	if (rc) {
		return rc;
	}

	return (int)(ioin >> 24);
}

int water_loop_pump_driver_configure(void)
{
	if (!spi_is_ready_dt(&pump_drv)) {
		LOG_ERR("stepper driver SPI not ready");
		return -ENODEV;
	}

	const int ver = water_loop_pump_driver_version();

	if (ver < 0) {
		LOG_ERR("stepper driver SPI read failed (%d)", ver);
		return ver;
	}
	if (ver != TMC5160_VERSION) {
		/* Definitive: a wrong version byte means the link is wrong -
		 * wiring, SPI mode, or a different part fitted.  Nothing about
		 * the current setting below would be trustworthy. */
		LOG_ERR("stepper driver reports version 0x%02x, expected 0x%02x "
			"- check CS, MISO and SPI mode 3", ver, TMC5160_VERSION);
		return -ENODEV;
	}

	(void)tmc_write(TMC_GSTAT, 0x07);   /* clear latched flags by writing 1s */

	const uint32_t r_sense_mohm =
		DT_PROP(PUMP_DRV_NODE, sense_resistor_milliohms);
	const uint32_t run_ma = DT_PROP(PUMP_DRV_NODE, run_current_ma);
	const uint32_t hold_ma = DT_PROP(PUMP_DRV_NODE, hold_current_ma);
	const uint32_t microsteps = DT_PROP(PUMP_DRV_NODE, microsteps);

	/*
	 * Datasheet: I_RMS = (GLOBALSCALER/256) * ((CS+1)/32) * (V_FS/R_SENSE)
	 *                    * (1/sqrt(2))
	 *
	 * IRUN is pinned at 31 (full CS) and the whole scaling is done with
	 * GLOBALSCALER, which is the coarser knob but keeps the chopper's
	 * current regulation resolution at its best.
	 *
	 *   GLOBALSCALER = 256 * I_RMS * sqrt(2) * R_SENSE / V_FS
	 *
	 * A value of 0 means 256 in the register, and values under 32 are not
	 * recommended by the datasheet, so the result is clamped into [32,256].
	 */
	const float r_sense = (float)r_sense_mohm / 1000.0f;
	const float i_run = (float)run_ma / 1000.0f;
	float gs_f = 256.0f * i_run * 1.41421356f * r_sense / TMC_V_FS;
	uint32_t gs = (uint32_t)cal_clampf(gs_f, 32.0f, 256.0f);

	/* IHOLD is a fraction of IRUN, in the same 0..31 scale. */
	uint32_t ihold = (uint32_t)((31.0f * (float)hold_ma) / (float)run_ma);

	ihold = MIN(ihold, 31U);

	/* MRES: 0 = 256 microsteps, 8 = full step.  log2(256/microsteps). */
	uint32_t mres = 0;

	for (uint32_t m = microsteps; m < 256U; m <<= 1) {
		mres++;
	}

	/*
	 * CHOPCONF: spreadCycle with the datasheet's suggested starting point -
	 * TOFF=3, HSTRT=4, HEND=1, TBL=2 - plus the microstep setting.
	 * spreadCycle rather than stealthChop: quiet running is a nice-to-have
	 * for a lab instrument, but a peristaltic head against a head of water
	 * is a load where spreadCycle's torque behaviour is the safer default.
	 */
	const uint32_t chopconf = (mres << 24) | (2U << 15) | (1U << 7) |
				  (4U << 4) | 3U;

	(void)tmc_write(TMC_GCONF, 0x00000000);        /* spreadCycle, STEP/DIR */
	(void)tmc_write(TMC_GLOBALSCALER, gs & 0xFF);  /* 256 encodes as 0      */
	(void)tmc_write(TMC_IHOLD_IRUN,
			(6U << 16) | (31U << 8) | ihold);  /* IHOLDDELAY=6      */
	(void)tmc_write(TMC_TPOWERDOWN, 10);
	(void)tmc_write(TMC_CHOPCONF, chopconf);

	/* Recompute what was actually programmed, so the log states the current
	 * the motor will really see rather than the one that was asked for. */
	const float i_actual = ((float)(gs == 0 ? 256 : gs) / 256.0f) *
			       (TMC_V_FS / r_sense) / 1.41421356f;

	LOG_INF("TMC5160 v0x%02x: %u microsteps, R_sense %u mOhm, "
		"GLOBALSCALER %u -> %.2f A RMS run (asked %.2f), %.2f A hold",
		ver, microsteps, r_sense_mohm, gs, (double)i_actual,
		(double)i_run,
		(double)(i_actual * (float)ihold / 31.0f));

	if (fabsf(i_actual - i_run) > 0.15f) {
		LOG_WRN("programmed run current is %.2f A, not the %.2f A "
			"requested - GLOBALSCALER quantisation or a clamp",
			(double)i_actual, (double)i_run);
	}

	return 0;
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

	/* Configure the driver while its enable is still de-asserted, so the
	 * motor cannot move on a half-written register set. */
	if (water_loop_pump_driver_configure() != 0) {
		LOG_WRN("stepper driver not configured - the pump will run at "
			"whatever current the driver powers up with");
		missing++;
	}

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
