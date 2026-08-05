"""
Calorimeter — L1 plant, three-state state-space model.

    states       x = [theta_i, theta_w, theta_e]     chamber air / block+water / guard gap
    commands     u = [P_aux, P_e]                    what you can set
    disturbances d = [P_DUT, theta_rail, Ta_dot]     what you suffer
    output       y = P_meas                          the reading

All temperatures are deviation coordinates, theta = T - T_ambient, so ambient is 0.
theta_rail is the WATER inlet temperature from the reject loop -- not theta_i.

mdot is bilinear (it multiplies a state), so the model is LTI only at a frozen
flow.  Build one plant per operating point with build_plant(mcp).

Reference: Calorimetry/Design/System Modeling Final.md, Part I.
"""

from pathlib import Path

import yaml                        # pyright: ignore[reportMissingModuleSource]
import numpy as np                 # pyright: ignore[reportMissingModuleSource]
import control as ct               # pyright: ignore[reportMissingModuleSource]


# --------------------------------------------------------------- parameters

PARAMS_PATH = Path(__file__).parent / "params.yaml"

with open(PARAMS_PATH, "r") as f:
    p = yaml.safe_load(f)

C_i, C_w, C_e        = p["C_i"], p["C_w"], p["C_e"]
UA_rad, G_gap, G_out = p["UA_rad"], p["G_gap"], p["G_out"]
alpha                = p["alpha"]
c_p_w, rho_w         = p["c_p_w"], p["rho_w"]

STATES  = ["th_i", "th_w", "th_e"]
INPUTS  = ["P_aux", "P_e", "P_DUT", "th_rail", "Ta_dot"]
OUTPUTS = ["P_meas"]

IU, ID = slice(0, 2), slice(2, 5)   # commands / disturbances inside the combined B and D


# ------------------------------------------------------------------ helpers

def mcp_from_mdot(mdot):
    """Mass flow [kg/s] -> advection conductance [W/K]."""
    return mdot * c_p_w


def mdot_from_mcp(mcp):
    """Advection conductance [W/K] -> mass flow [kg/s]."""
    return mcp / c_p_w


def mL_per_min(mdot):
    """Mass flow [kg/s] -> volumetric flow [mL/min], for talking to the pump."""
    return mdot / rho_w * 6e7


def mcp_for_corner(P, dT_set):
    """Auto-range: the advection conductance [W/K] that puts dT_set kelvin
    across the block at P watts.  Straight from P = mcp * dT."""
    return P / dT_set


# -------------------------------------------------------------------- audit

def audit(A, mcp, tol=1e-9):
    """Structural checks that must pass before any number is trusted.

    Space State §30 / System Modeling Final §4.  These catch WIRING errors --
    signs, missing bridges, bad couplings.  They do NOT catch a parameter that
    is simply the wrong size; build_plant's range assert covers that.

    Open-loop plant only.  The closed loop legitimately grows a complex pair
    (the guard integrator is a second storage type), so never run this on A_cl.
    """
    assert np.all(np.diag(A) < 0),                     "diagonal must be negative"
    assert abs(A[0].sum()) < tol,                      "air: no path out except via water/guard"
    assert abs(A[1].sum() + mcp / C_w) < tol,          "water leaks to the rail (metered)"
    assert abs(A[2].sum() + G_out / C_e) < tol,        "guard leaks to ambient (UNmetered = the error)"
    assert abs(C_i * A[0, 1] - C_w * A[1, 0]) < tol,   "UA_rad bridge, capacitance-weighted"
    assert abs(C_i * A[0, 2] - C_e * A[2, 0]) < tol,   "G_gap bridge, capacitance-weighted"
    assert A[1, 2] == 0 and A[2, 1] == 0,              "water and guard do not touch"
    lam = np.linalg.eigvals(A)
    assert np.all(lam.real < 0),                       "passive plant must be stable"
    assert np.all(abs(lam.imag) < tol),                "dissipative single-domain => real eigenvalues"


# -------------------------------------------------------------- the matrices

