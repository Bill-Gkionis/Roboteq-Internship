/*
 * model_gen.h - the plant, the gains and the gates, as C constants.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ===========================================================================
 *  GENERATED FILE - regenerate, do not hand-edit
 * ===========================================================================
 *  Source of truth:  ~/roboteq/python/params.yaml
 *  Generator:        tools/gen_model.py
 *  Regenerate with:  python tools/gen_model.py       (from the app directory)
 *
 *  The generator is deliberately NOT wired into CMake.  Doing so would make
 *  every build depend on a working Python + PyYAML, and a build that cannot
 *  run without a scripting language is a build that breaks on someone else's
 *  machine six months from now.  The trade is that this file can go stale, so
 *  the generator stamps CAL_PARAMS_HASH below and the firmware logs it with
 *  every measurement point: if the hash in a campaign log does not match the
 *  params.yaml the campaign was planned against, the point is suspect.
 *  (Zephyr Suggestions D9 / s5.5 - "one source of truth".)
 * ===========================================================================
 */

#ifndef CALORIMETRY_MODEL_GEN_H_
#define CALORIMETRY_MODEL_GEN_H_

#define CAL_PARAMS_HASH "7a8bcc53"   /* sha256[:8] of params.yaml */

/* ------------------------------------------------- capacitances [J/K] ----- */
#define CAL_C_I      650.0f    /* chamber air + fixtures + foam inner skin   */
#define CAL_C_W     1800.0f    /* block + resident water + radiator + DUT    */
#define CAL_C_E      300.0f    /* guard gap air + skins + foil + heater      */
#define CAL_C_R     2000.0f    /* reservoir + reject-side water              */

/* ------------------------------------------------- conductances [W/K] ----- */
#define CAL_UA_RAD    30.00f   /* chamber air <-> water bridge              */
#define CAL_G_GAP      0.56f   /* chamber air <-> guard gap - THE LEAK      */
#define CAL_G_OUT      0.45f   /* guard gap -> ambient - the unmetered exit */
#define CAL_UA_REJ    20.00f   /* reject radiator -> room                   */

/* ------------------------------------------------- operating point -------- */
/*
 * alpha is the DUT's split between the water block and the chamber air.  The
 * shipping rig is the radiator-only case, alpha = 0: the copper-block
 * extension that made alpha = 0.97 meaningful was dropped from build scope on
 * 2026-08-27.  params.yaml still carries 0.97 as its default, which is stale -
 * exactly the kind of divergence this header's hash exists to catch.
 *
 * The split changes the TRANSIENT and never the answer: with the guard nulled
 * the DC gain P_DUT -> P_meas is exactly 1 for every alpha, because the meter
 * is physically incapable of telling how the heat reached the water.
 */
#define CAL_ALPHA      0.0f
#define CAL_TAU_S      5.0f    /* sensor thermal lag [s] - [cal], measure it */

/* ------------------------------------------------- water properties ------- */
/*
 * Evaluated every tick, not frozen.  A volumetric flow meter plus a fixed
 * density is worth ~0.90 W of avoidable bias at 150 W over a 20-40 degC swing;
 * c_p contributes another 0.10 W.  About one watt recovered for two short
 * polynomials - the best watts-per-line in the whole firmware.
 *
 * Fits against steam-table values over 10-60 degC:
 *   rho: max error 0.06 kg/m3 (0.006 %) across 20-60 degC
 *   c_p: max error 1 J/(kg K) (0.024 %)
 *
 * Plumbing consequence: the pump and the flow meter must sit on the COLD leg,
 * so the displaced volume is at the inlet temperature the RTD pair already
 * measures.  Otherwise a third probe is needed just to evaluate rho.
 */
#define CAL_RHO_A   1001.17f      /* rho(T) = A + B*T + C*T^2   [kg/m^3]     */
#define CAL_RHO_B     -0.072250f
#define CAL_RHO_C     -0.0037875f
#define CAL_CP_A    4188.0f       /* cp(T)  = A + B*T + C*T^2   [J/(kg K)]   */
#define CAL_CP_B      -0.5f
#define CAL_CP_C       0.0075f

/* ------------------------------------------------- guard PI --------------- */
/*
 * lambda-tuning (IMC) on the SISO loop P_e -> theta_e, whose plant is
 *   K_e = 1/(G_gap+G_out) = 0.9901 K/W,  tau_e = C_e/(G_gap+G_out) = 297.0 s
 * with one knob, the desired closed-loop time constant lambda_g:
 *   Kp = tau_e / (K_e * lambda_g),   Ki = Kp / tau_e
 *
 * These gains are in WATTS PER KELVIN, not duty per kelvin.  That is why
 * heaters.c closes an inner power loop underneath: feeding this straight to a
 * duty cycle multiplies the loop gain by V^2/R - a number nobody wrote down,
 * different for every heater, drifting with rail sag and heater temperature.
 *
 * The guard node does not see the water flow, so unlike the reject loop these
 * two numbers are flow-independent and need no gain scheduling.
 */
