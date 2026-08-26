# Zephyr board support: Waveshare ESP32-S3-LCD-1.47B

Out-of-tree Zephyr board definition for the
[Waveshare ESP32-S3-LCD-1.47B](https://www.waveshare.com/wiki/ESP32-S3-LCD-1.47B),
written against Zephyr `main` @ `54cdacb16600` (2026-08-26), hardware model v2.
Not yet validated on hardware — see the checklist below before trusting it.

Board target: **`esp32s3_lcd_1_47b/esp32s3/procpu`** (plus `…/appcpu` for AMP).

Every pin assignment was cross-verified against three independent sources before
being written down: Waveshare's shipped ESP-IDF demo headers, the official
schematic PDF (net-by-net), and a live `esptool` chip identification of the
actual unit. The full audit trail (per-pin direction/strapping analysis) lives in
`~/roboteq/zephyr/docs/plans/2026-08-26-waveshare-esp32s3-lcd-1-47b-board.md`.

## What is enabled

| Peripheral | DT node / access | Pins | Notes |
|---|---|---|---|
| Console + shell | `usb_serial` (USB Serial/JTAG) | GPIO19/20 (dedicated) | no UART bridge chip on this board |
| 1.47" LCD 172×320 | `sitronix,st7789v` via `zephyr,mipi-dbi-spi` on `spi3`, `chosen zephyr,display` | SCLK 40, MOSI 45, CS 42 (hw), DC 41, RST 39 | 12 MHz, write-only, init values from vendor panel driver |
| LCD backlight | `pwm-led0` alias → LEDC ch0, 4 kHz | GPIO46 | **active high**, off at reset; apps must turn it on |
| IMU QMI8658 | `qst,qmi8658a` on `i2c0` @ 0x6b (400 kHz) | SDA 48, SCL 47, INT2 → GPIO12 | INT1 → GPIO13 unused |
| microSD | `sdhc0`, SDMMC 4-bit, `zephyr,sdmmc-disk` "SD" | CLK 14, CMD 15, D0 16, D1 18, D2 17, D3 21 | 10k pull-ups on board; 40 MHz cap |
| RGB LED (WS2812B) | `led-strip` alias, `worldsemi,ws2812-spi` on `spi2` | GPIO38 | 7 MHz SPI timing, GRB order |
| Battery voltage | `vbatt` (`voltage-divider`) → ADC1 ch0 | GPIO1 | VBAT/3 (200k/100k); gain 1/4, 12-bit |
| BOOT button | `sw0` alias, `gpio-keys` | GPIO0 | active low, pull-up |
| UART0 pads | `uart0` 115200 | TX 43, RX 44 | rear solder pads |
| Wi-Fi / BLE / TRNG / WDT | enabled | — | standard ESP32-S3 |
| PSRAM | 8MB octal, `CONFIG_ESP_SPIRAM=y` default | in-package (GPIO33–37, never muxed) | speed left at upstream default; raise with `CONFIG_SPIRAM_SPEED_80M=y` later |
| Flash | 16MB, AMP partition layout (`partitions_0x0_amp_16M.dtsi`) | dedicated quad pins | MCUboot-ready via sysbuild |

Deliberately untouched: GPIO2–11 (free header pins), GPIO13 (IMU INT1),
GPIO19/20 (USB), GPIO33–37 (octal PSRAM — must never be assigned).

## Using it (once the Zephyr SDK + workspace are set up)

Two options:

1. **External board root (no tree changes):**

   ```sh
   west build -b esp32s3_lcd_1_47b/esp32s3/procpu samples/hello_world -- \
     -DBOARD_ROOT=$HOME/roboteq/zephyr/waveshare-esp32-s3-1.47B
   ```

2. **In-tree (for the upstream PR):** copy `boards/waveshare/esp32s3_lcd_1_47b/`
   into `zephyr/boards/waveshare/` and build with just
   `-b esp32s3_lcd_1_47b/esp32s3/procpu`.

Suggested bring-up order (each step exercises strictly more hardware):

1. `samples/hello_world` — proves build + flash + USB console. **Do this first.**
2. `samples/basic/button` — GPIO0 BOOT button.
3. `samples/drivers/led_strip` — RGB LED (verify colors: if red/green are
   swapped, flip the first two entries of `color-mapping`).
4. `samples/basic/blinky_pwm` — backlight FET (screen glow changes).
5. `samples/sensor/accel_polling` — IMU over I2C.
6. `samples/drivers/display` — full LCD path (remember the backlight).
7. `samples/subsys/fs/fs_sample` — microSD last (newest driver path on S3), with
   a scratch card.

## Known items to confirm on hardware (offline analysis can't settle these)

- **WS2812 color order** assumed GRB (WS2812B standard). Worst case: swapped
  colors, zero risk.
- **Display byte order**: `ram-param = [00 f0]` (Zephyr convention) instead of
  the vendor's `[00 e8]`. If display colors look bit-mangled (not just swapped),
  try `[00 e8]`. Zero risk either way.
- **IMU part revision**: driver targets QMI8658**A**; an older QMI8658C die would
  fail the WHO_AM_I probe cleanly (log error, nothing else).
- **IMU INT polarity** assumed active-high (upstream binding default). If
  data-ready never triggers in interrupt mode, try `GPIO_ACTIVE_LOW`. Input-only
  pin, zero risk.
- **SDHC on ESP32-S3** is recent upstream (the ESP32-S3-GEEK board uses the same
  path). First mount on a scratch card.

None of these can damage the hardware — every ESP-driven pin lands on a
high-impedance input or a properly pulled bus line, and the three repurposed
strapping pins (0, 45, 46) all have reset-safe passive states (verified on the
schematic: 10k pull-up on GPIO0, ~11k pull-down on GPIO46, GPIO45 floats to its
default strap and is only driven after boot).

## Upstreaming

Target: `zephyr/boards/waveshare/esp32s3_lcd_1_47b/` — the directory is already
in upstream form (naming per `esp32s3_geek`, headers per `esp32s3_rlcd_4_2`,
docs per current board-doc directives). Before opening the PR: run the on-target
validation above, add a board photo `doc/img/esp32s3_lcd_1_47b.webp` (upstream
requires one for the `zephyr:board::` directive), and run
`scripts/ci/check_compliance.py` from the Zephyr tree.
