"""
Calorimeter — closed-loop simulation, probe map and plots.

Builds on calorimetry-ss.py's L1 plant and wraps the guard loop around it:

    4 states = 3 plant (theta_i, theta_w, theta_e) + 1 guard integrator (x_I)

Saturation and anti-windup are NOT linear, so they cannot live in A_cl.  That is
why this file integrates the system numerically instead of using ct.step():
inside the limits it is exactly the LTI model, outside it is a different one.

Run:  python calorimetry_sim.py
"""

from pathlib import Path
import importlib.util
import datetime
import sys

# ---- prove which interpreter this is, before anything can fail -------------
print(f"[calorimetry_sim] python {sys.version.split()[0]}   {sys.executable}")

_missing = []
for _name, _pip in (("numpy", "numpy"), ("matplotlib", "matplotlib"),
                    ("yaml", "pyyaml"), ("control", "control")):
    try:
        __import__(_name)
    except ImportError:
        _missing.append(_pip)

if _missing:
    print("\n  !! these packages are missing FROM THIS INTERPRETER: "
          + ", ".join(_missing))
    print("\n  install them into this exact interpreter (not just 'pip install'):")
    print(f"     {sys.executable} -m pip install " + " ".join(_missing))
    print("\n  if that fails, this Python is probably too new for one of the wheels.")
    print("  Any 3.10-3.12 interpreter will work:")
    print("     python3.12 -m pip install numpy matplotlib pyyaml control")
    print(f"     python3.12 {Path(__file__).name}")
    sys.exit(1)

# ---- writability of the output folder, checked once, up front --------------
_OUT_DIR = Path(__file__).parent.resolve()
try:
    _probe = _OUT_DIR / ".write_test"
    _probe.write_text("ok")
except OSError as _err:
    print(f"\n  !! cannot write to {_OUT_DIR}\n     {_err}")
    sys.exit(1)
try:
    _probe.unlink()          # tidy up; deleting is not required, only writing is
except OSError:
    pass

import numpy as np                 # pyright: ignore[reportMissingModuleSource]
import matplotlib                  # pyright: ignore[reportMissingModuleSource]
import matplotlib.pyplot as plt    # pyright: ignore[reportMissingModuleSource]

SHOW  = True      # True -> also pop the figures up (needs a GUI backend, see the banner)
SAVED = []        # every file this run wrote, reported at the end

# calorimetry-ss.py has a hyphen, so load it by path rather than by import
_spec = importlib.util.spec_from_file_location(
    "cal_ss", Path(__file__).parent / "calorimetry-ss.py")
ss = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ss)

p = ss.p
C_i, C_w, C_e        = ss.C_i, ss.C_w, ss.C_e
UA_rad, G_gap, G_out = ss.UA_rad, ss.G_gap, ss.G_out
alpha                = ss.alpha


# =============================================================== controller

