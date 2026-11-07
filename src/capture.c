// SPDX-License-Identifier: GPL-2.0-or-later
//
// Zero-copy Wayland capture via ext-image-copy-capture-v1.
//
// A dedicated thread owns its own Wayland connection. It keeps a small ring
// of GBM-allocated dma-bufs; the compositor copies frames into them on the
// GPU, and the OBS graphics thread imports them as textures once (per
// allocation) and samples whichever buffer finished most recently. Nothing
// ever goes through system memory and the OBS render thread never waits on
// the compositor.

#include "capture.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <gbm.h>
#include <linux/dma-buf.h>
#include <wayland-client.h>
#include <xf86drm.h>

#include <obs-module.h>
#include <util/bmem.h>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"

#define LOG(level, fmt, ...) blog(level, "[hyprland-capture] " fmt, ##__VA_ARGS__)

#define NUM_BUFFERS 3
#define MAX_PLANES 4
#define RETRY_INTERVAL_NS (1000ull * 1000 * 1000)
#define IDLE_POLL_MS 250
#define FENCE_TIMEOUT_MS 100

// Formats we can hand to OBS, in order of preference. Alpha is ignored when
// drawing (screen content is opaque), so X and A variants are equivalent.
static const struct {
	uint32_t drm;
	enum gs_color_format gs;
} preferred_formats[] = {
	{DRM_FORMAT_XRGB8888, GS_BGRX},
	{DRM_FORMAT_ARGB8888, GS_BGRA},
	{DRM_FORMAT_XBGR8888, GS_RGBA},
	{DRM_FORMAT_ABGR8888, GS_RGBA},
};
#define NUM_FORMATS (sizeof(preferred_formats) / sizeof(preferred_formats[0]))

struct modifier_list {
	bool present;
	uint64_t *mods;
	size_t n;
};

struct hc_output {
	struct wl_list link;
	struct hc_capture *cap;
	struct wl_output *output;
	uint32_t global_name;
	char *name;
};

struct hc_toplevel {
	struct wl_list link;
	struct hc_capture *cap;
	struct ext_foreign_toplevel_handle_v1 *handle;
	char *app_id, *title;
	char *pending_app_id, *pending_title;
	bool ready;
};

struct hc_buffer {
	struct gbm_bo *bo;
	struct wl_buffer *wl_buffer;
	uint32_t n_planes;
	int fds[MAX_PLANES];
	uint32_t strides[MAX_PLANES];
	uint32_t offsets[MAX_PLANES];
	uint64_t modifier;
	uint64_t generation; // 0 = unallocated
};

struct hc_texture {
	gs_texture_t *tex;
	uint64_t generation;
	bool failed;
};

struct owned_target {
	enum hc_target_type type;
	char *output_name, *app_id, *title;
	bool paint_cursor;
};

struct hc_capture {
	pthread_t thread;
	bool thread_started;
	int wake_fd;
	atomic_bool quit;
	atomic_bool want_frame;
	atomic_bool target_dirty;

	pthread_mutex_t target_mutex;
	struct owned_target target;

	// What OBS can import (read-only once the thread runs).
	struct modifier_list obs_mods[NUM_FORMATS];
	bool obs_implicit_modifiers;

	// ---- capture thread only ----
	struct wl_display *display;
	struct wl_registry *registry;
	struct ext_image_copy_capture_manager_v1 *copy_manager;
	struct ext_output_image_capture_source_manager_v1 *output_sources;
	struct ext_foreign_toplevel_image_capture_source_manager_v1 *toplevel_sources;
	struct ext_foreign_toplevel_list_v1 *toplevel_list;
	struct zwp_linux_dmabuf_v1 *dmabuf;
	struct wl_list outputs;
	struct wl_list toplevels;

	struct ext_image_capture_source_v1 *source;
	struct ext_image_copy_capture_session_v1 *session;
	struct ext_image_copy_capture_frame_v1 *frame;
	void *session_target;
	bool session_ready;
	bool session_stopped;
	uint64_t retry_at;
	int capturing;
	uint32_t frame_failures;

	uint32_t pending_width, pending_height;
	bool pending_has_device;
	dev_t pending_device;
	struct modifier_list pending_mods[NUM_FORMATS];

	int drm_fd;
	dev_t drm_device;
	struct gbm_device *gbm;

	// ---- shared with the graphics thread, guarded by buf_mutex ----
	pthread_mutex_t buf_mutex;
	struct hc_buffer buffers[NUM_BUFFERS];
	uint32_t width, height;
	uint32_t drm_format;
	enum gs_color_format color_format;
	int latest;
	uint64_t next_generation;

	// ---- graphics thread only ----
	struct hc_texture textures[NUM_BUFFERS];
};

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void wake(struct hc_capture *cap)
{
	uint64_t one = 1;
	ssize_t r = write(cap->wake_fd, &one, sizeof(one));
	(void)r;
}

static void free_modifier_list(struct modifier_list *list)
{
	free(list->mods);
	*list = (struct modifier_list){0};
}

