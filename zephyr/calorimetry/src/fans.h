/*
 * fans.h - the three fan groups.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * All three groups are electrically identical (4-wire 25 kHz PWM fans on the
 * 12 V rail), so they share one code path and differ only by which devicetree
 * PWM channel they name.
 *
 * They are NOT identical in what they mean, though:
 *
 *   INNER   exists to enforce the lump model - it recirculates air between the
 *           DUT and the pickup radiator so the chamber is one thermal node.
 *           Its duty is FROZEN for the whole campaign, because changing it
 *           changes P_aux and UA_rad at the same time: two model parameters at
 *           once, which invalidates every point before and after the change.
 *           That rule is enforced here in code, not left to discipline.
 *
 *   GUARD   stirs the gap so the guard is one node too.  Outside boundary B1,
 *           so its watts never touch the reading.
 *
 *   REJECT  is the cooling actuator for the reject loop, and the only fan
 *           group whose duty is meant to move during a run.  That loop buys
 *           schedule, not accuracy.
 *
 * TACHOMETERS.  The carrier board routes one tach line per chain, because only
 * fan #1 of a PST daisy chain reports tach at all.  So a tach can prove that
 * SOMETHING in the chain is turning, and it cannot tell you which of three has
 * stopped - that is what the rail current is for, since a stopped fan draws
 * visibly less.  Both signals are exposed here and both are checked: they fail
 * in different ways, which is the point of having two.
 */

#ifndef CALORIMETRY_FANS_H_
#define CALORIMETRY_FANS_H_

#include "types.h"

enum fan_group {
	FAN_INNER = 0,
	FAN_GUARD,
	FAN_REJECT,
	FAN_COUNT
};

int fans_init(void);

/**
 * Convert the accumulated tach edges into RPM.  Called once per control tick
 * with the measured interval.
 */
void fans_service(float dt);

/**
 * Set one group's duty, 0.0 .. 1.0.
 *
 * Refuses to change FAN_INNER while it is frozen and returns -EPERM; use
 * fans_freeze_inner(false) first, which the supervisor only permits outside a
 * campaign.
 */
int fans_set(enum fan_group g, float duty);

float fans_get(enum fan_group g);

/** Freeze / unfreeze the inner group.  Freezing latches the current duty. */
void fans_freeze_inner(bool freeze);
bool fans_inner_frozen(void);

/**
 * Last measured speed [RPM], or -1 when this group has no tach line on this
 * board.  A PC fan emits two pulses per revolution.
 */
float fans_rpm(enum fan_group g);

/** True when this group has a tach line wired. */
bool fans_has_tach(enum fan_group g);

/**
 * Fan health, combining both signals: a group commanded above ~20 % duty that
 * reports (almost) no rotation, or whose rail current has collapsed against
 * the signature learned at POST.  Returns the first unhealthy group, or
 * FAN_COUNT.
 */
enum fan_group fans_unhealthy(void);

/** Record the current each group draws when it is known good (POST). */
void fans_learn_signature(enum fan_group g, float amps);

/** All groups off.  Called by the safety path. */
void fans_all_off(void);

const char *fans_name(enum fan_group g);

#endif /* CALORIMETRY_FANS_H_ */
