"""Pull every number the report quotes straight out of the model, so nothing is
transcribed by hand.  Runs the same code the figures were made with."""
import importlib.util, sys
from pathlib import Path
import numpy as np
import control as ct

PY = Path.home() / "roboteq" / "python"
sys.path.insert(0, str(PY))

_spec = importlib.util.spec_from_file_location("cal_ss", PY / "calorimetry-ss.py")
ss = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(ss)

_spec2 = importlib.util.spec_from_file_location("cal_sim", PY / "calorimetry_sim.py")
sim = importlib.util.module_from_spec(_spec2)
sim.__name__ = "cal_sim"
_spec2.loader.exec_module(sim)

# calorimetry_sim.py loads its OWN copy of calorimetry-ss.py, so patching alpha
# has to reach both module objects or the sim silently keeps the YAML default.
def set_alpha(a):
    ss.alpha = a
    sim.ss.alpha = a
    sim.alpha = a

p = ss.p


def dc_gain_open(mcp):
    s = ss.build_plant(mcp)
    return ct.dcgain(s).ravel()[ss.INPUTS.index("P_DUT")]


def eps_divider(mcp):
    R_adv, R_rad, R_gap, R_out = 1/mcp, 1/ss.UA_rad, 1/ss.G_gap, 1/ss.G_out
    return (R_adv + (1 - ss.alpha)*R_rad) / (R_adv + R_rad + R_gap + R_out)


def run(P, dT, t_end=9000.0, guard=None):
    mcp = ss.mcp_for_corner(P, dT)
    r = sim.simulate(P, mcp, t_end=t_end, guard=guard)
    return r


def summarize(P, dT, t_end=9000.0):
    mcp = ss.mcp_for_corner(P, dT)
    r = run(P, dT, t_end)
    flow = ss.mL_per_min(ss.mdot_from_mcp(mcp))
    return dict(
        P=P, dT=dT, mcp=mcp, flow=flow,
        gain_open=dc_gain_open(mcp),
        eps_open_pct=100*(1-dc_gain_open(mcp)),
        eps_final=r["eps"][-1],
        gain_closed=r["P_meas"][-1]/P,
        settle=sim.settling_time(r["t"], r["P_meas"])/60,
        peak_e=np.max(np.abs(r["e"])),
        peak_leak=np.max(np.abs(r["P_leak"])),
        Pe_ss=r["P_e"][-1],
        Pe_peak=r["P_e"].max(),
        th_i=r["th_i"][-1], th_w=r["th_w"][-1], th_e=r["th_e"][-1],
        P_out=r["P_out"][-1],
        saturated=r["saturated"].any(),
    )


def banner(t):
    print("\n" + "=" * 78); print(t); print("=" * 78)


# --------------------------------------------------------------- modes
banner("MODES at mcp = 15 W/K  (150 W / 10 K), alpha = %.2f" % ss.alpha)
A, *_ = ss.build_matrices(15.0)
np.set_printoptions(precision=6, suppress=True)
print("A =\n", A)
for tau, v in ss.modes(A):
    print(f"  tau = {tau:9.2f} s   v = [{v[0]:+.4f} {v[1]:+.4f} {v[2]:+.4f}]")

banner("PER-NODE first-order numbers (mcp = 15)")
print(f"  air   : sumG = {ss.UA_rad+ss.G_gap:7.3f} W/K   tau = {ss.C_i/(ss.UA_rad+ss.G_gap):8.1f} s")
print(f"  water : sumG = {ss.UA_rad+15.0:7.3f} W/K   tau = {ss.C_w/(ss.UA_rad+15.0):8.1f} s")
print(f"  guard : sumG = {ss.G_gap+ss.G_out:7.3f} W/K   tau = {ss.C_e/(ss.G_gap+ss.G_out):8.1f} s")
print(f"  K_e = {1/(ss.G_gap+ss.G_out):.4f} K/W  tau_e = {ss.C_e/(ss.G_gap+ss.G_out):.1f} s"
      f"  beta = {ss.G_gap/(ss.G_gap+ss.G_out):.4f}")
g = sim.GuardPI()
print(f"  {g}")

# ------------------------------------------------ the four plotted points
set_alpha(0.97)
banner("THE FOUR PLOTTED OPERATING POINTS   (alpha = %.2f, as the PNGs were run)" % ss.alpha)
hdr = (f"{'P':>5} {'dTset':>6} {'mcp':>6} {'mL/min':>8} {'gain_open':>10} {'eps_open%':>10} "
       f"{'gain_cl':>10} {'settle_m':>9} {'peak|e|':>8} {'leak_pk':>8} {'Pe_ss':>7} {'Pe_pk':>7} "
       f"{'th_i':>6} {'th_w':>6} {'th_e':>6} {'P_out':>6}")