static void free_target(struct owned_target *t)
{
	free(t->output_name);
	free(t->app_id);
	free(t->title);
	*t = (struct owned_target){0};
}

static char *dup_or_empty(const char *s)
{
	return strdup(s ? s : "");
}

/* ------------------------------------------------------------------------ */
/* Buffers                                                                  */

static void free_buffer(struct hc_buffer *buf)
{
	if (buf->wl_buffer)
		wl_buffer_destroy(buf->wl_buffer);
	for (uint32_t i = 0; i < buf->n_planes; i++)
		if (buf->fds[i] >= 0)
			close(buf->fds[i]);
	if (buf->bo)
		gbm_bo_destroy(buf->bo);
	*buf = (struct hc_buffer){0};
}

// Caller holds buf_mutex.
static void free_buffers_locked(struct hc_capture *cap)
{
	for (int i = 0; i < NUM_BUFFERS; i++)
		free_buffer(&cap->buffers[i]);
	cap->latest = -1;
	cap->width = cap->height = 0;
	cap->drm_format = 0;
}

static void free_buffers(struct hc_capture *cap)
{
	pthread_mutex_lock(&cap->buf_mutex);
	free_buffers_locked(cap);
	pthread_mutex_unlock(&cap->buf_mutex);
}

static bool allocate_buffer(struct hc_capture *cap, struct hc_buffer *buf, uint32_t width, uint32_t height,
			    uint32_t format, const uint64_t *mods, size_t n_mods)
{
	*buf = (struct hc_buffer){0};
	for (int i = 0; i < MAX_PLANES; i++)
		buf->fds[i] = -1;

	if (n_mods > 0)
		buf->bo = gbm_bo_create_with_modifiers2(cap->gbm, width, height, format, mods, (unsigned)n_mods,
							GBM_BO_USE_RENDERING);
	else
		buf->bo = gbm_bo_create(cap->gbm, width, height, format, GBM_BO_USE_RENDERING);
	if (!buf->bo)
		return false;

	int planes = gbm_bo_get_plane_count(buf->bo);
	if (planes < 1 || planes > MAX_PLANES) {
		free_buffer(buf);
		return false;
	}
	buf->n_planes = (uint32_t)planes;
	buf->modifier = gbm_bo_get_modifier(buf->bo);

	struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(cap->dmabuf);
	for (int p = 0; p < planes; p++) {
		buf->fds[p] = gbm_bo_get_fd_for_plane(buf->bo, p);
		buf->strides[p] = gbm_bo_get_stride_for_plane(buf->bo, p);
		buf->offsets[p] = gbm_bo_get_offset(buf->bo, p);
		if (buf->fds[p] < 0) {
			zwp_linux_buffer_params_v1_destroy(params);
			free_buffer(buf);
			return false;
		}
		zwp_linux_buffer_params_v1_add(params, buf->fds[p], (uint32_t)p, buf->offsets[p], buf->strides[p],
					       (uint32_t)(buf->modifier >> 32), (uint32_t)(buf->modifier & 0xffffffff));
	}
	buf->wl_buffer = zwp_linux_buffer_params_v1_create_immed(params, (int32_t)width, (int32_t)height, format, 0);
	zwp_linux_buffer_params_v1_destroy(params);
	if (!buf->wl_buffer) {
		free_buffer(buf);
		return false;
	}
	buf->generation = ++cap->next_generation;
	return true;
}

static bool allocate_buffers(struct hc_capture *cap, uint32_t width, uint32_t height, size_t format_index,
			     const uint64_t *mods, size_t n_mods)
{
	uint32_t format = preferred_formats[format_index].drm;
	struct hc_buffer fresh[NUM_BUFFERS];

	for (int i = 0; i < NUM_BUFFERS; i++) {
		if (!allocate_buffer(cap, &fresh[i], width, height, format, mods, n_mods)) {
			for (int j = 0; j < i; j++)
				free_buffer(&fresh[j]);
			return false;
		}
	}

	pthread_mutex_lock(&cap->buf_mutex);
	free_buffers_locked(cap);
	memcpy(cap->buffers, fresh, sizeof(fresh));
	cap->width = width;
	cap->height = height;
	cap->drm_format = format;
	cap->color_format = preferred_formats[format_index].gs;
	pthread_mutex_unlock(&cap->buf_mutex);

	char fourcc[5] = {(char)(format & 0xff), (char)((format >> 8) & 0xff), (char)((format >> 16) & 0xff),
			  (char)(format >> 24), 0};
	LOG(LOG_INFO, "allocated %d x %ux%u %s dma-bufs (modifier 0x%" PRIx64 ", %u plane(s))", NUM_BUFFERS, width,
	    height, fourcc, cap->buffers[0].modifier, cap->buffers[0].n_planes);
	return true;
}

