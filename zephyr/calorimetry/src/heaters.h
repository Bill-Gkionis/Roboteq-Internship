/*
 * heaters.h - the two chamber heaters, driven in WATTS.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * THE ONE IDEA IN THIS MODULE
 * ---------------------------
 * The guard PI's gains are tuned in watts per kelvin (Kp = 5.000 W/K).  If a
 * PI output in watts is sent straight to a duty cycle, the real loop gain
 * becomes Kp * dP/d(duty) = Kp * V^2/R - a number that is not 1, is different
 * for every heater, and DRIFTS as the 12 V rail sags under fan load and as the
 * heater's own resistance rises with temperature.  The rig would then be
 * running a controller whose gain nobody ever wrote down.
 *
 * So this module closes an INNER POWER LOOP: the caller asks for watts, the
 * INA226 on that rail says what the heater is actually dissipating, and the
 * duty is trimmed until they agree.  With that in place the model's gains
 * transfer to hardware unchanged, and rail sag, R(T) drift and MOSFET
 * R_DS(on) all become inner-loop disturbances instead of outer-loop gain
 * errors.
 *
 * The inner loop is deliberately slow (a few seconds) relative to nothing in
 * particular and fast relative to everything thermal: the plant's quickest
 * mode is 15 s, so there is no interaction to worry about.
 */

#ifndef CALORIMETRY_HEATERS_H_
#define CALORIMETRY_HEATERS_H_

#include "types.h"

enum heater_id {
	HEATER_INNER = 0,  /* INSIDE B1: chamber preheat AND the DUT-substitution
			    * calibration reference, hence measurement grade   */
	HEATER_GUARD,      /* the guard actuator; outside B1               */
	HEATER_COUNT
};

int heaters_init(void);

/**
 * Ask for a power, in watts.  What actually reaches the gate is decided by the
 * interlock (heaters_set_enable) and by the inner power loop.
 */
void heaters_set_power(enum heater_id h, float watts);

/** The last demand, in watts. */
float heaters_get_demand(enum heater_id h);
/** The duty the inner loop settled on, 0..1. */
float heaters_get_duty(enum heater_id h);

/**
 * Run one step of both inner power loops.  Called once per control tick with
 * the measured power for each rail.
 */
void heaters_service(float dt);

/**
 * THE INTERLOCK.  Heaters may only be energised when ALL of:
 *   - flow is above the minimum,
 *   - every zone sensor is VALID and fresher than 3 ticks,
 *   - every node temperature is below its limit,
 *   - no latched fault.
 *
 * The middle clause is the one people leave out and it is the one that burns
 * rigs: a hung I2C bus does not report an error, it reports the last value
 * forever, and the guard PI will happily integrate against a plausible
 * constant while the real chamber climbs.
 *
 * Passing false forces both channels to zero immediately.
 */
void heaters_set_enable(bool enable);
bool heaters_enabled(void);

/** Unconditional, immediate, both channels.  The safety path calls this. */
void heaters_all_off(void);

const char *heaters_name(enum heater_id h);

#endif /* CALORIMETRY_HEATERS_H_ */
