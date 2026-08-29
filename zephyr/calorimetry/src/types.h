/*
 * types.h - the data every module shares.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * This header holds no code and depends on no other module, which is what
 * keeps the include graph a tree instead of a knot: every .c file may include
 * types.h, and no header here includes another module's header.
 *
 * The one idea worth reading twice is `struct meas`.  A sensor reading in this
 * firmware is never a bare float - it is a value, the time it was taken, and
 * whether it can be trusted.  That is not defensive padding; it is the
 * mechanism by which a dead I2C line becomes a latched fault instead of a
 * fire.  A hung bus does not report an error, it reports the LAST VALUE
 * FOREVER: the guard PI would then see a constant, plausible error, integrate
 * against it, and drive the heater to the rail while the real chamber climbs.
 * Staleness detection is the difference between a fault and a fire.
 */

#ifndef CALORIMETRY_TYPES_H_
#define CALORIMETRY_TYPES_H_

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/sys/util.h>

/* ------------------------------------------------------------------ timing */

/* The control tick.  1 Hz, and it is the right answer twice over:
 *  - the closed-loop bandwidth is ~1/60 s^-1, so 1 s of sampling puts the
 *    discrete poles at |z| = 0.82..0.9966 - fast enough for margin, slow
 *    enough that they do not all crowd onto z = 1 where finite precision
 *    cannot tell them apart (System Modeling Final s25);
 *  - and the guard integrator's per-tick increment stays ~3e-5 of its own
 *    value, which is 300x float32's epsilon.  At 1 ms it would be 3e-8 and the
 *    increments would be silently discarded - the integrator freezes and the
 *    accuracy quietly dies (Zephyr Suggestions s2).
 */
#define CAL_TICK_MS        1000
#define CAL_SAFETY_TICK_MS 100     /* safety thread: 10 Hz, never touches a bus */

/* A reading older than this is refused by the control law and the interlock. */
#define CAL_STALE_TICKS    3
#define CAL_STALE_MS       (CAL_STALE_TICKS * CAL_TICK_MS)

/* --------------------------------------------------------- a sensor reading */

#define MEAS_VALID     BIT(0)   /* the last read succeeded                   */
#define MEAS_STALE     BIT(1)   /* older than CAL_STALE_MS                   */
#define MEAS_SATURATED BIT(2)   /* the front end clipped                     */
#define MEAS_RETRIED   BIT(3)   /* succeeded, but only after a bus retry     */
#define MEAS_DEAD      BIT(4)   /* N consecutive failures - excluded for good */

struct meas {
	float   v;       /* engineering units - deg C, mL/min, W, ...      */
	int64_t t_ms;    /* k_uptime_get() when it was taken               */
	uint8_t flags;   /* MEAS_* above                                   */
};

/** True only if the value may be fed to a control law. */
static inline bool meas_ok(const struct meas *m, int64_t now_ms)
{
	return (m->flags & MEAS_VALID) && !(m->flags & MEAS_DEAD) &&
	       (now_ms - m->t_ms) <= CAL_STALE_MS;
}

/* --------------------------------------------------------- supervisor state */

/*
 * The supervisor is a state machine, not a controller.  Every transition that
 * can invalidate a measurement point is encoded here rather than left to
 * operator discipline.
 */
enum cal_state {
	CAL_ST_BOOT = 0,
	CAL_ST_SELFTEST,   /* POST: rails, bus scan, isothermal plausibility  */
	CAL_ST_IDLE,       /* everything off but the sensors                  */
	CAL_ST_CAL_SOAK,   /* isothermal: learn per-sensor offsets -> NVS     */
	CAL_ST_CAL_TARE,   /* pump on, no heat: learn the dT zero -> NVS      */
	CAL_ST_CAL_SUBST,  /* substitution: inject a known W, check the meter */
	CAL_ST_SET_RANGE,  /* pick flow + dT_set + gain row, reset observer   */
	CAL_ST_PREHEAT,    /* drive the chamber to Tss with the cal resistor  */
	CAL_ST_SETTLE,     /* loops closed, gates being evaluated             */
	CAL_ST_DWELL,      /* both gates green, holding for the dwell         */
	CAL_ST_LOG,        /* emit the point with its own uncertainty         */
	CAL_ST_FAULT,      /* latched; outputs safe; needs an explicit clear  */
	CAL_ST_COUNT
};

const char *cal_state_name(enum cal_state s);

/* ------------------------------------------------------------------- faults */

/*
 * Latched fault bits.  Tier 1 (hardware-timed, bus-free) faults are listed
 * first; tier 2 (measurement) faults follow.  Anything in TIER1_MASK kills the
 * outputs immediately; a tier-2 fault aborts the POINT, which may or may not
 * mean killing power.
 */
#define FAULT_ALERT_PIN      BIT(0)  /* INA226 wired-OR ALERT went low       */
#define FAULT_OVERTEMP       BIT(1)  /* a node above its limit               */
#define FAULT_SENSOR_STALE   BIT(2)  /* a required sensor stopped updating   */
#define FAULT_SENSOR_DEAD    BIT(3)  /* N consecutive bus failures           */
#define FAULT_CTRL_STALLED   BIT(4)  /* control thread missed its check-in   */
#define FAULT_NO_FLOW        BIT(5)  /* heater interlock: flow below minimum */
#define FAULT_POST_FAILED    BIT(6)  /* a power-on self-test row failed      */