static bool open_gbm(struct hc_capture *cap, dev_t device)
{
	if (cap->gbm && cap->drm_device == device)
		return true;

	if (cap->gbm) {
		free_buffers(cap);
		gbm_device_destroy(cap->gbm);
		close(cap->drm_fd);
		cap->gbm = NULL;
		cap->drm_fd = -1;
	}

	drmDevice *dev = NULL;
	if (drmGetDeviceFromDevId(device, 0, &dev) != 0) {
		LOG(LOG_ERROR, "unknown DRM device %u:%u", major(device), minor(device));
		return false;
	}
	const char *node = NULL;
	if (dev->available_nodes & (1 << DRM_NODE_RENDER))
		node = dev->nodes[DRM_NODE_RENDER];
	else if (dev->available_nodes & (1 << DRM_NODE_PRIMARY))
		node = dev->nodes[DRM_NODE_PRIMARY];

	int fd = node ? open(node, O_RDWR | O_CLOEXEC) : -1;
	if (fd < 0) {
		LOG(LOG_ERROR, "cannot open DRM node %s: %s", node ? node : "(none)", strerror(errno));
		drmFreeDevice(&dev);
		return false;
	}
	struct gbm_device *gbm = gbm_create_device(fd);
	if (!gbm) {
		LOG(LOG_ERROR, "gbm_create_device failed for %s", node);
		close(fd);
		drmFreeDevice(&dev);
		return false;
	}
	LOG(LOG_INFO, "allocating buffers on %s", node);
	drmFreeDevice(&dev);
	cap->drm_fd = fd;
	cap->drm_device = device;
	cap->gbm = gbm;
	return true;
}

// Picks a format/modifier set both the compositor and OBS accept, and
// (re)allocates the buffer ring if anything changed.
static bool apply_constraints(struct hc_capture *cap)
{
	uint32_t width = cap->pending_width, height = cap->pending_height;
	if (width == 0 || height == 0 || !cap->pending_has_device) {
		LOG(LOG_WARNING, "compositor offered no usable dma-buf constraints");
		return false;
	}
	if (!open_gbm(cap, cap->pending_device))
		return false;

	pthread_mutex_lock(&cap->buf_mutex);
	bool unchanged = cap->buffers[0].generation != 0 && cap->width == width && cap->height == height;
	uint32_t current_format = cap->drm_format;
	pthread_mutex_unlock(&cap->buf_mutex);

	for (size_t f = 0; f < NUM_FORMATS; f++) {
		struct modifier_list *offered = &cap->pending_mods[f];
		struct modifier_list *importable = &cap->obs_mods[f];
		if (!offered->present || !importable->present)
			continue;

		if (unchanged && current_format == preferred_formats[f].drm)
			return true;

		// Explicit modifiers both sides understand.
		uint64_t *common = calloc(offered->n ? offered->n : 1, sizeof(uint64_t));
		size_t n_common = 0;
		bool implicit_ok = false;
		for (size_t i = 0; i < offered->n; i++) {
			uint64_t mod = offered->mods[i];
			if (mod == DRM_FORMAT_MOD_INVALID) {
				implicit_ok = cap->obs_implicit_modifiers;
				continue;
			}
			for (size_t j = 0; j < importable->n; j++) {
				if (importable->mods[j] == mod) {
					common[n_common++] = mod;
					break;
				}
			}
		}

		bool ok = false;
		if (n_common > 0)
			ok = allocate_buffers(cap, width, height, f, common, n_common);
		if (!ok && implicit_ok)
			ok = allocate_buffers(cap, width, height, f, NULL, 0);
		free(common);
		if (ok)
			return true;
	}

	LOG(LOG_ERROR, "no dma-buf format/modifier is shared by the compositor and OBS");
	return false;
}

/* ------------------------------------------------------------------------ */
/* Session / frames                                                         */

static void stop_session(struct hc_capture *cap)
{
	if (cap->frame) {
		ext_image_copy_capture_frame_v1_destroy(cap->frame);
		cap->frame = NULL;
	}
	if (cap->session) {
		ext_image_copy_capture_session_v1_destroy(cap->session);
		cap->session = NULL;
	}
	if (cap->source) {
		ext_image_capture_source_v1_destroy(cap->source);
		cap->source = NULL;
	}
	for (size_t f = 0; f < NUM_FORMATS; f++)
		free_modifier_list(&cap->pending_mods[f]);
	cap->pending_has_device = false;
	cap->pending_width = cap->pending_height = 0;
	cap->session_target = NULL;
	cap->session_ready = false;
	cap->session_stopped = false;
	cap->capturing = -1;
	// Blank instead of showing a stale frame of something that went away.
	free_buffers(cap);
}

// Blocks until the compositor's GPU copy into the buffer has finished, so the
// graphics thread never samples a half-written frame.
static void wait_for_buffer(int fd)
{
	struct dma_buf_export_sync_file req = {.flags = DMA_BUF_SYNC_READ, .fd = -1};
	if (ioctl(fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &req) != 0 || req.fd < 0)
		return;
	struct pollfd pfd = {.fd = req.fd, .events = POLLIN};
	poll(&pfd, 1, FENCE_TIMEOUT_MS);
	close(req.fd);
}

