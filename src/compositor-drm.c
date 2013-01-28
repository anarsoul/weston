/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2011 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <assert.h>
#include <sys/mman.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <gbm.h>
#include <libbacklight.h>
#include <libudev.h>

#include "compositor.h"
#include "gl-renderer.h"
#include "pixman-renderer.h"
#include "evdev.h"
#include "launcher-util.h"

static int option_current_mode = 0;
static char *output_name;
static char *output_mode;
static char *output_transform;
static struct wl_list configured_output_list;

enum output_config {
	OUTPUT_CONFIG_INVALID = 0,
	OUTPUT_CONFIG_OFF,
	OUTPUT_CONFIG_PREFERRED,
	OUTPUT_CONFIG_CURRENT,
	OUTPUT_CONFIG_MODE,
	OUTPUT_CONFIG_MODELINE
};

struct drm_configured_output {
	char *name;
	char *mode;
	uint32_t transform;
	int32_t width, height;
	drmModeModeInfo crtc_mode;
	enum output_config config;
	struct wl_list link;
};

struct drm_compositor {
	struct weston_compositor base;

	struct udev *udev;
	struct wl_event_source *drm_source;

	struct udev_monitor *udev_monitor;
	struct wl_event_source *udev_drm_source;

	struct {
		int id;
		int fd;
	} drm;
	struct gbm_device *gbm;
	uint32_t *crtcs;
	int num_crtcs;
	uint32_t crtc_allocator;
	uint32_t connector_allocator;
	struct tty *tty;

	/* we need these parameters in order to not fail drmModeAddFB2()
	 * due to out of bounds dimensions, and then mistakenly set
	 * sprites_are_broken:
	 */
	uint32_t min_width, max_width;
	uint32_t min_height, max_height;
	int no_addfb2;

	struct wl_list sprite_list;
	int sprites_are_broken;
	int sprites_hidden;

	int cursors_are_broken;

	int use_pixman;

	uint32_t prev_state;
};

struct drm_mode {
	struct weston_mode base;
	drmModeModeInfo mode_info;
};

struct drm_output;

struct drm_fb {
	struct drm_output *output;
	uint32_t fb_id, stride, handle, size;
	int fd;
	int is_client_buffer;
	struct weston_buffer_reference buffer_ref;

	/* Used by gbm fbs */
	struct gbm_bo *bo;

	/* Used by dumb fbs */
	void *map;
};

struct drm_output {
	struct weston_output   base;

	char *name;
	uint32_t crtc_id;
	int pipe;
	uint32_t connector_id;
	drmModeCrtcPtr original_crtc;

	int vblank_pending;
	int page_flip_pending;

	struct gbm_surface *surface;
	struct gbm_bo *cursor_bo[2];
	struct weston_plane cursor_plane;
	struct weston_plane fb_plane;
	struct weston_surface *cursor_surface;
	int current_cursor;
	struct drm_fb *current, *next;
	struct backlight *backlight;

	struct drm_fb *dumb[2];
	pixman_image_t *image[2];
	int current_image;
	pixman_region32_t previous_damage;
};

/*
 * An output has a primary display plane plus zero or more sprites for
 * blending display contents.
 */
struct drm_sprite {
	struct wl_list link;

	struct weston_plane plane;

	struct drm_fb *current, *next;
	struct drm_output *output;
	struct drm_compositor *compositor;

	uint32_t possible_crtcs;
	uint32_t plane_id;
	uint32_t count_formats;

	int32_t src_x, src_y;
	uint32_t src_w, src_h;
	uint32_t dest_x, dest_y;
	uint32_t dest_w, dest_h;

	uint32_t formats[];
};

struct drm_seat {
	struct weston_seat base;
	struct wl_list devices_list;
	struct udev_monitor *udev_monitor;
	struct wl_event_source *udev_monitor_source;
	char *seat_id;
};

static void
drm_output_set_cursor(struct drm_output *output);

static int
drm_sprite_crtc_supported(struct weston_output *output_base, uint32_t supported)
{
	struct weston_compositor *ec = output_base->compositor;
	struct drm_compositor *c =(struct drm_compositor *) ec;
	struct drm_output *output = (struct drm_output *) output_base;
	int crtc;

	for (crtc = 0; crtc < c->num_crtcs; crtc++) {
		if (c->crtcs[crtc] != output->crtc_id)
			continue;

		if (supported & (1 << crtc))
			return -1;
	}

	return 0;
}

static void
drm_fb_destroy_callback(struct gbm_bo *bo, void *data)
{
	struct drm_fb *fb = data;
	struct gbm_device *gbm = gbm_bo_get_device(bo);

	if (fb->fb_id)
		drmModeRmFB(gbm_device_get_fd(gbm), fb->fb_id);

	weston_buffer_reference(&fb->buffer_ref, NULL);

	free(data);
}

static struct drm_fb *
drm_fb_create_dumb(struct drm_compositor *ec, unsigned width, unsigned height)
{
	struct drm_fb *fb;
	int ret;

	struct drm_mode_create_dumb create_arg;
	struct drm_mode_destroy_dumb destroy_arg;
	struct drm_mode_map_dumb map_arg;

	fb = calloc(1, sizeof *fb);
	if (!fb)
		return NULL;

	create_arg.bpp = 32;
	create_arg.width = width;
	create_arg.height = height;

	ret = drmIoctl(ec->drm.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_arg);
	if (ret)
		goto err_fb;

	fb->handle = create_arg.handle;
	fb->stride = create_arg.pitch;
	fb->size = create_arg.size;
	fb->fd = ec->drm.fd;

	ret = drmModeAddFB(ec->drm.fd, width, height, 24, 32,
			   fb->stride, fb->handle, &fb->fb_id);
	if (ret)
		goto err_bo;

	memset(&map_arg, 0, sizeof(map_arg));
	map_arg.handle = fb->handle;
	drmIoctl(fb->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_arg);

	if (ret)
		goto err_add_fb;

	fb->map = mmap(0, fb->size, PROT_WRITE,
		       MAP_SHARED, ec->drm.fd, map_arg.offset);
	if (fb->map == MAP_FAILED)
		goto err_add_fb;

	return fb;

err_add_fb:
	drmModeRmFB(ec->drm.fd, fb->fb_id);
err_bo:
	memset(&destroy_arg, 0, sizeof(destroy_arg));
	destroy_arg.handle = create_arg.handle;
	drmIoctl(ec->drm.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_arg);
err_fb:
	free(fb);
	return NULL;
}

static void
drm_fb_destroy_dumb(struct drm_fb *fb)
{
	struct drm_mode_destroy_dumb destroy_arg;

	if (!fb->map)
		return;

	if (fb->fb_id)
		drmModeRmFB(fb->fd, fb->fb_id);

	weston_buffer_reference(&fb->buffer_ref, NULL);

	munmap(fb->map, fb->size);

	memset(&destroy_arg, 0, sizeof(destroy_arg));
	destroy_arg.handle = fb->handle;
	drmIoctl(fb->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_arg);

	free(fb);
}

static struct drm_fb *
drm_fb_get_from_bo(struct gbm_bo *bo,
		   struct drm_compositor *compositor, uint32_t format)
{
	struct drm_fb *fb = gbm_bo_get_user_data(bo);
	uint32_t width, height;
	uint32_t handles[4], pitches[4], offsets[4];
	int ret;

	if (fb)
		return fb;

	fb = calloc(1, sizeof *fb);
	if (!fb)
		return NULL;

	fb->bo = bo;

	width = gbm_bo_get_width(bo);
	height = gbm_bo_get_height(bo);
	fb->stride = gbm_bo_get_stride(bo);
	fb->handle = gbm_bo_get_handle(bo).u32;
	fb->size = fb->stride * height;
	fb->fd = compositor->drm.fd;

	if (compositor->min_width > width || width > compositor->max_width ||
	    compositor->min_height > height ||
	    height > compositor->max_height) {
		weston_log("bo geometry out of bounds\n");
		goto err_free;
	}

	ret = -1;

	if (format && !compositor->no_addfb2) {
		handles[0] = fb->handle;
		pitches[0] = fb->stride;
		offsets[0] = 0;

		ret = drmModeAddFB2(compositor->drm.fd, width, height,
				    format, handles, pitches, offsets,
				    &fb->fb_id, 0);
		if (ret) {
			weston_log("addfb2 failed: %m\n");
			compositor->no_addfb2 = 1;
			compositor->sprites_are_broken = 1;
		}
	}

	if (ret)
		ret = drmModeAddFB(compositor->drm.fd, width, height, 24, 32,
				   fb->stride, fb->handle, &fb->fb_id);

	if (ret) {
		weston_log("failed to create kms fb: %m\n");
		goto err_free;
	}

	gbm_bo_set_user_data(bo, fb, drm_fb_destroy_callback);

	return fb;

err_free:
	free(fb);
	return NULL;
}

static void
drm_fb_set_buffer(struct drm_fb *fb, struct wl_buffer *buffer)
{
	assert(fb->buffer_ref.buffer == NULL);

	fb->is_client_buffer = 1;

	weston_buffer_reference(&fb->buffer_ref, buffer);
}

static void
drm_output_release_fb(struct drm_output *output, struct drm_fb *fb)
{
	if (!fb)
		return;

	if (fb->map &&
            (fb != output->dumb[0] && fb != output->dumb[1])) {
		drm_fb_destroy_dumb(fb);
	} else if (fb->bo) {
		if (fb->is_client_buffer)
			gbm_bo_destroy(fb->bo);
		else
			gbm_surface_release_buffer(output->surface,
						   output->current->bo);
	}
}

static uint32_t
drm_output_check_scanout_format(struct drm_output *output,
				struct weston_surface *es, struct gbm_bo *bo)
{
	uint32_t format;
	pixman_region32_t r;

	format = gbm_bo_get_format(bo);