#define CAL_TAU_E     297.0297f   /* s   - the guard shell's own time constant */
#define CAL_KE          0.990099f /* K/W - its DC gain                         */
#define CAL_BETA        0.554455f /* -   - passive tracking; < 1 is WHY PI     */
#define CAL_KP_G        5.0f      /* W/K                                       */
#define CAL_KI_G        0.016833f /* W/(K s)                                   */
#define CAL_TT_G      297.0297f   /* s   - back-calculation AW time constant   */
#define CAL_PE_MAX    100.0f      /* W   - guard heater ceiling                */

/* ------------------------------------------------- reject PI -------------- */
/*
 * Kp_r = C_r / lambda_r happens to be flow-independent; Ki_r is not, because
 * the reject rail's time constant is C_r/(UA_rej + mdot*cp).  Ki is therefore
 * computed per range in control-system.c rather than frozen here.
 *
 * This loop buys SCHEDULE, not accuracy: unregulated, the inlet walks 7.3 K
 * across a 150 W sweep and re-charges the 55-minute foam wall at every load
 * point.  Its watts sit outside boundary B1, so they cannot bias the reading.
 */
#define CAL_KP_R       20.0f      /* W/K */
#define CAL_LAMBDA_R  100.0f      /* s   */

/* Full-scale heat the reject radiator can move at 100 % fan.  [cal] - trim it
 * at bring-up rung F4.  Only the mapping "cooling watts -> fan duty" depends
 * on it, and that mapping affects settling time, never the reading. */
#define CAL_REJECT_FULL_W  250.0f
#define CAL_REJECT_FAN_MIN   0.30f  /* never fully stop the reject fans */

/* ------------------------------------------------- budget and gates ------- */
#define CAL_P_ACC       1.0f      /* W - the accuracy target                  */
#define CAL_P_THR       0.1f      /* W - residual allowed when the gate opens */

/*
 * The null gate.  The leak is exactly G_gap * e watts, so the largest null
 * error consistent with the accuracy target is P_acc / G_gap.
 */
#define CAL_NULL_GATE_K  (CAL_P_ACC / CAL_G_GAP)

/*
 * The steady gate is |dP/dt| < P_thr / tau_dom, and tau_dom is per point, not
 * a constant: a hard-coded W/h number is ~3x wrong at one end of the range.
 *
 *   tau_dom = max( (C_i + C_w) / (mdot*cp) , tau_e )
 *
 *   - the first term is the whole thermal mass draining through the meter;
 *   - the second is the guard shell, dominant once flow is high enough that
 *     the first term is short.
 *
 * Checked against the two corners the model reports (SMF s22):
 *   15 W  (mcp = 3):  max(816.7, 297.0) = 816.7 s vs 807.6 s  -> +1.1 %
 *   150 W (mcp = 15): max(163.3, 297.0) = 297.0 s vs 293.6 s  -> +1.2 %
 * Both conservative - a slightly longer tau means a slightly tighter gate.
 */

/* ------------------------------------------------- ranging table ---------- */
/*
 * Auto-ranging is FREE: the water setpoint has exactly zero DC gain to the
 * error, so moving dT_set between points cannot shift the answer - only the
 * transient, which the gates already catch.
 */
struct cal_range {
	float p_max;    /* W - use this row for loads up to here */
	float dt_set;   /* K - target water rise                 */
};

#define CAL_RANGE_COUNT 3
static const struct cal_range cal_ranges[CAL_RANGE_COUNT] = {
	{   50.0f,   5.0f },
	{  200.0f,  10.0f },
	{  600.0f,  25.0f },
};

/* ------------------------------------------------- limits ----------------- */
#define CAL_T_INNER_MAX   80.0f   /* deg C - hard cutout in firmware  */
#define CAL_T_GUARD_MAX   80.0f
#define CAL_T_WATER_MAX   70.0f
#define CAL_FLOW_MIN_ML    2.0f   /* mL/min below which heaters are refused */
#define CAL_FLOW_MAX_ML  400.0f   /* the pump's commanded ceiling here      */

/* Integrator pinned this long means the guard cannot reach the null, which
 * means the reading is biased by an unknown amount.  That is a MEASUREMENT
 * fault, not a warning. */
#define CAL_INTEGRATOR_PIN_S  300

/* Transient null-gate breaches are NORMAL at and above 150 W (|e| peaks at
 * 1.91 K against a 1.79 K gate) and must not fault.  Only a breach that
 * survives past the settling budget is one. */
#define CAL_SETTLE_BUDGET_S  5400   /* 90 min - past the 62 min cold corner */
#define CAL_DWELL_S           300   /* 5 min of both gates green            */

#endif /* CALORIMETRY_MODEL_GEN_H_ */
