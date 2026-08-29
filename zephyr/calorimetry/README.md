# DJC calorimeter firmware

Zephyr application for the double-jacket calorimeter: reads the DUT's power
loss as heat (`P = mdot * cp * dT`), servos an air-gap guard to null the wall
gradient, and logs each point with its own uncertainty.

Physics, gains and gates come from `Calorimetry/Design/System Modeling Final.md`
in the Obsidian vault; `src/model_gen.h` is generated from the same
`params.yaml` the Python simulation reads.

---

## Build

From inside `~/roboteq/zephyrproject`, with the venv active:

```sh
source ~/roboteq/venv/bin/activate

# base: panel + control + HTTP dashboard (polling)
west build -p auto -b esp32s3_lcd_1_47b/esp32s3/procpu ~/roboteq/zephyr/calorimetry -- \
    -DBOARD_ROOT=$HOME/roboteq/zephyr/waveshare-esp32-s3-1.47B

# with the radio + WebSocket push
west build -p auto -b esp32s3_lcd_1_47b/esp32s3/procpu ~/roboteq/zephyr/calorimetry -- \
    -DBOARD_ROOT=$HOME/roboteq/zephyr/waveshare-esp32-s3-1.47B \
    -DEXTRA_CONF_FILE=wifi.conf

west flash
west espressif monitor
```

Both variants are verified building clean against Zephyr 4.4.99 (`main`) with
SDK 1.0.1.  The Wi-Fi variant needs the `tf-psa-crypto` module — see the header
of `wifi.conf` for the two commands that add it.

## Layout

```
CMakeLists.txt      build; reads VERSION.txt into APP_VERSION_STR
prj.conf            base config
wifi.conf           optional: radio + WebSocket (needs tf-psa-crypto)
app.overlay         PROVISIONAL dev-kit pin map — the only file the carrier
                    board changes
dts/bindings/       one custom binding for the discrete IO lines
sections-rom.ld     linker section for the HTTP server's resource table
tools/gen_model.py  params.yaml -> src/model_gen.h

src/
  main.c            boot order + the 1 Hz tick + shell commands
  types.h           the shared data types (struct meas, snapshot, faults)
  model_gen.h       GENERATED: plant, gains, gates
  control-system.*  pure C, no Zephyr: meter, PIs, gates, observer
  temp-sense.*      TMP117 chamber temperatures (I2C)
  power-sense.*     INA226 rail metering + the hardware over-current arm
  water-loop.*      pump, RTD dT pair, turbine flow meter
  fans.*            three PWM fan groups
  heaters.*         two heaters, driven in WATTS via an inner power loop
  calibration.*     isothermal soak / dT tare / substitution + NVS
  system.*          supervisor state machine, safety thread, POST, faults
  ui.*              LVGL panel: RUN / CTRL / NET
  iot.*             Wi-Fi, HTTP dashboard, WebSocket push
  webapp/index.html the dashboard, gzipped into flash at build time
```

## Operating it

Over the USB console (`west espressif monitor`), everything is under `cal`:

```
cal status              everything, once
cal start / cal stop    run or abort a measurement point
cal tss <degC>          chamber setpoint
cal dtw <K>             water dT setpoint (picks the flow)
cal inlet <degC>        reject-loop inlet setpoint
cal soak                isothermal offset calibration  -> NVS
cal tare [mL/min]       dT zero for this flow range    -> NVS
cal subst [W]           substitution check (inject a known W)
cal post                re-run the self test
cal wifi <ssid> <psk>   store credentials and associate
cal clear               clear latched faults
```

The panel's buttons and the web dashboard submit the *same* commands through
`sys_cmd_submit()`, so the interlocks apply identically whatever the transport.

## Bring-up order

Do not climb past a rung that is red.

| # | Stage | Exit criterion |
|---|---|---|
| F0 | build + flash + shell | every `DEVICE_DT_GET` resolves; `cal status` prints |
| F1 | acquisition | `cal soak` passes: all sensors within ±2 K, offsets in NVS |
| F2 | actuators + POST | every POST row within tolerance |
| F3 | **the meter** | `cal subst 100` reads back within ±1 W — **the whole instrument in miniature** |
| F4 | reject loop | inlet holds setpoint ±0.2 K across a load sweep |
| F5 | guard loop | `|e|` at the noise floor; `guard_i` converges; AW verified by forcing saturation |
| F6 | gates + supervisor | one point start-to-logged, unattended |
| F7 | observer | `p_hat` agrees with the fully settled `p_meas` at every corner |
| F8 | safety | **actually break things**: pull SDA low, unplug a sensor, stall a fan, stop the pump, hang the control loop |

F8 must be done by breaking things, not by reading code. An untested fault path
is a fault path that does not exist.

## Known limitations

- `app.overlay` is a **bring-up** pin map on the Waveshare dev kit, not the
  carrier board. It disables the microSD slot and the RGB LED to free pins and
  SPI2. Replacing it is the entire porting job.
- Two MAX31865 converters, not one muxed across both probes. Their reference
  resistors do not cancel in the difference; the per-range in-situ tare absorbs
  the mismatch until the carrier board adds a 4-wire mux.
- The disturbance observer is the reduced-order energy-balance form, and it
  runs in **shadow mode** — logged, never gating.
- No TLS. LAN only, token on the control endpoint. Adding TLS is a `prj.conf`
  change plus a certificate, not a code change.
