// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Snapshot of what can be captured right now, for the properties UI.
struct hc_enum_output {
	char *name;        // "DP-3"
	char *description; // "ASUS ... (DP-3)"
};

struct hc_enum_window {
	char *app_id;
	char *title;
};

struct hc_enum {
	struct hc_enum_output *outputs;
	int n_outputs;
	struct hc_enum_window *windows;
	int n_windows;
};

// Opens a short-lived Wayland connection and lists outputs and windows.
// Always returns a valid (possibly empty) result; free with hc_enum_free().
struct hc_enum hc_enumerate(void);
void hc_enum_free(struct hc_enum *e);