static void frame_transform(void *data, struct ext_image_copy_capture_frame_v1 *frame, uint32_t transform)
{
	(void)data;
	(void)frame;
	(void)transform;
}

static void frame_damage(void *data, struct ext_image_copy_capture_frame_v1 *frame, int32_t x, int32_t y,
			 int32_t width, int32_t height)
{
	(void)data;
	(void)frame;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
}

static void frame_presentation_time(void *data, struct ext_image_copy_capture_frame_v1 *frame, uint32_t sec_hi,
				    uint32_t sec_lo, uint32_t nsec)
{
	(void)data;
	(void)frame;
	(void)sec_hi;
	(void)sec_lo;
	(void)nsec;
}

static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *frame)
{
	struct hc_capture *cap = data;
	int idx = cap->capturing;

	ext_image_copy_capture_frame_v1_destroy(frame);
	cap->frame = NULL;
	cap->capturing = -1;
	cap->frame_failures = 0;

	pthread_mutex_lock(&cap->buf_mutex);
	int fd = (idx >= 0 && cap->buffers[idx].generation) ? cap->buffers[idx].fds[0] : -1;
	pthread_mutex_unlock(&cap->buf_mutex);
	if (fd < 0)
		return;

	wait_for_buffer(fd);

	pthread_mutex_lock(&cap->buf_mutex);
	if (cap->buffers[idx].generation)
		cap->latest = idx;
	pthread_mutex_unlock(&cap->buf_mutex);
}

static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *frame, uint32_t reason)
{
	struct hc_capture *cap = data;
	ext_image_copy_capture_frame_v1_destroy(frame);
	cap->frame = NULL;
	cap->capturing = -1;

	switch (reason) {
	case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS:
		// New constraints + done follow; wait for them before capturing again.
		cap->session_ready = false;
		break;
	case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED:
		cap->session_stopped = true;
		break;
	default:
		if (++cap->frame_failures == 10)
			LOG(LOG_WARNING, "compositor keeps failing frames (unknown reason)");
		break;
	}
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
	.transform = frame_transform,
	.damage = frame_damage,
	.presentation_time = frame_presentation_time,
	.ready = frame_ready,
	.failed = frame_failed,
};

static void start_frame(struct hc_capture *cap)
{
	if (!cap->session || !cap->session_ready || cap->frame)
		return;

	pthread_mutex_lock(&cap->buf_mutex);
	int idx = -1;
	for (int i = 0; i < NUM_BUFFERS; i++) {
		if (i != cap->latest && cap->buffers[i].generation) {
			idx = i;
			break;
		}
	}
	struct wl_buffer *buffer = idx >= 0 ? cap->buffers[idx].wl_buffer : NULL;
	int32_t width = (int32_t)cap->width, height = (int32_t)cap->height;
	pthread_mutex_unlock(&cap->buf_mutex);
	if (!buffer)
		return;

	atomic_store(&cap->want_frame, false);
	cap->capturing = idx;
	cap->frame = ext_image_copy_capture_session_v1_create_frame(cap->session);
	ext_image_copy_capture_frame_v1_add_listener(cap->frame, &frame_listener, cap);
	ext_image_copy_capture_frame_v1_attach_buffer(cap->frame, buffer);
	ext_image_copy_capture_frame_v1_damage_buffer(cap->frame, 0, 0, width, height);
	ext_image_copy_capture_frame_v1_capture(cap->frame);
}

static void session_buffer_size(void *data, struct ext_image_copy_capture_session_v1 *session, uint32_t width,
				uint32_t height)
{
	(void)session;
	struct hc_capture *cap = data;
	cap->pending_width = width;
	cap->pending_height = height;
}

static void session_shm_format(void *data, struct ext_image_copy_capture_session_v1 *session, uint32_t format)
{
	(void)data;
	(void)session;
	(void)format;
}

static void session_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *session,
				  struct wl_array *device)
{
	(void)session;
	struct hc_capture *cap = data;
	if (device->size != sizeof(dev_t))
		return;
	memcpy(&cap->pending_device, device->data, sizeof(dev_t));
	cap->pending_has_device = true;
}

static void session_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *session, uint32_t format,
				  struct wl_array *modifiers)
{
	(void)session;
	struct hc_capture *cap = data;
	for (size_t f = 0; f < NUM_FORMATS; f++) {
		if (preferred_formats[f].drm != format)
			continue;
		struct modifier_list *list = &cap->pending_mods[f];
		free_modifier_list(list);
		list->present = true;
		list->n = modifiers->size / sizeof(uint64_t);
		if (list->n) {
			list->mods = malloc(modifiers->size);
			memcpy(list->mods, modifiers->data, modifiers->size);
		}
	}
}

static void session_done(void *data, struct ext_image_copy_capture_session_v1 *session)
{
	(void)session;
	struct hc_capture *cap = data;
	cap->session_ready = apply_constraints(cap);
	for (size_t f = 0; f < NUM_FORMATS; f++)
		free_modifier_list(&cap->pending_mods[f]);
	cap->pending_has_device = false;
	if (!cap->session_ready)
		cap->session_stopped = true; // tear down and retry later
}