class GuardPI:
    """PI + saturation + back-calculation anti-windup on the guard heater.

    The loop this controller closes is a SISO first-order plant extracted from
    the state space:   P_e -> theta_e  =  K_e / (tau_e·s + 1)
    Reference: theta_i, which MOVES -- hence the feedforward term.

    Gains come from lambda-tuning (IMC): one knob, the closed-loop time constant.
    """

    def __init__(self, lam=None, P_max=None, use_aw=True, P_ff=0.0):
        self.K_e   = 1.0 / (G_gap + G_out)          # K/W   -- DC gain of the loop's plant
        self.tau_e = C_e / (G_gap + G_out)          # s     -- its time constant
        self.beta  = G_gap / (G_gap + G_out)        # -     -- passive tracking, < 1 = why PI

        lam       = p["lambda_guard"] if lam is None else lam
        self.Kp   = self.tau_e / (self.K_e * lam)   # W/K
        self.Ki   = self.Kp / self.tau_e            # W/(K·s)
        self.T_t  = self.Kp / self.Ki               # = tau_e; anti-windup time constant

        self.P_max  = p["P_e_max"] if P_max is None else P_max
        self.use_aw = use_aw
        self.P_ff   = P_ff

    def __call__(self, th_i, th_e, x_I):
        """-> (e, v, P_e, dx_I).  v is the demand BEFORE the limiter."""
        e   = th_i - th_e                                  # null error [K]
        v   = self.Kp * e + self.Ki * x_I + self.P_ff      # unsaturated demand [W]
        P_e = min(max(v, 0.0), self.P_max)                 # floor is 0: heaters cannot cool

        if self.use_aw:
            dx_I = e + (P_e - v) / self.T_t   # back-calculation; exactly 0 when not clipped
        else:
            dx_I = e                          # naive integrator -> winds up

        return e, v, P_e, dx_I

    def __repr__(self):
        return (f"GuardPI(Kp={self.Kp:.3f} W/K, Ki={self.Ki:.5f} W/(K·s), "
                f"T_t={self.T_t:.0f} s, P_max={self.P_max:.1f} W, AW={self.use_aw})")


# ================================================================ simulator

def simulate(P_DUT, mcp, t_end=3600.0, dt=0.5, guard=None,
             th_rail=5.0, P_aux=0.0, Ta_dot=0.0, presettle=True):
    """Integrate plant + guard loop.  Returns a dict of named probe traces.

    P_DUT     : watts, a float or a callable f(t) -> watts
    mcp       : advection conductance [W/K] -- frozen for the run (it is bilinear)
    th_rail   : water inlet temperature, deviation from ambient [K]
    presettle : start from the rig parked with the DUT off, so t=0 is a settled
                machine and the trace is a clean step response rather than a
                cold-start artefact.
    """
    guard = GuardPI() if guard is None else guard
    A, B_u, B_d, Cy, D_u, D_d = ss.build_matrices(mcp)
    ss.audit(A, mcp)

    P_of = P_DUT if callable(P_DUT) else (lambda t: P_DUT)

    def deriv(t, z, P_fun):
        x, x_I = z[:3], z[3]
        _, _, P_e, dx_I = guard(x[0], x[2], x_I)
        u = np.array([P_aux, P_e])
        d = np.array([P_fun(t), th_rail, Ta_dot])
        return np.concatenate([A @ x + B_u @ u + B_d @ d, [dx_I]])

    def rk4(P_fun, z0, t_grid, h):
        Z = np.zeros((len(t_grid), 4))
        Z[0] = z0
        for k in range(len(t_grid) - 1):            # classic RK4, fixed step
            z, tk = Z[k], t_grid[k]
            k1 = deriv(tk,         z,               P_fun)
            k2 = deriv(tk + h / 2, z + h / 2 * k1,  P_fun)
            k3 = deriv(tk + h / 2, z + h / 2 * k2,  P_fun)
            k4 = deriv(tk + h,     z + h * k3,      P_fun)
            Z[k + 1] = z + h / 6 * (k1 + 2 * k2 + 2 * k3 + k4)
        return Z

    z0 = np.zeros(4)
    if presettle:                                   # park the rig, DUT off
        t_pre = np.arange(0.0, 30000.0, 5.0)
        z0 = rk4(lambda t: 0.0, z0, t_pre, 5.0)[-1]

    n = int(t_end / dt) + 1
    t = np.linspace(0.0, t_end, n)
    Z = rk4(P_of, z0, t, dt)

    # ---- rebuild every probe from the stored trajectory ------------------
    th_i, th_w, th_e, x_I = Z.T
    P_dut = np.array([P_of(tk) for tk in t])

    e, v, P_e = np.zeros(n), np.zeros(n), np.zeros(n)
    for k in range(n):
        e[k], v[k], P_e[k], _ = guard(th_i[k], th_e[k], x_I[k])

    P_meas = mcp * (th_w - th_rail) - P_aux

    return {
        "t": t, "guard": guard, "mcp": mcp,
        # --- powers you can put a wattmeter on ---------------------------
        "P_DUT":   P_dut,                        # the measurand            [W]
        "P_meas":  P_meas,                       # the reading              [W]
        "eps":     P_meas - P_dut,               # THE ERROR TRACE          [W]
        "P_e":     P_e,                          # guard heater, after sat  [W]
        "v":       v,                            # demand before sat        [W]
        "P_aux":   np.full(n, P_aux),            # metered auxiliaries      [W]
        "P_rad":   UA_rad * (th_i - th_w),       # through the radiator     [W]
        "P_adv":   mcp * (th_w - th_rail),       # carried by the flow      [W]
        "P_leak":  G_gap * e,                    # air -> gap, THE LEAK     [W]
        "P_out":   G_out * th_e,                 # gap -> ambient, UNMETERED[W]
        # --- temperatures -------------------------------------------------
        "th_i": th_i, "th_w": th_w, "th_e": th_e,
        "th_rail": np.full(n, th_rail),
        "e":     e,                              # null error               [K]
        "dT_w":  th_w - th_rail,                 # the auto-range variable  [K]
        # --- controller internals ----------------------------------------
        "x_I":       x_I,                        # integrator, windup detector
        "sat_hi":    v > guard.P_max + 1e-9,     # pinned at the ceiling
        "sat_lo":    v < -1e-9,                  # pinned at 0 -- heaters cannot cool
        "saturated": np.abs(v - P_e) > 1e-9,
    }


