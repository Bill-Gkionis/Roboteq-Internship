# Display hello-world — first on-hardware run of the 1.47B board port

Date: 2026-08-26. This app was the first thing ever built, flashed, and run
against the out-of-tree board definition in
`~/roboteq/zephyr/waveshare-esp32-s3-1.47B/` (which was **not modified** —
Zephyr pulls it read-only via `-DBOARD_ROOT`). One app exercises four things
at once:

| Subsystem | Path proven |
|---|---|
| Console | `printk` → USB Serial/JTAG (`usb_serial`, the bare USB-C port) |
| Display | ST7789V 172×320 → `zephyr,mipi-dbi-spi` → SPI3 @ 12 MHz |
| UI stack | LVGL 9.6 (label widgets + 1 Hz updates, color depth 16 + byte swap from the board's `Kconfig.defconfig`) |
| Backlight | LEDC PWM ch0 → GPIO46 FET, 100 % duty (**off at reset by design** — the app must switch it on) |
| RGB LED | WS2812B via `worldsemi,ws2812-spi` on SPI2 (GPIO38) — 2 s each of pure R/G/B, then a rainbow sweep |

## The app (3 files)

- `CMakeLists.txt` — standard Zephyr app boilerplate.
- `prj.conf` — a handful of options. `CONFIG_DISPLAY=y` is enough for the
  whole panel chain because each driver defaults on from the devicetree
  (`ST7789V` → selects `MIPI_DBI`; `MIPI_DBI_SPI` → selects `SPI`; LEDC PWM
  driver defaults on once `CONFIG_PWM=y`; `CONFIG_LED_STRIP=y` likewise
  pulls the WS2812-SPI driver).
- `src/main.c` — renders the first frame **before** `display_blanking_off()`
  and only then raises the backlight, so no power-on garbage is ever visible.
  Then loops: `lv_timer_handler()` every 10 ms, counter label + `printk` every
  second.

## Commands (reproduce from scratch)

```sh
source ~/roboteq/venv/bin/activate
cd ~/roboteq/zephyrproject

# build (board files pulled read-only from BOARD_ROOT)
west build -p always -b esp32s3_lcd_1_47b/esp32s3/procpu ~/roboteq/zephyr/test -- \
  -DBOARD_ROOT=$HOME/roboteq/zephyr/waveshare-esp32-s3-1.47B

# flash — pin the port explicitly (matters once the AliExpress devkit is also plugged in)
west flash --esp-device /dev/cu.usbmodem1101

# watch the console (Ctrl+] to quit) …
west espressif monitor -p /dev/cu.usbmodem1101
# … or, dependency-free:  screen /dev/cu.usbmodem1101 115200   (quit: Ctrl-A K)
```

After editing `src/main.c`, just `west build` (no flags needed — the build dir
remembers everything), then `west flash`.

## What the 2026-08-26 run produced

- **Build**: clean first try. Flash 423 KB / 16 MB, DRAM 87 KB / 399 KB.
  No Wi-Fi in this app, so no Espressif blobs were needed.
- **esptool during flash**: `ESP32-S3 (QFN56) rev v0.2, Embedded PSRAM 8MB
  (AP_3v3)` — independently confirms the R8/octal-PSRAM choice in the board
  files. Wrote 423204 bytes in 3.8 s, **hash verified**.
- **Console**:

  ```
  I (esp_psram): Found 8MB PSRAM device
  *** Booting Zephyr OS build 4adba2d7a09e ***
  Hello World! esp32s3_lcd_1_47b/esp32s3/procpu
  display + backlight up, entering UI loop
  alive: 0 s
  alive: 1 s
  …
  ```

  `display + backlight up` means: display device ready, ST7789V init sequence
  accepted over SPI3, first LVGL frame flushed, DISPON sent, PWM at 100 %.

### Harmless boot-log oddity, explained

The ROM prints `SHA-256 comparison failed … Attempting to boot anyway`. That
is *expected* with Zephyr's ESP "Simple Boot" images: the image carries no
appended SHA digest (esptool said so at build time), the ROM checks it anyway,
shrugs, and boots. Not an error. It disappears if you ever build `--sysbuild`
(MCUboot).

## Second run — mirrored text, root cause, and the MADCTL fix

First-run visual result: everything correct **except the text read
right-to-left** — the image was horizontally mirrored.

Root cause: the board dts programs `mdac = <0x00>` into MADCTL (0x36), but
the vendor's own demo programs **0x48 = MX | BGR**
(`ESP32-S3-LCD-1.47B-Test/main/LCD_Driver/ST7789.c`:
`.rgb_endian = LCD_RGB_ENDIAN_BGR` → 0x08, then
`esp_lcd_panel_mirror(panel, true, false)` → 0x40). The panel scans columns
opposite to the ST7789 default *and* has BGR subpixel order.

Why the color squares didn't catch it: the two errors cancel there. The
mirror parks the logical-red square on the right and logical-blue on the
left; the missing BGR bit then displays each with red↔blue swapped — putting
red back on the left. Squares: perfect. Glyphs: mirrored. (White text is
immune to all of it, which is exactly why the squares existed.)

Two supporting facts checked before applying: the `x-offset = 34` is
mirror-invariant because the 172 px glass sits centered in the 240-column
ST7789 RAM (34 columns each side), and `ram-param [00 f0]` is now
hardware-validated (byte-order errors would have mangled hues, which never
appeared).

The fix was first proven via a temporary `app.overlay`
(`&st7789v { mdac = <0x48>; };`) and confirmed visually: text left-to-right,
squares still RED-GREEN-BLUE. It was then **promoted into the board dts**
(`esp32s3_lcd_1_47b_esp32s3_procpu.dts`, `mdac = <0x48>` with an explanatory
comment) and the overlay deleted; `build/zephyr/zephyr.dts` provenance now
shows the value coming from the board file. The upstream PR carries it
automatically.

## Third run — WS2812 rainbow (and why it was "deep yellow" before)

Until this run nothing had ever clocked data into the WS2812, so it showed
random power-on latch state — the mysterious deep yellow. The app now drives
it from the main loop: **two seconds each of pure RED, GREEN, BLUE (each
phase announced on the console) and then an endless rainbow**, one full hue
cycle every 3.6 s, capped at ~24 % brightness.

The R/G/B preamble is the actual test: it validates the
`color-mapping = <GREEN RED BLUE>` order in the dts (a README
known-unknown). If the LED shows green while the console says
`WS2812: RED`, the first two entries of `color-mapping` must be swapped —
cosmetic, zero risk. Console output observed on this run:

```
WS2812: RED      (t = 0 s)
WS2812: GREEN    (t = 2 s)
WS2812: BLUE     (t = 4 s)
WS2812: rainbow  (t = 6 s)
```

with no driver errors — SPI2 (LED) and SPI3 (display) running side by side.

## Visual checklist (the part software can't verify — SPI is write-only)

1. Backlight visibly on.
2. Three squares near the top, left → right: **RED, GREEN, BLUE**
   (unchanged from run 1 — see the cancellation story above).
3. `Hello, World!` + `esp32s3_lcd_1_47b` centered, white on black,
   reading **left-to-right**.
4. Bottom counter ticking once per second, in step with the console lines.
5. WS2812: red for 2 s, green for 2 s, blue for 2 s (matching the console
   announcements), then rainbow.

(Items 1–4 were confirmed by eye on 2026-08-26 — text direction and square
order both correct after the mdac fix.)

## If flashing ever refuses to connect

Hold **BOOT**, tap **RESET**, release **BOOT** → ROM download mode, always
recoverable. Then `west flash --esp-device /dev/cu.usbmodem1101` again.

## Where this leaves the bring-up ladder

Steps 1 (hello_world/console), 3 (WS2812), 4 (backlight PWM), and 6 (display)
of the board README's ladder are covered by this app. Still untested hardware —
all on buses this app never touches: BOOT button (`samples/basic/button`),
IMU over I2C (`samples/sensor/accel_polling` — would also settle the
QMI8658A-vs-C question), microSD over SDMMC (`samples/subsys/fs/fs_sample`,
scratch card first). All are zero-damage-risk per the board README's pin
audit.
