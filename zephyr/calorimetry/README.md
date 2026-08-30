# DJC calorimeter firmware

Zephyr application for the double-jacket calorimeter: reads the DUT's power
loss as heat (`P = mdot * cp * dT`), servos an air-gap guard to null the wall
gradient, and logs each point with its own uncertainty.

Physics, gains and gates come from `Calorimetry/Design/System Modeling Final.md`
in the Obsidian vault; `src/model_gen.h` is generated from the same
`params.yaml` the Python simulation reads.

---

## Two boards

| Board target | What it is | Overlay |
|---|---|---|
| `esp32s3_devkitc/esp32s3/procpu` | **the carrier board** — ESP32-S3-DevKitC-1-N8R8 on the calorimeter PCB, headless | `boards/esp32s3_devkitc_esp32s3_procpu.overlay` |
| `esp32s3_lcd_1_47b/esp32s3/procpu` | the bench dev kit, and the only build with the LVGL panel | `boards/esp32s3_lcd_1_47b_esp32s3_procpu.overlay` |

Both present the **same device set through the same aliases**, so they exercise
identical code paths. There is no `app.overlay`: Zephyr prefers a
`boards/<target>.overlay` when one exists, and an `app.overlay` alongside it
would silently apply to only some builds.

## Build

From inside `~/roboteq/zephyrproject`, with the venv active:

```sh
source ~/roboteq/venv/bin/activate

# --- the carrier board -----------------------------------------------------
west build -p auto -b esp32s3_devkitc/esp32s3/procpu ~/roboteq/zephyr/calorimetry \
    -S espressif-psram-8M -- -DEXTRA_CONF_FILE=wifi.conf

# --- the bench dev kit (panel + display) -----------------------------------
west build -p auto -b esp32s3_lcd_1_47b/esp32s3/procpu ~/roboteq/zephyr/calorimetry -- \
    -DBOARD_ROOT=$HOME/roboteq/zephyr/waveshare-esp32-s3-1.47B \
    -DEXTRA_CONF_FILE=wifi.conf

west flash
west espressif monitor      # dev kit: native USB.  Carrier: UART0 via CP2102
```

`-S espressif-psram-8M` is what makes the carrier build an **N8R8**: the
upstream `esp32s3_devkitc` board file is the N8 variant (8 MB flash, no PSRAM)
and the snippet adds the 8 MB octal PSRAM. Drop `-DEXTRA_CONF_FILE=wifi.conf`
for a build with no radio — the dashboard then polls instead of pushing.

All four combinations are verified building clean against Zephyr 4.4.99
(`main`) with SDK 1.0.1:

| | FLASH | DRAM |
|---|---|---|
| carrier, no radio | 368 kB / 8 MB | 165 kB / 390 kB (41%) |
| carrier + Wi-Fi | 742 kB / 8 MB | 261 kB / 390 kB (65%) |
| dev kit, no radio | 717 kB / 16 MB | 228 kB / 390 kB (57%) |
| dev kit + Wi-Fi | 1091 kB / 16 MB | 322 kB / 390 kB (81%) |

The Wi-Fi variants need the `tf-psa-crypto` module — see the header of
`wifi.conf`.

## Layout

```
CMakeLists.txt      build; reads VERSION.txt into APP_VERSION_STR
prj.conf            shared config
wifi.conf           optional: radio + WebSocket (needs tf-psa-crypto)
boards/             one .overlay + one .conf per board target
dts/bindings/       three custom bindings: discrete IO, thermal zone, stepper
sections-rom.ld     linker section for the HTTP server's resource table
tools/gen_model.py  params.yaml -> src/model_gen.h

src/
  main.c            boot order + the 1 Hz tick + shell commands
  types.h           the shared data types (struct meas, snapshot, faults)
  model_gen.h       GENERATED: plant, gains, gates
  control-system.*  pure C, no Zephyr: meter, PIs, gates, observer
  temp-sense.*      TMP117 chamber temperatures, grouped into zones
  power-sense.*     INA226 + INA3221 rail metering, over-current arm
  water-loop.*      pump (incl. TMC5160 SPI config), RTD dT pair, flow meter
  fans.*            three PWM fan groups + tachometers
  heaters.*         two heaters, driven in WATTS via an inner power loop
  calibration.*     isothermal soak / dT tare / substitution + NVS
  system.*          supervisor state machine, safety thread, POST, faults
  ui.*              LVGL panel: RUN / CTRL / NET
  iot.*             Wi-Fi, HTTP dashboard, WebSocket push
  webapp/index.html the dashboard, gzipped into flash at build time
```

## Orienting in this code

`graphify-out/` holds a knowledge graph of the whole application — 307 nodes and
716 edges across 11 communities, built from the AST plus a hand-written semantic
layer that captures the things an AST cannot see: **why** the guard integrator is
the accuracy, why heaters are driven in watts, what boundary B1 means for
metering, and which overlay owns which pin.

```
graphify-out/graph.html        open in a browser — pan/zoom, coloured by community
graphify-out/GRAPH_REPORT.md   god nodes, cross-community bridges, audit trail
graphify-out/graph.json        the raw graph (GraphRAG-ready)
```

Ask it questions instead of grepping:

```sh
graphify query "how does a measurement point get logged"
graphify path "The heater interlock" "safety_thread()"
graphify explain "Boundary B1 - the inner chamber"
graphify <this dir> --update      # after editing sources
```

It is inert as far as the build is concerned — Zephyr's source list is explicit
in `CMakeLists.txt` and its auto-discovery only looks at `boards/`, `socs/`,
`dts/`, `snippets/`, `prj.conf` and `app.overlay`. Verified: both Wi-Fi builds
were rebuilt from scratch with `graphify-out/` present and produced byte-identical
sizes, with no reference to it in `CMakeCache.txt`, `.config` or `zephyr.dts`.

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

- **The carrier board has no display.** Its pin budget spends all 23 usable
  GPIOs, so the LVGL panel exists only on the dev-kit build. On the carrier
  board the operator interface is the web dashboard plus the UART console, and
  the BOOT button is bound to STOP. Freeing pins for a panel means taking one
  of Design.md's four listed levers.
- **GPIO8 is claimed for the INA226 ALERT line**, which Design.md leaves as the
  one spare. If the schematic uses it for something else, delete `alert-gpios`
  from `cal_io` — the firmware falls back to a software over-current check one
  tick slower, and says so at boot.
- Two MAX31865 converters, not one muxed across both probes. Their reference
  resistors do not cancel in the difference; the per-range in-situ tare absorbs
  the mismatch until the board adds a 4-wire mux.
- The TMC5160 run current defaults to a deliberately conservative **1 A RMS**,
  because the pump vendor has not published the motor's phase current. Raise
  `run-current-ma` in the overlay once that number exists.
- The board temperature sensor's part is **not confirmed** (`Components.md` is
  empty); it is declared as a TMP11x at 0x48. The firmware treats it as
  optional, so a different part is a one-line `compatible` change.
- The disturbance observer is the reduced-order energy-balance form, and it
  runs in **shadow mode** — logged, never gating.
- No TLS. LAN only, token on the control endpoint. Adding TLS is a `prj.conf`
  change plus a certificate, not a code change.
