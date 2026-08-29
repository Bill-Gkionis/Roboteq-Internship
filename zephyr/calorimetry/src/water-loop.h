/*
 * water-loop.h - the pump, the dT pair and the flow meter.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * This is the crown jewel of the instrument: the reading is
 * mdot * cp * dT, so everything in this file lands on the answer directly.
 *
 * Two decisions are already made and are load-bearing:
 *
 *  1. THE BIOTECH TURBINE IS THE FLOW METER, FULL STOP.  The pump is purely
 *     the flow actuator.  An earlier plan made the stepper's own step count
 *     the primary metrology with the turbine as a witness; that was reversed.
 *     There is therefore no step-count-based metrology anywhere in this file -
 *     step rate is a COMMAND, never a measurement.
 *
 *  2. Every millikelvin of MISMATCH between the two RTDs is mdot*cp watts of
 *     bias - about 19 mW/mK at the design point.  Uncalibrated Class A Pt100
 *     would be +-0.21 K, i.e. ~4 W: four times the whole error budget from one
 *     part choice.  What buys it back is the in-situ tare in calibration.c,
 *     stored per flow range - because the tare also absorbs pump work and tube
 *     heat exchange, both of which are flow dependent.
 */

#ifndef CALORIMETRY_WATER_LOOP_H_
#define CALORIMETRY_WATER_LOOP_H_

#include "types.h"

int water_loop_init(void);

/** Read both RTDs and the turbine.  Called once per control tick. */
void water_loop_read(float dt);

struct meas water_loop_t_in(void);    /* deg C, cold leg (chamber inlet)  */
struct meas water_loop_t_out(void);   /* deg C, hot leg (chamber outlet)  */
struct meas water_loop_flow(void);    /* mL/min, from the turbine         */

/* ------------------------------------------------------------------ pump -- */

/**
 * Command a volumetric flow [mL/min].
 *
 * The pump is a peristaltic head on a stepper: the STEP frequency sets the
 * flow.  An open-loop step rate from the calibrated mL/step gets close, and a
 * slow outer trim closes the remaining error against the turbine - which is
 * the only sensor that can see a slipped tube, an air lock, an occlusion or
 * peristaltic tube fatigue, all of which change mL/step SILENTLY.
 *
 * Passing 0 stops the pump and disables the driver.
 */
void water_loop_set_flow(float ml_per_min);
float water_loop_get_flow_setpoint(void);
/** Fraction of the pump's commanded ceiling, for the UI. */
float water_loop_pump_fraction(void);

/** Run the flow trim.  Called once per control tick. */
void water_loop_service(float dt);

/** Stop the pump and drop the driver's enable. */
void water_loop_stop(void);

/* ---------------------------------------------------------- calibration --- */

/** Turbine K-factor [pulses per litre].  A [cal] value; gravimetric. */
void  water_loop_set_k_factor(float pulses_per_litre);
float water_loop_get_k_factor(void);

/** Pump volume per step [mL].  A [cal] value; only seeds the open-loop term. */
void  water_loop_set_ml_per_step(float ml);
float water_loop_get_ml_per_step(void);

/** The dT zero for the active flow range [K]. */
void  water_loop_set_tare(float tare_k);
float water_loop_get_tare(void);

/** Raw turbine pulse total, for the POST and for diagnostics. */
int32_t water_loop_pulse_count(void);

#endif /* CALORIMETRY_WATER_LOOP_H_ */
