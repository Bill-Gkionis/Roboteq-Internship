# Waveshare ESP32-S3-LCD-1.47B Zephyr Board Port — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A complete, hardware-safe, upstream-quality Zephyr hardware-model-v2 board
definition for the Waveshare ESP32-S3-LCD-1.47B, delivered as an external
`BOARD_ROOT` tree at `~/roboteq/zephyr/waveshare-esp32-s3-1.47B/`.

**Architecture:** New board `esp32s3_lcd_1_47b` (vendor `waveshare`), two build
targets `…/esp32s3/procpu` and `…/esp32s3/appcpu`, modeled 1:1 on the upstream
`boards/waveshare/esp32s3_geek` structure (same vendor, same SoC family, nearly the
same peripheral set), with peripheral nodes cross-checked against
`esp32s3_touch_lcd_1_28` (LEDC backlight), `esp32s3_matrix` (WS2812-over-SPI,
USB-Serial/JTAG console), and `esp32s3_rlcd_4_2` (octal PSRAM defaults, battery
divider + ADC channel).

**Tech Stack:** Zephyr `main` @ `54cdacb16600a90a4e4b29f4538bd090666f5662`
(2026-08-26, sparse clone in session scratchpad). No SDK locally — validation is
offline devicetree compilation via C preprocessor + `edtlib`
(`scripts/dts/python-devicetree`) using `~/roboteq/venv` python.

**Spec:** `~/roboteq/zephyr/HANDOFF.md` (+ scope set in-conversation: full board
directory destined for upstream, not an app overlay).

## Global Constraints

- Hardware safety first: every pin assignment must be triple-source verified
  (see pin map below) before appearing in a `.dts`/`.dtsi` file.
- GPIO33–37 must never be referenced (in-package octal PSRAM lines; unconnected on
  the schematic).
- GPIO19/20 must never be pinmuxed (native USB D−/D+; dedicated pads).
- Upstream conventions exactly: hwmv2 layout, SPDX `Apache-2.0`,
  `SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors` (style of the
  newest Waveshare board, `esp32s3_rlcd_4_2`).
- No efuse-related anything; nothing in this deliverable touches the device.

## Verified pin map (three independent sources agree)

Sources: **(A)** Waveshare demo firmware headers
(`~/Downloads/ESP32-S3-LCD-1/ESP-IDF/ESP32-S3-LCD-1.47B-Test/main/*/*.h`, read
directly), **(B)** official schematic
(`~/Downloads/ESP32-S3-LCD-1.47B_schematic_diagram.pdf`, net table + component
sheets), **(C)** live `esptool` chip identification from the prior session
(HANDOFF.md). No disagreements found.

