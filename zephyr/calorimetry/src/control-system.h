/*
 * control-system.h - the C port of the Python model: meter, PIs, gates,
 *                    disturbance observer.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * EVERYTHING IN THIS MODULE IS PLAIN C WITH NO ZEPHYR DEPENDENCY.  That is a
 * deliberate constraint, and it is the single highest-leverage decision in the
 * firmware: the same object code can be linked into three places -
 *
 *   1. the firmware, where it ships;
 *   2. a host build (native_sim / a plain gcc test), where anti-windup, gate
 *      hysteresis and fault latching get regression-tested before the rig
 *      physically exists;
 *   3. the Python simulation via ctypes, so the tuning in
 *      calorimetry_sim.py is done against THE CODE THAT WILL ACTUALLY RUN.
 *
 * The classic failure of model-based control is not a wrong model, it is the
 * port.  Linking the same C into the simulation removes that failure class.
 *
 * Every function here is a pure function of its arguments and one context
 * struct.  No globals, no statics, no time source, no logging.
 */

#ifndef CALORIMETRY_CONTROL_SYSTEM_H_
#define CALORIMETRY_CONTROL_SYSTEM_H_

#include <stdbool.h>
#include <stdint.h>

#include "model_gen.h"

/* ======================================================== water properties */

/** Density of water [kg/m^3] at T [deg C]. */
float cal_rho_w(float t_c);

/** Specific heat of water [J/(kg K)] at T [deg C]. */
float cal_cp_w(float t_c);

/* ================================================================ the meter */

/**
 * The calorimeter equation, with everything the model says must be in it.
 *
 *   P = rho(T_meter) * V_dot * cp(T_mean) * dT  -  P_aux
 *
 * @param v_dot_ml_min  volumetric flow from the turbine [mL/min]
 * @param t_in          water temperature entering the chamber [deg C]
 * @param t_out         water temperature leaving the chamber [deg C]
 * @param dt_tare       the in-situ zero for this flow range [K], subtracted
 *                      from (t_out - t_in).  It absorbs probe mismatch AND
 *                      pump work AND tube heat exchange, all of which are
 *                      flow dependent - which is exactly why the tare table
 *                      is indexed by flow range and must be re-selected when
 *                      the range changes.
 * @param p_aux         metered auxiliary power dissipated INSIDE boundary B1
 *                      [W].  Every watt in here that is not subtracted is
 *                      reported as DUT loss one-for-one.
 * @param mcp_out       if non-NULL, receives mdot*cp [W/K] - the meter scale,
 *                      also needed for the gate and the observer.
 * @return the reading [W]
 */
float cal_meter_power(float v_dot_ml_min, float t_in, float t_out,
		      float dt_tare, float p_aux, float *mcp_out);

/** Volumetric flow [mL/min] that puts dt_set kelvin across the block at p watts. */
float cal_flow_for_point(float p_w, float dt_set, float t_mean);

/** Pick the ranging row for a load; never returns NULL. */
const struct cal_range *cal_range_for(float p_w);

/**
 * Dominant time constant at this flow [s].  The steady gate is
 * P_thr / tau_dom, so this is what makes the gate per-point instead of a
 * hard-coded number that is ~3x wrong at one end of the range.
 */
float cal_tau_dom(float mcp);

/* ================================================================= PI loops */

/**
 * A PI controller with saturation and back-calculation anti-windup.
 *
 * The integrator is ONE FLOAT, not a history: a PI controller has no window
 * and no buffer, the entire past is compressed into the current value.  Four
 * bytes.  What matters is not capacity but precision - at a 1 s tick the
 * per-tick increment stays ~3e-5 of the accumulated value, 300x float32's
 * epsilon, so nothing is silently discarded.
 *
 * Anti-windup is not a refinement here.  The guard's integral action is what
 * makes the instrument exact (it turns a 1.31 % bias into an exact zero), so
 * the moment the heater saturates, the thing that guarantees the accuracy
 * stops accumulating.  Saturation WITHOUT anti-windup is worse than no
 * saturation at all.
 */