	switch (format) {
	case GBM_FORMAT_XRGB8888:
		return format;
	case GBM_FORMAT_ARGB8888:
		/* We can only scanout an ARGB buffer if the surface's
		 * opaque region covers the whole output */
		pixman_region32_init(&r);
		pixman_region32_subtract(&r, &output->base.region,
					 &es->opaque);

		if (!pixman_region32_not_empty(&r))
			format = GBM_FORMAT_XRGB8888;
		else
			format = 0;

		pixman_region32_fini(&r);

		return format;
	default:
		return 0;
	}
}

static struct weston_plane *
drm_output_prepare_scanout_surface(struct weston_output *_output,
				   struct weston_surface *es)
{
	struct drm_output *output = (struct drm_output *) _output;
	struct drm_compositor *c =
		(struct drm_compositor *) output->base.compositor;
	struct wl_buffer *buffer = es->buffer_ref.buffer;
	struct gbm_bo *bo;
	uint32_t format;

	if (es->geometry.x != output->base.x ||
	    es->geometry.y != output->base.y ||
	    buffer == NULL || c->gbm == NULL ||
	    buffer->width != output->base.current->width ||
	    buffer->height != output->base.current->height ||
	    output->base.transform != es->buffer_transform ||
	    es->transform.type != TRANSFORM_NONE)
		return NULL;

	bo = gbm_bo_import(c->gbm, GBM_BO_IMPORT_WL_BUFFER,
			   buffer, GBM_BO_USE_SCANOUT);

	/* Unable to use the buffer for scanout */
	if (!bo)
		return NULL;

	format = drm_output_check_scanout_format(output, es, bo);
	if (format == 0) {
		gbm_bo_destroy(bo);
		return NULL;
	}

	output->next = drm_fb_get_from_bo(bo, c, format);
	if (!output->next) {
		gbm_bo_destroy(bo);
		return NULL;
	}

	drm_fb_set_buffer(output->next, buffer);

	return &output->fb_plane;
}

static void
drm_output_render_gl(struct drm_output *output, pixman_region32_t *damage)
{
	struct drm_compositor *c =
		(struct drm_compositor *) output->base.compositor;
	struct gbm_bo *bo;

	c->base.renderer->repaint_output(&output->base, damage);

	bo = gbm_surface_lock_front_buffer(output->surface);
	if (!bo) {
		weston_log("failed to lock front buffer: %m\n");
		return;
	}

	output->next = drm_fb_get_from_bo(bo, c, GBM_FORMAT_XRGB8888);
	if (!output->next) {
		weston_log("failed to get drm_fb for bo\n");
		gbm_surface_release_buffer(output->surface, bo);
		return;
	}
}

static void
drm_output_render_pixman(struct drm_output *output, pixman_region32_t *damage)
{
	struct weston_compositor *ec = output->base.compositor;
	pixman_region32_t total_damage, previous_damage;

	pixman_region32_init(&total_damage);
	pixman_region32_init(&previous_damage);

	pixman_region32_copy(&previous_damage, damage);

	pixman_region32_union(&total_damage, damage, &output->previous_damage);
	pixman_region32_copy(&output->previous_damage, &previous_damage);

	output->current_image ^= 1;

	output->next = output->dumb[output->current_image];
	pixman_renderer_output_set_buffer(&output->base,
					  output->image[output->current_image]);

	ec->renderer->repaint_output(&output->base, &total_damage);

	pixman_region32_fini(&total_damage);
	pixman_region32_fini(&previous_damage);
}

static void
drm_output_render(struct drm_output *output, pixman_region32_t *damage)
{
	struct drm_compositor *c =
		(struct drm_compositor *) output->base.compositor;

	if (c->use_pixman)
		drm_output_render_pixman(output, damage);
	else
		drm_output_render_gl(output, damage);

	pixman_region32_subtract(&c->base.primary_plane.damage,
				 &c->base.primary_plane.damage, damage);
}

static void
drm_output_repaint(struct weston_output *output_base,
		   pixman_region32_t *damage)
{
	struct drm_output *output = (struct drm_output *) output_base;
	struct drm_compositor *compositor =
		(struct drm_compositor *) output->base.compositor;
	struct drm_sprite *s;
	struct drm_mode *mode;
	int ret = 0;

	if (!output->next)
		drm_output_render(output, damage);
	if (!output->next)
		return;

	mode = container_of(output->base.current, struct drm_mode, base);
	if (!output->current) {
		ret = drmModeSetCrtc(compositor->drm.fd, output->crtc_id,
				     output->next->fb_id, 0, 0,
				     &output->connector_id, 1,
				     &mode->mode_info);
		if (ret) {
			weston_log("set mode failed: %m\n");
			return;
		}
	}

	if (drmModePageFlip(compositor->drm.fd, output->crtc_id,
			    output->next->fb_id,
			    DRM_MODE_PAGE_FLIP_EVENT, output) < 0) {
		weston_log("queueing pageflip failed: %m\n");
		return;
	}

	output->page_flip_pending = 1;

	drm_output_set_cursor(output);

	/*
	 * Now, update all the sprite surfaces
	 */
	wl_list_for_each(s, &compositor->sprite_list, link) {
		uint32_t flags = 0, fb_id = 0;
		drmVBlank vbl = {
			.request.type = DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT,
			.request.sequence = 1,
		};

		if ((!s->current && !s->next) ||
		    !drm_sprite_crtc_supported(output_base, s->possible_crtcs))
			continue;

		if (s->next && !compositor->sprites_hidden)
			fb_id = s->next->fb_id;

		ret = drmModeSetPlane(compositor->drm.fd, s->plane_id,
				      output->crtc_id, fb_id, flags,
				      s->dest_x, s->dest_y,
				      s->dest_w, s->dest_h,
				      s->src_x, s->src_y,
				      s->src_w, s->src_h);
		if (ret)
			weston_log("setplane failed: %d: %s\n",
				ret, strerror(errno));

		if (output->pipe > 0)
			vbl.request.type |= DRM_VBLANK_SECONDARY;

		/*
		 * Queue a vblank signal so we know when the surface
		 * becomes active on the display or has been replaced.
		 */
		vbl.request.signal = (unsigned long)s;
		ret = drmWaitVBlank(compositor->drm.fd, &vbl);
		if (ret) {
			weston_log("vblank event request failed: %d: %s\n",
				ret, strerror(errno));
		}

		s->output = output;
		output->vblank_pending = 1;
	}

	return;
}

static void
vblank_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec,
	       void *data)
{
	struct drm_sprite *s = (struct drm_sprite *)data;
	struct drm_output *output = s->output;
	uint32_t msecs;

	output->vblank_pending = 0;

	drm_output_release_fb(output, s->current);
	s->current = s->next;
	s->next = NULL;

	if (!output->page_flip_pending) {
		msecs = sec * 1000 + usec / 1000;
		weston_output_finish_frame(&output->base, msecs);
	}
}

static void
page_flip_handler(int fd, unsigned int frame,
		  unsigned int sec, unsigned int usec, void *data)
{
	struct drm_output *output = (struct drm_output *) data;
	uint32_t msecs;

	output->page_flip_pending = 0;

	drm_output_release_fb(output, output->current);
	output->current = output->next;
	output->next = NULL;

	if (!output->vblank_pending) {
		msecs = sec * 1000 + usec / 1000;
		weston_output_finish_frame(&output->base, msecs);
	}
}

static uint32_t
drm_output_check_sprite_format(struct drm_sprite *s,
			       struct weston_surface *es, struct gbm_bo *bo)
{
	uint32_t i, format;

	format = gbm_bo_get_format(bo);

	if (format == GBM_FORMAT_ARGB8888) {
		pixman_region32_t r;

		pixman_region32_init(&r);
		pixman_region32_subtract(&r, &es->transform.boundingbox,
					 &es->transform.opaque);

		if (!pixman_region32_not_empty(&r))
			format = GBM_FORMAT_XRGB8888;

		pixman_region32_fini(&r);
	}

	for (i = 0; i < s->count_formats; i++)
		if (s->formats[i] == format)
			return format;

	return 0;
}

static int
drm_surface_transform_supported(struct weston_surface *es)
{
	struct weston_matrix *matrix = &es->transform.matrix;
	int i;

	if (es->transform.type == TRANSFORM_NONE)
		return 1;

	for (i = 0; i < 16; i++) {
		switch (i) {
		case 10:
		case 15:
			if (matrix->d[i] != 1.0)
				return 0;
			break;
		case 0:
		case 5:
		case 12:
		case 13:
			break;
		default:
			if (matrix->d[i] != 0.0)
				return 0;
			break;
		}
	}

	return 1;
}

static struct weston_plane *
drm_output_prepare_overlay_surface(struct weston_output *output_base,
				   struct weston_surface *es)
{
	struct weston_compositor *ec = output_base->compositor;
	struct drm_compositor *c =(struct drm_compositor *) ec;
	struct drm_sprite *s;
	int found = 0;
	struct gbm_bo *bo;
	pixman_region32_t dest_rect, src_rect;
	pixman_box32_t *box, tbox;
	uint32_t format;
	wl_fixed_t sx1, sy1, sx2, sy2;

	if (c->gbm == NULL)
		return NULL;

	if (es->buffer_transform != output_base->transform)
		return NULL;

	if (c->sprites_are_broken)
		return NULL;

	if (es->output_mask != (1u << output_base->id))
		return NULL;

	if (es->buffer_ref.buffer == NULL)
		return NULL;

	if (es->alpha != 1.0f)
		return NULL;

	if (wl_buffer_is_shm(es->buffer_ref.buffer))
		return NULL;

	if (!drm_surface_transform_supported(es))
		return NULL;

	wl_list_for_each(s, &c->sprite_list, link) {
		if (!drm_sprite_crtc_supported(output_base, s->possible_crtcs))
			continue;

		if (!s->next) {
			found = 1;
			break;
		}
	}

	/* No sprites available */
	if (!found)
		return NULL;

	bo = gbm_bo_import(c->gbm, GBM_BO_IMPORT_WL_BUFFER,
			   es->buffer_ref.buffer, GBM_BO_USE_SCANOUT);
	if (!bo)
		return NULL;

	format = drm_output_check_sprite_format(s, es, bo);
	if (format == 0) {
		gbm_bo_destroy(bo);
		return NULL;
	}

