/*
 * system.h - the supervisor, the safety thread, the self-test and the shared
 *            state cluster.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * THE THREE SAFETY TIERS, and the top one has no firmware in it at all:
 *
 *   0  hardware   instant   bimetallic cutout in series with the heater 12 V,
 *                           a fuse per rail, gate pull-downs, PSU OCP.
 *                           Covers firmware being wrong, hung, or absent.
 *   1  safety     <=100 ms  this file's `safety` thread.  INA226 ALERT ->
 *                           GPIO IRQ (the trip itself is hardware-timed;
 *                           firmware only latches it), over-temperature on
 *                           CACHED values, staleness, the heater interlock.
 *   2  supervisor <=1 s     gate violations, sensor disagreement, sustained
 *                           saturation, flow mismatch.  These are MEASUREMENT
 *                           faults: they abort the point, not necessarily the
 *                           power.
 *
 * THE ONE RULE THAT MAKES THE PARTITION EARN ITS KEEP: the safety thread never
 * touches a bus.  It reads GPIO lines and the last CACHED sensor values with
 * their timestamps.  A hung I2C bus therefore cannot hang the safety thread -
 * which matters because a hung bus is also exactly what freezes the sensor
 * value the guard integrator is happily accumulating against.
 */

#ifndef CALORIMETRY_SYSTEM_H_
#define CALORIMETRY_SYSTEM_H_

#include "types.h"

/* ------------------------------------------------------------ lifecycle --- */

/** Bind hardware, start the safety thread, arm the watchdog. */
int sys_init(void);

/** Run the power-on self test.  Returns 0 when every row passed. */
int sys_post_run(void);

/* --------------------------------------------------------- shared state --- */

/** Copy the current snapshot.  Safe from any thread. */
void sys_snapshot_get(struct cal_snapshot *out);

/** Publish a new snapshot.  Only the control thread calls this. */
void sys_snapshot_set(const struct cal_snapshot *in);

/* --------------------------------------------------------------- faults --- */

/** Latch one or more FAULT_* bits.  Idempotent; logs only on a new bit. */
void sys_fault_raise(uint32_t bits, const char *why);

/** Current latched fault word. */
uint32_t sys_faults(void);

/** Explicit operator clear.  Never automatic. */
void sys_fault_clear(void);

/** Human-readable name of the lowest set bit, for the panel. */
const char *sys_fault_name(uint32_t faults);

/* ------------------------------------------------------------ interlock --- */

/**
 * Evaluate the heater interlock from the current snapshot and apply it.
 * Called every control tick.  Returns the result, which is also cached for the
 * safety thread and the UI.
 */
bool sys_interlock_eval(const struct cal_snapshot *s);

/* ----------------------------------------------------------- supervisor --- */

/** One supervisor step.  Called once per control tick, after the control law. */
void sys_supervisor_step(struct cal_snapshot *s, float dt);

enum cal_state sys_state(void);

/* ------------------------------------------------------------- commands --- */

/**
 * The ONE door every operator action goes through, whatever it arrived on.
 *
 * The panel, the shell and the WebSocket all call this.  That is what makes
 * "local has priority" true by construction instead of by a trust flag: a
 * remote command cannot reach an actuator by a path a local one could be
 * refused on, because there is only one path.
 */
int sys_cmd_submit(const struct cal_cmd *cmd);

/** Pop the next pending command.  Returns false when the queue is empty. */
bool sys_cmd_pop(struct cal_cmd *out);

/* ---------------------------------------------------------- heartbeat ----- */

/**
 * The control thread calls this every tick.  The safety thread feeds the
 * hardware watchdog ONLY if this has been called recently, so a hung control
 * loop resets the board - and the external gate pull-downs make reset mean
 * heaters off.
 */
void sys_ctrl_heartbeat(void);

#endif /* CALORIMETRY_SYSTEM_H_ */
