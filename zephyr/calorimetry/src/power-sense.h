/*
 * power-sense.h - rail metering.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * WHICH RAILS ARE METERED, AND TO WHAT GRADE, IS A BOUNDARY DECISION, NOT A
 * COMPONENT DECISION.  Boundary B1 is the inner chamber.  Every watt
 * dissipated inside it that is not metered is reported as DUT loss
 * one-for-one, because the model's D_u = -1 entry is what cancels P_aux out of
 * the reading - and it can only cancel what it was told about.
 *
 * The corollary is counter-intuitive: the GUARD HEATER, the largest power in
 * the rig, needs the LEAST accurate measurement of anything here, because once
 * the guard loop is closed its integral action drives that channel's DC gain
 * to the reading to exactly zero.  Precision parts belong inside B1, not on
 * the biggest load.
 *
 * The carrier board splits this across two part types, and this module hides
 * the difference: two INA226s on the heater rails (the inner one 4-wire Kelvin
 * at the resistor's own terminals), and one three-channel INA3221 for the two
 * fan chains and the pump.
 */

#ifndef CALORIMETRY_POWER_SENSE_H_
#define CALORIMETRY_POWER_SENSE_H_

#include "types.h"

enum power_rail {
	PWR_HEAT_INNER = 0, /* INSIDE B1 - measurement grade, 4-wire Kelvin  */
	PWR_HEAT_GUARD,     /* outside B1 - protection grade                 */
	PWR_FAN_INNER,      /* INSIDE B1 - measurement grade, mandatory      */
	PWR_FAN_GUARD,      /* outside B1 - the guard's own energy balance   */
	PWR_PUMP,           /* outside B1 - health only                      */
	PWR_COUNT
};

int  power_sense_init(void);
void power_sense_read_all(void);

/** Latest power on one rail [W]. */
struct meas power_sense_get(enum power_rail rail);
/** Latest current on one rail [A] - the POST and fan-health signal. */
float power_sense_current(enum power_rail rail);
/** Latest bus voltage on one rail [V] - rail sag detection. */
float power_sense_voltage(enum power_rail rail);

/**
 * Sum of every metered load INSIDE boundary B1 [W].  This is the P_aux the
 * meter subtracts.
 */
float power_sense_p_aux(void);

/**
 * Program the INA226 over-current ALERT limit on a heater rail.
 *
 * The Zephyr ina2xx driver exposes no threshold attribute, so this writes the
 * Mask/Enable (0x06) and Alert Limit (0x07) registers directly.  Once armed,
 * the chip does the trip in hardware and the firmware's only job is to latch
 * the resulting GPIO edge - which is why the safety thread can act on it
 * without ever touching a bus.
 *
 * Returns -ENOTSUP for a rail that is not an INA226.
 */
int power_sense_arm_alert(enum power_rail rail, float trip_amps);

/**
 * Software over-current backstop, for a board where the ALERT line is not
 * routed (or a rail that has no ALERT pin at all).  One control tick slower
 * than the hardware trip, so it is a second line and not a replacement.
 * Returns the first rail over its limit, or PWR_COUNT.
 */
enum power_rail power_sense_overcurrent(void);
void power_sense_set_limit(enum power_rail rail, float amps);

const char *power_sense_name(enum power_rail rail);
bool power_sense_inside_b1(enum power_rail rail);

#endif /* CALORIMETRY_POWER_SENSE_H_ */
