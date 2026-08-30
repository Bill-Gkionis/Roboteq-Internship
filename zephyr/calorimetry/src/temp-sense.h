/*
 * temp-sense.h - the chamber temperature sensors, as ZONES.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A zone is a lumped thermal node and the set of sensors that measure it.  The
 * firmware reads every sensor in a zone and averages them, excluding any that
 * has been declared dead.
 *
 * That indirection exists for one reason: the guard is heading toward being
 * ZONAL - N faces instead of one lump.  With zone membership in the devicetree
 * (`calorimeter,thermal-zone`), adding a guard face is a phandle in the
 * overlay.  With it compiled into C, it would be a refactor.
 *
 * On the carrier board the sensors sit behind a TCA9548A on a dedicated I2C
 * bus, so each zone's leg can be recovered independently - which is the other
 * half of why the mux is worth its two euros.
 *
 * Each TMP117 is configured from the overlay into continuous conversion with
 * 64-sample hardware averaging on a 1000 ms cycle, so it averages over exactly
 * one control tick: no firmware filter, no aliasing, and the noise floor sits
 * at the part's repeatability rather than its accuracy.
 */

#ifndef CALORIMETRY_TEMP_SENSE_H_
#define CALORIMETRY_TEMP_SENSE_H_

#include "types.h"

enum temp_channel {
	TEMP_CH_INNER = 0,   /* inner chamber air - drives BOTH loops */
	TEMP_CH_GUARD,       /* guard gap                             */
	TEMP_CH_COUNT
};

/** Bind every sensor.  Returns 0 when all of them are ready. */
int temp_sense_init(void);

/** Read every sensor in every zone.  Called once per control tick. */
void temp_sense_read_all(void);

/** A zone's mean temperature, offsets applied, dead sensors excluded. */
struct meas temp_sense_get(enum temp_channel ch);

const char *temp_sense_name(enum temp_channel ch);

/** True when every zone has a fresh, valid mean. */
bool temp_sense_all_ok(int64_t now_ms);

/* ------------------------------------------------- per-sensor, for the soak */

/** How many sensors this zone actually has. */
int temp_sense_sensor_count(enum temp_channel ch);

/**
 * One sensor's RAW reading (offset NOT applied), for the isothermal soak.
 * Returns false if that sensor did not read this tick.
 */
bool temp_sense_raw(enum temp_channel ch, int idx, float *out);

/**
 * Per-sensor offset from the isothermal soak, in kelvin, subtracted on every
 * read.  Parts from one reel have CORRELATED offsets, so averaging several of
 * them does not divide the error - only a soak against a known-isothermal rig
 * does.
 */
void  temp_sense_set_offset(enum temp_channel ch, int idx, float offset_k);
float temp_sense_get_offset(enum temp_channel ch, int idx);

/* ------------------------------------------------------- board temperature */

/**
 * The sensor on the MOSFET heatsink, if the board has one.  Diagnostic and
 * protection only: it is outside every boundary and enters no control law.
 * Returns a reading with MEAS_VALID clear when the board has no such sensor,
 * which is the dev kit's case.
 */
struct meas temp_sense_board(void);
bool        temp_sense_has_board(void);

#endif /* CALORIMETRY_TEMP_SENSE_H_ */