static void session_stopped(void *data, struct ext_image_copy_capture_session_v1 *session)
{
	(void)session;
	struct hc_capture *cap = data;
	cap->session_stopped = true;
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
	.buffer_size = session_buffer_size,
	.shm_format = session_shm_format,
	.dmabuf_device = session_dmabuf_device,
	.dmabuf_format = session_dmabuf_format,
	.done = session_done,
	.stopped = session_stopped,
};

static struct hc_toplevel *find_toplevel(struct hc_capture *cap, const char *app_id, const char *title)
{
	struct hc_toplevel *tl, *by_app = NULL;
	wl_list_for_each (tl, &cap->toplevels, link) {
		if (!tl->ready || !tl->app_id || strcasecmp(tl->app_id, app_id) != 0)
			continue;
		if (title[0] == '\0' || (tl->title && strcmp(tl->title, title) == 0))
			return tl;
		if (!by_app)
			by_app = tl;
	}
	return by_app;
}

static void start_session(struct hc_capture *cap)
{
	if (!cap->copy_manager || !cap->dmabuf)
		return;

	pthread_mutex_lock(&cap->target_mutex);
	struct owned_target t = {
		.type = cap->target.type,
		.output_name = dup_or_empty(cap->target.output_name),
		.app_id = dup_or_empty(cap->target.app_id),
		.title = dup_or_empty(cap->target.title),
		.paint_cursor = cap->target.paint_cursor,
	};
	pthread_mutex_unlock(&cap->target_mutex);

	if (t.type == HC_TARGET_OUTPUT && cap->output_sources) {
		struct hc_output *out, *match = NULL;
		wl_list_for_each (out, &cap->outputs, link) {
			if (t.output_name[0] == '\0' || (out->name && strcmp(out->name, t.output_name) == 0)) {
				match = out;
				break;
			}
		}
		if (match) {
			cap->source = ext_output_image_capture_source_manager_v1_create_source(cap->output_sources,
											       match->output);
			cap->session_target = match;
			LOG(LOG_INFO, "capturing output %s", match->name ? match->name : "?");
		}
	} else if (t.type == HC_TARGET_TOPLEVEL && cap->toplevel_sources && t.app_id[0]) {
		struct hc_toplevel *tl = find_toplevel(cap, t.app_id, t.title);
		if (tl) {
			cap->source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
				cap->toplevel_sources, tl->handle);
			cap->session_target = tl;
			LOG(LOG_INFO, "capturing window %s \"%s\"", tl->app_id, tl->title ? tl->title : "");
		}
	}

	if (cap->source) {
		uint32_t options = t.paint_cursor ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0;
		cap->session = ext_image_copy_capture_manager_v1_create_session(cap->copy_manager, cap->source, options);
		ext_image_copy_capture_session_v1_add_listener(cap->session, &session_listener, cap);
	}
	free_target(&t);
}

/* ------------------------------------------------------------------------ */
/* Outputs, toplevels, registry                                             */

static void output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
			    int32_t subpixel, const char *make, const char *model, int32_t transform)
{
	(void)data;
	(void)o;
	(void)x;
	(void)y;
	(void)pw;
	(void)ph;
	(void)subpixel;
	(void)make;
	(void)model;
	(void)transform;
}

static void output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh)
{
	(void)data;
	(void)o;
	(void)flags;
	(void)w;
	(void)h;
	(void)refresh;
}

static void output_done(void *data, struct wl_output *o)
{
	(void)o;
	struct hc_output *out = data;
	// A newly plugged-in output may be the one we are waiting for.
	if (!out->cap->session)
		out->cap->retry_at = 0;
}

static void output_scale(void *data, struct wl_output *o, int32_t factor)
{
	(void)data;
	(void)o;
	(void)factor;
}

static void output_name(void *data, struct wl_output *o, const char *name)
{
	(void)o;
	struct hc_output *out = data;
	free(out->name);
	out->name = strdup(name);
}

