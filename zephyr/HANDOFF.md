# Handoff: Zephyr devicetree for Waveshare ESP32-S3-LCD-1.47B

This doc was written by a prior Claude Code session after identifying the board from
photos, cross-referencing Waveshare's own shipped firmware source, and running live
read-only `esptool` queries against the actual connected unit. Treat the facts below
as verified, not guessed — sources/method are noted so you can re-check anything.
The user is deliberately risk-averse about hardware damage; match that with precision
(cite/verify claims, flag real unknowns as unknowns) rather than generic reassurance.

*Updated after the user restructured `zephyr/` and downloaded the schematic — see
"Project location" and "Schematic" below for what changed.*

## Task

Write a Zephyr devicetree overlay enabling this board's peripherals. No upstream
Zephyr board definition exists for this specific Waveshare board (checked, nothing
found as of Aug 2026) — this is a from-scratch board-port job, starting from the
generic ESP32-S3 SoC support.

**Before writing anything, confirm scope with the user** — see "Suggested phased
approach" below. Don't assume full scope (display+IMU+SD+LED all at once).

## Project location

`~/roboteq/zephyr/` now contains two app directories (restructured by the user after
this doc was first written):

- **`~/roboteq/zephyr/calorimetry/`** — the original app skeleton, moved here. Still
  all empty placeholders (`CMakelists.txt`, `prj.conf`, `app.overlay`, `VERSION.txt`,
  `src/main.c` — all 0 bytes, nothing to reconcile). The name suggests this board's
  actual intended job is tied to the user's other project under `~/roboteq/python/`
  (thermal/calorimetry step-response and control simulation — `calorimetry_sim.py`,
  `params.yaml`, response/windup plots). Worth asking the user what the calorimetry
  app should actually do with this board before picking which peripherals matter most
  — that may reorder the phased approach below more usefully than driver-maturity
  order alone.
- **`~/roboteq/zephyr/test/`** — new, currently empty. No stated purpose yet, but it's
  a natural fit for the "bare bring-up" phase (step 1 below) kept separate from the
  real `calorimetry` app — worth proposing to the user rather than assuming.

Target file for the devicetree work is most likely
**`~/roboteq/zephyr/calorimetry/app.overlay`** (or `zephyr/test/app.overlay` if a
separate bring-up app gets set up there) — confirm which with the user.

**Known issue, still present after the restructure**: `zephyr/calorimetry/CMakelists.txt`
should be `CMakeLists.txt` (capital L). Zephyr's build system expects that exact
filename; this only "works" today because macOS's default filesystem (APFS) is
case-insensitive. Will break on Linux/CI. Flag it to the user rather than silently
renaming.

Python venv with `esptool` + `west` installed and confirmed conflict-free:
`~/roboteq/venv` (gitignored — see below).

## Hardware identity — verified live via esptool, this exact unit

```
Chip type:          ESP32-S3 (QFN56), revision v0.2
Features:           Wi-Fi, BT 5 (LE), Dual Core + LP Core, 240MHz, Embedded PSRAM 8MB
Crystal frequency:  40MHz
USB mode:           USB-Serial/JTAG (native USB, no bridge chip)
MAC:                e8:f6:0a:92:5c:b8
Flash:              16MB, manufacturer 0x85 (Puya), quad SPI (4 data lines), 3.3V
Security:           Secure Boot disabled, Flash Encryption disabled, all eFuse key
                     blocks empty, SPI_BOOT_CRYPT_CNT=0 — factory default, nothing burned
```

Chip is bare ESP32-S3R8 (embedded PSRAM, no WROOM module can visible on the board).
Zephyr base target: `esp32s3_devkitc` board family, built with
`-S psram-8M -S flash-16M` snippets — matches this exact configuration.

## Confirmed pin map

Source: Waveshare's own shipped ESP-IDF demo firmware headers (primary source — read
directly, not OCR'd off a schematic). Local paths below if you want to re-verify
anything yourself before trusting it further.

| Function | Pin(s) | Notes |
|---|---|---|
| LCD SCLK / MOSI | GPIO40 / GPIO45 | `SPI3_HOST` (hw SPI3); MISO unused (display is write-only) |
| LCD DC / RST / CS | GPIO41 / GPIO39 / GPIO42 | same SPI bus |
| LCD backlight | GPIO46 | PWM via LEDC |
| LCD panel | 172×320, ST7789 | controller offset x=34, y=0 (glass is narrower than controller RAM) |
| Touch | none | this is the base/"B" variant — no touch controller (`TOUCH_CS = -1` in source) |
| IMU (QMI8658) SCL / SDA | GPIO47 / GPIO48 | I2C0 @ 400kHz, addr 0x6A or 0x6B (SA0-dependent) |
| SD/TF: CLK/CMD/D0/D1/D2/D3 | 14/15/16/18/17/21 | **SDMMC 4-bit host peripheral, NOT SPI** — see caveat below |
| RGB LED (addressable) | GPIO38 | WS2812-style, driven via RMT (`espressif/led_strip` on the IDF side) |
| Battery voltage sense | ADC1 channel 0 (=GPIO1) | |
| User/BOOT button | GPIO0 | shared with the boot-strap pin |

