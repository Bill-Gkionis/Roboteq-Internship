/*
 * temp-sense.h - the I2C chamber temperature sensors.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * One TMP117 per chamber (inner and guard).  The sensor is configured from the
 * devicetree into continuous conversion with 64-sample hardware averaging on a
 * 1000 ms cycle, so it averages over exactly one control tick: the firmware
 * needs no filter, cannot alias, and the noise floor sits at the part's
 * repeatability rather than its accuracy.
 *
 * Growing to a zonal guard (N faces instead of one lump) is an overlay edit
 * plus one line in the table in temp-sense.c - deliberately not a refactor.
 */

#ifndef CALORIMETRY_TEMP_SENSE_H_
#define CALORIMETRY_TEMP_SENSE_H_

#include "types.h"

enum temp_channel {
	TEMP_CH_INNER = 0,   /* inner chamber air - drives BOTH loops      */
	TEMP_CH_GUARD,       /* guard gap                                  */
	TEMP_CH_COUNT
};

/** Bind the devices.  Returns 0 if every sensor is present and ready. */
int temp_sense_init(void);

/** Read every channel into its cached struct meas.  Called once per tick. */
void temp_sense_read_all(void);

/** Latest reading for one channel (already offset-corrected). */
struct meas temp_sense_get(enum temp_channel ch);

/**
 * Per-sensor offset from the isothermal soak, in kelvin, subtracted on every
 * read.  Parts from one reel have CORRELATED offsets, so averaging several of
 * them does not divide the error - only a soak against a known-isothermal rig
 * does.
 */
void  temp_sense_set_offset(enum temp_channel ch, float offset_k);
float temp_sense_get_offset(enum temp_channel ch);

/** Human name, for logs and the panel. */
const char *temp_sense_name(enum temp_channel ch);

/** True when every channel has produced a fresh, valid reading. */
bool temp_sense_all_ok(int64_t now_ms);

#endif /* CALORIMETRY_TEMP_SENSE_H_ */