def build_matrices(mcp):
    """The six blocks, at a frozen advection conductance mcp [W/K].

    Row i of every matrix is node i's balance law divided by node i's
    capacitance; column j is the symbol it is computed from.  mcp appears in
    four of them -- one conductance, two ends, twice each.
    """
    # A : states -> state rates
    A = np.array([
        [-(UA_rad + G_gap) / C_i,  UA_rad / C_i,            G_gap / C_i           ],
        [ UA_rad / C_w,           -(UA_rad + mcp) / C_w,    0.0                   ],
        [ G_gap / C_e,             0.0,                    -(G_gap + G_out) / C_e ],
    ])

    # B_u : commands (P_aux, P_e) -> state rates.
    # One non-zero per column: each actuator touches exactly one node, which is
    # why the guard can be tuned as a standalone SISO loop.
    B_u = np.array([
        [1.0 / C_i, 0.0      ],
        [0.0,       0.0      ],
        [0.0,       1.0 / C_e],
    ])

    # B_d : disturbances (P_DUT, theta_rail, Ta_dot) -> state rates.
    # The Ta_dot column is -1 in every row: that is the deviation coordinate
    # system moving under you, not heat flowing through a conductance.
    B_d = np.array([
        [(1 - alpha) / C_i, 0.0,       -1.0],
        [alpha / C_w,       mcp / C_w, -1.0],
        [0.0,               0.0,       -1.0],
    ])

    # Output: P_meas = mcp*(theta_w - theta_rail) - P_aux
    #   theta_w is a state        -> Cy
    #   theta_rail, P_aux are not -> D_d, D_u  (they reach the reading with no dynamics)
    Cy  = np.array([[0.0, mcp, 0.0]])
    D_u = np.array([[-1.0, 0.0]])
    D_d = np.array([[0.0, -mcp, 0.0]])

    return A, B_u, B_d, Cy, D_u, D_d


def build_plant(mcp, run_audit=True):
    """LTI plant at a frozen flow, as a named python-control state-space object."""
    assert 1.0 < mcp < 50.0, (
        f"mcp = {mcp:.3g} W/K is outside the design sweep (3-30 W/K). "
        f"That is {mL_per_min(mdot_from_mcp(mcp)):.0f} mL/min -- check your units."
    )

    A, B_u, B_d, Cy, D_u, D_d = build_matrices(mcp)

    if run_audit:
        audit(A, mcp)

    return ct.ss(A, np.hstack([B_u, B_d]), Cy, np.hstack([D_u, D_d]),
                 states=STATES, inputs=INPUTS, outputs=OUTPUTS)


# ------------------------------------------------------------------- report

def modes(A):
    """[(tau, normalised eigenvector)] per mode, slowest first.

    The eigenvalues say how fast; the eigenvectors say which physical process.
    No transfer function will tell you the second one.
    """
    lam, V = np.linalg.eig(A)
    lam, V = lam.real, V.real
    out = []
    for k in np.argsort(lam)[::-1]:           # least negative first = slowest first
        v = V[:, k] / V[np.argmax(abs(V[:, k])), k]
        out.append((-1.0 / lam[k], v))
    return out


def report(mcp):
    sys = build_plant(mcp)
    mdot = mdot_from_mcp(mcp)
    g = ct.dcgain(sys).ravel()

    print(f"\noperating point:  mcp = {mcp:.3f} W/K    mdot = {mdot:.6g} kg/s"
          f"    ({mL_per_min(mdot):.1f} mL/min)")

    print(f"\n  modes                     [{'  '.join(STATES)}]")
    for tau, v in modes(np.asarray(sys.A)):
        print(f"    tau = {tau:8.1f} s     v = [{v[0]:+.3f} {v[1]:+.3f} {v[2]:+.3f}]")

    print("\n  DC gains to P_meas   (guard OPEN)")
    for name, gain in zip(INPUTS, g):
        print(f"    {name:>8s} -> {gain:14.6f}")

    eps = 1.0 - g[INPUTS.index("P_DUT")]
    R_adv, R_rad, R_gap, R_out = 1 / mcp, 1 / UA_rad, 1 / G_gap, 1 / G_out
    eps_div = (R_adv + (1 - alpha) * R_rad) / (R_adv + R_rad + R_gap + R_out)
    print(f"\n  systematic error   eps = {eps:.6f}   "
          f"= {eps * 100:.2f} %   = {eps * 150:.2f} W at 150 W")
    print(f"    leakage divider gives  {eps_div:.6f}   (agree to {abs(eps - eps_div):.1e})")


if __name__ == "__main__":
    report(mcp_from_mdot(p["mdot"]))

    print("\n" + "-" * 70)
    print("corner sweep -- flow follows the load, so every corner is a new plant")
    print("DC gain is guard OPEN: the deficit the guard loop has to close\n")
    print(f"{'P [W]':>7} {'dT [K]':>7} {'mL/min':>9} {'mcp':>7}"
          f" {'tau_slow':>9} {'tau_fast':>9} {'DC gain':>10}")
    for P, dT in [(15, 5), (50, 8), (150, 10), (600, 25)]:
        mcp = mcp_for_corner(P, dT)
        s = build_plant(mcp)
        taus = [t for t, _ in modes(np.asarray(s.A))]
        gain = ct.dcgain(s).ravel()[INPUTS.index("P_DUT")]
        print(f"{P:7.0f} {dT:7.1f} {mL_per_min(mdot_from_mcp(mcp)):9.1f} {mcp:7.2f}"
              f" {taus[0]:9.1f} {taus[-1]:9.1f} {gain:10.6f}")
