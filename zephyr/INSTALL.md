# Minimal Zephyr install for this Mac (ESP32-S3-LCD-1.47B + spare ESP32)

Written 2026-08-26 against Zephyr `main` @ `54cdacb16600` (the revision the
1.47B board files were validated against). Recommended SDK for that revision:
**Zephyr SDK 1.0.1** (read from `zephyr/SDK_VERSION` — note it's the new 1.x
line, not the 0.16/0.17 you used on WSL).

Nothing below runs automatically — every step is yours to paste. Steps are
ordered; sizes are approximate.

## What you get (and what you deliberately don't)

| Piece | You install | You skip (bloat avoided) |
|---|---|---|
| SDK | minimal bundle + **only** the ESP32-S3 toolchain (+1 more after we identify the AliExpress chip) + macOS host tools — ~0.3–0.5 GB | the full SDK bundle with ~20 toolchains (many GB) |
| Modules | `hal_espressif`, `mbedtls`, `lvgl`, `mcuboot` | ~40 other HALs/libs (`hal_nordic`, `hal_stm32`, `hal_nxp`, `hostap`, …). Verified today: ESP32 Wi-Fi does **not** use `hostap` — it has its own supplicant in the Espressif blobs, and it already `select`s MBEDTLS |
| Python | `west packages pip --install` → base requirements + the 4 active modules' requirements only (~20 small wheels) | `pip install -r scripts/requirements.txt` — that file pulls build-test + run-test + extras + compliance sets; this is what melted your WSL installs |
| Host tools | `brew install cmake ninja` (+ optional `dtc`) | qemu, doc toolchain, etc. |
| Python env | your existing `~/roboteq/venv` (Python 3.14, west 1.5.0, esptool 5.3.1 — all current) | any new venv/pyenv |

Feature coverage check for the internship: **I2C** and **PWM/LEDC** drivers are
in-tree (come with `hal_espressif`); **LVGL** is the `lvgl` module
(`zephyr/samples/subsys/display/lvgl`, demos in `zephyr/samples/modules/lvgl`);
**HTTPS server** is the in-tree HTTP server + TLS from the `mbedtls` module
(`zephyr/samples/net/sockets/http_server`), running over ESP32 Wi-Fi, which
additionally needs the Espressif blobs (step 7).

## Resulting layout

```
~/roboteq/
├── venv/                    # existing — the only Python env used
├── zephyrproject/           # new west workspace
│   ├── .west/
│   ├── zephyr/              # the RTOS  (shallow ~0.3 GB; full history ~2 GB)
│   ├── modules/hal/espressif/
│   ├── modules/crypto/mbedtls/
│   ├── modules/lib/gui/lvgl/
│   └── bootloader/mcuboot/
├── zephyr-sdk-1.0.1/        # toolchain(s) + host tools
└── zephyr/                  # your apps + the 1.47B board dir (unchanged)
```

## The commands

### 0. Host tools (one-time)

```sh
brew install cmake ninja
brew install dtc            # optional: extra devicetree lint warnings; tiny
```

(`gperf` already exists at /usr/bin; git and clang come with CLT; ccache is a
nice-to-have later.)

### 1. Workspace init — shallow clone of zephyr

```sh
source ~/roboteq/venv/bin/activate

mkdir -p ~/roboteq/zephyrproject && cd ~/roboteq/zephyrproject
git clone --depth 1 https://github.com/zephyrproject-rtos/zephyr.git zephyr
west init -l zephyr
```

`--depth 1` keeps zephyr ~0.3 GB instead of ~2 GB. Trade-off: no history for
`git log`/rebase. When it's time to send the 1.47B board upstream, un-shallow
with `git -C zephyr fetch --unshallow` (or work from your GitHub fork then).
If you'd rather have full history now, replace the two lines with
`west init ~/roboteq/zephyrproject`.

To reproduce exactly what the board files were validated against, add
`git -C zephyr fetch --depth 1 origin 54cdacb16600a90a4e4b29f4538bd090666f5662
&& git -C zephyr checkout FETCH_HEAD` before `west init -l zephyr`.

### 2. Scope the modules (the anti-bloat step)

```sh
west config manifest.project-filter -- "-.*,+hal_espressif,+mbedtls,+lvgl,+mcuboot"
```

`-.*` deactivates every optional project, then the `+` entries re-activate the
four you need. `west update` and `west packages` both respect this. `west list`
shows what's active.

### 3. Fetch the modules — shallow too

```sh
west update --narrow -o=--depth=1
```

`hal_espressif` is the big one (ESP-IDF-derived); shallow keeps the whole
module set well under ~1 GB.

### 4. Register the CMake package (one-time, tiny)

```sh
west zephyr-export
```

### 5. Python dependencies — preview first, then install

