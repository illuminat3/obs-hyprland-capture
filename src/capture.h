// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <graphics/graphics.h>

enum hc_target_type {
	HC_TARGET_OUTPUT,
	HC_TARGET_TOPLEVEL,
};

struct hc_target {
	enum hc_target_type type;
	const char *output_name; // HC_TARGET_OUTPUT: wl_output name, e.g. "DP-3" (empty = first output)
	const char *app_id;      // HC_TARGET_TOPLEVEL: window app_id / class
	const char *title;       // HC_TARGET_TOPLEVEL: preferred title (empty = any window of app_id)
	bool paint_cursor;
};

struct hc_capture;

// Must be called from a thread that may enter the OBS graphics context.
struct hc_capture *hc_capture_create(void);
void hc_capture_destroy(struct hc_capture *cap);

// Thread-safe. Strings are copied.
void hc_capture_set_target(struct hc_capture *cap, const struct hc_target *target);

// Called once per OBS video tick while the source is visible: asks the
// capture thread to grab the next frame from the compositor.
void hc_capture_request_frame(struct hc_capture *cap);

// Graphics thread only.
void hc_capture_render(struct hc_capture *cap);
uint32_t hc_capture_width(struct hc_capture *cap);
uint32_t hc_capture_height(struct hc_capture *cap);