#define FAULT_INTEGRATOR_PIN BIT(8)  /* guard integrator at the rail > 5 min */
#define FAULT_FLOW_MISMATCH  BIT(9)  /* commanded vs measured flow disagree  */
#define FAULT_NULL_BREACH    BIT(10) /* null gate breached PAST the budget   */
#define FAULT_FAN_HEALTH     BIT(11) /* fan rail current outside signature   */

#define FAULT_TIER1_MASK                                                       \
	(FAULT_ALERT_PIN | FAULT_OVERTEMP | FAULT_SENSOR_STALE |                \
	 FAULT_SENSOR_DEAD | FAULT_CTRL_STALLED | FAULT_NO_FLOW |               \
	 FAULT_POST_FAILED)

/* ------------------------------------------------------- the shared cluster */

/*
 * One snapshot of everything happening in the rig.  Written by the control
 * thread once per tick; read by the safety thread, the panel and the network.
 * Readers take a copy under a mutex (sys_snapshot_get) and then work on their
 * copy, so nothing that renders a UI can hold up the control loop.
 */
struct cal_snapshot {
	int64_t t_ms;
	uint32_t tick;

	/* --- raw sensor channels ------------------------------------------ */
	struct meas t_inner;     /* deg C, inner chamber air                  */
	struct meas t_guard;     /* deg C, guard gap                          */
	struct meas t_water_in;  /* deg C, water INTO the chamber (cold leg)  */
	struct meas t_water_out; /* deg C, water OUT of the chamber           */
	struct meas flow;        /* mL/min, Biotech turbine - THE flow meter  */

	struct meas p_heat_inner; /* W, measurement grade, INSIDE B1          */
	struct meas p_heat_guard; /* W, protection grade, outside B1          */
	struct meas p_fan_inner;  /* W, measurement grade, INSIDE B1          */
	struct meas p_pump;       /* W, health only, outside B1               */

	/* --- derived ------------------------------------------------------- */
	float p_aux;      /* W, everything metered inside B1                  */
	float p_meas;     /* W, the reading: mdot*cp*dT - P_aux               */
	float p_hat;      /* W, observer estimate (shadow mode)               */
	float e_null;     /* K, T_inner - T_guard: the leak driver            */
	float dt_water;   /* K, T_out - T_in, tare removed                    */
	float mdot;       /* kg/s                                             */
	float mcp;        /* W/K, the advection conductance = the meter scale */

	/* --- controller internals ------------------------------------------ */
	float guard_i;      /* W, the guard integrator. This IS the accuracy. */
	float guard_demand; /* W, guard PI output after saturation            */
	float inner_demand; /* W, inner chamber heater demand                 */
	float p_slope;      /* W/s, rolling LSQ slope of p_meas               */
	float p_slope_gate; /* W/s, the threshold it must be under            */

	/* --- actuator commands, 0..1 --------------------------------------- */
	float duty_heat_inner;
	float duty_heat_guard;
	float duty_fan_inner;
	float duty_fan_guard;
	float duty_fan_reject;
	float pump_frac;      /* fraction of the pump's maximum flow          */
	float flow_set;       /* mL/min commanded                             */

	/* --- setpoints ----------------------------------------------------- */
	float t_inner_set;    /* deg C, chamber steady-state target           */
	float dt_water_set;   /* K, the auto-range variable                   */

	/* --- status --------------------------------------------------------- */
	enum cal_state state;
	uint32_t faults;      /* latched FAULT_* bits                         */
	bool gate_null;       /* |e| < P_acc / G_gap                          */
	bool gate_steady;     /* |dP/dt| < P_thr / tau_dom                    */
	bool heaters_enabled; /* the interlock's answer, not a request        */
	int32_t settle_s;     /* seconds spent in SETTLE                      */
	int32_t dwell_s;      /* seconds of both-gates-green so far           */

	/* --- last logged point ---------------------------------------------- */
	float last_point_w;   /* mean p_meas over the dwell                   */
	float last_point_se;  /* standard error of that mean                  */
	bool  last_point_valid;
};

/* ------------------------------------------------------------- commands ---- */

/*
 * Every operator action - from the panel, the shell or the network - becomes
 * one of these and goes through ONE function (sys_cmd_submit).  That is what
 * makes "local has priority" true by construction rather than by a trust flag:
 * a remote command cannot reach an actuator by any path a local one could be
 * refused on.
 */
enum cal_cmd_type {
	CAL_CMD_NONE = 0,
	CAL_CMD_START,        /* IDLE -> SET_RANGE -> ... a measurement point  */
	CAL_CMD_STOP,         /* back to IDLE, outputs safe                    */
	CAL_CMD_SET_TSS,      /* arg = chamber setpoint, deg C                 */
	CAL_CMD_SET_DTW,      /* arg = water dT_set, K (picks the flow)        */
	CAL_CMD_CAL_SOAK,     /* run the isothermal offset calibration         */
	CAL_CMD_CAL_TARE,     /* run the dT tare at the current flow range     */
	CAL_CMD_CAL_SUBST,    /* run the substitution check, arg = watts       */
	CAL_CMD_CLEAR_FAULT,  /* explicit, never automatic                     */
};

struct cal_cmd {
	enum cal_cmd_type type;
	float arg;
	const char *origin;   /* "panel" | "shell" | "net" - goes in the log   */
};

#endif /* CALORIMETRY_TYPES_H_ */