def settling_time(t, y, frac=0.01):
    """Time after which y stays within frac of its final value."""
    final = y[-1]
    if abs(final) < 1e-12:
        return np.nan
    outside = np.where(np.abs(y - final) > abs(final) * frac)[0]
    return t[outside[-1]] if len(outside) else 0.0


# =================================================================== report

def print_probe_map(r):
    t, P = r["t"], r["P_DUT"][-1]
    print(f"\n{r['guard']}")
    print(f"operating point: P_DUT = {P:.0f} W, mcp = {r['mcp']:.2f} W/K, "
          f"flow = {ss.mL_per_min(ss.mdot_from_mcp(r['mcp'])):.0f} mL/min\n")

    rows = [
        ("P_meas",  "the reading",                  r["P_meas"], "W"),
        ("eps",     "*** reading - truth ***",      r["eps"],    "W"),
        ("e",       "null error, th_i - th_e",      r["e"],      "K"),
        ("P_leak",  "the leak, G_gap*e",            r["P_leak"], "W"),
        ("P_e",     "guard heater demand",          r["P_e"],    "W"),
        ("P_rad",   "watts through the radiator",   r["P_rad"],  "W"),
        ("P_adv",   "watts carried by the flow",    r["P_adv"],  "W"),
        ("P_out",   "watts lost to ambient",        r["P_out"],  "W"),
        ("dT_w",    "auto-range variable",          r["dT_w"],   "K"),
        ("th_i",    "chamber air",                  r["th_i"],   "K"),
        ("th_w",    "block + water",                r["th_w"],   "K"),
        ("th_e",    "guard gap",                    r["th_e"],   "K"),
        ("x_I",     "guard integrator (windup)",    r["x_I"],    "K·s"),
    ]
    print(f"{'probe':>8}  {'what it is':<30} {'peak':>10} {'final':>12}  unit")
    print("-" * 74)
    for name, what, sig, unit in rows:
        pk = sig[np.argmax(np.abs(sig))]
        print(f"{name:>8}  {what:<30} {pk:10.4f} {sig[-1]:12.6f}  {unit}")

    print(f"\n  1% settling of P_meas : {settling_time(t, r['P_meas'])/60:6.1f} min")
    print(f"  closed-loop error     : {r['eps'][-1]:+.2e} W   "
          f"(DC gain = {r['P_meas'][-1]/P:.8f})")
    print(f"  saturated             : {'YES' if r['saturated'].any() else 'no'}"
          f"  ({100*r['saturated'].mean():.1f} % of the run)")


