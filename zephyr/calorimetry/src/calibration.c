/*
 * calibration.c - isothermal soak, per-range dT tare, substitution check.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "calibration.h"
#include "model_gen.h"
#include "control-system.h"
#include "temp-sense.h"
#include "water-loop.h"
#include "heaters.h"
#include "fans.h"

LOG_MODULE_REGISTER(calib, LOG_LEVEL_INF);

/* How long each averaging phase runs.  Short enough to be usable during a
 * bring-up session, long enough that TMP117 repeatability averages down. */
#define SOAK_SECONDS   120
#define TARE_SECONDS   120
#define SUBST_SECONDS  600   /* the meter has to actually settle */

/* One tare per ranging row, because the tare is flow dependent. */
#define TARE_ROWS CAL_RANGE_COUNT

/* --------------------------------------------------------------- store ---- */

static struct {
	float t_offset[TEMP_CH_COUNT];   /* K, per chamber sensor      */
	float tare[TARE_ROWS];           /* K, per flow range          */
	float k_factor;                  /* pulses/L, turbine          */
	float ml_per_step;               /* mL, pump displacement      */
	bool  observer_trusted;          /* may the observer gate?     */
} store;

static int settings_set_cb(const char *name, size_t len,
			   settings_read_cb read_cb, void *cb_arg)
{
	const char *next;

	if (settings_name_steq(name, "toff", &next) && next) {
		const int i = atoi(next);

		if (i >= 0 && i < TEMP_CH_COUNT && len == sizeof(float)) {
			return read_cb(cb_arg, &store.t_offset[i], len) > 0
				       ? 0 : -EINVAL;
		}
	}
	if (settings_name_steq(name, "tare", &next) && next) {
		const int i = atoi(next);

		if (i >= 0 && i < TARE_ROWS && len == sizeof(float)) {
			return read_cb(cb_arg, &store.tare[i], len) > 0
				       ? 0 : -EINVAL;
		}
	}
	if (settings_name_steq(name, "kfac", NULL) && len == sizeof(float)) {
		return read_cb(cb_arg, &store.k_factor, len) > 0 ? 0 : -EINVAL;
	}
	if (settings_name_steq(name, "mlstep", NULL) && len == sizeof(float)) {
		return read_cb(cb_arg, &store.ml_per_step, len) > 0 ? 0 : -EINVAL;
	}
	if (settings_name_steq(name, "obstrust", NULL) && len == sizeof(bool)) {
		return read_cb(cb_arg, &store.observer_trusted, len) > 0
			       ? 0 : -EINVAL;
	}

	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(cal, "cal", NULL, settings_set_cb, NULL, NULL);

static void save_float(const char *key, float v)
{
	char path[24];

	(void)snprintk(path, sizeof(path), "cal/%s", key);
	const int rc = settings_save_one(path, &v, sizeof(v));

	if (rc) {
		LOG_ERR("could not persist %s (%d)", path, rc);
	}
}

int calibration_init(void)
{
	int rc = settings_subsys_init();

	if (rc) {
		LOG_ERR("settings subsystem init failed (%d) - "
			"calibration values will NOT persist", rc);
		return rc;
	}

	/* Defaults, overwritten by whatever NVS actually holds. */
	store.k_factor = water_loop_get_k_factor();
	store.ml_per_step = water_loop_get_ml_per_step();

	rc = settings_load();
	if (rc) {
		LOG_WRN("settings_load failed (%d) - using defaults", rc);
	}

	for (int i = 0; i < TEMP_CH_COUNT; i++) {
		temp_sense_set_offset((enum temp_channel)i, store.t_offset[i]);
	}
	water_loop_set_k_factor(store.k_factor);
	water_loop_set_ml_per_step(store.ml_per_step);

	LOG_INF("calibration loaded: K = %.1f pulses/L, %.5f mL/step, "
		"observer %s",
		(double)store.k_factor, (double)store.ml_per_step,
		store.observer_trusted ? "TRUSTED" : "shadow only");

	return 0;
}

float calibration_tare_for_flow(float ml_per_min)
{
	/* Pick the row the same way the ranging table is picked, so the tare
	 * and the flow always come from the same row. */
	for (int i = 0; i < TARE_ROWS; i++) {
		const float row_flow = cal_flow_for_point(cal_ranges[i].p_max,
							  cal_ranges[i].dt_set,
							  25.0f);
		if (ml_per_min <= row_flow) {
			return store.tare[i];
		}
	}

	return store.tare[TARE_ROWS - 1];
}

static int tare_row_for_flow(float ml_per_min)
{
	for (int i = 0; i < TARE_ROWS; i++) {
		const float row_flow = cal_flow_for_point(cal_ranges[i].p_max,
							  cal_ranges[i].dt_set,
							  25.0f);
		if (ml_per_min <= row_flow) {
			return i;
		}
	}
	return TARE_ROWS - 1;
}

/* ------------------------------------------------------------ procedures -- */

static struct {
	enum cal_cmd_type active;
	enum cal_state pub;
	float arg;
	int elapsed_s;
	int total_s;
	struct cal_stat acc[4];   /* one per averaged channel */
	int tare_row;
} run;

static void finish(void)
{
	heaters_all_off();
	water_loop_stop();
	run.active = CAL_CMD_NONE;
	run.pub = CAL_ST_IDLE;
}

void calibration_request(enum cal_cmd_type type, float arg)
{
	run.active = type;
	run.arg = arg;
	run.elapsed_s = 0;
	run.tare_row = 0;

	for (int i = 0; i < 4; i++) {
		cal_stat_reset(&run.acc[i]);
	}

	switch (type) {
	case CAL_CMD_CAL_SOAK:
		run.pub = CAL_ST_CAL_SOAK;
		run.total_s = SOAK_SECONDS;
		/* Everything off: the whole point is that the rig is
		 * un-driven and therefore genuinely isothermal. */
		heaters_all_off();
		water_loop_stop();
		fans_all_off();
		LOG_INF("SOAK: %d s, everything off", run.total_s);
		break;

	case CAL_CMD_CAL_TARE:
		run.pub = CAL_ST_CAL_TARE;
		run.total_s = TARE_SECONDS;
		heaters_all_off();
		/* arg is the flow to tare at; default to the mid range. */
		if (run.arg < 1.0f) {
			run.arg = cal_flow_for_point(cal_ranges[1].p_max,
						      cal_ranges[1].dt_set,
						      25.0f);
		}
		run.tare_row = tare_row_for_flow(run.arg);
		water_loop_set_tare(0.0f);   /* measure the RAW dT */
		water_loop_set_flow(run.arg);
		LOG_INF("TARE: %d s at %.1f mL/min (row %d)",
			run.total_s, (double)run.arg, run.tare_row);
		break;

	case CAL_CMD_CAL_SUBST:
		run.pub = CAL_ST_CAL_SUBST;
		run.total_s = SUBST_SECONDS;
		if (run.arg < 1.0f) {
			run.arg = 100.0f;
		}
		/* Flow chosen for the injected power, exactly as a real point
		 * would be ranged. */
		water_loop_set_flow(cal_flow_for_point(
			run.arg, cal_range_for(run.arg)->dt_set, 25.0f));
		(void)fans_set(FAN_INNER, 0.7f);
		fans_freeze_inner(true);
		(void)fans_set(FAN_REJECT, 1.0f);
		heaters_set_enable(true);
		heaters_set_power(HEATER_INNER, run.arg);
		LOG_INF("SUBST: injecting %.1f W for %d s",
			(double)run.arg, run.total_s);
		break;

	default:
		run.active = CAL_CMD_NONE;
		break;
	}
}

static float last_injected, last_read, last_ratio;

bool calibration_service(struct cal_snapshot *s, float dt)
{
	if (run.active == CAL_CMD_NONE) {
		return true;
	}

	run.elapsed_s += (int)(dt + 0.5f);

	/* Only the last two thirds are averaged; the first third lets the
	 * procedure's own transient die. */
	const bool averaging = run.elapsed_s > (run.total_s / 3);

	switch (run.active) {
	case CAL_CMD_CAL_SOAK:
		if (averaging) {
			for (int i = 0; i < TEMP_CH_COUNT; i++) {
				struct meas m = temp_sense_get(
					(enum temp_channel)i);

				if (m.flags & MEAS_VALID) {
					/* The stored offset is already
					 * subtracted on read, so add it back to
					 * recover the RAW reading. */
					cal_stat_push(&run.acc[i],
						      m.v + store.t_offset[i]);
				}
			}
		}

		if (run.elapsed_s >= run.total_s) {
			/* The reference is the group mean: an isothermal rig
			 * has no better absolute truth available, and relative
			 * agreement is what the leak term actually depends on. */
			float mean = 0.0f;
			int n = 0;

			for (int i = 0; i < TEMP_CH_COUNT; i++) {
				if (run.acc[i].n) {
					mean += cal_stat_mean(&run.acc[i]);
					n++;
				}
			}
			if (n == 0) {
				LOG_ERR("SOAK: no valid samples");
				finish();
				return true;
			}
			mean /= (float)n;

			for (int i = 0; i < TEMP_CH_COUNT; i++) {
				if (!run.acc[i].n) {
					continue;
				}
				const float off = cal_stat_mean(&run.acc[i]) - mean;
				char key[12];

				store.t_offset[i] = off;
				temp_sense_set_offset((enum temp_channel)i, off);
				(void)snprintk(key, sizeof(key), "toff/%d", i);
				save_float(key, off);
				LOG_INF("SOAK: '%s' offset %+.4f K",
					temp_sense_name((enum temp_channel)i),
					(double)off);
			}
			finish();
			return true;
		}
		break;

	case CAL_CMD_CAL_TARE:
		if (averaging) {
			struct meas ti = s->t_water_in;
			struct meas to = s->t_water_out;

			if ((ti.flags & MEAS_VALID) && (to.flags & MEAS_VALID)) {
				cal_stat_push(&run.acc[0], to.v - ti.v);
			}
		}

		if (run.elapsed_s >= run.total_s) {
			if (run.acc[0].n < 10) {
				LOG_ERR("TARE: too few samples");
				finish();
				return true;
			}

			const float tare = cal_stat_mean(&run.acc[0]);
			const float se = cal_stat_stderr(&run.acc[0]);
			char key[12];

			store.tare[run.tare_row] = tare;
			water_loop_set_tare(tare);
			(void)snprintk(key, sizeof(key), "tare/%d", run.tare_row);
			save_float(key, tare);

			LOG_INF("TARE row %d @ %.1f mL/min: %+.4f K "
				"(SE %.4f K, = %.3f W at this flow)",
				run.tare_row, (double)run.arg, (double)tare,
				(double)se, (double)(tare * s->mcp));
			finish();
			return true;
		}
		break;

	case CAL_CMD_CAL_SUBST:
		if (averaging) {
			cal_stat_push(&run.acc[0], s->p_meas);
			cal_stat_push(&run.acc[1],
				      s->p_heat_inner.v);
		}

		if (run.elapsed_s >= run.total_s) {
			last_read = cal_stat_mean(&run.acc[0]);
			last_injected = cal_stat_mean(&run.acc[1]);
			last_ratio = (last_injected > 1.0f)
					     ? (last_read / last_injected)
					     : 0.0f;

			/*
			 * Note what this comparison really is.  P_aux already
			 * subtracts the inner heater, so a correct rig reads
			 * approximately ZERO here: the substitution power goes
			 * in and comes straight back out of the reading.  What
			 * the ratio tests is the whole chain - flow scale, dT
			 * pair, tare and the P_aux subtraction together.
			 */
			LOG_INF("SUBST: injected %.2f W, meter read %.3f W, "
				"residual %.3f W (%.2f %% of injected)",
				(double)last_injected, (double)last_read,
				(double)last_read,
				(double)(100.0f * last_ratio));

			if (last_injected > 1.0f &&
			    fabsf(last_read) > CAL_P_ACC) {
				LOG_WRN("SUBST: residual exceeds the +-%.1f W "
					"accuracy target - the flow calibration "
					"is the first thing to suspect",
					(double)CAL_P_ACC);
			}
			finish();
			return true;
		}
		break;

	default:
		finish();
		return true;
	}

	s->state = run.pub;

	return false;
}

enum cal_state calibration_state(void)
{
	return run.pub;
}

float calibration_progress(void)
{
	if (run.active == CAL_CMD_NONE || run.total_s <= 0) {
		return 0.0f;
	}
	return cal_clampf((float)run.elapsed_s / (float)run.total_s, 0.0f, 1.0f);
}

void calibration_last_subst(float *injected_w, float *read_w, float *ratio)
{
	if (injected_w) {
		*injected_w = last_injected;
	}
	if (read_w) {
		*read_w = last_read;
	}
	if (ratio) {
		*ratio = last_ratio;
	}
}