| GPIO | Function | DT consumer | Direction / drives | Boot-strap / safety notes |
|---|---|---|---|---|
| 0 | BOOT button (Key2) | `gpio-keys` button0 | input, ext. 10K PU (R10) | strap: boot mode; input-only use — safe |
| 1 | VBAT sense via 200K/100K 1% divider (R1/R4), VBAT/3 | `&adc0` ch0 + `voltage-divider` | analog input | — |
| 12 | QMI8658 INT2 | `qmi8658a` `int-gpios` | input (IMU drives) | driver DRDY default is INT2 (`int-pin=2`) |
| 13 | QMI8658 INT1 | (documented only) | input, untouched | left high-Z; note in docs |
| 14 | SDIO_SCK → TF CLK, 10K PU (R26) | `sdhc0` pinctrl CLKOUT | output → card input | — |
| 15 | SDIO_CMD → TF CMD, 10K PU (R25) | `sdhc0` pinctrl CMD | bidir, pulled up | — |
| 16 | SDIO_D0 → TF D0, 10K PU (R27) | `sdhc0` pinctrl DATA0 | bidir, pulled up | — |
| 17 | SDIO_D2 → TF D2, 10K PU (R23, via 0R R22) | `sdhc0` pinctrl DATA2 | bidir, pulled up | — |
| 18 | SDIO_D1 → TF D1, 10K PU (R28, via 0R R29) | `sdhc0` pinctrl DATA1 | bidir, pulled up | — |
| 19 | USB D− (via 22R R19) | `usb_serial` / `usb_otg` peripheral | dedicated USB pad | never pinmux |
| 20 | USB D+ (via 22R R20) | `usb_serial` / `usb_otg` peripheral | dedicated USB pad | never pinmux |
| 21 | SDIO_D3 → TF CD/D3, 10K PU (R24) | `sdhc0` pinctrl DATA3 | bidir, pulled up | — |
| 33–37 | (octal PSRAM, in-package; unconnected on schematic) | — | — | **must not appear anywhere** |
| 38 | RGB_IO → WS2812B-0807 DI (10K PU R21) | `&spi2` SPIM2 MOSI, `ws2812-spi` | output → LED input | idle-low required (`line-idle-low`, `output-low`) |
| 39 | LCD_RST → LCD RES | `mipi_dbi` `reset-gpios` ACTIVE_LOW | output → LCD input | JTAG MTCK pin; USB-JTAG unaffected |
| 40 | LCD_CLK → LCD SCL | SPIM3 SCLK pinctrl | output → LCD input | JTAG MTDO pin; USB-JTAG unaffected |
| 41 | LCD_DC → LCD D/C | `mipi_dbi` `dc-gpios` ACTIVE_HIGH | output → LCD input | JTAG MTDI pin |
| 42 | LCD_CS → LCD CS | SPIM3 CSEL pinctrl (hw CS) | output → LCD input | JTAG MTMS pin |
| 43 | U0TXD → pad (via 499R R15) | `&uart0` TX | output → open pad | — |
| 44 | U0RXD → pad | `&uart0` RX | input, pull-up | — |
| 45 | LCD_DIN → LCD SDA | SPIM3 MOSI pinctrl | output → LCD input (write-only panel) | strap: VDD_SPI voltage — floats low via internal PD at reset, repurposed only after boot. Safe |
| 46 | LCD_BL → 1K (R14) → SI2302 N-FET gate, 10K gate PD (R16); FET switches LEDK low side | `&ledc0` ch0 → `pwm-leds` | output → FET gate | **active HIGH** (demo `BK_LIGHT_ON_LEVEL 1` + low-side N-FET); strap: ROM msgs — ~11K PD keeps it low at reset. Safe, backlight off at boot |
| 47 | IMU_SCL, 10K PU (R17) | `&i2c0` SCL pinctrl (open-drain) | open-drain bus | — |
| 48 | IMU_SDA, 10K PU (R18) | `&i2c0` SDA pinctrl (open-drain) | open-drain bus | — |
| 2–11 | free, broken out on header U2 | — | untouched, high-Z | user pins |

Electrical audit conclusion: every ESP-driven pin lands on a high-impedance input
(LCD logic inputs, FET gate, WS2812 DI, SD card inputs) or a properly pulled
open-drain/bidirectional bus line. No pin in this DT can drive against another
push-pull output. All three strapping pins that this board repurposes (0, 45, 46)
have reset-safe passive states and are only driven post-boot.

## Chip / memory identity

- Bare **ESP32-S3R8**: 8MB in-package **octal** PSRAM → include
  `<espressif/esp32s3/esp32s3_r8.dtsi>`; Kconfig `select SOC_ESP32S3_R8`;
  defconfig `CONFIG_ESP_SPIRAM=y` + `CONFIG_SPIRAM_MODE_OCT=y` (pattern:
  `esp32s3_rlcd_4_2`, same memory).
- **16MB quad flash** (schematic: W25Q128JVSI; live unit: Puya — same size/mode)
  → `&flash0 { reg/ranges 16M }` + `<espressif/partitions_0x0_amp_16M.dtsi>`.
- Native **USB-Serial/JTAG only** (no bridge chip) → `zephyr,console`/`shell-uart`
  = `&usb_serial` (pattern: `esp32s3_matrix`).

