#!/usr/bin/env python3
"""Generate src/model_gen.h from ~/roboteq/python/params.yaml.

    python tools/gen_model.py            # from the application directory

Why this exists
---------------
The classic failure of model-based control is not a wrong model, it is the
PORT: Python says zeta = 0.755, the C has a sign flipped in the anti-windup or
integrates in the wrong units, and nobody notices until the rig behaves unlike
every plot in System Modeling Final - with no way to tell which of thirty
differences is responsible.  Deriving the firmware's constants from the same
params.yaml the simulation reads removes that entire failure class.

It is deliberately NOT run from CMake: a build that needs a working Python and
PyYAML is a build that breaks on someone else's machine.  Instead the params
hash is stamped into the header, printed at boot and logged with every point,
so a stale header is detectable rather than silent.

PyYAML is optional - a tiny parser covers the flat subset params.yaml uses.
"""

import hashlib
import pathlib
import re
import sys

APP = pathlib.Path(__file__).resolve().parent.parent
PARAMS = pathlib.Path.home() / "roboteq" / "python" / "params.yaml"
OUT = APP / "src" / "model_gen.h"


def load_params(path):
    """Read the flat 'key: value' pairs.  Uses PyYAML when present."""
    text = path.read_text()
    try:
        import yaml
        return yaml.safe_load(text), text
    except ImportError:
        pass
    out = {}
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*:\s*([-+0-9.eE]+)$", line)
        if m:
            out[m.group(1)] = float(m.group(2))
    return out, text


def main():
    if not PARAMS.exists():
        sys.exit(f"params.yaml not found at {PARAMS}")

    p, raw = load_params(PARAMS)
    h = hashlib.sha256(raw.encode()).hexdigest()[:8]

    C_i, C_w, C_e, C_r = p["C_i"], p["C_w"], p["C_e"], p["C_r"]
    UA_rad, G_gap, G_out, UA_rej = p["UA_rad"], p["G_gap"], p["G_out"], p["UA_rej"]
    lam_g, lam_r, Pe_max = p["lambda_guard"], p["lambda_reject"], p["P_e_max"]

    # --- guard loop: lambda-tuning (IMC) on P_e -> theta_e ------------------
    K_e = 1.0 / (G_gap + G_out)          # K/W
    tau_e = C_e / (G_gap + G_out)        # s
    beta = G_gap / (G_gap + G_out)       # passive tracking, < 1 => needs I
    Kp_g = tau_e / (K_e * lam_g)         # W/K
    Ki_g = Kp_g / tau_e                  # W/(K s)
    Tt_g = Kp_g / Ki_g                   # = tau_e

    # --- reject loop --------------------------------------------------------
    Kp_r = C_r / lam_r                   # W/K, flow independent

    # alpha: params.yaml still defaults to the dropped copper-block operating
    # point (0.97).  The shipping rig is radiator-only.
    alpha = 0.0

    ranges = p.get("ranges") or [
        {"P_max": 50.0, "dT_set": 5.0},
        {"P_max": 200.0, "dT_set": 10.0},
        {"P_max": 600.0, "dT_set": 25.0},
    ]
    rows = "\n".join(
        f"\t{{ {r['P_max']:6.1f}f, {r['dT_set']:5.1f}f }},"
        for r in ranges
    )

    text = TEMPLATE.format(
        hash=h, C_i=C_i, C_w=C_w, C_e=C_e, C_r=C_r,
        UA_rad=UA_rad, G_gap=G_gap, G_out=G_out, UA_rej=UA_rej,
        alpha=alpha, tau_s=p["tau_s"],
        tau_e=tau_e, K_e=K_e, beta=beta, Kp_g=Kp_g, Ki_g=Ki_g,
        Tt_g=Tt_g, Pe_max=Pe_max, Kp_r=Kp_r, lam_r=lam_r,
        P_acc=p["P_acc"], P_thr=p["P_thr"],
        n_ranges=len(ranges), range_rows=rows,
    )
    OUT.write_text(text)
    print(f"wrote {OUT}  (params hash {h})")
    print(f"  guard PI : Kp = {Kp_g:.4f} W/K   Ki = {Ki_g:.6f} W/(K s)"
          f"   tau_e = {tau_e:.2f} s")
    print(f"  reject   : Kp = {Kp_r:.4f} W/K   (Ki is flow dependent)")
    print(f"  null gate: |e| < {p['P_acc'] / G_gap:.3f} K")