static void output_description(void *data, struct wl_output *o, const char *description)
{
	(void)data;
	(void)o;
	(void)description;
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

static void destroy_output(struct hc_output *out)
{
	if (wl_output_get_version(out->output) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
		wl_output_release(out->output);
	else
		wl_output_destroy(out->output);
	wl_list_remove(&out->link);
	free(out->name);
	free(out);
}

static void toplevel_closed(void *data, struct ext_foreign_toplevel_handle_v1 *handle)
{
	struct hc_toplevel *tl = data;
	struct hc_capture *cap = tl->cap;
	if (cap->session_target == tl) {
		stop_session(cap);
		cap->retry_at = 0;
	}
	ext_foreign_toplevel_handle_v1_destroy(handle);
	wl_list_remove(&tl->link);
	free(tl->app_id);
	free(tl->title);
	free(tl->pending_app_id);
	free(tl->pending_title);
	free(tl);
}

static void toplevel_done(void *data, struct ext_foreign_toplevel_handle_v1 *handle)
{
	(void)handle;
	struct hc_toplevel *tl = data;
	if (tl->pending_app_id) {
		free(tl->app_id);
		tl->app_id = tl->pending_app_id;
		tl->pending_app_id = NULL;
	}
	if (tl->pending_title) {
		free(tl->title);
		tl->title = tl->pending_title;
		tl->pending_title = NULL;
	}
	tl->ready = true;
	if (!tl->cap->session)
		tl->cap->retry_at = 0;
}

static void toplevel_title(void *data, struct ext_foreign_toplevel_handle_v1 *handle, const char *title)
{
	(void)handle;
	struct hc_toplevel *tl = data;
	free(tl->pending_title);
	tl->pending_title = strdup(title);
}

static void toplevel_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *handle, const char *app_id)
{
	(void)handle;
	struct hc_toplevel *tl = data;
	free(tl->pending_app_id);
	tl->pending_app_id = strdup(app_id);
}

static void toplevel_identifier(void *data, struct ext_foreign_toplevel_handle_v1 *handle, const char *identifier)
{
	(void)data;
	(void)handle;
	(void)identifier;
}

static const struct ext_foreign_toplevel_handle_v1_listener toplevel_listener = {
	.closed = toplevel_closed,
	.done = toplevel_done,
	.title = toplevel_title,
	.app_id = toplevel_app_id,
	.identifier = toplevel_identifier,
};

static void list_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *list,
			  struct ext_foreign_toplevel_handle_v1 *handle)
{
	(void)list;
	struct hc_capture *cap = data;
	struct hc_toplevel *tl = calloc(1, sizeof(*tl));
	tl->cap = cap;
	tl->handle = handle;
	wl_list_insert(&cap->toplevels, &tl->link);
	ext_foreign_toplevel_handle_v1_add_listener(handle, &toplevel_listener, tl);
}

static void list_finished(void *data, struct ext_foreign_toplevel_list_v1 *list)
{
	(void)data;
	(void)list;
}

