/*
 * ui.h - the LVGL front panel.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Three screens, in the shape of a 3D printer's panel: RUN (what the rig is
 * doing), CONTROL (what you can tell it to do), NET (how to reach it).
 *
 * The panel does not own any state.  It renders a copy of the snapshot and it
 * submits commands through sys_cmd_submit(), the same door the WebSocket and
 * the shell use.  That is what makes "the panel cannot bypass the interlocks"
 * a structural fact rather than a promise - there is no second path to an
 * actuator for it to take.
 *
 * Compiles to nothing when CONFIG_LVGL is off, so a headless carrier board
 * build needs no source changes.
 */

#ifndef CALORIMETRY_UI_H_
#define CALORIMETRY_UI_H_

#include "types.h"

/** Build the screens and start the LVGL thread.  Safe to call with no panel. */
int ui_init(void);

/** Seconds of inactivity before the backlight goes out.  0 disables sleep. */
void ui_set_sleep_timeout(int seconds);
int  ui_get_sleep_timeout(void);

/** Tell the panel what to show for network state (called by iot.c). */
void ui_set_net_status(const char *ssid, const char *ip, bool connected);

#endif /* CALORIMETRY_UI_H_ */
