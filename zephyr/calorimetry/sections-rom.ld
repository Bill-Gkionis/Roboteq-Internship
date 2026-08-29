/* SPDX-License-Identifier: Apache-2.0
 *
 * The Zephyr HTTP server collects the resources belonging to one service into
 * a linker "iterable section" whose name is derived from the service name used
 * in HTTP_SERVICE_DEFINE(). Declaring it here is what makes
 * HTTP_RESOURCE_DEFINE(..., cal_http, ...) in src/iot.c link.
 *
 * Ref: zephyr/samples/net/sockets/http_server/sections-rom.ld
 */
#include <zephyr/linker/iterable_sections.h>

ITERABLE_SECTION_ROM(http_resource_desc_cal_http, Z_LINK_ITERABLE_SUBALIGN)