```sh
west packages pip --install -- --dry-run   # shows exactly what pip WOULD do
west packages pip --install                # does it (minutes, not days)
```

**Why your WSL installs took 2 days:** you were on
`pip install -r scripts/requirements.txt`, which chains five requirement sets
(build-test, run-test, extras, compliance…), and old pip's backtracking
resolver ground through them — compiling packages from source where wheels were
missing. `west packages pip` instead assembles ONE pip command from
`requirements-base.txt` plus only the *active* modules' requirements — small,
all prebuilt wheels on Apple Silicon. Your "install on error" instinct was a
reasonable workaround; this is the supported version of it.

If pip is ever slow again anywhere: `python -m pip install -U pip` first, add
`--only-binary :all:` to refuse source builds (fails fast instead of compiling
for hours), and `--dry-run` to see the plan before committing.

### 6. SDK — minimal bundle + only the toolchains you use

```sh
cd ~/roboteq/zephyrproject
west sdk install -b ~/roboteq \
  -t xtensa-espressif_esp32s3_zephyr-elf xtensa-espressif_esp32_zephyr-elf
```

> **One `-t`, all names after it.** `-t` does **not** accumulate when
> repeated: zephyr's `scripts/west_commands/sdk.py` declares it `nargs='+'`
> with argparse's default store action, so a second `-t` silently replaces the
> first (verified empirically 2026-08-26). An earlier revision of this file
> used two `-t` flags — that form installs only the *last* toolchain listed.

Two toolchains, because you have two chips: the S3 one for the 1.47B board, the
plain `esp32` one for the AliExpress devkit (identified below as a classic
ESP32 — different Xtensa core generation, so it needs its own toolchain).

This reads `SDK_VERSION` (1.0.1) from the tree, downloads
`zephyr-sdk-1.0.1_macos-aarch64_minimal.tar.xz` and the toolchains (1.x-line
assets are named `toolchain_gnu_macos-aarch64_<name>.tar.xz`; both verified
present in the v1.0.1 release, which is also the latest as of 2026-08-26),
installs to `~/roboteq/zephyr-sdk-1.0.1`, and registers it — no env vars
needed. Re-run any time with a single `-t` listing more names; toolchains
already on disk are skipped (`setup.sh` checks for the directory). On macOS
the host tools (openocd 9M, wget, qemu 645M) ship *inside* the minimal bundle,
so `-H` changes nothing here — `setup.sh`'s separate hosttools step is a
macOS no-op. If the qemu bulk bothers you it's safe to
`rm -rf ~/roboteq/zephyr-sdk-1.0.1/hosttools/opt/qemu{,-arc}` — Zephyr's qemu
targets don't cover ESP32 anyway — but keep `opt/wget` (setup.sh uses it for
future toolchain downloads) and `opt/openocd` (S3 USB-JTAG debugging).

**Status 2026-08-26 ~15:05:** minimal bundle + host tools + the **S3
toolchain are already installed and verified** (`…esp32s3_zephyr-elf-gcc
--version` → GCC 14.3.0 runs; CMake package registered in
`~/.cmake/packages/Zephyr-sdk`). The only piece missing is the plain-esp32
toolchain for the AliExpress devkit:

```sh
west sdk install -b ~/roboteq -t xtensa-espressif_esp32_zephyr-elf
```

### 7. Espressif blobs (required for Wi-Fi/BLE → your HTTPS server)

```sh
west blobs fetch hal_espressif
```

Proprietary Espressif radio libraries. Without them, `CONFIG_WIFI=y` builds
fail by design. Fetch once; sizeable but unavoidable for Wi-Fi work.

### 8. Smoke-test builds (nothing is flashed yet)

```sh
cd ~/roboteq/zephyrproject
west build -p auto -b esp32s3_lcd_1_47b/esp32s3/procpu zephyr/samples/hello_world -- \
  -DBOARD_ROOT=$HOME/roboteq/zephyr/waveshare-esp32-s3-1.47B
```

Then, with the 1.47B plugged in (it enumerates as `/dev/cu.usbmodem*` — native
USB, no driver needed):

```sh
west flash            # esptool auto-detects the port
west espressif monitor
```

If flashing ever can't sync: hold BOOT, tap RESET, release BOOT (ROM download
mode — always recoverable). Follow the bring-up ladder in the board README
(hello_world → button → LED → backlight → IMU → display → SD).

Your apps in `~/roboteq/zephyr/` build the same way — point west at the app dir:
`west build -p auto -b esp32s3_lcd_1_47b/esp32s3/procpu ~/roboteq/zephyr/test -- -DBOARD_ROOT=$HOME/roboteq/zephyr/waveshare-esp32-s3-1.47B`
(run from inside `~/roboteq/zephyrproject`). Plain builds use Espressif
"simple boot"; add `--sysbuild` when you want MCUboot + OTA-style slots.