Local reference paths (same machine):
- `~/Downloads/ESP32-S3-LCD-1/ESP-IDF/ESP32-S3-LCD-1.47B-Test/main/LCD_Driver/ST7789.h`
- `~/Downloads/ESP32-S3-LCD-1/ESP-IDF/ESP32-S3-LCD-1.47B-Test/main/I2C_Driver/I2C_Driver.h`
- `~/Downloads/ESP32-S3-LCD-1/ESP-IDF/ESP32-S3-LCD-1.47B-Test/main/SD_Card/SD_MMC.h`
- `~/Downloads/ESP32-S3-LCD-1/ESP-IDF/ESP32-S3-LCD-1.47B-Test/main/RGB/RGB.h`
- `~/Downloads/ESP32-S3-LCD-1/ESP-IDF/ESP32-S3-LCD-1.47B-Test/main/BAT_Driver/BAT_Driver.h`
- `~/Downloads/ESP32-S3-LCD-1/ESP-IDF/ESP32-S3-LCD-1.47B-Test/main/Button_Driver/Button_Driver.h`
- Waveshare wiki: https://www.waveshare.com/wiki/ESP32-S3-LCD-1.47B

**Schematic**: now downloaded —
`~/Downloads/ESP32-S3-LCD-1.47B_schematic_diagram.pdf`. Use it to cross-check the pin
table above (they should agree; the table was read from firmware source, not from
this schematic, so it's an independent check, not a duplicate source).

**Bonus reference**: the official Devicetree Specification is also sitting locally at
`~/Downloads/devicetree-specification-v0.4.pdf` — useful for DTS syntax questions
that aren't Zephyr-specific (node/property grammar, phandles, etc.).

## Zephyr-side landscape (researched, re-verify anything load-bearing)

- No upstream board file for this Waveshare model — you're writing an overlay on top
  of the generic ESP32-S3 SoC target, not extending an existing board.
- ST7789 display: Zephyr has a mainline driver (`sitronix,st7789v`, via the MIPI-DBI
  SPI abstraction). Should map directly onto the SPI pins above.
- QMI8658 IMU: no mainline Zephyr sensor driver found as of last check — likely needs
  to be written/ported. Confirm current status before assuming it exists.
- SD card: **the SDMMC 4-bit peripheral had no ESP32-S3 support in Zephyr's
  `drivers/sdhc/sdhc_esp32.c`** as of GitHub issue #75535 (opened July 2024, missing
  `hal/sdmmc_ll.h` — driver only covered original ESP32). That issue is now closed
  (checked Aug 2026) but the resolution wasn't confirmed in detail — check against
  whatever exact Zephyr revision gets checked out before relying on it. Documented
  fallback if still unsupported: SPI-mode SD access over the same physical pins
  (CMD→MOSI, D0→MISO, CLK→SCLK, D3→CS).
- RGB LED: Zephyr has mainline WS2812-over-RMT support on ESP32 — should be usable
  directly.
- ADC / button: both trivial, mainline (ADC1 channel driver, standard `gpio-keys`).
- LVGL (for later, once past the devicetree stage): has an official Zephyr OS
  integration already — seen in Waveshare's vendored LVGL copy at
  `.../components/lvgl__lvgl/docs/get-started/os/zephyr.md`. Not urgent now.

## Safety ground rules (established over prior conversation — carry forward)

- Routine reflashing (`esptool` / `west flash`) is always recoverable. The ROM
  bootloader is mask ROM, not erasable — BOOT+RST re-enters download mode from any
  flash state. Not a source of permanent damage.
- Devicetree/overlay authoring is pure text, zero hardware exposure by itself. Risk
  only appears once code built from it actually runs on the chip.
- The one genuinely irreversible action is burning eFuses (`espefuse.py
  burn_efuse`/`burn_key`, disabling JTAG/USB-download, enabling secure
  boot/flash encryption). Verified live above: nothing burned, all factory-default.
  Nothing in this task should go near `espefuse.py`.
- The one real remaining caution is electrical: app code driving a pin in a way that
  conflicts with fixed hardware wiring. Mitigated by cross-referencing the pin table
  above (sourced from Waveshare's shipped firmware, not invented) rather than guessing
  — and now also against the schematic PDF, since both are available.
- **Don't run anything that writes to the device (flash/erase) without the user's
  explicit go-ahead in the moment.** This handoff is scoped to producing overlay text,
  not to touching the hardware.

## Suggested phased approach (recommendation, not mandate — confirm with user first)

1. Bare bring-up: get `west build`/`west flash` working end-to-end with something
   trivial (console output, or reading the GPIO0 button) — proves the toolchain/flash
   loop before adding peripheral complexity. `zephyr/test/` may be meant for exactly
   this — ask.
2. Layer in roughly by Zephyr driver maturity: button → RGB LED (WS2812/RMT) →
   battery ADC → I2C bus + IMU → SPI display → SD card (SDMMC, most uncertain, save
   for last) — though if the user explains what the "calorimetry" app actually needs
   to do, prioritize by that instead.