TEMPLATE = r'''/*
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

#define CAL_PARAMS_HASH "{hash}"   /* sha256[:8] of params.yaml */

/* ------------------------------------------------- capacitances [J/K] ----- */
#define CAL_C_I      {C_i:.1f}f    /* chamber air + fixtures + foam inner skin   */
#define CAL_C_W     {C_w:.1f}f    /* block + resident water + radiator + DUT    */
#define CAL_C_E      {C_e:.1f}f    /* guard gap air + skins + foil + heater      */
#define CAL_C_R     {C_r:.1f}f    /* reservoir + reject-side water              */

/* ------------------------------------------------- conductances [W/K] ----- */
#define CAL_UA_RAD    {UA_rad:.2f}f   /* chamber air <-> water bridge              */
#define CAL_G_GAP      {G_gap:.2f}f   /* chamber air <-> guard gap - THE LEAK      */
#define CAL_G_OUT      {G_out:.2f}f   /* guard gap -> ambient - the unmetered exit */
#define CAL_UA_REJ    {UA_rej:.2f}f   /* reject radiator -> room                   */

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
#define CAL_ALPHA      {alpha:.1f}f
#define CAL_TAU_S      {tau_s:.1f}f    /* sensor thermal lag [s] - [cal], measure it */

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
 *   K_e = 1/(G_gap+G_out) = {K_e:.4f} K/W,  tau_e = C_e/(G_gap+G_out) = {tau_e:.1f} s
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
#define CAL_TAU_E     {tau_e:.4f}f   /* s   - the guard shell's own time constant */
#define CAL_KE          {K_e:.6f}f /* K/W - its DC gain                         */
#define CAL_BETA        {beta:.6f}f /* -   - passive tracking; < 1 is WHY PI     */
#define CAL_KP_G        {Kp_g:.1f}f      /* W/K                                       */
#define CAL_KI_G        {Ki_g:.6f}f /* W/(K s)                                   */
#define CAL_TT_G      {Tt_g:.4f}f   /* s   - back-calculation AW time constant   */
#define CAL_PE_MAX    {Pe_max:.1f}f      /* W   - guard heater ceiling                */

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
#define CAL_KP_R       {Kp_r:.1f}f      /* W/K */
#define CAL_LAMBDA_R  {lam_r:.1f}f      /* s   */

/* Full-scale heat the reject radiator can move at 100 % fan.  [cal] - trim it
 * at bring-up rung F4.  Only the mapping "cooling watts -> fan duty" depends
 * on it, and that mapping affects settling time, never the reading. */
#define CAL_REJECT_FULL_W  250.0f
#define CAL_REJECT_FAN_MIN   0.30f  /* never fully stop the reject fans */

/* ------------------------------------------------- budget and gates ------- */
#define CAL_P_ACC       {P_acc:.1f}f      /* W - the accuracy target                  */
#define CAL_P_THR       {P_thr:.1f}f      /* W - residual allowed when the gate opens */

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
struct cal_range {{
	float p_max;    /* W - use this row for loads up to here */
	float dt_set;   /* K - target water rise                 */
}};

#define CAL_RANGE_COUNT {n_ranges}
static const struct cal_range cal_ranges[CAL_RANGE_COUNT] = {{
{range_rows}
}};

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
'''

if __name__ == "__main__":
    main()