## The AliExpress board — identified 2026-08-26

Read-only `esptool` on `/dev/cu.usbserial-110` (your `chip-id`, my `flash-id`):

```
Chip type:  ESP32-D0WD-V3 (revision v3.1)   Crystal: 40MHz   MAC: fc:e8:c0:7e:56:cc
Flash:      4MB, manufacturer 0x68 (Boya), device 0x4016, 3.3V
```

Classic dual-core ESP32, **silicon revision 3.1 — the final ECO3 stepping**,
which is the fully supported one in Zephyr (the `ESP32_USE_UNSUPPORTED_REVISION`
escape hatch exists only for Rev 0/1; you don't need it). No board files to
write — use the upstream target:

```sh
west build -p auto -b esp32_devkitc/esp32/procpu zephyr/samples/hello_world
west flash --esp-device /dev/cu.usbserial-110
```

The `--esp-device` flag is worth being explicit about here: unlike the 1.47B,
this board has no native USB, so it talks through a USB-UART bridge, and with
both boards plugged in autodetection can pick the wrong one. Console is UART0
at 115200 on the same port (`west espressif monitor`).

Fit check against `esp32_devkitc` upstream: it declares **4MB flash — matches
your chip exactly**. It also selects the WROVER-E variant, which declares 8MB
PSRAM, but I checked and no board file enables `CONFIG_ESP_SPIRAM`, so PSRAM
stays off unless you ask for it. That means the target works whether your
module is a WROOM (no PSRAM) or a WROVER (has it) — check the metal can's
label. If it says WROVER and you want the extra RAM, add `CONFIG_ESP_SPIRAM=y`.
If it says WROOM and you ever need GPIO16/17, note the SoC dtsi reserves them
(they're PSRAM lines on WROVER, free on WROOM) — override with
`&gpio0 { gpio-reserved-ranges = <6 6>, <20 1>, <24 1>, <28 4>; };`.

Enabled out of the box: UART0/1/2, GPIO, **I2C0 on GPIO21 (SDA) / GPIO22
(SCL)**, SPI2/3, I2S, timers, touch, TRNG, Wi-Fi, BLE.

### PWM needs one overlay line on this board

The ESP32 LEDC node is `status = "disabled"` in the SoC devicetree and
`esp32_devkitc` doesn't enable it (the 1.47B board does — that's why the
backlight works there). For your PWM work on this devkit, add an
`app.overlay` next to your `prj.conf`:

```dts
#include <zephyr/dt-bindings/pinctrl/esp32-pinctrl.h>

&pinctrl {
	ledc0_default: ledc0_default {
		group1 {
			pinmux = <LEDC_CH0_GPIO2>;   /* pick any free GPIO */
			output-enable;
		};
	};
};

&ledc0 {
	status = "okay";
	pinctrl-0 = <&ledc0_default>;
	pinctrl-names = "default";
	#address-cells = <1>;
	#size-cells = <0>;

	channel0@0 {
		reg = <0x0>;
		timer = <0>;
	};
};
```

Then `CONFIG_PWM=y` in `prj.conf`. Channel macros for other pins follow the
same pattern (`LEDC_CH<n>_GPIO<pin>`); the header lists 480 of them, so almost
any GPIO works.

### If you plug in yet another ESP-family board later

| Chip reported | Zephyr board target | SDK toolchain (`-t`) |
|---|---|---|
| ESP32-S2 | `esp32s2_devkitc` | `xtensa-espressif_esp32s2_zephyr-elf` |
| ESP32-C3 | `esp32c3_devkitm` / `esp32c3_devkitc` | `riscv64-zephyr-elf` |
| ESP32-C6 | `esp32c6_devkitc` | `riscv64-zephyr-elf` |

Identify the same read-only way: `esptool --port /dev/cu.<port> chip-id`.

## When something's missing later (the 3 recipes)

- **CMake: unknown board / module, or a driver Kconfig you expected doesn't
  exist** → a module is filtered out. `west list` to check, then append `,+name`
  to the `manifest.project-filter` value (step 2), `west update name`, rebuild.
- **`ModuleNotFoundError` / pip package missing during build** → a newly
  activated module brought new requirements: `west packages pip --install`.
- **Linker errors about `libphy`/net80211, or a "blobs" warning with Wi-Fi
  enabled** → `west blobs fetch hal_espressif`.

Upgrading later: `git -C zephyr fetch --depth 1 origin main && git -C zephyr
checkout FETCH_HEAD`, then `west update --narrow -o=--depth=1`,
`west packages pip --install`, and re-check `cat zephyr/SDK_VERSION` — if it
moved, `west sdk install` again with your `-t` list.