# ==================================================================== plots

def _save(fig, path):
    """Write the figure and prove it -- path, size and wall-clock time.
    If the timestamp does not advance when you re-run, your viewer is caching,
    not the script."""
    out = _OUT_DIR / path
    fig.savefig(out, dpi=140)
    SAVED.append(out)
    st = out.stat()
    print(f"  saved {out}   ({st.st_size / 1024:.0f} kB, "
          f"{datetime.datetime.fromtimestamp(st.st_mtime):%H:%M:%S})")
    if not SHOW:
        plt.close(fig)      # keep it open only if we intend to display it later
    return out


def plot_response(r, path=None):
    if path is None:                       # filename follows the operating point,
        path = (f"response_{r['P_DUT'][-1]:.0f}W_"      # so a changed run cannot
                f"dT{r['dT_w'][-1]:.0f}K.png")          # silently overwrite nothing
    t = r["t"] / 60.0
    fig, ax = plt.subplots(2, 3, figsize=(15, 7.5))
    fig.suptitle(f"Closed-loop step response — P_DUT = {r['P_DUT'][-1]:.0f} W, "
                 f"mcp = {r['mcp']:.1f} W/K", fontsize=12)

    ax[0, 0].plot(t, r["P_DUT"], "k--", lw=1.2, label="P_DUT (truth)")
    ax[0, 0].plot(t, r["P_meas"], lw=1.6, label="P_meas (reading)")
    ax[0, 0].set_ylabel("W"); ax[0, 0].set_title("the measurement")

    ax[0, 1].plot(t, r["eps"], lw=1.5, color="crimson")
    ax[0, 1].axhline(0, color="k", lw=0.6)
    ax[0, 1].axhline(p["P_acc"], color="grey", ls=":", lw=1)
    ax[0, 1].axhline(-p["P_acc"], color="grey", ls=":", lw=1, label=f"±{p['P_acc']} W target")
    ax[0, 1].set_ylim(-6 * p["P_acc"], 3 * p["P_acc"])   # the t=0 transient runs off-scale
    ax[0, 1].set_ylabel("W")
    ax[0, 1].set_title("eps = reading − truth   (y-axis clipped)")

    ax[0, 2].plot(t, r["th_i"], label="θi  air")
    ax[0, 2].plot(t, r["th_w"], label="θw  block+water")
    ax[0, 2].plot(t, r["th_e"], label="θe  guard")
    ax[0, 2].plot(t, r["th_rail"], "k:", lw=1, label="θrail")
    ax[0, 2].set_ylabel("K above ambient"); ax[0, 2].set_title("node temperatures")

    ax[1, 0].plot(t, r["e"], lw=1.5, label="e = θi − θe")
    gate = p["P_acc"] / G_gap
    ax[1, 0].axhline(gate, color="grey", ls=":", lw=1, label=f"null gate {gate:.2f} K")
    ax[1, 0].set_ylabel("K"); ax[1, 0].set_title("guard null error")

    ax[1, 1].plot(t, r["v"], ls="--", lw=1.2, label="v  (demand)")
    ax[1, 1].plot(t, r["P_e"], lw=1.6, label="P_e (after limiter)")
    ax[1, 1].axhline(r["guard"].P_max, color="crimson", ls=":", lw=1, label="P_max")
    ax[1, 1].set_ylabel("W"); ax[1, 1].set_title("actuator — saturation shows as v ≠ P_e")

    ax[1, 2].plot(t, r["P_rad"],  label="P_rad  air→water")
    ax[1, 2].plot(t, r["P_adv"],  label="P_adv  flow (= the reading)")
    ax[1, 2].plot(t, r["P_leak"], label="P_leak air→gap")
    ax[1, 2].plot(t, r["P_out"],  label="P_out  gap→ambient (unmetered)")
    ax[1, 2].set_ylabel("W"); ax[1, 2].set_title("where the watts actually go")

    for a in ax.ravel():
        a.set_xlabel("minutes"); a.grid(alpha=0.25); a.legend(fontsize=8)
    fig.tight_layout()
    print()
    _save(fig, path)
    return fig