struct cal_pi {
	/* tuning */
	float kp;      /* W/K  */
	float ki;      /* W/(K s) */
	float tt;      /* s - back-calculation time constant, normally kp/ki */
	float u_min;   /* W - normally 0: HEATERS CANNOT COOL */
	float u_max;   /* W */

	/* state */
	float i;       /* the integrator, in WATTS (actuator units)          */
	float u;       /* last output, after saturation                      */
	float u_raw;   /* last output, before saturation                     */
	bool  sat;     /* was the last output clipped?                       */
	float sat_s;   /* seconds spent pinned at a rail                     */
};

/** Set gains and limits; clears the state. */
void cal_pi_init(struct cal_pi *pi, float kp, float ki, float u_min, float u_max);

/**
 * Preload the integrator so the loop starts where feedforward says it should.
 * Called on loop ENTRY, and it does two things: it makes the transfer bumpless,
 * and it removes a several-minute integrator charging transient from every
 * phase change.
 */
void cal_pi_preload(struct cal_pi *pi, float u0);

/**
 * One step.
 * @param e   error [K]
 * @param ff  feedforward [W], already slew-limited by the caller
 * @param dt  MEASURED seconds since the last step, not the nominal 1.000
 * @return the saturated output [W]
 */
float cal_pi_step(struct cal_pi *pi, float e, float ff, float dt);

/* ============================================== guard loop feedforward ==== */

/**
 * P_ff = G_out * (T_water_in + dT_set + (1-alpha) * P_hat / UA_rad)
 *
 * A wrong feedforward is SAFE: the water setpoint, the metered auxiliaries and
 * the feedforward all have exactly zero DC gain to the error, so a stale or
 * wrong P_hat degrades the transient null and can never reach the answer.
 * Treat it as free insurance and do not over-engineer it.
 */
float cal_guard_ff(float t_water_in, float dt_set, float p_hat);

/**
 * Slew limiter.  Feedforward must be RAMPED, not stepped: at 150 W a
 * feedforward step is itself a disturbance on the leak path and pushes 1 %
 * settling from 9.1 to 13.2 minutes.  The DUT load, by contrast, must be a
 * STEP - the observer needs the disturbance constant over its window.
 */
float cal_slew(float current, float target, float rate_per_s, float dt);

/* ========================================================== slope estimate */

/*
 * A rolling least-squares slope over a decimated ring.
 *
 * The window has to be long (15-30 min) because the slope being measured is
 * 0.4-1.2 W/h.  A full 1 Hz ring for 30 min is 7.2 kB; recursive running sums
 * are O(1) but cannot DROP the oldest sample, so they give an expanding window
 * rather than a sliding one.  Pre-averaging to one sample per DECIM seconds
 * and keeping N of them gives a true sliding window, anti-aliased by the
 * pre-average, for a few hundred bytes.
 */
#define CAL_SLOPE_MAX_N 180

struct cal_slope {
	int   decim_s;                  /* seconds per stored sample        */
	int   n;                        /* ring length (<= CAL_SLOPE_MAX_N) */
	float acc;                      /* running sum of the pre-average   */
	int   acc_n;
	float ring[CAL_SLOPE_MAX_N];
	int   count;                    /* samples stored so far            */
	int   head;                     /* next write index                 */
	float slope;                    /* units per second                 */
	bool  ready;                    /* ring is full: slope is meaningful */
};

void  cal_slope_init(struct cal_slope *s, int decim_s, int n);
void  cal_slope_reset(struct cal_slope *s);
/** Feed one sample per control tick.  Returns the current slope [unit/s]. */
float cal_slope_push(struct cal_slope *s, float value, float dt);

/* ==================================================================== gates */