	s->next = drm_fb_get_from_bo(bo, c, format);
	if (!s->next) {
		gbm_bo_destroy(bo);
		return NULL;
	}

	drm_fb_set_buffer(s->next, es->buffer_ref.buffer);

	box = pixman_region32_extents(&es->transform.boundingbox);
	s->plane.x = box->x1;
	s->plane.y = box->y1;

	/*
	 * Calculate the source & dest rects properly based on actual
	 * position (note the caller has called weston_surface_update_transform()
	 * for us already).
	 */
	pixman_region32_init(&dest_rect);
	pixman_region32_intersect(&dest_rect, &es->transform.boundingbox,
				  &output_base->region);
	pixman_region32_translate(&dest_rect, -output_base->x, -output_base->y);
	box = pixman_region32_extents(&dest_rect);
	tbox = weston_transformed_rect(output_base->width,
				       output_base->height,
				       output_base->transform, *box);
	s->dest_x = tbox.x1;
	s->dest_y = tbox.y1;
	s->dest_w = tbox.x2 - tbox.x1;
	s->dest_h = tbox.y2 - tbox.y1;
	pixman_region32_fini(&dest_rect);

	pixman_region32_init(&src_rect);
	pixman_region32_intersect(&src_rect, &es->transform.boundingbox,
				  &output_base->region);
	box = pixman_region32_extents(&src_rect);

	weston_surface_from_global_fixed(es,
					 wl_fixed_from_int(box->x1),
					 wl_fixed_from_int(box->y1),
					 &sx1, &sy1);
	weston_surface_from_global_fixed(es,
					 wl_fixed_from_int(box->x2),
					 wl_fixed_from_int(box->y2),
					 &sx2, &sy2);

	if (sx1 < 0)
		sx1 = 0;
	if (sy1 < 0)
		sy1 = 0;
	if (sx2 > wl_fixed_from_int(es->geometry.width))
		sx2 = wl_fixed_from_int(es->geometry.width);
	if (sy2 > wl_fixed_from_int(es->geometry.height))
		sy2 = wl_fixed_from_int(es->geometry.height);

	tbox.x1 = sx1;
	tbox.y1 = sy1;
	tbox.x2 = sx2;
	tbox.y2 = sy2;

	tbox = weston_transformed_rect(wl_fixed_from_int(es->geometry.width),
				       wl_fixed_from_int(es->geometry.height),
				       es->buffer_transform, tbox);

	s->src_x = tbox.x1 << 8;
	s->src_y = tbox.y1 << 8;
	s->src_w = (tbox.x2 - tbox.x1) << 8;
	s->src_h = (tbox.y2 - tbox.y1) << 8;
	pixman_region32_fini(&src_rect);

	return &s->plane;
}

static struct weston_plane *
drm_output_prepare_cursor_surface(struct weston_output *output_base,
				  struct weston_surface *es)
{
	struct drm_compositor *c =
		(struct drm_compositor *) output_base->compositor;
	struct drm_output *output = (struct drm_output *) output_base;

	if (c->gbm == NULL)
		return NULL;
	if (output->base.transform != WL_OUTPUT_TRANSFORM_NORMAL)
		return NULL;
	if (output->cursor_surface)
		return NULL;
	if (es->output_mask != (1u << output_base->id))
		return NULL;
	if (c->cursors_are_broken)
		return NULL;
	if (es->buffer_ref.buffer == NULL ||
	    !wl_buffer_is_shm(es->buffer_ref.buffer) ||
	    es->geometry.width > 64 || es->geometry.height > 64)
		return NULL;

	output->cursor_surface = es;

	return &output->cursor_plane;
}

static void
drm_output_set_cursor(struct drm_output *output)
{
	struct weston_surface *es = output->cursor_surface;
	struct drm_compositor *c =
		(struct drm_compositor *) output->base.compositor;
	EGLint handle, stride;
	struct gbm_bo *bo;
	uint32_t buf[64 * 64];
	unsigned char *s;
	int i, x, y;

	output->cursor_surface = NULL;
	if (es == NULL) {
		drmModeSetCursor(c->drm.fd, output->crtc_id, 0, 0, 0);
		return;
	}

	if (es->buffer_ref.buffer &&
	    pixman_region32_not_empty(&output->cursor_plane.damage)) {
		pixman_region32_fini(&output->cursor_plane.damage);
		pixman_region32_init(&output->cursor_plane.damage);
		output->current_cursor ^= 1;
		bo = output->cursor_bo[output->current_cursor];
		memset(buf, 0, sizeof buf);
		stride = wl_shm_buffer_get_stride(es->buffer_ref.buffer);
		s = wl_shm_buffer_get_data(es->buffer_ref.buffer);
		for (i = 0; i < es->geometry.height; i++)
			memcpy(buf + i * 64, s + i * stride,
			       es->geometry.width * 4);

		if (gbm_bo_write(bo, buf, sizeof buf) < 0)
			weston_log("failed update cursor: %m\n");

		handle = gbm_bo_get_handle(bo).s32;
		if (drmModeSetCursor(c->drm.fd,
				     output->crtc_id, handle, 64, 64)) {
			weston_log("failed to set cursor: %m\n");
			c->cursors_are_broken = 1;
		}
	}

	x = es->geometry.x - output->base.x;
	y = es->geometry.y - output->base.y;
	if (output->cursor_plane.x != x || output->cursor_plane.y != y) {
		if (drmModeMoveCursor(c->drm.fd, output->crtc_id, x, y)) {
			weston_log("failed to move cursor: %m\n");
			c->cursors_are_broken = 1;
		}

		output->cursor_plane.x = x;
		output->cursor_plane.y = y;
	}
}

static void
drm_assign_planes(struct weston_output *output)
{
	struct drm_compositor *c =
		(struct drm_compositor *) output->compositor;
	struct drm_output *drm_output = (struct drm_output *) output;
	struct drm_sprite *s;
	struct weston_surface *es, *next;
	pixman_region32_t overlap, surface_overlap;
	struct weston_plane *primary, *next_plane;

	/* Reset the opaque region of the planes */
	pixman_region32_fini(&drm_output->cursor_plane.opaque);
	pixman_region32_init(&drm_output->cursor_plane.opaque);
	pixman_region32_fini(&drm_output->fb_plane.opaque);
	pixman_region32_init(&drm_output->fb_plane.opaque);

	wl_list_for_each (s, &c->sprite_list, link) {
		if (!drm_sprite_crtc_supported(output, s->possible_crtcs))
			continue;

		pixman_region32_fini(&s->plane.opaque);
		pixman_region32_init(&s->plane.opaque);
	}

	/*
	 * Find a surface for each sprite in the output using some heuristics:
	 * 1) size
	 * 2) frequency of update
	 * 3) opacity (though some hw might support alpha blending)
	 * 4) clipping (this can be fixed with color keys)
	 *
	 * The idea is to save on blitting since this should save power.
	 * If we can get a large video surface on the sprite for example,
	 * the main display surface may not need to update at all, and
	 * the client buffer can be used directly for the sprite surface
	 * as we do for flipping full screen surfaces.
	 */
	pixman_region32_init(&overlap);
	primary = &c->base.primary_plane;
	wl_list_for_each_safe(es, next, &c->base.surface_list, link) {
		/* test whether this buffer can ever go into a plane:
		 * non-shm, or small enough to be a cursor
		 */
		if ((es->buffer_ref.buffer &&
		     !wl_buffer_is_shm(es->buffer_ref.buffer)) ||
		    (es->geometry.width <= 64 && es->geometry.height <= 64))
			es->keep_buffer = 1;
		else
			es->keep_buffer = 0;

		pixman_region32_init(&surface_overlap);
		pixman_region32_intersect(&surface_overlap, &overlap,
					  &es->transform.boundingbox);

		next_plane = NULL;
		if (pixman_region32_not_empty(&surface_overlap))
			next_plane = primary;
		if (next_plane == NULL)
			next_plane = drm_output_prepare_cursor_surface(output, es);
		if (next_plane == NULL)
			next_plane = drm_output_prepare_scanout_surface(output, es);
		if (next_plane == NULL)
			next_plane = drm_output_prepare_overlay_surface(output, es);
		if (next_plane == NULL)
			next_plane = primary;
		weston_surface_move_to_plane(es, next_plane);
		if (next_plane == primary)
			pixman_region32_union(&overlap, &overlap,
					      &es->transform.boundingbox);

		pixman_region32_fini(&surface_overlap);
	}
	pixman_region32_fini(&overlap);
}

static void
drm_output_fini_pixman(struct drm_output *output);

static void
drm_output_destroy(struct weston_output *output_base)
{
	struct drm_output *output = (struct drm_output *) output_base;
	struct drm_compositor *c =
		(struct drm_compositor *) output->base.compositor;
	drmModeCrtcPtr origcrtc = output->original_crtc;

	if (output->backlight)
		backlight_destroy(output->backlight);

	/* Turn off hardware cursor */
	drmModeSetCursor(c->drm.fd, output->crtc_id, 0, 0, 0);

	/* Restore original CRTC state */
	drmModeSetCrtc(c->drm.fd, origcrtc->crtc_id, origcrtc->buffer_id,
		       origcrtc->x, origcrtc->y,
		       &output->connector_id, 1, &origcrtc->mode);
	drmModeFreeCrtc(origcrtc);

	c->crtc_allocator &= ~(1 << output->crtc_id);
	c->connector_allocator &= ~(1 << output->connector_id);

	if (c->use_pixman) {
		drm_output_fini_pixman(output);
	} else {
		gl_renderer_output_destroy(output_base);
		gbm_surface_destroy(output->surface);
	}

	weston_plane_release(&output->fb_plane);
	weston_plane_release(&output->cursor_plane);

	weston_output_destroy(&output->base);
	wl_list_remove(&output->base.link);

	free(output->name);
	free(output);
}