## Display node values (from this board's own shipped panel driver `Vernon_ST7789T.c`)

`sitronix,st7789v` @ SPI3, hw CS, 12MHz (demo-proven), `MIPI_DBI_MODE_SPI_4WIRE`,
`write-only`: width 172, height 320, x-offset 34, y-offset 0, mdac 0x00,
colmod ← `pixel-format = PANEL_PIXEL_FORMAT_RGB_565` (driver derives 0x55 = demo),
porch `[0c 0c 00 33 33]`, gctrl 0x75, vcom 0x1a, lcm 0x80, vrhs 0x13 + vdvs 0x20
(driver then emits VDVVRHEN like demo), pwctrl1 `[a4 a1]`,
pvgam `[d0 0d 14 0d 0d 09 38 44 4e 3a 17 18 2f 30]`,
nvgam `[d0 09 0f 08 07 14 37 44 4d 38 15 16 2c 2e]`, inversion ON (omit
`inversion-off`; demo sends 0x21). Deliberate deviation: `ram-param = [00 f0]`
(Zephyr/big-endian byte order, as all upstream st7789v boards) instead of the
demo's `[00 e8]` (IDF little-endian DMA) — display byte order must match the
Zephyr display pipeline, not ESP-IDF's. Omit: gamma (GAMSET), cmd2en, rgb-param —
demo never sends them; driver skips absent properties.

## Task list

### Task 1: Scaffolding
**Files (all under `~/roboteq/zephyr/waveshare-esp32-s3-1.47B/boards/waveshare/esp32s3_lcd_1_47b/`):**
`board.yml`, `Kconfig.esp32s3_lcd_1_47b` (select SOC_ESP32S3_R8 + PROCPU/APPCPU),
`Kconfig` (board heap add: 4096/256), `Kconfig.defconfig` (LVGL depth-16 + swap
under DISPLAY/LVGL guards), `Kconfig.sysbuild` (MCUboot + signature NONE),
`board.cmake` (esp32 + openocd includes), `support/openocd.cfg`
(esp_usb_jtag + esp32s3 target). All content mirrors `esp32s3_geek` with names
swapped.
- [x] Write all seven files
- [x] Cross-check symbol names `BOARD_ESP32S3_LCD_1_47B[_ESP32S3_PROCPU|_ESP32S3_APPCPU]` match hwmv2 auto-generated form

### Task 2: Pinctrl
**File:** `esp32s3_lcd_1_47b-pinctrl.dtsi` — groups exactly as pin map:
`spim3_default` (SCLK40 + CSEL42; MOSI45 output-low), `spim2_ws2812_led`
(MOSI38 output-low), `i2c0_default` (SDA48/SCL47 pull-up open-drain output-high),
`ledc0_default` (CH0 GPIO46 output-enable), `sdhc0_default` (six SDHC0_* pins,
bias-pull-up output-high), `uart0_default` (TX43 output-high, RX44 pull-up).
- [x] Write file; verify each macro exists in `esp32s3-pinctrl.h` (already grepped — all 15 present)

### Task 3: procpu devicetree
**File:** `esp32s3_lcd_1_47b_esp32s3_procpu.dts` — r8 dtsi + amp_16M partitions;
chosen (sram1, usb_serial console/shell, flash0, slot0_partition, display,
bt-hci); aliases (sw0, watchdog0, sdhc0, led-strip, pwm-led0); gpio-keys BOOT;
pwmleds backlight `<&ledc0 0 PWM_HZ(4000)>` (demo LEDC freq); vbatt
voltage-divider (100k/300k, `&adc0 0`); mipi_dbi + st7789v node per table above;
`&i2c0` 400kHz (`I2C_BITRATE_FAST`, demo value) + `qmi8658a@6b`
(int-gpios `&gpio0 12 GPIO_ACTIVE_HIGH`, int-pin 2); `&spi2` + ws2812
(WS2812C timing macros, chain-length 1, GRB color-mapping); `&spi3` LCD bus;
`&ledc0` channel0@0; `&adc0` channel@0 (`ADC_GAIN_1_4`, internal ref, 12-bit);
`&sdhc0` (pinctrl, bus-width 4, 40MHz, `zephyr,sdmmc-disk` child); flash0 16M;
gpio0/gpio1, uart0 (43/44), usb_serial, `zephyr_udc0: &usb_otg`, trng0, wdt0,
wifi, esp32_bt_hci all okay.
- [x] Write file
- [x] Line-by-line pin audit against the pin map table