static const struct ext_foreign_toplevel_list_v1_listener toplevel_list_listener = {
	.toplevel = list_toplevel,
	.finished = list_finished,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
			    uint32_t version)
{
	struct hc_capture *cap = data;

	if (strcmp(interface, wl_output_interface.name) == 0) {
		struct hc_output *out = calloc(1, sizeof(*out));
		out->cap = cap;
		out->global_name = name;
		out->output = wl_registry_bind(registry, name, &wl_output_interface, version < 4 ? version : 4);
		wl_output_add_listener(out->output, &output_listener, out);
		wl_list_insert(cap->outputs.prev, &out->link);
	} else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0) {
		cap->copy_manager = wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1);
	} else if (strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0) {
		cap->output_sources =
			wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, 1);
	} else if (strcmp(interface, ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) == 0) {
		cap->toplevel_sources = wl_registry_bind(
			registry, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
	} else if (strcmp(interface, ext_foreign_toplevel_list_v1_interface.name) == 0) {
		cap->toplevel_list = wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, 1);
		ext_foreign_toplevel_list_v1_add_listener(cap->toplevel_list, &toplevel_list_listener, cap);
	} else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0 && version >= 3) {
		cap->dmabuf = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 3);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)registry;
	struct hc_capture *cap = data;
	struct hc_output *out, *tmp;
	wl_list_for_each_safe (out, tmp, &cap->outputs, link) {
		if (out->global_name != name)
			continue;
		if (cap->session_target == out) {
			stop_session(cap);
			cap->retry_at = now_ns() + RETRY_INTERVAL_NS;
		}
		destroy_output(out);
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* ------------------------------------------------------------------------ */
/* Connection lifecycle                                                     */

static void disconnect(struct hc_capture *cap)
{
	if (!cap->display)
		return;
	stop_session(cap);

	struct hc_toplevel *tl, *tl_tmp;
	wl_list_for_each_safe (tl, tl_tmp, &cap->toplevels, link) {
		ext_foreign_toplevel_handle_v1_destroy(tl->handle);
		wl_list_remove(&tl->link);
		free(tl->app_id);
		free(tl->title);
		free(tl->pending_app_id);
		free(tl->pending_title);
		free(tl);
	}
	struct hc_output *out, *out_tmp;
	wl_list_for_each_safe (out, out_tmp, &cap->outputs, link)
		destroy_output(out);

	if (cap->toplevel_list)
		ext_foreign_toplevel_list_v1_destroy(cap->toplevel_list);
	if (cap->toplevel_sources)
		ext_foreign_toplevel_image_capture_source_manager_v1_destroy(cap->toplevel_sources);
	if (cap->output_sources)
		ext_output_image_capture_source_manager_v1_destroy(cap->output_sources);
	if (cap->copy_manager)
		ext_image_copy_capture_manager_v1_destroy(cap->copy_manager);
	if (cap->dmabuf)
		zwp_linux_dmabuf_v1_destroy(cap->dmabuf);
	if (cap->registry)
		wl_registry_destroy(cap->registry);
	cap->toplevel_list = NULL;
	cap->toplevel_sources = NULL;
	cap->output_sources = NULL;
	cap->copy_manager = NULL;
	cap->dmabuf = NULL;
	cap->registry = NULL;

	wl_display_disconnect(cap->display);
	cap->display = NULL;

	if (cap->gbm) {
		gbm_device_destroy(cap->gbm);
		close(cap->drm_fd);
		cap->gbm = NULL;
		cap->drm_fd = -1;
	}
}

static bool connect_display(struct hc_capture *cap)
{
	cap->display = wl_display_connect(NULL);
	if (!cap->display)
		return false;
	cap->registry = wl_display_get_registry(cap->display);
	wl_registry_add_listener(cap->registry, &registry_listener, cap);
	// First roundtrip binds globals, second receives output names / toplevels.
	if (wl_display_roundtrip(cap->display) < 0 || wl_display_roundtrip(cap->display) < 0) {
		disconnect(cap);
		return false;
	}

	if (!cap->copy_manager)
		LOG(LOG_ERROR, "compositor does not support ext-image-copy-capture-v1");
	if (!cap->dmabuf)
		LOG(LOG_ERROR, "compositor does not support linux-dmabuf v3+");
	return true;
}

static void *capture_thread(void *data)
{
	struct hc_capture *cap = data;
	bool logged_connect_failure = false;

	while (!atomic_load(&cap->quit)) {
		if (!cap->display) {
			if (!connect_display(cap)) {
				if (!logged_connect_failure)
					LOG(LOG_WARNING, "cannot connect to the Wayland display, retrying");
				logged_connect_failure = true;
				struct pollfd pfd = {.fd = cap->wake_fd, .events = POLLIN};
				poll(&pfd, 1, 1000);
				uint64_t v;
				if (pfd.revents & POLLIN && read(cap->wake_fd, &v, sizeof(v)) < 0)
					continue;
				continue;
			}
			logged_connect_failure = false;
			cap->retry_at = 0;
		}

		if (atomic_exchange(&cap->target_dirty, false)) {
			stop_session(cap);
			cap->retry_at = 0;
		}
		if (cap->session_stopped) {
			stop_session(cap);
			cap->retry_at = now_ns() + RETRY_INTERVAL_NS;
		}
		if (!cap->session && now_ns() >= cap->retry_at) {
			start_session(cap);
			if (!cap->session)
				cap->retry_at = now_ns() + RETRY_INTERVAL_NS;
		}
		if (atomic_load(&cap->want_frame))
			start_frame(cap);

		while (wl_display_prepare_read(cap->display) != 0)
			wl_display_dispatch_pending(cap->display);
		if (wl_display_flush(cap->display) < 0 && errno != EAGAIN) {
			wl_display_cancel_read(cap->display);
			LOG(LOG_WARNING, "lost the Wayland connection, reconnecting");
			disconnect(cap);
			continue;
		}

		struct pollfd pfds[2] = {
			{.fd = wl_display_get_fd(cap->display), .events = POLLIN},
			{.fd = cap->wake_fd, .events = POLLIN},
		};
		int ret = poll(pfds, 2, cap->session ? -1 : IDLE_POLL_MS);
		if (ret > 0 && (pfds[0].revents & POLLIN)) {
			if (wl_display_read_events(cap->display) < 0) {
				LOG(LOG_WARNING, "lost the Wayland connection, reconnecting");
				disconnect(cap);
				continue;
			}
		} else {
			wl_display_cancel_read(cap->display);
		}
		if (ret > 0 && (pfds[0].revents & (POLLERR | POLLHUP))) {
			LOG(LOG_WARNING, "Wayland connection closed, reconnecting");
			disconnect(cap);
			continue;
		}
		if (ret > 0 && (pfds[1].revents & POLLIN)) {
			uint64_t v;
			ssize_t r = read(cap->wake_fd, &v, sizeof(v));
			(void)r;
		}
		if (wl_display_dispatch_pending(cap->display) < 0) {
			LOG(LOG_WARNING, "Wayland protocol error %d, reconnecting", wl_display_get_error(cap->display));
			disconnect(cap);
		}
	}

	disconnect(cap);
	return NULL;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */

static void query_obs_formats(struct hc_capture *cap)
{
	enum gs_dmabuf_flags flags = GS_DMABUF_FLAG_NONE;
	uint32_t *formats = NULL;
	size_t n_formats = 0;

	obs_enter_graphics();
	if (gs_query_dmabuf_capabilities(&flags, &formats, &n_formats)) {
		cap->obs_implicit_modifiers = (flags & GS_DMABUF_FLAG_IMPLICIT_MODIFIERS_SUPPORTED) != 0;
		for (size_t f = 0; f < NUM_FORMATS; f++) {
			bool supported = false;
			for (size_t i = 0; i < n_formats; i++)
				supported |= formats[i] == preferred_formats[f].drm;
			if (!supported)
				continue;
			uint64_t *mods = NULL;
			size_t n_mods = 0;
			if (!gs_query_dmabuf_modifiers_for_format(preferred_formats[f].drm, &mods, &n_mods))
				continue;
			struct modifier_list *list = &cap->obs_mods[f];
			list->present = true;
			list->n = n_mods;
			if (n_mods) {
				list->mods = malloc(n_mods * sizeof(uint64_t));
				memcpy(list->mods, mods, n_mods * sizeof(uint64_t));
			}
			bfree(mods);
		}
	} else {
		LOG(LOG_ERROR, "OBS cannot import dma-bufs on this renderer");
	}
	bfree(formats);
	obs_leave_graphics();
}

struct hc_capture *hc_capture_create(void)
{
	struct hc_capture *cap = calloc(1, sizeof(*cap));
	cap->drm_fd = -1;
	cap->latest = -1;
	cap->capturing = -1;
	wl_list_init(&cap->outputs);
	wl_list_init(&cap->toplevels);
	pthread_mutex_init(&cap->target_mutex, NULL);
	pthread_mutex_init(&cap->buf_mutex, NULL);

	query_obs_formats(cap);

	cap->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (cap->wake_fd < 0 || pthread_create(&cap->thread, NULL, capture_thread, cap) != 0) {
		LOG(LOG_ERROR, "failed to start capture thread");
		return cap;
	}
	cap->thread_started = true;
	return cap;
}

void hc_capture_destroy(struct hc_capture *cap)
{
	if (!cap)
		return;
	if (cap->thread_started) {
		atomic_store(&cap->quit, true);
		wake(cap);
		pthread_join(cap->thread, NULL);
	}
	free_buffers(cap);

	obs_enter_graphics();
	for (int i = 0; i < NUM_BUFFERS; i++)
		if (cap->textures[i].tex)
			gs_texture_destroy(cap->textures[i].tex);
	obs_leave_graphics();

	for (size_t f = 0; f < NUM_FORMATS; f++)
		free_modifier_list(&cap->obs_mods[f]);
	free_target(&cap->target);
	if (cap->wake_fd >= 0)
		close(cap->wake_fd);
	pthread_mutex_destroy(&cap->target_mutex);
	pthread_mutex_destroy(&cap->buf_mutex);
	free(cap);
}

void hc_capture_set_target(struct hc_capture *cap, const struct hc_target *target)
{
	pthread_mutex_lock(&cap->target_mutex);
	free_target(&cap->target);
	cap->target.type = target->type;
	cap->target.output_name = dup_or_empty(target->output_name);
	cap->target.app_id = dup_or_empty(target->app_id);
	cap->target.title = dup_or_empty(target->title);
	cap->target.paint_cursor = target->paint_cursor;
	pthread_mutex_unlock(&cap->target_mutex);

	atomic_store(&cap->target_dirty, true);
	wake(cap);
}

void hc_capture_request_frame(struct hc_capture *cap)
{
	if (!atomic_exchange(&cap->want_frame, true))
		wake(cap);
}

void hc_capture_render(struct hc_capture *cap)
{
	pthread_mutex_lock(&cap->buf_mutex);
	int idx = cap->latest;
	if (idx < 0) {
		pthread_mutex_unlock(&cap->buf_mutex);
		return;
	}

	struct hc_buffer *buf = &cap->buffers[idx];
	struct hc_texture *t = &cap->textures[idx];
	if (t->generation != buf->generation) {
		if (t->tex)
			gs_texture_destroy(t->tex);
		uint64_t mods[MAX_PLANES];
		for (int i = 0; i < MAX_PLANES; i++)
			mods[i] = buf->modifier;
		t->tex = gs_texture_create_from_dmabuf(cap->width, cap->height, cap->drm_format, cap->color_format,
						       buf->n_planes, buf->fds, buf->strides, buf->offsets,
						       buf->modifier == DRM_FORMAT_MOD_INVALID ? NULL : mods);
		t->generation = buf->generation;
		if (!t->tex && !t->failed)
			LOG(LOG_ERROR, "OBS failed to import the captured dma-buf");
		t->failed = !t->tex;
	}

	if (t->tex) {
		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);
		const bool previous_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(true);
		gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
		gs_effect_set_texture_srgb(image, t->tex);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(t->tex, 0, cap->width, cap->height);
		gs_enable_framebuffer_srgb(previous_srgb);
	}
	pthread_mutex_unlock(&cap->buf_mutex);
}

uint32_t hc_capture_width(struct hc_capture *cap)
{
	pthread_mutex_lock(&cap->buf_mutex);
	uint32_t w = cap->latest >= 0 ? cap->width : 0;
	pthread_mutex_unlock(&cap->buf_mutex);
	return w;
}

uint32_t hc_capture_height(struct hc_capture *cap)
{
	pthread_mutex_lock(&cap->buf_mutex);
	uint32_t h = cap->latest >= 0 ? cap->height : 0;
	pthread_mutex_unlock(&cap->buf_mutex);
	return h;
}