static struct drm_mode *
choose_mode (struct drm_output *output, struct weston_mode *target_mode)
{
	struct drm_mode *tmp_mode = NULL, *mode;

	if (output->base.current->width == target_mode->width && 
	    output->base.current->height == target_mode->height &&
	    (output->base.current->refresh == target_mode->refresh ||
	     target_mode->refresh == 0))
		return (struct drm_mode *)output->base.current;

	wl_list_for_each(mode, &output->base.mode_list, base.link) {
		if (mode->mode_info.hdisplay == target_mode->width &&
		    mode->mode_info.vdisplay == target_mode->height) {
			if (mode->mode_info.vrefresh == target_mode->refresh || 
          		    target_mode->refresh == 0) {
				return mode;
			} else if (!tmp_mode) 
				tmp_mode = mode;
		}
	}

	return tmp_mode;
}

static int
drm_output_init_egl(struct drm_output *output, struct drm_compositor *ec);
static int
drm_output_init_pixman(struct drm_output *output, struct drm_compositor *c);

static int
drm_output_switch_mode(struct weston_output *output_base, struct weston_mode *mode)
{
	struct drm_output *output;
	struct drm_mode *drm_mode;
	struct drm_compositor *ec;

	if (output_base == NULL) {
		weston_log("output is NULL.\n");
		return -1;
	}

	if (mode == NULL) {
		weston_log("mode is NULL.\n");
		return -1;
	}

	ec = (struct drm_compositor *)output_base->compositor;
	output = (struct drm_output *)output_base;
	drm_mode  = choose_mode (output, mode);

	if (!drm_mode) {
		weston_log("%s, invalid resolution:%dx%d\n", __func__, mode->width, mode->height);
		return -1;
	}

	if (&drm_mode->base == output->base.current)
		return 0;

	output->base.current->flags = 0;

	output->base.current = &drm_mode->base;
	output->base.current->flags =
		WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;

	/* reset rendering stuff. */
	drm_output_release_fb(output, output->current);
	drm_output_release_fb(output, output->next);
	output->current = output->next = NULL;

	if (ec->use_pixman) {
		drm_output_fini_pixman(output);
		if (drm_output_init_pixman(output, ec) < 0) {
			weston_log("failed to init output pixman state with "
				   "new mode\n");
			return -1;
		}
	} else {
		gl_renderer_output_destroy(&output->base);
		gbm_surface_destroy(output->surface);

		if (drm_output_init_egl(output, ec) < 0) {
			weston_log("failed to init output egl state with "
				   "new mode");
			return -1;
		}
	}

	return 0;
}

static int
on_drm_input(int fd, uint32_t mask, void *data)
{
	drmEventContext evctx;

	memset(&evctx, 0, sizeof evctx);
	evctx.version = DRM_EVENT_CONTEXT_VERSION;
	evctx.page_flip_handler = page_flip_handler;
	evctx.vblank_handler = vblank_handler;
	drmHandleEvent(fd, &evctx);

	return 1;
}

static int
init_drm(struct drm_compositor *ec, struct udev_device *device)
{
	const char *filename, *sysnum;
	int fd;

	sysnum = udev_device_get_sysnum(device);
	if (sysnum)
		ec->drm.id = atoi(sysnum);
	if (!sysnum || ec->drm.id < 0) {
		weston_log("cannot get device sysnum\n");
		return -1;
	}

	filename = udev_device_get_devnode(device);
	fd = open(filename, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		/* Probably permissions error */
		weston_log("couldn't open %s, skipping\n",
			udev_device_get_devnode(device));
		return -1;
	}

	weston_log("using %s\n", filename);

	ec->drm.fd = fd;


	return 0;
}

static int
init_egl(struct drm_compositor *ec)
{
	ec->gbm = gbm_create_device(ec->drm.fd);

	if (gl_renderer_create(&ec->base, ec->gbm, gl_renderer_opaque_attribs,
			NULL) < 0) {
		gbm_device_destroy(ec->gbm);
		return -1;
	}

	return 0;
}

static int
init_pixman(struct drm_compositor *ec)
{
	return pixman_renderer_init(&ec->base);
}

static struct drm_mode *
drm_output_add_mode(struct drm_output *output, drmModeModeInfo *info)
{
	struct drm_mode *mode;
	uint64_t refresh;

	mode = malloc(sizeof *mode);
	if (mode == NULL)
		return NULL;

	mode->base.flags = 0;
	mode->base.width = info->hdisplay;
	mode->base.height = info->vdisplay;

	/* Calculate higher precision (mHz) refresh rate */
	refresh = (info->clock * 1000000LL / info->htotal +
		   info->vtotal / 2) / info->vtotal;

	if (info->flags & DRM_MODE_FLAG_INTERLACE)
		refresh *= 2;
	if (info->flags & DRM_MODE_FLAG_DBLSCAN)
		refresh /= 2;
	if (info->vscan > 1)
	    refresh /= info->vscan;

	mode->base.refresh = refresh;
	mode->mode_info = *info;

	if (info->type & DRM_MODE_TYPE_PREFERRED)
		mode->base.flags |= WL_OUTPUT_MODE_PREFERRED;

	wl_list_insert(output->base.mode_list.prev, &mode->base.link);

	return mode;
}

static int
drm_subpixel_to_wayland(int drm_value)
{
	switch (drm_value) {
	default:
	case DRM_MODE_SUBPIXEL_UNKNOWN:
		return WL_OUTPUT_SUBPIXEL_UNKNOWN;
	case DRM_MODE_SUBPIXEL_NONE:
		return WL_OUTPUT_SUBPIXEL_NONE;
	case DRM_MODE_SUBPIXEL_HORIZONTAL_RGB:
		return WL_OUTPUT_SUBPIXEL_HORIZONTAL_RGB;
	case DRM_MODE_SUBPIXEL_HORIZONTAL_BGR:
		return WL_OUTPUT_SUBPIXEL_HORIZONTAL_BGR;
	case DRM_MODE_SUBPIXEL_VERTICAL_RGB:
		return WL_OUTPUT_SUBPIXEL_VERTICAL_RGB;
	case DRM_MODE_SUBPIXEL_VERTICAL_BGR:
		return WL_OUTPUT_SUBPIXEL_VERTICAL_BGR;
	}
}

/* returns a value between 0-255 range, where higher is brighter */
static uint32_t
drm_get_backlight(struct drm_output *output)
{
	long brightness, max_brightness, norm;

	brightness = backlight_get_brightness(output->backlight);
	max_brightness = backlight_get_max_brightness(output->backlight);

	/* convert it on a scale of 0 to 255 */
	norm = (brightness * 255)/(max_brightness);

	return (uint32_t) norm;
}

/* values accepted are between 0-255 range */
static void
drm_set_backlight(struct weston_output *output_base, uint32_t value)
{
	struct drm_output *output = (struct drm_output *) output_base;
	long max_brightness, new_brightness;

	if (!output->backlight)
		return;

	if (value > 255)
		return;

	max_brightness = backlight_get_max_brightness(output->backlight);

	/* get denormalized value */
	new_brightness = (value * max_brightness) / 255;

	backlight_set_brightness(output->backlight, new_brightness);
}

static drmModePropertyPtr
drm_get_prop(int fd, drmModeConnectorPtr connector, const char *name)
{
	drmModePropertyPtr props;
	int i;

	for (i = 0; i < connector->count_props; i++) {
		props = drmModeGetProperty(fd, connector->props[i]);
		if (!props)
			continue;

		if (!strcmp(props->name, name))
			return props;

		drmModeFreeProperty(props);
	}

	return NULL;
}

static void
drm_set_dpms(struct weston_output *output_base, enum dpms_enum level)
{
	struct drm_output *output = (struct drm_output *) output_base;
	struct weston_compositor *ec = output_base->compositor;
	struct drm_compositor *c = (struct drm_compositor *) ec;
	drmModeConnectorPtr connector;
	drmModePropertyPtr prop;

	connector = drmModeGetConnector(c->drm.fd, output->connector_id);
	if (!connector)
		return;

	prop = drm_get_prop(c->drm.fd, connector, "DPMS");
	if (!prop) {
		drmModeFreeConnector(connector);
		return;
	}

	drmModeConnectorSetProperty(c->drm.fd, connector->connector_id,
				    prop->prop_id, level);
	drmModeFreeProperty(prop);
	drmModeFreeConnector(connector);
}

static const char *connector_type_names[] = {
	"None",
	"VGA",
	"DVI",
	"DVI",
	"DVI",
	"Composite",
	"TV",
	"LVDS",
	"CTV",
	"DIN",
	"DP",
	"HDMI",
	"HDMI",
	"TV",
	"eDP",
};

static int
find_crtc_for_connector(struct drm_compositor *ec,
			drmModeRes *resources, drmModeConnector *connector)
{
	drmModeEncoder *encoder;
	uint32_t possible_crtcs;
	int i, j;

	for (j = 0; j < connector->count_encoders; j++) {
		encoder = drmModeGetEncoder(ec->drm.fd, connector->encoders[j]);
		if (encoder == NULL) {
			weston_log("Failed to get encoder.\n");
			return -1;
		}
		possible_crtcs = encoder->possible_crtcs;
		drmModeFreeEncoder(encoder);

		for (i = 0; i < resources->count_crtcs; i++) {
			if (possible_crtcs & (1 << i) &&
			    !(ec->crtc_allocator & (1 << resources->crtcs[i])))
				return i;
		}
	}

	return -1;
}

/* Init output state that depends on gl or gbm */
static int
drm_output_init_egl(struct drm_output *output, struct drm_compositor *ec)
{
	int i, flags;

	output->surface = gbm_surface_create(ec->gbm,
					     output->base.current->width,
					     output->base.current->height,
					     GBM_FORMAT_XRGB8888,
					     GBM_BO_USE_SCANOUT |
					     GBM_BO_USE_RENDERING);
	if (!output->surface) {
		weston_log("failed to create gbm surface\n");
		return -1;
	}

	if (gl_renderer_output_create(&output->base, output->surface) < 0) {
		weston_log("failed to create gl renderer output state\n");
		gbm_surface_destroy(output->surface);
		return -1;
	}

	flags = GBM_BO_USE_CURSOR_64X64 | GBM_BO_USE_WRITE;

	for (i = 0; i < 2; i++) {
		if (output->cursor_bo[i])
			continue;

		output->cursor_bo[i] =
			gbm_bo_create(ec->gbm, 64, 64, GBM_FORMAT_ARGB8888,
				      flags);
	}

	if (output->cursor_bo[0] == NULL || output->cursor_bo[1] == NULL) {
		weston_log("cursor buffers unavailable, using gl cursors\n");
		ec->cursors_are_broken = 1;
	}

	return 0;
}

