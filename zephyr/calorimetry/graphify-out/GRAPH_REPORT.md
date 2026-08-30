# Graph Report - calorimetry  (2026-08-30)

## Corpus Check
- Corpus is ~31,919 words - fits in a single context window. You may not need a graph.

## Summary
- 307 nodes · 716 edges · 11 communities
- Extraction: 70% EXTRACTED · 30% INFERRED · 0% AMBIGUOUS · INFERRED: 217 edges (avg confidence: 0.86)
- Token cost: 0 input · 0 output

## Community Hubs (Navigation)
- Control Law and Gates
- Board Topology and Fans
- Network, Dashboard and Build Config
- LVGL Front Panel
- Supervisor State Machine
- Actuator Commands and Ranging
- Acquisition and Isothermal Calibration
- Heaters and Rail Metering
- Shell and Module Interfaces
- Water Loop and the Meter
- Model Generator

## God Nodes (most connected - your core abstractions)
1. `control()` - 23 edges
2. `calibration_request()` - 15 edges
3. `calibration_service()` - 15 edges
4. `main()` - 15 edges
5. `cal_clampf()` - 13 edges
6. `acquire()` - 13 edges
7. `tick()` - 12 edges
8. `safety_thread()` - 12 edges
9. `goto_state()` - 12 edges
10. `post_check_fan()` - 11 edges

## Surprising Connections (you probably didn't know these)
- `GPIO8 claimed for the INA226 wired-OR ALERT` --conceptually_related_to--> `Software over-current backstop`  [INFERRED]
  boards/esp32s3_devkitc_esp32s3_procpu.overlay → src/power-sense.h
- `CMakeLists.txt - build definition` --conceptually_related_to--> `gen_model.py - params.yaml to model_gen.h`  [INFERRED]
  CMakeLists.txt → tools/gen_model.py
- `The substitution check` --conceptually_related_to--> `Bring-up ladder F0-F8`  [INFERRED]
  src/calibration.h → README.md
- `GPIO8 claimed for the INA226 wired-OR ALERT` --rationale_for--> `power_sense_arm_alert()`  [EXTRACTED]
  boards/esp32s3_devkitc_esp32s3_procpu.overlay → src/power-sense.c
- `GPIO8 claimed for the INA226 wired-OR ALERT` --rationale_for--> `alert_isr()`  [EXTRACTED]
  boards/esp32s3_devkitc_esp32s3_procpu.overlay → src/system.c

## Import Cycles
- None detected.

## Hyperedges (group relationships)
- **The measurement chain: flow, dT, aux power, the reading** — src_water_loop_read_flow, src_water_loop_read_rtd, src_power_sense_power_sense_p_aux, src_control_system_cal_meter_power, src_main_meter, meter_equation [INFERRED 0.95]
- **What actually makes the instrument unbiased** — guard_null_loop, integrator_is_accuracy, anti_windup_backcalc, heater_cascade_watts, src_control_system_cal_pi_step [INFERRED 0.95]
- **The safety chain, hardware to supervisor** — three_safety_tiers, safety_thread_bus_free, heater_interlock, struct_meas_triple, src_system_safety_thread, src_system_sys_interlock_eval, alert_gpio8, software_overcurrent_backstop [INFERRED 0.95]

## Communities (11 total, 0 thin omitted)

### Community 0 - "Control Law and Gates"
Cohesion: 0.09
Nodes (39): Back-calculation anti-windup, Boundary B1 - the inner chamber, Disturbance observer, energy-balance form, The dT tare is per flow range, The guard null loop, Heaters driven in WATTS, not duty, The integrator IS the accuracy, The null gate |e| < P_acc / G_gap (+31 more)

### Community 1 - "Board Topology and Fans"
Cohesion: 0.08
Nodes (31): GPIO8 claimed for the INA226 wired-OR ALERT, Carrier board Kconfig fragment (headless), Carrier board overlay (ESP32-S3-DevKitC-1-N8R8), Dev-kit Kconfig fragment (display + LVGL), Dev-kit overlay (Waveshare ESP32-S3-LCD-1.47B), Devicetree as data (D10), calorimeter,io binding - discrete IO, Fan health needs two signals (+23 more)

### Community 2 - "Network, Dashboard and Build Config"
Cohesion: 0.08
Nodes (29): CMakeLists.txt - build definition, params.yaml is the single source of truth, prj.conf - shared Kconfig, The control core has no Zephyr dependency, associate(), settings_read_cb, cmd_from_name(), cmd_handler() (+21 more)