print(hdr); print("-" * len(hdr))
plotted = [(150, 10), (150, 15), (150, 20), (200, 8)]
for P, dT in plotted:
    d = summarize(P, dT, 5400)
    print(f"{d['P']:5.0f} {d['dT']:6.1f} {d['mcp']:6.2f} {d['flow']:8.1f} {d['gain_open']:10.6f} "
          f"{d['eps_open_pct']:10.3f} {d['gain_closed']:10.6f} {d['settle']:9.1f} {d['peak_e']:8.3f} "
          f"{d['peak_leak']:8.3f} {d['Pe_ss']:7.2f} {d['Pe_peak']:7.2f} {d['th_i']:6.2f} "
          f"{d['th_w']:6.2f} {d['th_e']:6.2f} {d['P_out']:6.2f}")

# ------------------------------------------------------- shipping case
banner("DESIGN CORNER SWEEP, SHIPPING CONFIG (alpha = 0.0, radiator-only)")
set_alpha(0.0)
print(hdr); print("-" * len(hdr))
for P, dT in [(15, 5), (50, 8), (100, 10), (150, 10), (200, 10)]:
    d = summarize(P, dT, 14000)
    print(f"{d['P']:5.0f} {d['dT']:6.1f} {d['mcp']:6.2f} {d['flow']:8.1f} {d['gain_open']:10.6f} "
          f"{d['eps_open_pct']:10.3f} {d['gain_closed']:10.6f} {d['settle']:9.1f} {d['peak_e']:8.3f} "
          f"{d['peak_leak']:8.3f} {d['Pe_ss']:7.2f} {d['Pe_peak']:7.2f} {d['th_i']:6.2f} "
          f"{d['th_w']:6.2f} {d['th_e']:6.2f} {d['P_out']:6.2f}")

banner("ALPHA INVARIANCE — same corner, both configs")
for a in (0.0, 0.97):
    set_alpha(a)
    d = summarize(150, 10, 9000)
    print(f"  alpha={a:4.2f}: open gain {d['gain_open']:.6f}  closed gain {d['gain_closed']:.8f} "
          f" eps_final {d['eps_final']:+.3e} W   settle {d['settle']:.1f} min  "
          f" zero tau_z = {a*ss.C_i/ss.UA_rad:.1f} s")

# ---------------------------------------------------------- ablation
banner("ABLATION — what the guard is worth (150 W, dT 10 K, alpha = 0.0)")
set_alpha(0.0)
mcp = ss.mcp_for_corner(150, 10)
cfgs = {}
gp = sim.GuardPI();                       cfgs["PI (integral action)"] = gp
gp2 = sim.GuardPI(); gp2.Ki = 0.0;        cfgs["P-only (Ki = 0)"] = gp2
gp3 = sim.GuardPI(); gp3.Kp = 0.0; gp3.Ki = 0.0; cfgs["guard OFF (open loop)"] = gp3
print(f"{'config':<26} {'DC gain':>10} {'deficit %':>10} {'err@150W':>10} {'e_ss [K]':>10} {'G_gap*e_ss':>11}")
for name, gd in cfgs.items():
    r = sim.simulate(150.0, mcp, t_end=20000.0, guard=gd)
    gain = r["P_meas"][-1] / 150.0
    print(f"{name:<26} {gain:10.6f} {100*(1-gain):10.4f} {150*(1-gain):10.3f} "
          f"{r['e'][-1]:10.5f} {ss.G_gap*r['e'][-1]:11.4f}")

set_alpha(0.0)
banner("SYSTEMATIC ERROR (guard open) vs flow — the leakage divider, alpha = 0")
print(f"{'mcp':>6} {'mL/min':>9} {'DC gain':>10} {'eps':>10} {'divider':>10} {'agree':>10}")
for mcp in (3.0, 5.0, 7.5, 10.0, 15.0, 20.0, 25.0, 30.0):
    gain = dc_gain_open(mcp)
    print(f"{mcp:6.2f} {ss.mL_per_min(ss.mdot_from_mcp(mcp)):9.1f} {gain:10.6f} "
          f"{1-gain:10.6f} {eps_divider(mcp):10.6f} {abs((1-gain)-eps_divider(mcp)):10.2e}")

banner("FLOW SIZING — mL/min needed vs power and water dT")
print(f"{'P [W]':>7}" + "".join(f"{d:>9}" for d in (5, 10, 15, 20, 30, 40)))
print(f"{'dT [K]->':>7}" + "-" * 54)
for P in (15, 25, 50, 100, 150, 200):
    row = f"{P:7.0f}"
    for dT in (5, 10, 15, 20, 30, 40):
        row += f"{ss.mL_per_min(ss.mdot_from_mcp(P/dT)):9.1f}"
    print(row)

banner("NULL GATE / BUDGET CONSTANTS")
print(f"  P_acc          = {p['P_acc']} W")
print(f"  G_gap          = {ss.G_gap} W/K   ->  R_th(in) = {1/ss.G_gap:.3f} K/W")
print(f"  null gate      = P_acc / G_gap = {p['P_acc']/ss.G_gap:.3f} K")
print(f"  G_out          = {ss.G_out} W/K")
print(f"  UA_rad         = {ss.UA_rad} W/K  -> stiffness ratio UA_rad/G_gap = {ss.UA_rad/ss.G_gap:.0f}x")
print(f"  C_i+C_w        = {ss.C_i+ss.C_w} J/K")
print(f"  P_e_max        = {p['P_e_max']} W")
print(f"  lambda_guard   = {p['lambda_guard']} s")