static int
drm_output_init_pixman(struct drm_output *output, struct drm_compositor *c)
{
	int w = output->base.current->width;
	int h = output->base.current->height;
	unsigned int i;

	/* FIXME error checking */

	for (i = 0; i < ARRAY_LENGTH(output->dumb); i++) {
		output->dumb[i] = drm_fb_create_dumb(c, w, h);
		if (!output->dumb[i])
			goto err;

		output->image[i] =
			pixman_image_create_bits(PIXMAN_x8r8g8b8, w, h,
						 output->dumb[i]->map,
						 output->dumb[i]->stride);
		if (!output->image[i])
			goto err;
	}

	if (pixman_renderer_output_create(&output->base) < 0)
		goto err;

	pixman_region32_init_rect(&output->previous_damage,
				  output->base.x, output->base.y, w, h);

	return 0;

err:
	for (i = 0; i < ARRAY_LENGTH(output->dumb); i++) {
		if (output->dumb[i])
			drm_fb_destroy_dumb(output->dumb[i]);
		if (output->image[i])
			pixman_image_unref(output->image[i]);

		output->dumb[i] = NULL;
		output->image[i] = NULL;
	}

	return -1;
}

static void
drm_output_fini_pixman(struct drm_output *output)
{
	unsigned int i;

	pixman_renderer_output_destroy(&output->base);
	pixman_region32_fini(&output->previous_damage);

	for (i = 0; i < ARRAY_LENGTH(output->dumb); i++) {
		drm_fb_destroy_dumb(output->dumb[i]);
		pixman_image_unref(output->image[i]);
		output->dumb[i] = NULL;
		output->image[i] = NULL;
	}
}

static int
create_output_for_connector(struct drm_compositor *ec,
			    drmModeRes *resources,
			    drmModeConnector *connector,
			    int x, int y, struct udev_device *drm_device)
{
	struct drm_output *output;
	struct drm_mode *drm_mode, *next, *preferred, *current, *configured;
	struct weston_mode *m;
	struct drm_configured_output *o = NULL, *temp;
	drmModeEncoder *encoder;
	drmModeModeInfo crtc_mode;
	drmModeCrtc *crtc;
	int i;
	char name[32];
	const char *type_name;

	i = find_crtc_for_connector(ec, resources, connector);
	if (i < 0) {
		weston_log("No usable crtc/encoder pair for connector.\n");
		return -1;
	}

	output = malloc(sizeof *output);
	if (output == NULL)
		return -1;

	memset(output, 0, sizeof *output);
	output->base.subpixel = drm_subpixel_to_wayland(connector->subpixel);
	output->base.make = "unknown";
	output->base.model = "unknown";
	wl_list_init(&output->base.mode_list);

	if (connector->connector_type < ARRAY_LENGTH(connector_type_names))
		type_name = connector_type_names[connector->connector_type];
	else
		type_name = "UNKNOWN";
	snprintf(name, 32, "%s%d", type_name, connector->connector_type_id);
	output->name = strdup(name);

	output->crtc_id = resources->crtcs[i];
	output->pipe = i;
	ec->crtc_allocator |= (1 << output->crtc_id);
	output->connector_id = connector->connector_id;
	ec->connector_allocator |= (1 << output->connector_id);

	output->original_crtc = drmModeGetCrtc(ec->drm.fd, output->crtc_id);

	/* Get the current mode on the crtc that's currently driving
	 * this connector. */
	encoder = drmModeGetEncoder(ec->drm.fd, connector->encoder_id);
	memset(&crtc_mode, 0, sizeof crtc_mode);
	if (encoder != NULL) {
		crtc = drmModeGetCrtc(ec->drm.fd, encoder->crtc_id);
		drmModeFreeEncoder(encoder);
		if (crtc == NULL)
			goto err_free;
		if (crtc->mode_valid)
			crtc_mode = crtc->mode;
		drmModeFreeCrtc(crtc);
	}

	for (i = 0; i < connector->count_modes; i++) {
		drm_mode = drm_output_add_mode(output, &connector->modes[i]);
		if (!drm_mode)
			goto err_free;
	}

	preferred = NULL;
	current = NULL;
	configured = NULL;

	wl_list_for_each(temp, &configured_output_list, link) {
		if (strcmp(temp->name, output->name) == 0) {
			if (temp->mode)
				weston_log("%s mode \"%s\" in config\n",
							temp->name, temp->mode);
			o = temp;
			break;
		}
	}

	if (o && o->config == OUTPUT_CONFIG_OFF) {
		weston_log("Disabling output %s\n", o->name);

		drmModeSetCrtc(ec->drm.fd, output->crtc_id,
							0, 0, 0, 0, 0, NULL);
		goto err_free;
	}

	wl_list_for_each(drm_mode, &output->base.mode_list, base.link) {
		if (o && o->config == OUTPUT_CONFIG_MODE &&
			o->width == drm_mode->base.width &&
			o->height == drm_mode->base.height)
			configured = drm_mode;
		if (!memcmp(&crtc_mode, &drm_mode->mode_info, sizeof crtc_mode))
			current = drm_mode;
		if (drm_mode->base.flags & WL_OUTPUT_MODE_PREFERRED)
			preferred = drm_mode;
	}

	if (o && o->config == OUTPUT_CONFIG_MODELINE) {
		configured = drm_output_add_mode(output, &o->crtc_mode);
		if (!configured)
			goto err_free;
		current = configured;
	}

	if (current == NULL && crtc_mode.clock != 0) {
		current = drm_output_add_mode(output, &crtc_mode);
		if (!current)
			goto err_free;
	}

	if (o && o->config == OUTPUT_CONFIG_CURRENT)
		configured = current;

	if (option_current_mode && current)
		output->base.current = &current->base;
	else if (configured)
		output->base.current = &configured->base;
	else if (preferred)
		output->base.current = &preferred->base;
	else if (current)
		output->base.current = &current->base;

	if (output->base.current == NULL) {
		weston_log("no available modes for %s\n", output->name);
		goto err_free;
	}

	output->base.current->flags |= WL_OUTPUT_MODE_CURRENT;

	weston_output_init(&output->base, &ec->base, x, y,
			   connector->mmWidth, connector->mmHeight,
			   o ? o->transform : WL_OUTPUT_TRANSFORM_NORMAL);

	if (ec->use_pixman) {
		if (drm_output_init_pixman(output, ec) < 0) {
			weston_log("Failed to init output pixman state\n");
			goto err_output;
		}
	} else if (drm_output_init_egl(output, ec) < 0) {
		weston_log("Failed to init output gl state\n");
		goto err_output;
	}

	output->backlight = backlight_init(drm_device,
					   connector->connector_type);
	if (output->backlight) {
		output->base.set_backlight = drm_set_backlight;
		output->base.backlight_current = drm_get_backlight(output);
	}

	wl_list_insert(ec->base.output_list.prev, &output->base.link);

	output->base.origin = output->base.current;
	output->base.repaint = drm_output_repaint;
	output->base.destroy = drm_output_destroy;
	output->base.assign_planes = drm_assign_planes;
	output->base.set_dpms = drm_set_dpms;
	output->base.switch_mode = drm_output_switch_mode;

	weston_plane_init(&output->cursor_plane, 0, 0);
	weston_plane_init(&output->fb_plane, 0, 0);

	weston_log("Output %s, (connector %d, crtc %d)\n",
		   output->name, output->connector_id, output->crtc_id);
	wl_list_for_each(m, &output->base.mode_list, link)
		weston_log_continue("  mode %dx%d@%.1f%s%s%s\n",
				    m->width, m->height, m->refresh / 1000.0,
				    m->flags & WL_OUTPUT_MODE_PREFERRED ?
				    ", preferred" : "",
				    m->flags & WL_OUTPUT_MODE_CURRENT ?
				    ", current" : "",
				    connector->count_modes == 0 ?
				    ", built-in" : "");

	return 0;

err_output:
	weston_output_destroy(&output->base);
err_free:
	wl_list_for_each_safe(drm_mode, next, &output->base.mode_list,
							base.link) {
		wl_list_remove(&drm_mode->base.link);
		free(drm_mode);
	}

	drmModeFreeCrtc(output->original_crtc);
	ec->crtc_allocator &= ~(1 << output->crtc_id);
	ec->connector_allocator &= ~(1 << output->connector_id);
	free(output->name);
	free(output);

	return -1;
}

static void
create_sprites(struct drm_compositor *ec)
{
	struct drm_sprite *sprite;
	drmModePlaneRes *plane_res;
	drmModePlane *plane;
	uint32_t i;

	plane_res = drmModeGetPlaneResources(ec->drm.fd);
	if (!plane_res) {
		weston_log("failed to get plane resources: %s\n",
			strerror(errno));
		return;
	}

	for (i = 0; i < plane_res->count_planes; i++) {
		plane = drmModeGetPlane(ec->drm.fd, plane_res->planes[i]);
		if (!plane)
			continue;

		sprite = malloc(sizeof(*sprite) + ((sizeof(uint32_t)) *
						   plane->count_formats));
		if (!sprite) {
			weston_log("%s: out of memory\n",
				__func__);
			free(plane);
			continue;
		}

		memset(sprite, 0, sizeof *sprite);

		sprite->possible_crtcs = plane->possible_crtcs;
		sprite->plane_id = plane->plane_id;
		sprite->current = NULL;
		sprite->next = NULL;
		sprite->compositor = ec;
		sprite->count_formats = plane->count_formats;
		memcpy(sprite->formats, plane->formats,
		       plane->count_formats * sizeof(plane->formats[0]));
		drmModeFreePlane(plane);
		weston_plane_init(&sprite->plane, 0, 0);

		wl_list_insert(&ec->sprite_list, &sprite->link);
	}

	free(plane_res->planes);
	free(plane_res);
}

