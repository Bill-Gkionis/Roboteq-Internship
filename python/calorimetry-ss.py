import yaml # pyright: ignore[reportMissingModuleSource]
import numpy as np # pyright: ignore[reportMissingModuleSource]
import matplotlib.pyplot as plt # pyright: ignore[reportMissingModuleSource]
import control as ct # pyright: ignore[reportMissingModuleSource]
import scipy.optimize as opt # pyright: ignore[reportMissingModuleSource]

with open('params.yaml', 'r') as f:
    params = yaml.safe_load(f)

for key, value in params.items():
    if isinstance(value, (int, float)):
        globals()[key] = np.float64(value)


# State-space A matrix: states -> state rates, order (theta_i, theta_w, theta_e)
C_i, C_w, C_e = params['C_i'], params['C_w'], params['C_e']
UA_rad, G_gap, G_out = params['UA_rad'], params['G_gap'], params['G_out']
mdot, c_p_w = params['mdot'], params['c_p_w']

A = np.array([
    [-(UA_rad + G_gap) / C_i,       UA_rad / C_i,                        G_gap / C_i           ],
    [ UA_rad / C_w,                 -(UA_rad + mdot * c_p_w) / C_w,      0.0                   ],
    [ G_gap / C_e,                   0.0,                                -(G_gap + G_out) / C_e],
])
print(A)

# B_u matrix: commands (P_aux, P_e) -> state rates
B_u = np.array([
    [1.0 / C_i, 0.0       ],
    [0.0,       0.0       ],
    [0.0,       1.0 / C_e ],
])
print(B_u)

# B_d matrix: disturbances (P_DUT, theta_rail, T_a_dot) -> state rates
alpha = params['alpha']

B_d = np.array([
    [(1 - alpha) / C_i, 0.0,                -1.0],
    [alpha / C_w,       mdot * c_p_w / C_w, -1.0],
    [0.0,               0.0,                -1.0],
])
print(B_d)