def demo_windup(P_DUT=600.0, dT=25.0, t_end=5000.0, t_dump=1500.0):
    """Saturation and anti-windup, at three actuator ceilings.

    Load steps to P_DUT, then dumps to 15 W at t_dump -- the error reverses,
    which is when a wound-up integrator has to be unwound and shows itself.

    The three ceilings are DERIVED from this operating point, not hard-coded,
    so the demo still means the same thing when you change P_DUT and dT:

      as designed  -> P_e_max from the YAML; never saturates
      marginal     -> between the steady demand and the transient peak;
                      clips on the way up, then releases
      undersized   -> below the steady demand.  Permanently clipped, and NO
                      controller fixes it.  That is a sizing failure, not tuning.
    """
    mcp = ss.mcp_for_corner(P_DUT, dT)

    def load(t):
        return P_DUT if t < t_dump else 15.0

    # size the ceilings from an unclipped probe run
    probe  = simulate(load, mcp, t_end, guard=GuardPI(P_max=1e9))
    i_dump = np.searchsorted(probe["t"], t_dump) - 1
    P_ss   = probe["P_e"][i_dump]                       # steady demand at this load
    P_peak = probe["P_e"].max()                         # transient peak
    P_marg = P_ss + 0.35 * (P_peak - P_ss)
    P_und  = 0.70 * P_ss

    runs = {
        f"P_max = {p['P_e_max']:.0f} W  (as designed)":
            simulate(load, mcp, t_end, guard=GuardPI(P_max=p["P_e_max"])),
        f"P_max = {P_marg:.1f} W,  AW on":
            simulate(load, mcp, t_end, guard=GuardPI(P_max=P_marg, use_aw=True)),
        f"P_max = {P_marg:.1f} W,  AW OFF":
            simulate(load, mcp, t_end, guard=GuardPI(P_max=P_marg, use_aw=False)),
        f"P_max = {P_und:.1f} W  (undersized)":
            simulate(load, mcp, t_end, guard=GuardPI(P_max=P_und)),
    }

    print(f"  guard demand at {P_DUT:.0f} W:  steady {P_ss:.2f} W, "
          f"transient peak {P_peak:.2f} W")

    fig, ax = plt.subplots(1, 3, figsize=(15, 4.2))
    fig.suptitle(f"Saturation & anti-windup — {P_DUT:.0f} W step, load dumped to 15 W "
                 f"at {t_dump/60:.0f} min", fontsize=12)
    for name, r in runs.items():
        tm = r["t"] / 60
        ax[0].plot(tm, r["e"],   lw=1.4, label=name)
        ax[1].plot(tm, r["x_I"], lw=1.4, label=name)
        ax[2].plot(tm, r["P_e"], lw=1.4, label=name)
    ax[0].set_title("null error e = θi − θe  [K]"); ax[0].axhline(0, color="k", lw=0.6)
    ax[1].set_title("integrator x_I  [K·s]   ← windup lives here")
    ax[2].set_title("P_e actually delivered  [W]")
    for a in ax:
        a.set_xlabel("minutes"); a.grid(alpha=0.25); a.legend(fontsize=7.5)
    fig.tight_layout()
    _save(fig, f"windup_{P_DUT:.0f}W_dT{dT:.0f}K.png")
    print()

    gate = p["P_acc"] / G_gap

    def recovery(r):
        """Minutes after the load dump for |e| to come back inside the null gate."""
        i = np.searchsorted(r["t"], t_dump)
        bad = np.where(np.abs(r["e"][i:]) > gate)[0]
        return (r["t"][i + bad[-1]] - t_dump) / 60 if len(bad) else 0.0

    print(f"{'configuration':<30} {'@ceiling':>9} {'@floor':>8} "
          f"{'peak x_I':>10} {'recovery':>10}")
    for name, r in runs.items():
        print(f"{name:<30} {100*r['sat_hi'].mean():7.1f} % {100*r['sat_lo'].mean():6.1f} % "
              f"{np.max(np.abs(r['x_I'])):10.0f} {recovery(r):8.1f} m")
    print(f"\n  null gate = P_acc/G_gap = {gate:.2f} K")
    print("  '@floor' is the heater pinned at 0 W after the load dump -- it cannot cool,")
    print("  so that recovery is set by G_out bleeding the gap off, not by the controller.")
    return fig


