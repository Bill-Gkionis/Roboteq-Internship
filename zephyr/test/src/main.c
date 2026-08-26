/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hello-world smoke test for the Waveshare ESP32-S3-LCD-1.47B board port.
 *
 * Exercises, in one shot: USB Serial/JTAG console (printk), the ST7789V
 * 172x320 panel over MIPI-DBI SPI, LVGL rendering, the LEDC PWM
 * backlight, and the on-board WS2812B RGB LED on SPI2.
 *
 * Expected on the panel: black background, three squares near the top
 * reading RED / GREEN / BLUE left-to-right, "Hello, World!" centered,
 * and a seconds counter at the bottom ticking in lockstep with a 1 Hz
 * console line.
 *
 * Expected on the WS2812: two seconds each of pure red, green, blue
 * (each phase announced on the console -- this validates the dts
 * color-mapping order, not just that the LED lights up), then an
 * endless rainbow sweep, one full cycle every 3.6 s.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/sys/printk.h>
#include <lvgl.h>
#include <stdio.h>

static const struct pwm_dt_spec backlight = PWM_DT_SPEC_GET(DT_ALIAS(pwm_led0));
static const struct device *const strip = DEVICE_DT_GET(DT_ALIAS(led_strip));

/* A WS2812 at 0xff is desk-lamp bright; keep the test easy on the eyes. */
#define STRIP_MAX 60

/*
 * The squares diagnose the ram-param byte-order question flagged in the
 * board README: swapped bytes would mangle red and blue into other hues,
 * while white text alone (0xFFFF) would look fine under any byte order.
 */
static void color_square(lv_color_t color, int32_t x_ofs)
{
	lv_obj_t *sq = lv_obj_create(lv_screen_active());

	lv_obj_set_size(sq, 40, 40);
	lv_obj_align(sq, LV_ALIGN_TOP_MID, x_ofs, 24);
	lv_obj_set_style_radius(sq, 0, LV_PART_MAIN);
	lv_obj_set_style_border_width(sq, 0, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_bg_color(sq, color, LV_PART_MAIN);
}

static struct led_rgb hue_to_rgb(uint16_t hue)
{
	uint8_t sector = hue / 60U;
	uint8_t up = (uint16_t)STRIP_MAX * (hue % 60U) / 60U;
	uint8_t down = STRIP_MAX - up;

	switch (sector) {
	case 0:
		return (struct led_rgb){ .r = STRIP_MAX, .g = up, .b = 0 };
	case 1:
		return (struct led_rgb){ .r = down, .g = STRIP_MAX, .b = 0 };
	case 2:
		return (struct led_rgb){ .r = 0, .g = STRIP_MAX, .b = up };
	case 3:
		return (struct led_rgb){ .r = 0, .g = down, .b = STRIP_MAX };
	case 4:
		return (struct led_rgb){ .r = up, .g = 0, .b = STRIP_MAX };
	default:
		return (struct led_rgb){ .r = STRIP_MAX, .g = 0, .b = down };
	}
}

int main(void)
{
	const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	lv_obj_t *screen;
	lv_obj_t *title;
	lv_obj_t *board;
	lv_obj_t *counter;
	char buf[16];
	uint32_t last_s = UINT32_MAX;
	const char *led_phase = NULL;
	uint16_t hue = 0;
	bool strip_ok;
	int ret;

	printk("Hello World! %s\n", CONFIG_BOARD_TARGET);

	if (!device_is_ready(display)) {
		printk("display %s not ready\n", display->name);
		return 0;
	}
	if (!pwm_is_ready_dt(&backlight)) {
		printk("backlight PWM %s not ready\n", backlight.dev->name);
		return 0;
	}

	/* The LED is a bonus peripheral here: losing it shouldn't take the
	 * display test down with it.
	 */
	strip_ok = device_is_ready(strip);
	if (!strip_ok) {
		printk("WS2812 %s not ready -- continuing without it\n",
		       strip->name);
	}

	screen = lv_screen_active();
	lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
	lv_obj_set_style_text_color(screen, lv_color_white(), LV_PART_MAIN);

	color_square(lv_color_make(0xff, 0x00, 0x00), -50);
	color_square(lv_color_make(0x00, 0xff, 0x00), 0);
	color_square(lv_color_make(0x00, 0x00, 0xff), 50);

	title = lv_label_create(screen);
	lv_label_set_text(title, "Hello, World!");
	lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

	board = lv_label_create(screen);
	lv_label_set_text(board, CONFIG_BOARD);
	lv_obj_align(board, LV_ALIGN_CENTER, 0, 24);

	counter = lv_label_create(screen);
	lv_label_set_text(counter, "0 s");
	lv_obj_align(counter, LV_ALIGN_BOTTOM_MID, 0, -12);

	/* Render the first frame into panel RAM while the panel is still
	 * dark, so neither power-on garbage nor a blank flash is visible.
	 */
	lv_timer_handler();

	ret = display_blanking_off(display);
	if (ret < 0 && ret != -ENOSYS) {
		printk("blanking off failed: %d\n", ret);
		return 0;
	}

	ret = pwm_set_pulse_dt(&backlight, backlight.period);
	if (ret < 0) {
		printk("backlight on failed: %d\n", ret);
		return 0;
	}

	printk("display + backlight up, entering UI loop\n");

	while (1) {
		uint32_t s = (uint32_t)(k_uptime_get() / 1000U);

		if (s != last_s) {
			last_s = s;
			snprintf(buf, sizeof(buf), "%u s", s);
			lv_label_set_text(counter, buf);
			printk("alive: %u s\n", s);
		}

		if (strip_ok) {
			struct led_rgb px;
			const char *phase;

			/* First six seconds: solid red, green, blue (2 s
			 * each) so the dts color-mapping can be verified by
			 * eye against the console; rainbow ever after.
			 */
			if (s < 2) {
				px = (struct led_rgb){ .r = STRIP_MAX };
				phase = "RED";
			} else if (s < 4) {
				px = (struct led_rgb){ .g = STRIP_MAX };
				phase = "GREEN";
			} else if (s < 6) {
				px = (struct led_rgb){ .b = STRIP_MAX };
				phase = "BLUE";
			} else {
				px = hue_to_rgb(hue);
				hue = (hue + 1) % 360U;
				phase = "rainbow";
			}

			if (phase != led_phase) {
				led_phase = phase;
				printk("WS2812: %s\n", phase);
			}

			ret = led_strip_update_rgb(strip, &px, 1);
			if (ret < 0) {
				printk("WS2812 update failed: %d -- disabling\n",
				       ret);
				strip_ok = false;
			}
		}

		lv_timer_handler();
		k_msleep(10);
	}

	return 0;
}