### Task 4: appcpu devicetree + defconfigs + twister yamls
**Files:** `…_appcpu.dts` (geek appcpu pattern: r8 dtsi, amp_16M, sram1, ipc/shm
chosen, slot0_appcpu_partition, flash0 16M, trng0);
`…_procpu_defconfig` (GPIO/CONSOLE/SERIAL/UART_CONSOLE/CLOCK_CONTROL +
ESP_SPIRAM + SPIRAM_MODE_OCT); `…_appcpu_defconfig` (CLOCK_CONTROL);
`…_procpu.yaml` (supported: adc, display, gpio, i2c, pwm, sdhc, spi, uart),
`…_appcpu.yaml` (geek's ignore_tags list).
- [x] Write all five files

### Task 5: Documentation
**Files:** `doc/index.rst` (geek/rlcd template: overview, hardware list, wiki +
schematic links, espressif common includes, validate-on-hardware notes incl.
backlight-must-be-driven note) and top-level `README.md` (what this is, BOARD_ROOT
usage, `west build -b esp32s3_lcd_1_47b/esp32s3/procpu … -DBOARD_ROOT=…`,
bring-up order, validation checklist, upstreaming path `boards/waveshare/…`).
- [x] Write both files

### Task 6: Offline validation (the "test" for this plan)
- [x] Preprocess procpu + appcpu dts with `cpp` using the include roots
  `dts/xtensa`, `dts/vendor`, `dts/common`, `dts`, `include` of the cloned tree +
  the board dir
- [x] Run `edtlib.EDT(..., bindings_dirs=[zephyr/dts/bindings])` from
  `scripts/dts/python-devicetree` via `~/roboteq/venv/bin/python`
- [x] Zero errors required; triage any warnings; fix and re-run until clean
- [x] Kconfig sanity: grep selected symbols exist in the tree

### Task 7: Review
- [x] Fresh-eyes self-review against this plan's pin map (every GPIO number in
  every file re-checked against the table)
- [x] Run requesting-code-review / code-review pass on the new directory
- [x] Report to user with validation transcript + on-hardware checklist

## Known validate-on-hardware items (not resolvable offline — flagged, not guessed)

1. WS2812 color order assumed GRB (WS2812B standard; IDF component default).
   Worst case: red/green swapped. No damage possible.
2. `ram-param [00 f0]` byte-order choice (see above). Worst case: swapped colors.
   No damage possible.
3. QMI8658 silkscreen/part revision: driver is for QMI8658**A**; if the fitted die
   is an old QMI8658C revision the WHO_AM_I check may fail cleanly (sensor init
   error, nothing else). Register maps are compatible.
4. SDHC on ESP32-S3 is new upstream (geek uses it); first SD mount should be
   tested with a scratch card.
5. IMU INT polarity assumed ACTIVE_HIGH per upstream binding example; if DRDY
   never fires, try ACTIVE_LOW. Input-only either way — no damage possible.
6. PSRAM left at upstream default speed (rlcd pattern); demo used 80MHz — can be
   raised later via `CONFIG_SPIRAM_SPEED_80M=y` after basic bring-up.

## Out of scope

- QMI8658 INT1 (IO13): described in docs only; Zephyr driver uses one INT pin.
- Fuel-gauge composite node (board ships without a battery — divider node is
  enough; doc shows how to add a fuel gauge in an app overlay).
- LP core target, MCUboot signing, LVGL app glue, SD-over-SPI fallback (only
  needed if native SDHC misbehaves; 0R links R22/R29 + pin table documented).
