/* Generated by wayland-scanner 1.22.0 */

/*
 * Copyright © 2019 Purism SPC
 *
 * Permission to use, copy, modify, distribute, and sell this
 * software and its documentation for any purpose is hereby granted
 * without fee, provided that the above copyright notice appear in
 * all copies and that both that copyright notice and this permission
 * notice appear in supporting documentation, and that the name of
 * the copyright holders not be used in advertising or publicity
 * pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
 * ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
 * THIS SOFTWARE.
 */

#include <stdlib.h>
#include <stdint.h>
#include "wayland-util.h"

#ifndef __has_attribute
# define __has_attribute(x) 0  /* Compatibility with non-clang compilers. */
#endif

#if (__has_attribute(visibility) || defined(__GNUC__) && __GNUC__ >= 4)
#define WL_PRIVATE __attribute__ ((visibility("hidden")))
#else
#define WL_PRIVATE
#endif

extern const struct wl_interface zwlr_output_configuration_head_v1_interface;
extern const struct wl_interface zwlr_output_configuration_v1_interface;
extern const struct wl_interface zwlr_output_head_v1_interface;
extern const struct wl_interface zwlr_output_mode_v1_interface;

static const struct wl_interface *wlr_output_management_unstable_v1_types[] = {
	NULL,
	NULL,
	NULL,
	&zwlr_output_configuration_v1_interface,
	NULL,
	&zwlr_output_head_v1_interface,
	&zwlr_output_mode_v1_interface,
	&zwlr_output_mode_v1_interface,
	&zwlr_output_configuration_head_v1_interface,
	&zwlr_output_head_v1_interface,
	&zwlr_output_head_v1_interface,
	&zwlr_output_mode_v1_interface,
};

static const struct wl_message zwlr_output_manager_v1_requests[] = {
	{ "create_configuration", "nu", wlr_output_management_unstable_v1_types + 3 },
	{ "stop", "", wlr_output_management_unstable_v1_types + 0 },
};

static const struct wl_message zwlr_output_manager_v1_events[] = {
	{ "head", "n", wlr_output_management_unstable_v1_types + 5 },
	{ "done", "u", wlr_output_management_unstable_v1_types + 0 },
	{ "finished", "", wlr_output_management_unstable_v1_types + 0 },
};

WL_PRIVATE const struct wl_interface zwlr_output_manager_v1_interface = {
	"zwlr_output_manager_v1", 4,
	2, zwlr_output_manager_v1_requests,
	3, zwlr_output_manager_v1_events,
};

static const struct wl_message zwlr_output_head_v1_requests[] = {
	{ "release", "3", wlr_output_management_unstable_v1_types + 0 },
};

static const struct wl_message zwlr_output_head_v1_events[] = {
	{ "name", "s", wlr_output_management_unstable_v1_types + 0 },
	{ "description", "s", wlr_output_management_unstable_v1_types + 0 },
	{ "physical_size", "ii", wlr_output_management_unstable_v1_types + 0 },
	{ "mode", "n", wlr_output_management_unstable_v1_types + 6 },
	{ "enabled", "i", wlr_output_management_unstable_v1_types + 0 },
	{ "current_mode", "o", wlr_output_management_unstable_v1_types + 7 },
	{ "position", "ii", wlr_output_management_unstable_v1_types + 0 },
	{ "transform", "i", wlr_output_management_unstable_v1_types + 0 },
	{ "scale", "f", wlr_output_management_unstable_v1_types + 0 },
	{ "finished", "", wlr_output_management_unstable_v1_types + 0 },
	{ "make", "2s", wlr_output_management_unstable_v1_types + 0 },
	{ "model", "2s", wlr_output_management_unstable_v1_types + 0 },
	{ "serial_number", "2s", wlr_output_management_unstable_v1_types + 0 },
	{ "adaptive_sync", "4u", wlr_output_management_unstable_v1_types + 0 },
};

WL_PRIVATE const struct wl_interface zwlr_output_head_v1_interface = {
	"zwlr_output_head_v1", 4,
	1, zwlr_output_head_v1_requests,
	14, zwlr_output_head_v1_events,
};

static const struct wl_message zwlr_output_mode_v1_requests[] = {
	{ "release", "3", wlr_output_management_unstable_v1_types + 0 },
};

static const struct wl_message zwlr_output_mode_v1_events[] = {
	{ "size", "ii", wlr_output_management_unstable_v1_types + 0 },
	{ "refresh", "i", wlr_output_management_unstable_v1_types + 0 },
	{ "preferred", "", wlr_output_management_unstable_v1_types + 0 },
	{ "finished", "", wlr_output_management_unstable_v1_types + 0 },
};

WL_PRIVATE const struct wl_interface zwlr_output_mode_v1_interface = {
	"zwlr_output_mode_v1", 3,
	1, zwlr_output_mode_v1_requests,
	4, zwlr_output_mode_v1_events,
};

static const struct wl_message zwlr_output_configuration_v1_requests[] = {
	{ "enable_head", "no", wlr_output_management_unstable_v1_types + 8 },
	{ "disable_head", "o", wlr_output_management_unstable_v1_types + 10 },
	{ "apply", "", wlr_output_management_unstable_v1_types + 0 },
	{ "test", "", wlr_output_management_unstable_v1_types + 0 },
	{ "destroy", "", wlr_output_management_unstable_v1_types + 0 },
};

static const struct wl_message zwlr_output_configuration_v1_events[] = {
	{ "succeeded", "", wlr_output_management_unstable_v1_types + 0 },
	{ "failed", "", wlr_output_management_unstable_v1_types + 0 },
	{ "cancelled", "", wlr_output_management_unstable_v1_types + 0 },
};

WL_PRIVATE const struct wl_interface zwlr_output_configuration_v1_interface = {
	"zwlr_output_configuration_v1", 4,
	5, zwlr_output_configuration_v1_requests,
	3, zwlr_output_configuration_v1_events,
};

static const struct wl_message zwlr_output_configuration_head_v1_requests[] = {
	{ "set_mode", "o", wlr_output_management_unstable_v1_types + 11 },
	{ "set_custom_mode", "iii", wlr_output_management_unstable_v1_types + 0 },
	{ "set_position", "ii", wlr_output_management_unstable_v1_types + 0 },
	{ "set_transform", "i", wlr_output_management_unstable_v1_types + 0 },
	{ "set_scale", "f", wlr_output_management_unstable_v1_types + 0 },
	{ "set_adaptive_sync", "4u", wlr_output_management_unstable_v1_types + 0 },
};

WL_PRIVATE const struct wl_interface zwlr_output_configuration_head_v1_interface = {
	"zwlr_output_configuration_head_v1", 4,
	6, zwlr_output_configuration_head_v1_requests,
	0, NULL,
};

