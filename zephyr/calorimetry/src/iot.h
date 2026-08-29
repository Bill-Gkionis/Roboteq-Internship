/*
 * iot.h - Wi-Fi, the HTTP dashboard and the WebSocket telemetry push.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * THE ARCHITECTURE, IN ONE SENTENCE: don't stream pixels, stream state.
 *
 * The rig sends NUMBERS - one JSON snapshot per second - and the LVGL panel
 * and any browser each draw their own picture of it, natively, at their own
 * resolution.  A screen-scrape mirror would be locked to the panel's 172 px
 * and could only ever be upscaled onto a 27" monitor; a VNC-style stream would
 * add real attack surface to solve a "smooth live feel" problem that a 1 Hz
 * instrument does not have.  Nothing here requires LVGL to know the network
 * exists, which is exactly what sidesteps "LVGL isn't built for HTTP".
 *
 * SECURITY SCOPE: same LAN only (the lab's Wi-Fi), never internet exposed.
 * Read-only telemetry is open on that LAN; the command endpoint requires a
 * token that is shown on the panel's NET screen.  That is not "harden against
 * the internet" - it is "nobody else on the office Wi-Fi gets to POST a
 * setpoint change to a machine with two 100 W heaters in it".
 *
 * AND THE RULE THAT MATTERS MORE THAN ANY OF IT: a remote command is handed to
 * sys_cmd_submit(), the same function the panel and the shell call.  The
 * supervisor's interlocks decide whether it is honoured, never the transport
 * it arrived on.  "Local has priority" is therefore true by construction.
 */

#ifndef CALORIMETRY_IOT_H_
#define CALORIMETRY_IOT_H_

#include "types.h"

/** Load stored credentials, start the HTTP server, try to associate. */
int iot_init(void);

/** Store new credentials in NVS and (re)connect. */
int iot_set_credentials(const char *ssid, const char *psk);

/** Connection state, for the panel. */
bool iot_connected(void);
const char *iot_ip(void);
const char *iot_ssid(void);

/** The token the control endpoint requires.  Shown on the panel. */
const char *iot_token(void);

/** Serialise the snapshot as JSON.  Shared by /api/state and the WebSocket. */
int iot_snapshot_json(char *buf, size_t buf_len);

#endif /* CALORIMETRY_IOT_H_ */