### Community 3 - "LVGL Front Panel"
Cohesion: 0.13
Nodes (31): lv_color_t, lv_event_cb_t, lv_event_t, lv_font_t, lv_obj_t, cmd_status(), cal_state_name(), sys_fault_name() (+23 more)

### Community 4 - "Supervisor State Machine"
Cohesion: 0.13
Nodes (27): One door for every command, cal_stat_reset(), tick(), boot_run(), button_cb(), cal_run(), dwell_entry(), dwell_run() (+19 more)

### Community 5 - "Actuator Commands and Ranging"
Cohesion: 0.13
Nodes (27): The inner fan duty is frozen for the campaign, calibration_progress(), calibration_request(), calibration_tare_for_flow(), finish(), tare_row_for_flow(), cal_clampf(), cal_flow_for_point() (+19 more)

### Community 6 - "Acquisition and Isothermal Calibration"
Cohesion: 0.12
Nodes (23): calorimeter,thermal-zone binding, The isothermal soak, Bring-up ladder F0-F8, calibration_service(), save_float(), slot_of(), cal_stat_push(), fans_rpm() (+15 more)

### Community 7 - "Heaters and Rail Metering"
Cohesion: 0.12
Nodes (23): The heater interlock, The safety thread never touches a bus, Software over-current backstop, duty_feedforward(), heaters_enabled(), heaters_get_duty(), heaters_init(), heaters_service() (+15 more)

### Community 8 - "Shell and Module Interfaces"
Cohesion: 0.15
Nodes (12): settings_read_cb, settings_set_cb(), cmd_clear(), cmd_dtw(), cmd_post(), cmd_soak(), cmd_start(), cmd_stop() (+4 more)

### Community 9 - "Water Loop and the Meter"
Cohesion: 0.14
Nodes (22): calorimeter,stepper-spi binding, The turbine is the meter; the pump is the actuator, The calorimeter equation P = rho*V*cp*dT - P_aux, calibration_init(), post_check_pump(), post_check_stepper(), read_flow(), read_rtd() (+14 more)

### Community 10 - "Model Generator"
Cohesion: 0.67
Nodes (3): load_params(), main(), Read the flat 'key: value' pairs. Uses PyYAML when present.

## Knowledge Gaps
- **2 isolated node(s):** `Dev-kit Kconfig fragment (display + LVGL)`, `Bring-up ladder F0-F8`
  These have ≤1 connection - possible missing edges or undocumented components.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `control()` connect `Control Law and Gates` to `Board Topology and Fans`, `Supervisor State Machine`, `Actuator Commands and Ranging`, `Heaters and Rail Metering`, `Shell and Module Interfaces`, `Water Loop and the Meter`?**
  _High betweenness centrality (0.081) - this node is a cross-community bridge._
- **Why does `Carrier board overlay (ESP32-S3-DevKitC-1-N8R8)` connect `Board Topology and Fans` to `Water Loop and the Meter`, `Supervisor State Machine`, `Acquisition and Isothermal Calibration`, `Heaters and Rail Metering`?**
  _High betweenness centrality (0.050) - this node is a cross-community bridge._
- **Why does `main()` connect `Board Topology and Fans` to `Control Law and Gates`, `Network, Dashboard and Build Config`, `LVGL Front Panel`, `Supervisor State Machine`, `Actuator Commands and Ranging`, `Heaters and Rail Metering`, `Shell and Module Interfaces`, `Water Loop and the Meter`?**
  _High betweenness centrality (0.047) - this node is a cross-community bridge._
- **Are the 21 inferred relationships involving `control()` (e.g. with `cal_clampf()` and `cal_gates_eval()`) actually correct?**
  _`control()` has 21 INFERRED edges - model-reasoned connections that need verification._
- **Are the 13 inferred relationships involving `calibration_request()` (e.g. with `cal_flow_for_point()` and `cal_range_for()`) actually correct?**
  _`calibration_request()` has 13 INFERRED edges - model-reasoned connections that need verification._
- **Are the 9 inferred relationships involving `calibration_service()` (e.g. with `cal_stat_mean()` and `cal_stat_push()`) actually correct?**
  _`calibration_service()` has 9 INFERRED edges - model-reasoned connections that need verification._
- **Are the 12 inferred relationships involving `main()` (e.g. with `calibration_init()` and `fans_init()`) actually correct?**
  _`main()` has 12 INFERRED edges - model-reasoned connections that need verification._