static void
destroy_sprites(struct drm_compositor *compositor)
{
	struct drm_sprite *sprite, *next;
	struct drm_output *output;

	output = container_of(compositor->base.output_list.next,
			      struct drm_output, base.link);

	wl_list_for_each_safe(sprite, next, &compositor->sprite_list, link) {
		drmModeSetPlane(compositor->drm.fd,
				sprite->plane_id,
				output->crtc_id, 0, 0,
				0, 0, 0, 0, 0, 0, 0, 0);
		drm_output_release_fb(output, sprite->current);
		drm_output_release_fb(output, sprite->next);
		weston_plane_release(&sprite->plane);
		free(sprite);
	}
}

static int
create_outputs(struct drm_compositor *ec, uint32_t option_connector,
	       struct udev_device *drm_device)
{
	drmModeConnector *connector;
	drmModeRes *resources;
	int i;
	int x = 0, y = 0;

	resources = drmModeGetResources(ec->drm.fd);
	if (!resources) {
		weston_log("drmModeGetResources failed\n");
		return -1;
	}

	ec->crtcs = calloc(resources->count_crtcs, sizeof(uint32_t));
	if (!ec->crtcs) {
		drmModeFreeResources(resources);
		return -1;
	}

	ec->min_width  = resources->min_width;
	ec->max_width  = resources->max_width;
	ec->min_height = resources->min_height;
	ec->max_height = resources->max_height;

	ec->num_crtcs = resources->count_crtcs;
	memcpy(ec->crtcs, resources->crtcs, sizeof(uint32_t) * ec->num_crtcs);

	for (i = 0; i < resources->count_connectors; i++) {
		connector = drmModeGetConnector(ec->drm.fd,
						resources->connectors[i]);
		if (connector == NULL)
			continue;

		if (connector->connection == DRM_MODE_CONNECTED &&
		    (option_connector == 0 ||
		     connector->connector_id == option_connector)) {
			if (create_output_for_connector(ec, resources,
							connector, x, y,
							drm_device) < 0) {
				drmModeFreeConnector(connector);
				continue;
			}

			x += container_of(ec->base.output_list.prev,
					  struct weston_output,
					  link)->width;
		}

		drmModeFreeConnector(connector);
	}

	if (wl_list_empty(&ec->base.output_list)) {
		weston_log("No currently active connector found.\n");
		drmModeFreeResources(resources);
		return -1;
	}

	drmModeFreeResources(resources);

	return 0;
}

static void
update_outputs(struct drm_compositor *ec, struct udev_device *drm_device)
{
	drmModeConnector *connector;
	drmModeRes *resources;
	struct drm_output *output, *next;
	int x = 0, y = 0;
	int x_offset = 0, y_offset = 0;
	uint32_t connected = 0, disconnects = 0;
	int i;

	resources = drmModeGetResources(ec->drm.fd);
	if (!resources) {
		weston_log("drmModeGetResources failed\n");
		return;
	}

	/* collect new connects */
	for (i = 0; i < resources->count_connectors; i++) {
		int connector_id = resources->connectors[i];

		connector = drmModeGetConnector(ec->drm.fd, connector_id);
		if (connector == NULL)
			continue;

		if (connector->connection != DRM_MODE_CONNECTED) {
			drmModeFreeConnector(connector);
			continue;
		}

		connected |= (1 << connector_id);

		if (!(ec->connector_allocator & (1 << connector_id))) {
			struct weston_output *last =
				container_of(ec->base.output_list.prev,
					     struct weston_output, link);

			/* XXX: not yet needed, we die with 0 outputs */
			if (!wl_list_empty(&ec->base.output_list))
				x = last->x + last->width;
			else
				x = 0;
			y = 0;
			create_output_for_connector(ec, resources,
						    connector, x, y,
						    drm_device);
			weston_log("connector %d connected\n", connector_id);

		}
		drmModeFreeConnector(connector);
	}
	drmModeFreeResources(resources);

	disconnects = ec->connector_allocator & ~connected;
	if (disconnects) {
		wl_list_for_each_safe(output, next, &ec->base.output_list,
				      base.link) {
			if (x_offset != 0 || y_offset != 0) {
				weston_output_move(&output->base,
						 output->base.x - x_offset,
						 output->base.y - y_offset);
			}

			if (disconnects & (1 << output->connector_id)) {
				disconnects &= ~(1 << output->connector_id);
				weston_log("connector %d disconnected\n",
				       output->connector_id);
				x_offset += output->base.width;
				drm_output_destroy(&output->base);
			}
		}
	}

	/* FIXME: handle zero outputs, without terminating */	
	if (ec->connector_allocator == 0)
		wl_display_terminate(ec->base.wl_display);
}

static int
udev_event_is_hotplug(struct drm_compositor *ec, struct udev_device *device)
{
	const char *sysnum;
	const char *val;

	sysnum = udev_device_get_sysnum(device);
	if (!sysnum || atoi(sysnum) != ec->drm.id)
		return 0;

	val = udev_device_get_property_value(device, "HOTPLUG");
	if (!val)
		return 0;

	return strcmp(val, "1") == 0;
}

static int
udev_drm_event(int fd, uint32_t mask, void *data)
{
	struct drm_compositor *ec = data;
	struct udev_device *event;

	event = udev_monitor_receive_device(ec->udev_monitor);

	if (udev_event_is_hotplug(ec, event))
		update_outputs(ec, event);

	udev_device_unref(event);

	return 1;
}

static void
drm_restore(struct weston_compositor *ec)
{
	struct drm_compositor *d = (struct drm_compositor *) ec;

	if (weston_launcher_drm_set_master(&d->base, d->drm.fd, 0) < 0)
		weston_log("failed to drop master: %m\n");
	tty_reset(d->tty);
}

static const char default_seat[] = "seat0";

static void
device_added(struct udev_device *udev_device, struct drm_seat *master)
{
	struct weston_compositor *c;
	struct evdev_device *device;
	const char *devnode;
	const char *device_seat;
	const char *calibration_values;
	int fd;

	device_seat = udev_device_get_property_value(udev_device, "ID_SEAT");
	if (!device_seat)
		device_seat = default_seat;

	if (strcmp(device_seat, master->seat_id))
		return;

	c = master->base.compositor;
	devnode = udev_device_get_devnode(udev_device);

	/* Use non-blocking mode so that we can loop on read on
	 * evdev_device_data() until all events on the fd are
	 * read.  mtdev_get() also expects this. */
	fd = weston_launcher_open(c, devnode, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		weston_log("opening input device '%s' failed.\n", devnode);
		return;
	}

	device = evdev_device_create(&master->base, devnode, fd);
	if (!device) {
		close(fd);
		weston_log("not using input device '%s'.\n", devnode);
		return;
	}

	calibration_values =
		udev_device_get_property_value(udev_device,
					       "WL_CALIBRATION");

	if (calibration_values && sscanf(calibration_values,
					 "%f %f %f %f %f %f",
					 &device->abs.calibration[0],
					 &device->abs.calibration[1],
					 &device->abs.calibration[2],
					 &device->abs.calibration[3],
					 &device->abs.calibration[4],
					 &device->abs.calibration[5]) == 6) {
		device->abs.apply_calibration = 1;
		weston_log ("Applying calibration: %f %f %f %f %f %f\n",
			    device->abs.calibration[0],
			    device->abs.calibration[1],
			    device->abs.calibration[2],
			    device->abs.calibration[3],
			    device->abs.calibration[4],
			    device->abs.calibration[5]);
	}

	wl_list_insert(master->devices_list.prev, &device->link);
}

static void
evdev_add_devices(struct udev *udev, struct weston_seat *seat_base)
{
	struct drm_seat *seat = (struct drm_seat *) seat_base;
	struct udev_enumerate *e;
	struct udev_list_entry *entry;
	struct udev_device *device;
	const char *path, *sysname;

	e = udev_enumerate_new(udev);
	udev_enumerate_add_match_subsystem(e, "input");
	udev_enumerate_scan_devices(e);
	udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e)) {
		path = udev_list_entry_get_name(entry);
		device = udev_device_new_from_syspath(udev, path);

		sysname = udev_device_get_sysname(device);
		if (strncmp("event", sysname, 5) != 0) {
			udev_device_unref(device);
			continue;
		}

		device_added(device, seat);

		udev_device_unref(device);
	}
	udev_enumerate_unref(e);

	evdev_notify_keyboard_focus(&seat->base, &seat->devices_list);

	if (wl_list_empty(&seat->devices_list)) {
		weston_log(
			"warning: no input devices on entering Weston. "
			"Possible causes:\n"
			"\t- no permissions to read /dev/input/event*\n"
			"\t- seats misconfigured "
			"(Weston backend option 'seat', "
			"udev device property ID_SEAT)\n");
	}
}

static int
evdev_udev_handler(int fd, uint32_t mask, void *data)
{
	struct drm_seat *seat = data;
	struct udev_device *udev_device;
	struct evdev_device *device, *next;
	const char *action;
	const char *devnode;

	udev_device = udev_monitor_receive_device(seat->udev_monitor);
	if (!udev_device)
		return 1;

	action = udev_device_get_action(udev_device);
	if (!action)
		goto out;

	if (strncmp("event", udev_device_get_sysname(udev_device), 5) != 0)
		goto out;

	if (!strcmp(action, "add")) {
		device_added(udev_device, seat);
	}
	else if (!strcmp(action, "remove")) {
		devnode = udev_device_get_devnode(udev_device);
		wl_list_for_each_safe(device, next, &seat->devices_list, link)
			if (!strcmp(device->devnode, devnode)) {
				weston_log("input device %s, %s removed\n",
					   device->devname, device->devnode);
				evdev_device_destroy(device);
				break;
			}
	}

out:
	udev_device_unref(udev_device);

	return 0;
}