# ===================================================================== main

if __name__ == "__main__":
    # ==================== CHANGE THESE ====================
    P     = 150.0     # DUT power [W]
    dT    = 15.0      # ΔT across the block [K]  -> this sets the flow
    T_END = 3600.0    # simulated duration [s]
    # ======================================================
    # or from a shell, without editing anything:
    #     python calorimetry_sim.py 300 12
    if len(sys.argv) >= 3:
        P, dT = float(sys.argv[1]), float(sys.argv[2])

    backend = matplotlib.get_backend()
    print("=" * 74)
    print(f"running   {Path(__file__).resolve()}")
    print(f"writing   {Path(__file__).parent.resolve()}")
    print(f"backend   {backend}    SHOW = {SHOW}")
    if SHOW and backend.lower() == "agg":
        print("  !! 'Agg' is a file-only backend -- plt.show() cannot open a window.")
        print("     The PNGs will still be written. For windows, install a GUI backend:")
        print("       pip install PyQt5        (then re-run)")
    print("=" * 74)

    plt.close("all")                 # drop any figures left over from a previous run
    mcp = ss.mcp_for_corner(P, dT)

    r = simulate(P, mcp, t_end=T_END)
    print_probe_map(r)
    plot_response(r)

    print("\n" + "=" * 74)
    print(f"anti-windup demo — same operating point ({P:.0f} W, {dT:.0f} K)")
    demo_windup(P_DUT=P, dT=dT)

    print("\n" + "=" * 74)
    print("corner sweep — closed loop")
    print(f"{'P [W]':>7} {'dT':>5} {'mcp':>7} {'1% settle':>11} "
          f"{'peak |e|':>9} {'peak leak':>10} {'P_e ss':>8} {'final eps':>12}")
    for P, dT in [(15, 5), (50, 8), (150, 10), (600, 25)]:
        mcp = ss.mcp_for_corner(P, dT)
        r = simulate(P, mcp, t_end=8000.0)
        print(f"{P:7.0f} {dT:5.1f} {mcp:7.2f} "
              f"{settling_time(r['t'], r['P_meas'])/60:9.1f} m "
              f"{np.max(np.abs(r['e'])):9.3f} {np.max(np.abs(r['P_leak'])):10.3f} "
              f"{r['P_e'][-1]:8.2f} {r['eps'][-1]:+12.2e}")

    # ---- what this run actually wrote, in full -------------------------------
    print("\n" + "=" * 74)
    print(f"wrote {len(SAVED)} figure(s):")
    for f in SAVED:
        print(f"   {f}")

    if SHOW:
        if matplotlib.get_backend().lower() == "agg":
            print("\nbackend is 'Agg' -- no windows. Open the files above instead.")
        else:
            print("\nopening windows -- close them to end the script.")
            plt.show()          # once, at the very end, so it cannot block mid-run
    else:
        print(f"\nSHOW = False. Set it True at the top of {Path(__file__).name} "
              f"to pop the figures up as well.")
