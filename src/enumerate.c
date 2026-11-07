// SPDX-License-Identifier: GPL-2.0-or-later
#include "enumerate.h"

#include <stdlib.h>
#include <string.h>

#include <wayland-client.h>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"

#define MAX_OUTPUTS 32
#define MAX_WINDOWS 256

struct enum_state {
	struct wl_output *outputs[MAX_OUTPUTS];
	struct hc_enum_output output_info[MAX_OUTPUTS];
	int n_outputs;
	struct ext_foreign_toplevel_list_v1 *list;
	struct ext_foreign_toplevel_handle_v1 *handles[MAX_WINDOWS];
	struct hc_enum_window window_info[MAX_WINDOWS];
	int n_windows;
};

static char *dup_or_null(const char *s)
{
	return s ? strdup(s) : NULL;
}

/* outputs */

static void o_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sp,
		       const char *make, const char *model, int32_t t)
{
	(void)d;
	(void)o;
	(void)x;
	(void)y;
	(void)pw;
	(void)ph;
	(void)sp;
	(void)make;
	(void)model;
	(void)t;
}
static void o_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r)
{
	(void)d;
	(void)o;
	(void)f;
	(void)w;
	(void)h;
	(void)r;
}
static void o_done(void *d, struct wl_output *o)
{
	(void)d;
	(void)o;
}
static void o_scale(void *d, struct wl_output *o, int32_t s)
{
	(void)d;
	(void)o;
	(void)s;
}
static void o_name(void *d, struct wl_output *o, const char *name)
{
	(void)o;
	struct hc_enum_output *out = d;
	free(out->name);
	out->name = dup_or_null(name);
}
static void o_description(void *d, struct wl_output *o, const char *desc)
{
	(void)o;
	struct hc_enum_output *out = d;
	free(out->description);
	out->description = dup_or_null(desc);
}

static const struct wl_output_listener output_listener = {
	.geometry = o_geometry,
	.mode = o_mode,
	.done = o_done,
	.scale = o_scale,
	.name = o_name,
	.description = o_description,
};

/* toplevels */

static void t_closed(void *d, struct ext_foreign_toplevel_handle_v1 *h)
{
	(void)h;
	struct hc_enum_window *w = d;
	free(w->app_id);
	free(w->title);
	w->app_id = w->title = NULL;
}
static void t_done(void *d, struct ext_foreign_toplevel_handle_v1 *h)
{
	(void)d;
	(void)h;
}
static void t_title(void *d, struct ext_foreign_toplevel_handle_v1 *h, const char *title)
{
	(void)h;
	struct hc_enum_window *w = d;
	free(w->title);
	w->title = dup_or_null(title);
}
static void t_app_id(void *d, struct ext_foreign_toplevel_handle_v1 *h, const char *app_id)
{
	(void)h;
	struct hc_enum_window *w = d;
	free(w->app_id);
	w->app_id = dup_or_null(app_id);
}
static void t_identifier(void *d, struct ext_foreign_toplevel_handle_v1 *h, const char *id)
{
	(void)d;
	(void)h;
	(void)id;
}

static const struct ext_foreign_toplevel_handle_v1_listener toplevel_listener = {
	.closed = t_closed,
	.done = t_done,
	.title = t_title,
	.app_id = t_app_id,
	.identifier = t_identifier,
};

static void l_toplevel(void *d, struct ext_foreign_toplevel_list_v1 *l, struct ext_foreign_toplevel_handle_v1 *h)
{
	(void)l;
	struct enum_state *st = d;
	if (st->n_windows >= MAX_WINDOWS) {
		ext_foreign_toplevel_handle_v1_destroy(h);
		return;
	}
	// The handle's title/app_id events follow in the same batch, so the
	// listener must be attached right away (storage is fixed-size, so stable).
	int i = st->n_windows++;
	st->handles[i] = h;
	ext_foreign_toplevel_handle_v1_add_listener(h, &toplevel_listener, &st->window_info[i]);
}
static void l_finished(void *d, struct ext_foreign_toplevel_list_v1 *l)
{
	(void)d;
	(void)l;
}

static const struct ext_foreign_toplevel_list_v1_listener list_listener = {
	.toplevel = l_toplevel,
	.finished = l_finished,
};

/* registry */

static void r_global(void *d, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
	struct enum_state *st = d;
	if (strcmp(iface, wl_output_interface.name) == 0 && st->n_outputs < MAX_OUTPUTS) {
		int i = st->n_outputs++;
		st->outputs[i] = wl_registry_bind(reg, name, &wl_output_interface, version < 4 ? version : 4);
		wl_output_add_listener(st->outputs[i], &output_listener, &st->output_info[i]);
	} else if (strcmp(iface, ext_foreign_toplevel_list_v1_interface.name) == 0) {
		st->list = wl_registry_bind(reg, name, &ext_foreign_toplevel_list_v1_interface, 1);
		ext_foreign_toplevel_list_v1_add_listener(st->list, &list_listener, st);
	}
}
static void r_global_remove(void *d, struct wl_registry *reg, uint32_t name)
{
	(void)d;
	(void)reg;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = r_global,
	.global_remove = r_global_remove,
};

struct hc_enum hc_enumerate(void)
{
	struct hc_enum result = {0};
	struct enum_state *st = calloc(1, sizeof(*st));

	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		free(st);
		return result;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, st);
	// 1: globals bound. 2: output names + toplevel list. 3: safety margin.
	for (int i = 0; i < 3; i++)
		wl_display_roundtrip(display);

	for (int i = 0; i < st->n_windows; i++)
		ext_foreign_toplevel_handle_v1_destroy(st->handles[i]);
	if (st->list) {
		ext_foreign_toplevel_list_v1_stop(st->list);
		ext_foreign_toplevel_list_v1_destroy(st->list);
	}
	for (int i = 0; i < st->n_outputs; i++)
		wl_output_destroy(st->outputs[i]);
	wl_registry_destroy(registry);
	wl_display_disconnect(display);

	result.outputs = calloc((size_t)st->n_outputs + 1, sizeof(*result.outputs));
	for (int i = 0; i < st->n_outputs; i++) {
		if (st->output_info[i].name)
			result.outputs[result.n_outputs++] = st->output_info[i];
		else
			free(st->output_info[i].description);
	}
	// Drop windows that closed or never sent an app_id.
	result.windows = calloc((size_t)st->n_windows + 1, sizeof(*result.windows));
	for (int i = 0; i < st->n_windows; i++) {
		struct hc_enum_window *w = &st->window_info[i];
		if (w->app_id && w->app_id[0]) {
			result.windows[result.n_windows++] = *w;
		} else {
			free(w->app_id);
			free(w->title);
		}
	}
	free(st);
	return result;
}

void hc_enum_free(struct hc_enum *e)
{
	for (int i = 0; i < e->n_outputs; i++) {
		free(e->outputs[i].name);
		free(e->outputs[i].description);
	}
	for (int i = 0; i < e->n_windows; i++) {
		free(e->windows[i].app_id);
		free(e->windows[i].title);
	}
	free(e->outputs);
	free(e->windows);
	*e = (struct hc_enum){0};
}