static int
evdev_enable_udev_monitor(struct udev *udev, struct weston_seat *seat_base)
{
	struct drm_seat *master = (struct drm_seat *) seat_base;
	struct wl_event_loop *loop;
	struct weston_compositor *c = master->base.compositor;
	int fd;

	master->udev_monitor = udev_monitor_new_from_netlink(udev, "udev");
	if (!master->udev_monitor) {
		weston_log("udev: failed to create the udev monitor\n");
		return 0;
	}

	udev_monitor_filter_add_match_subsystem_devtype(master->udev_monitor,
			"input", NULL);

	if (udev_monitor_enable_receiving(master->udev_monitor)) {
		weston_log("udev: failed to bind the udev monitor\n");
		udev_monitor_unref(master->udev_monitor);
		return 0;
	}

	loop = wl_display_get_event_loop(c->wl_display);
	fd = udev_monitor_get_fd(master->udev_monitor);
	master->udev_monitor_source =
		wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
				     evdev_udev_handler, master);
	if (!master->udev_monitor_source) {
		udev_monitor_unref(master->udev_monitor);
		return 0;
	}

	return 1;
}

static void
evdev_disable_udev_monitor(struct weston_seat *seat_base)
{
	struct drm_seat *seat = (struct drm_seat *) seat_base;

	if (!seat->udev_monitor)
		return;

	udev_monitor_unref(seat->udev_monitor);
	seat->udev_monitor = NULL;
	wl_event_source_remove(seat->udev_monitor_source);
	seat->udev_monitor_source = NULL;
}

static void
drm_led_update(struct weston_seat *seat_base, enum weston_led leds)
{
	struct drm_seat *seat = (struct drm_seat *) seat_base;
	struct evdev_device *device;

	wl_list_for_each(device, &seat->devices_list, link)
		evdev_led_update(device, leds);
}

static void
evdev_input_create(struct weston_compositor *c, struct udev *udev,
		   const char *seat_id)
{
	struct drm_seat *seat;

	seat = malloc(sizeof *seat);
	if (seat == NULL)
		return;

	memset(seat, 0, sizeof *seat);
	weston_seat_init(&seat->base, c);
	seat->base.led_update = drm_led_update;

	wl_list_init(&seat->devices_list);
	seat->seat_id = strdup(seat_id);
	if (!evdev_enable_udev_monitor(udev, &seat->base)) {
		free(seat->seat_id);
		free(seat);
		return;
	}

	evdev_add_devices(udev, &seat->base);
}

static void
evdev_remove_devices(struct weston_seat *seat_base)
{
	struct drm_seat *seat = (struct drm_seat *) seat_base;
	struct evdev_device *device, *next;

	wl_list_for_each_safe(device, next, &seat->devices_list, link)
		evdev_device_destroy(device);

	if (seat->base.seat.keyboard)
		notify_keyboard_focus_out(&seat->base);
}

static void
evdev_input_destroy(struct weston_seat *seat_base)
{
	struct drm_seat *seat = (struct drm_seat *) seat_base;

	evdev_remove_devices(seat_base);
	evdev_disable_udev_monitor(&seat->base);

	weston_seat_release(seat_base);
	free(seat->seat_id);
	free(seat);
}

static void
drm_free_configured_output(struct drm_configured_output *output)
{
	free(output->name);
	free(output->mode);
	free(output);
}

static void
drm_destroy(struct weston_compositor *ec)
{
	struct drm_compositor *d = (struct drm_compositor *) ec;
	struct weston_seat *seat, *next;
	struct drm_configured_output *o, *n;

	wl_list_for_each_safe(seat, next, &ec->seat_list, link)
		evdev_input_destroy(seat);
	wl_list_for_each_safe(o, n, &configured_output_list, link)
		drm_free_configured_output(o);

	wl_event_source_remove(d->udev_drm_source);
	wl_event_source_remove(d->drm_source);

	weston_compositor_shutdown(ec);

	ec->renderer->destroy(ec);

	destroy_sprites(d);

	if (d->gbm)
		gbm_device_destroy(d->gbm);

	if (weston_launcher_drm_set_master(&d->base, d->drm.fd, 0) < 0)
		weston_log("failed to drop master: %m\n");
	tty_destroy(d->tty);

	free(d);
}

static void
drm_compositor_set_modes(struct drm_compositor *compositor)
{
	struct drm_output *output;
	struct drm_mode *drm_mode;
	int ret;

	wl_list_for_each(output, &compositor->base.output_list, base.link) {
		drm_mode = (struct drm_mode *) output->base.current;
		ret = drmModeSetCrtc(compositor->drm.fd, output->crtc_id,
				     output->current->fb_id, 0, 0,
				     &output->connector_id, 1,
				     &drm_mode->mode_info);
		if (ret < 0) {
			weston_log(
				"failed to set mode %dx%d for output at %d,%d: %m\n",
				drm_mode->base.width, drm_mode->base.height, 
				output->base.x, output->base.y);
		}
	}
}

static void
vt_func(struct weston_compositor *compositor, int event)
{
	struct drm_compositor *ec = (struct drm_compositor *) compositor;
	struct weston_seat *seat;
	struct drm_sprite *sprite;
	struct drm_output *output;

	switch (event) {
	case TTY_ENTER_VT:
		weston_log("entering VT\n");
		compositor->focus = 1;
		if (weston_launcher_drm_set_master(&ec->base, ec->drm.fd, 1)) {
			weston_log("failed to set master: %m\n");
			wl_display_terminate(compositor->wl_display);
		}
		compositor->state = ec->prev_state;
		drm_compositor_set_modes(ec);
		weston_compositor_damage_all(compositor);
		wl_list_for_each(seat, &compositor->seat_list, link) {
			evdev_add_devices(ec->udev, seat);
			evdev_enable_udev_monitor(ec->udev, seat);
		}
		break;
	case TTY_LEAVE_VT:
		weston_log("leaving VT\n");
		wl_list_for_each(seat, &compositor->seat_list, link) {
			evdev_disable_udev_monitor(seat);
			evdev_remove_devices(seat);
		}

		compositor->focus = 0;
		ec->prev_state = compositor->state;
		compositor->state = WESTON_COMPOSITOR_SLEEPING;

		/* If we have a repaint scheduled (either from a
		 * pending pageflip or the idle handler), make sure we
		 * cancel that so we don't try to pageflip when we're
		 * vt switched away.  The SLEEPING state will prevent
		 * further attemps at repainting.  When we switch
		 * back, we schedule a repaint, which will process
		 * pending frame callbacks. */

		wl_list_for_each(output, &ec->base.output_list, base.link) {
			output->base.repaint_needed = 0;
			drmModeSetCursor(ec->drm.fd, output->crtc_id, 0, 0, 0);
		}

		output = container_of(ec->base.output_list.next,
				      struct drm_output, base.link);

		wl_list_for_each(sprite, &ec->sprite_list, link)
			drmModeSetPlane(ec->drm.fd,
					sprite->plane_id,
					output->crtc_id, 0, 0,
					0, 0, 0, 0, 0, 0, 0, 0);

		if (weston_launcher_drm_set_master(&ec->base, ec->drm.fd, 0) < 0)
			weston_log("failed to drop master: %m\n");

		break;
	};
}

static void
switch_vt_binding(struct wl_seat *seat, uint32_t time, uint32_t key, void *data)
{
	struct drm_compositor *ec = data;

	tty_activate_vt(ec->tty, key - KEY_F1 + 1);
}

/*
 * Find primary GPU
 * Some systems may have multiple DRM devices attached to a single seat. This
 * function loops over all devices and tries to find a PCI device with the
 * boot_vga sysfs attribute set to 1.
 * If no such device is found, the first DRM device reported by udev is used.
 */
static struct udev_device*
find_primary_gpu(struct drm_compositor *ec, const char *seat)
{
	struct udev_enumerate *e;
	struct udev_list_entry *entry;
	const char *path, *device_seat, *id;
	struct udev_device *device, *drm_device, *pci;

	e = udev_enumerate_new(ec->udev);
	udev_enumerate_add_match_subsystem(e, "drm");
	udev_enumerate_add_match_sysname(e, "card[0-9]*");

	udev_enumerate_scan_devices(e);
	drm_device = NULL;
	udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e)) {
		path = udev_list_entry_get_name(entry);
		device = udev_device_new_from_syspath(ec->udev, path);
		if (!device)
			continue;
		device_seat = udev_device_get_property_value(device, "ID_SEAT");
		if (!device_seat)
			device_seat = default_seat;
		if (strcmp(device_seat, seat)) {
			udev_device_unref(device);
			continue;
		}

		pci = udev_device_get_parent_with_subsystem_devtype(device,
								"pci", NULL);
		if (pci) {
			id = udev_device_get_sysattr_value(pci, "boot_vga");
			if (id && !strcmp(id, "1")) {
				if (drm_device)
					udev_device_unref(drm_device);
				drm_device = device;
				break;
			}
		}

		if (!drm_device)
			drm_device = device;
		else
			udev_device_unref(device);
	}

	udev_enumerate_unref(e);
	return drm_device;
}

static void
planes_binding(struct wl_seat *seat, uint32_t time, uint32_t key, void *data)
{
	struct drm_compositor *c = data;

	switch (key) {
	case KEY_C:
		c->cursors_are_broken ^= 1;
		break;
	case KEY_V:
		c->sprites_are_broken ^= 1;
		break;
	case KEY_O:
		c->sprites_hidden ^= 1;
		break;
	default:
		break;
	}
}

