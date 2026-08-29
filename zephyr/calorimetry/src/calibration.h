/*
 * calibration.h - the three commissioning procedures, and the NVS store.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * EVERY [cal] VALUE LIVES IN NVS, NEVER IN THE IMAGE.  That is what makes
 * "commissioning values override computed estimates everywhere" mechanically
 * true instead of aspirational, and it means re-calibrating the rig never
 * requires a reflash.
 *
 * The three procedures, cheapest first:
 *
 *  SOAK   Before anything is powered the rig genuinely IS isothermal.  Log
 *         every sensor for a while at ambient and store the per-sensor
 *         offsets.  One procedure, three payoffs: relative calibration (group
 *         mismatch drops from ~0.14 K to the parts' repeatability), a wiring
 *         self-test (a swapped sensor or a duplicated address shows up as an
 *         outlier against a set that must physically agree), and the short
 *         form of it becomes the POST's plausibility gate.
 *
 *         Do NOT try to average the error away instead: parts from one reel
 *         have CORRELATED offsets, so averaging four of them does not halve
 *         the error.
 *
 *  TARE   Pump running at the point's flow, DUT off, heaters off, guard off:
 *         the dT reading SHOULD be zero.  Whatever it reads is the tare.
 *         It is not just sensor offset - it also absorbs pump work and tube
 *         heat exchange, which are FLOW DEPENDENT.  That is arguably correct
 *         (they really are part of the systematic tare) but it means the tare
 *         table is indexed by flow range and auto-ranging must re-select it.
 *         ~0.5 W of pump work at 300 mL/min is 26 mK - the same order as the
 *         thing being measured.  Never carry one tare across ranges.
 *
 *  SUBST  Inject a known 100 W into the inner resistor and confirm the meter
 *         reads 100 W.  This is the whole instrument in miniature: if a known
 *         100 W does not read back as 100 W, nothing above it means anything.
 *         The result is REPORTED, never silently applied - a scale correction
 *         is an operator decision with a paper trail.
 */

#ifndef CALORIMETRY_CALIBRATION_H_
#define CALORIMETRY_CALIBRATION_H_

#include "types.h"

/** Load every stored [cal] value from NVS.  Call once, early. */
int calibration_init(void);

/** Queue a procedure.  `type` is one of the CAL_CMD_CAL_* commands. */
void calibration_request(enum cal_cmd_type type, float arg);

/**
 * Advance the active procedure by one control tick.
 * @return true when nothing is running any more (the supervisor may leave).
 */
bool calibration_service(struct cal_snapshot *s, float dt);

/** The stored dT tare [K] for a flow, picked from the per-range table. */
float calibration_tare_for_flow(float ml_per_min);

/** Which procedure is running, for the UI. */
enum cal_state calibration_state(void);
/** 0..1 progress of the running procedure. */
float calibration_progress(void);

/** Result of the last substitution check: injected, read, and the ratio. */
void calibration_last_subst(float *injected_w, float *read_w, float *ratio);

#endif /* CALORIMETRY_CALIBRATION_H_ */