struct cal_gates {
	struct cal_slope p_slope;   /* on the READING, never on a temperature */
	bool null_ok;
	bool steady_ok;
	float slope_gate;           /* W/s, recomputed when the range changes */
	int   both_green_s;
};

void cal_gates_init(struct cal_gates *g);
/** Recompute the steady threshold for a new operating point. */
void cal_gates_set_range(struct cal_gates *g, float mcp);
/**
 * Evaluate both gates.
 * @param p_meas  the reading [W]
 * @param e_null  T_inner - T_guard [K]
 * @return true when BOTH have been green for the full dwell
 */
bool cal_gates_eval(struct cal_gates *g, float p_meas, float e_null, float dt);

/* ================================================= disturbance observer === */

/*
 * Reading the answer instead of waiting for it.
 *
 * The 62-minute cold-corner wait exists because the rig is waiting for a
 * capacitance to finish charging, not because the information is absent - the
 * information is in the transient.  With the guard nulled, the energy balance
 * on the (air + water) lump is exactly
 *
 *     (C_i + C_w) * d(theta_bar)/dt  =  P_DUT - P_meas
 *
 * where theta_bar is the capacitance-weighted mean of the two node
 * temperatures.  Rearranged, that IS the disturbance observer:
 *
 *     P_hat_DUT = P_meas + (C_i + C_w) * d(theta_bar)/dt
 *
 * Three things are worth noticing about this form:
 *  - at steady state the derivative is zero, so the estimate collapses onto
 *    the reading.  It therefore INHERITS the closed loop's exact unbiasedness
 *    rather than adding a new bias of its own;
 *  - there is no observer gain to tune and no gain table per flow range,
 *    because it is the energy balance rather than a fitted filter;
 *  - it is a reduced-order stand-in for the 11-state Kalman filter the model
 *    proposes.  The KF converges slightly faster and rejects noise better; the
 *    upgrade path is to compute L offline with scipy and swap this function,
 *    with everything around it unchanged.
 *
 * Its one real cost is that it differentiates a temperature, so sensor noise
 * is amplified by (C_i+C_w)/window.  A 120 s least-squares slope on a 10 mK
 * TMP117 gives about 0.06 W of estimator noise - fine for a quantity that runs
 * in SHADOW MODE and never gates a logged point.
 *
 * SHADOW MODE IS THE POINT: the estimate is computed and logged always, but
 * the gates keep using the raw reading until a runtime flag is flipped.  That
 * is one boolean in NVS and it is the difference between a validated 26x
 * speed-up and an unvalidated one.
 */
struct cal_observer {
	struct cal_slope theta_slope;   /* d(theta_bar)/dt [K/s]  */
	float c_tot;                    /* C_i + C_w [J/K]        */
	float p_hat;                    /* W                      */
	bool  trusted;                  /* NVS flag: may it gate? */
};

void cal_observer_init(struct cal_observer *o);
void cal_observer_reset(struct cal_observer *o);
/**
 * @param t_inner  chamber air temperature [deg C]
 * @param t_water  water temperature (use the outlet probe) [deg C]
 * @param p_meas   the raw reading [W]
 * @return the estimate [W]
 */
float cal_observer_step(struct cal_observer *o, float t_inner, float t_water,
			float p_meas, float dt);

/* ============================================================== statistics */

/** Running mean and standard error, for logging a point with its uncertainty. */
struct cal_stat {
	double sum;
	double sum_sq;
	uint32_t n;
};

void  cal_stat_reset(struct cal_stat *s);
void  cal_stat_push(struct cal_stat *s, float x);
float cal_stat_mean(const struct cal_stat *s);
/** Standard error OF THE MEAN.  A point without its uncertainty is not a
 *  measurement. */
float cal_stat_stderr(const struct cal_stat *s);

/* ================================================================= helpers */

float cal_clampf(float x, float lo, float hi);

#endif /* CALORIMETRY_CONTROL_SYSTEM_H_ */