static struct weston_compositor *
drm_compositor_create(struct wl_display *display,
		      int connector, const char *seat, int tty, int pixman,
		      int argc, char *argv[], const char *config_file)
{
	struct drm_compositor *ec;
	struct udev_device *drm_device;
	struct wl_event_loop *loop;
	struct weston_seat *weston_seat, *next;
	const char *path;
	uint32_t key;

	weston_log("initializing drm backend\n");

	ec = malloc(sizeof *ec);
	if (ec == NULL)
		return NULL;
	memset(ec, 0, sizeof *ec);

	/* KMS support for sprites is not complete yet, so disable the
	 * functionality for now. */
	ec->sprites_are_broken = 1;

	ec->use_pixman = pixman;

	if (weston_compositor_init(&ec->base, display, argc, argv,
				   config_file) < 0) {
		weston_log("weston_compositor_init failed\n");
		goto err_base;
	}

	ec->udev = udev_new();
	if (ec->udev == NULL) {
		weston_log("failed to initialize udev context\n");
		goto err_compositor;
	}

	ec->base.wl_display = display;
	ec->tty = tty_create(&ec->base, vt_func, tty);
	if (!ec->tty) {
		weston_log("failed to initialize tty\n");
		goto err_udev;
	}

	drm_device = find_primary_gpu(ec, seat);
	if (drm_device == NULL) {
		weston_log("no drm device found\n");
		goto err_tty;
	}
	path = udev_device_get_syspath(drm_device);

	if (init_drm(ec, drm_device) < 0) {
		weston_log("failed to initialize kms\n");
		goto err_udev_dev;
	}

	if (ec->use_pixman) {
		if (init_pixman(ec) < 0) {
			weston_log("failed to initialize pixman renderer\n");
			goto err_udev_dev;
		}
	} else {
		if (init_egl(ec) < 0) {
			weston_log("failed to initialize egl\n");
			goto err_udev_dev;
		}
	}

	ec->base.destroy = drm_destroy;
	ec->base.restore = drm_restore;

	ec->base.focus = 1;

	ec->prev_state = WESTON_COMPOSITOR_ACTIVE;

	for (key = KEY_F1; key < KEY_F9; key++)
		weston_compositor_add_key_binding(&ec->base, key,
						  MODIFIER_CTRL | MODIFIER_ALT,
						  switch_vt_binding, ec);

	wl_list_init(&ec->sprite_list);
	create_sprites(ec);

	if (create_outputs(ec, connector, drm_device) < 0) {
		weston_log("failed to create output for %s\n", path);
		goto err_sprite;
	}

	path = NULL;

	evdev_input_create(&ec->base, ec->udev, seat);

	loop = wl_display_get_event_loop(ec->base.wl_display);
	ec->drm_source =
		wl_event_loop_add_fd(loop, ec->drm.fd,
				     WL_EVENT_READABLE, on_drm_input, ec);

	ec->udev_monitor = udev_monitor_new_from_netlink(ec->udev, "udev");
	if (ec->udev_monitor == NULL) {
		weston_log("failed to intialize udev monitor\n");
		goto err_drm_source;
	}
	udev_monitor_filter_add_match_subsystem_devtype(ec->udev_monitor,
							"drm", NULL);
	ec->udev_drm_source =
		wl_event_loop_add_fd(loop,
				     udev_monitor_get_fd(ec->udev_monitor),
				     WL_EVENT_READABLE, udev_drm_event, ec);

	if (udev_monitor_enable_receiving(ec->udev_monitor) < 0) {
		weston_log("failed to enable udev-monitor receiving\n");
		goto err_udev_monitor;
	}

	udev_device_unref(drm_device);

	weston_compositor_add_debug_binding(&ec->base, KEY_O,
					    planes_binding, ec);
	weston_compositor_add_debug_binding(&ec->base, KEY_C,
					    planes_binding, ec);
	weston_compositor_add_debug_binding(&ec->base, KEY_V,
					    planes_binding, ec);

	return &ec->base;

err_udev_monitor:
	wl_event_source_remove(ec->udev_drm_source);
	udev_monitor_unref(ec->udev_monitor);
err_drm_source:
	wl_event_source_remove(ec->drm_source);
	wl_list_for_each_safe(weston_seat, next, &ec->base.seat_list, link)
		evdev_input_destroy(weston_seat);
err_sprite:
	ec->base.renderer->destroy(&ec->base);
	gbm_device_destroy(ec->gbm);
	destroy_sprites(ec);
err_udev_dev:
	udev_device_unref(drm_device);
err_tty:
	tty_destroy(ec->tty);
err_udev:
	udev_unref(ec->udev);
err_compositor:
	weston_compositor_shutdown(&ec->base);
err_base:
	free(ec);
	return NULL;
}

static int
set_sync_flags(drmModeModeInfo *mode, char *hsync, char *vsync)
{
	mode->flags = 0;

	if (strcmp(hsync, "+hsync") == 0)
		mode->flags |= DRM_MODE_FLAG_PHSYNC;
	else if (strcmp(hsync, "-hsync") == 0)
		mode->flags |= DRM_MODE_FLAG_NHSYNC;
	else
		return -1;

	if (strcmp(vsync, "+vsync") == 0)
		mode->flags |= DRM_MODE_FLAG_PVSYNC;
	else if (strcmp(vsync, "-vsync") == 0)
		mode->flags |= DRM_MODE_FLAG_NVSYNC;
	else
		return -1;

	return 0;
}

static int
check_for_modeline(struct drm_configured_output *output)
{
	drmModeModeInfo mode;
	char hsync[16];
	char vsync[16];
	char mode_name[16];
	float fclock;

	mode.type = DRM_MODE_TYPE_USERDEF;
	mode.hskew = 0;
	mode.vscan = 0;
	mode.vrefresh = 0;

	if (sscanf(output_mode, "%f %hd %hd %hd %hd %hd %hd %hd %hd %s %s",
						&fclock, &mode.hdisplay,
						&mode.hsync_start,
						&mode.hsync_end, &mode.htotal,
						&mode.vdisplay,
						&mode.vsync_start,
						&mode.vsync_end, &mode.vtotal,
						hsync, vsync) == 11) {
		if (set_sync_flags(&mode, hsync, vsync))
			return -1;

		sprintf(mode_name, "%dx%d", mode.hdisplay, mode.vdisplay);
		strcpy(mode.name, mode_name);

		mode.clock = fclock * 1000;
	} else
		return -1;

	output->crtc_mode = mode;

	return 0;
}

static void
drm_output_set_transform(struct drm_configured_output *output)
{
	if (!output_transform) {
		output->transform = WL_OUTPUT_TRANSFORM_NORMAL;
		return;
	}

	if (!strcmp(output_transform, "normal"))
		output->transform = WL_OUTPUT_TRANSFORM_NORMAL;
	else if (!strcmp(output_transform, "90"))
		output->transform = WL_OUTPUT_TRANSFORM_90;
	else if (!strcmp(output_transform, "180"))
		output->transform = WL_OUTPUT_TRANSFORM_180;
	else if (!strcmp(output_transform, "270"))
		output->transform = WL_OUTPUT_TRANSFORM_270;
	else if (!strcmp(output_transform, "flipped"))
		output->transform = WL_OUTPUT_TRANSFORM_FLIPPED;
	else if (!strcmp(output_transform, "flipped-90"))
		output->transform = WL_OUTPUT_TRANSFORM_FLIPPED_90;
	else if (!strcmp(output_transform, "flipped-180"))
		output->transform = WL_OUTPUT_TRANSFORM_FLIPPED_180;
	else if (!strcmp(output_transform, "flipped-270"))
		output->transform = WL_OUTPUT_TRANSFORM_FLIPPED_270;
	else {
		weston_log("Invalid transform \"%s\" for output %s\n",
						output_transform, output_name);
		output->transform = WL_OUTPUT_TRANSFORM_NORMAL;
	}

	free(output_transform);
	output_transform = NULL;
}

static void
output_section_done(void *data)
{
	struct drm_configured_output *output;

	output = malloc(sizeof *output);

	if (!output || !output_name || (output_name[0] == 'X') ||
					(!output_mode && !output_transform)) {
		free(output_name);
		free(output_mode);
		free(output_transform);
		free(output);
		output_name = NULL;
		output_mode = NULL;
		output_transform = NULL;
		return;
	}

	output->config = OUTPUT_CONFIG_INVALID;
	output->name = output_name;
	output->mode = output_mode;

	if (output_mode) {
		if (strcmp(output_mode, "off") == 0)
			output->config = OUTPUT_CONFIG_OFF;
		else if (strcmp(output_mode, "preferred") == 0)
			output->config = OUTPUT_CONFIG_PREFERRED;
		else if (strcmp(output_mode, "current") == 0)
			output->config = OUTPUT_CONFIG_CURRENT;
		else if (sscanf(output_mode, "%dx%d",
					&output->width, &output->height) == 2)
			output->config = OUTPUT_CONFIG_MODE;
		else if (check_for_modeline(output) == 0)
			output->config = OUTPUT_CONFIG_MODELINE;

		if (output->config == OUTPUT_CONFIG_INVALID)
			weston_log("Invalid mode \"%s\" for output %s\n",
							output_mode, output_name);
		output_mode = NULL;
	}

	drm_output_set_transform(output);

	wl_list_insert(&configured_output_list, &output->link);

	if (output_transform)
		free(output_transform);
	output_transform = NULL;
}

WL_EXPORT struct weston_compositor *
backend_init(struct wl_display *display, int argc, char *argv[],
	     const char *config_file)
{
	int connector = 0, tty = 0, use_pixman = 0;
	const char *seat = default_seat;

	const struct weston_option drm_options[] = {
		{ WESTON_OPTION_INTEGER, "connector", 0, &connector },
		{ WESTON_OPTION_STRING, "seat", 0, &seat },
		{ WESTON_OPTION_INTEGER, "tty", 0, &tty },
		{ WESTON_OPTION_BOOLEAN, "current-mode", 0, &option_current_mode },
		{ WESTON_OPTION_BOOLEAN, "use-pixman", 0, &use_pixman },
	};

	parse_options(drm_options, ARRAY_LENGTH(drm_options), argc, argv);

	wl_list_init(&configured_output_list);

	const struct config_key drm_config_keys[] = {
		{ "name", CONFIG_KEY_STRING, &output_name },
		{ "mode", CONFIG_KEY_STRING, &output_mode },
		{ "transform", CONFIG_KEY_STRING, &output_transform },
	};

	const struct config_section config_section[] = {
		{ "output", drm_config_keys,
		ARRAY_LENGTH(drm_config_keys), output_section_done },
	};

	parse_config_file(config_file, config_section,
				ARRAY_LENGTH(config_section), NULL);

	return drm_compositor_create(display, connector, seat, tty, use_pixman,
				     argc, argv, config_file);
}
