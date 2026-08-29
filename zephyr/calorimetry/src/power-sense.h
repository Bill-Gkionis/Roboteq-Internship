/*
 * power-sense.h - INA226 rail metering.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * WHICH RAILS ARE METERED, AND TO WHAT GRADE, IS A BOUNDARY DECISION, NOT A
 * COMPONENT DECISION.  Boundary B1 is the inner chamber.  Every watt
 * dissipated inside it that is not metered is reported as DUT loss
 * one-for-one, because the model's D_u = -1 entry is what cancels P_aux out of
 * the reading - and it can only cancel what it was told about.
 *
 * The corollary is counter-intuitive and worth stating: the GUARD HEATER, the
 * largest power in the rig, needs the LEAST accurate measurement of anything
 * here, because once the guard loop is closed its integral action drives that
 * channel's DC gain to the reading to exactly zero.  Precision parts belong
 * inside B1, not on the biggest load.
 *
 * This module also owns the tier-1 over-current trip, because the INA226's
 * ALERT pin is what makes it hardware-timed: firmware only latches it.
 */

#ifndef CALORIMETRY_POWER_SENSE_H_
#define CALORIMETRY_POWER_SENSE_H_

#include "types.h"

enum power_rail {
	PWR_HEAT_INNER = 0, /* INSIDE B1 - measurement grade, 4-wire Kelvin  */
	PWR_HEAT_GUARD,     /* outside B1 - protection grade                 */
	PWR_FAN_INNER,      /* INSIDE B1 - measurement grade, mandatory      */
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
 * the trip is done by the chip in hardware and the firmware's only job is to
 * latch the resulting GPIO edge - which is why the safety thread can act on it
 * without ever touching a bus.
 */
int power_sense_arm_alert(enum power_rail rail, float trip_amps);

const char *power_sense_name(enum power_rail rail);
bool power_sense_inside_b1(enum power_rail rail);

#endif /* CALORIMETRY_POWER_SENSE_H_ */
