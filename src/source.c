// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdlib.h>
#include <string.h>

#include <obs-module.h>
#include <util/dstr.h>

#include "capture.h"
#include "enumerate.h"

#define S_OUTPUT "output"
#define S_WINDOW "window"
#define S_SHOW_CURSOR "show_cursor"
#define WINDOW_SEPARATOR '\x1f' // between app_id and title in the stored setting

struct hc_source {
	obs_source_t *source;
	struct hc_capture *cap;
	bool window_mode;
};

/* ---- shared ------------------------------------------------------------ */

static void hc_update(void *data, obs_data_t *settings);

static void *hc_create(obs_data_t *settings, obs_source_t *source, bool window_mode)
{
	struct hc_source *s = bzalloc(sizeof(*s));
	s->source = source;
	s->window_mode = window_mode;
	s->cap = hc_capture_create();
	// Apply settings synchronously so the first session targets the right
	// output instead of briefly capturing the default one.
	hc_update(s, settings);
	return s;
}

static void hc_destroy(void *data)
{
	struct hc_source *s = data;
	hc_capture_destroy(s->cap);
	bfree(s);
}

static void hc_update(void *data, obs_data_t *settings)
{
	struct hc_source *s = data;
	struct hc_target target = {
		.type = s->window_mode ? HC_TARGET_TOPLEVEL : HC_TARGET_OUTPUT,
		.paint_cursor = obs_data_get_bool(settings, S_SHOW_CURSOR),
	};

	char *app_id = NULL;
	if (s->window_mode) {
		app_id = bstrdup(obs_data_get_string(settings, S_WINDOW));
		char *sep = strchr(app_id, WINDOW_SEPARATOR);
		if (sep) {
			*sep = '\0';
			target.title = sep + 1;
		}
		target.app_id = app_id;
	} else {
		target.output_name = obs_data_get_string(settings, S_OUTPUT);
	}

	hc_capture_set_target(s->cap, &target);
	bfree(app_id);
}

static void hc_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, S_SHOW_CURSOR, true);
}

static void hc_video_tick(void *data, float seconds)
{
	(void)seconds;
	struct hc_source *s = data;
	// Only pull frames from the compositor while something can see them.
	if (obs_source_showing(s->source))
		hc_capture_request_frame(s->cap);
}

static void hc_video_render(void *data, gs_effect_t *effect)
{
	(void)effect;
	struct hc_source *s = data;
	hc_capture_render(s->cap);
}

static uint32_t hc_get_width(void *data)
{
	struct hc_source *s = data;
	return hc_capture_width(s->cap);
}

static uint32_t hc_get_height(void *data)
{
	struct hc_source *s = data;
	return hc_capture_height(s->cap);
}

/* ---- screen ------------------------------------------------------------ */

static const char *screen_get_name(void *unused)
{
	(void)unused;
	return "Hyprland Screen Capture";
}

static void *screen_create(obs_data_t *settings, obs_source_t *source)
{
	return hc_create(settings, source, false);
}

static obs_properties_t *screen_properties(void *data)
{
	(void)data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *list =
		obs_properties_add_list(props, S_OUTPUT, "Monitor", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	struct hc_enum e = hc_enumerate();
	for (int i = 0; i < e.n_outputs; i++) {
		struct dstr label = {0};
		if (e.outputs[i].description && e.outputs[i].description[0])
			dstr_printf(&label, "%s: %s", e.outputs[i].name, e.outputs[i].description);
		else
			dstr_copy(&label, e.outputs[i].name);
		obs_property_list_add_string(list, label.array, e.outputs[i].name);
		dstr_free(&label);
	}
	hc_enum_free(&e);

	obs_properties_add_bool(props, S_SHOW_CURSOR, "Show cursor");
	return props;
}

static void screen_defaults(obs_data_t *settings)
{
	hc_defaults(settings);
	struct hc_enum e = hc_enumerate();
	if (e.n_outputs > 0)
		obs_data_set_default_string(settings, S_OUTPUT, e.outputs[0].name);
	hc_enum_free(&e);
}

struct obs_source_info hc_screen_source = {
	.id = "hyprland-screen-capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB | OBS_SOURCE_DO_NOT_DUPLICATE,
	.icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE,
	.get_name = screen_get_name,
	.create = screen_create,
	.destroy = hc_destroy,
	.update = hc_update,
	.get_defaults = screen_defaults,
	.get_properties = screen_properties,
	.video_tick = hc_video_tick,
	.video_render = hc_video_render,
	.get_width = hc_get_width,
	.get_height = hc_get_height,
};

/* ---- window ------------------------------------------------------------ */

static const char *window_get_name(void *unused)
{
	(void)unused;
	return "Hyprland Window Capture";
}

static void *window_create(obs_data_t *settings, obs_source_t *source)
{
	return hc_create(settings, source, true);
}

static int compare_windows(const void *a, const void *b)
{
	const struct hc_enum_window *wa = a, *wb = b;
	int r = strcasecmp(wa->app_id, wb->app_id);
	return r ? r : strcmp(wa->title ? wa->title : "", wb->title ? wb->title : "");
}

static obs_properties_t *window_properties(void *data)
{
	struct hc_source *s = data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *list =
		obs_properties_add_list(props, S_WINDOW, "Window", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	struct hc_enum e = hc_enumerate();
	qsort(e.windows, (size_t)e.n_windows, sizeof(*e.windows), compare_windows);
	for (int i = 0; i < e.n_windows; i++) {
		struct dstr value = {0}, label = {0};
		const char *title = e.windows[i].title ? e.windows[i].title : "";
		dstr_printf(&value, "%s%c%s", e.windows[i].app_id, WINDOW_SEPARATOR, title);
		dstr_printf(&label, "[%s] %s", e.windows[i].app_id, title);
		obs_property_list_add_string(list, label.array, value.array);
		dstr_free(&value);
		dstr_free(&label);
	}
	hc_enum_free(&e);

	// Keep the saved window selectable even while it is closed.
	if (s) {
		obs_data_t *settings = obs_source_get_settings(s->source);
		const char *current = obs_data_get_string(settings, S_WINDOW);
		bool found = false;
		for (size_t i = 0; i < obs_property_list_item_count(list); i++)
			found |= strcmp(obs_property_list_item_string(list, i), current) == 0;
		if (!found && current[0]) {
			struct dstr label = {0};
			dstr_copy(&label, current);
			dstr_replace(&label, "\x1f", "] ");
			dstr_insert(&label, 0, "[");
			dstr_cat(&label, " (not open)");
			obs_property_list_insert_string(list, 0, label.array, current);
			dstr_free(&label);
		}
		obs_data_release(settings);
	}

	obs_properties_add_text(props, "window_note",
				"Matches the window by app class, preferring the same title. If the exact title "
				"is gone (e.g. a browser tab changed), the first window of that app is used.",
				OBS_TEXT_INFO);
	obs_properties_add_bool(props, S_SHOW_CURSOR, "Show cursor");
	return props;
}

struct obs_source_info hc_window_source = {
	.id = "hyprland-window-capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB | OBS_SOURCE_DO_NOT_DUPLICATE,
	.icon_type = OBS_ICON_TYPE_WINDOW_CAPTURE,
	.get_name = window_get_name,
	.create = window_create,
	.destroy = hc_destroy,
	.update = hc_update,
	.get_defaults = hc_defaults,
	.get_properties = window_properties,
	.video_tick = hc_video_tick,
	.video_render = hc_video_render,
	.get_width = hc_get_width,
	.get_height = hc_get_height,
};
