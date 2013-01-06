/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2010-2011 Intel Corporation
 * Copyright © 2013 Vasily Khoruzhick <anarsoul@gmail.com>
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/shm.h>
#include <linux/input.h>

#include <xcb/xcb.h>
#include <xcb/shm.h>
#include <xcb/xcb_aux.h>
#ifdef HAVE_XCB_XKB
#include <xcb/xkb.h>
#endif

#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>

#include <xkbcommon/xkbcommon.h>

#include "compositor.h"
#include "gl-renderer.h"
#include "pixman-renderer.h"
#include "../shared/config-parser.h"
#include "../shared/cairo-util.h"

#define DEFAULT_AXIS_STEP_DISTANCE wl_fixed_from_int(10)

static char *output_name;
static char *output_mode;
static char *output_transform;
static int option_width;
static int option_height;
static int option_count;
static struct wl_list configured_output_list;

struct x11_configured_output {
	char *name;
	int width, height;
	uint32_t transform;
	struct wl_list link;
};

struct x11_compositor {
	struct weston_compositor	 base;

	Display			*dpy;
	xcb_connection_t	*conn;
	xcb_screen_t		*screen;
	xcb_cursor_t		 null_cursor;
	struct wl_array		 keys;
	struct wl_event_source	*xcb_source;
	struct xkb_keymap	*xkb_keymap;
	unsigned int		 has_xkb;
	uint8_t			 xkb_event_base;
	int			 use_shm;

	/* We could map multi-pointer X to multiple wayland seats, but
	 * for now we only support core X input. */
	struct weston_seat	 core_seat;

	struct {
		xcb_atom_t		 wm_protocols;
		xcb_atom_t		 wm_normal_hints;
		xcb_atom_t		 wm_size_hints;
		xcb_atom_t		 wm_delete_window;
		xcb_atom_t		 wm_class;
		xcb_atom_t		 net_wm_name;
		xcb_atom_t		 net_wm_icon;
		xcb_atom_t		 net_wm_state;
		xcb_atom_t		 net_wm_state_fullscreen;
		xcb_atom_t		 string;
		xcb_atom_t		 utf8_string;
		xcb_atom_t		 cardinal;
		xcb_atom_t		 xkb_names;
	} atom;
};

struct x11_output {
	struct weston_output	base;

	xcb_window_t		window;
	struct weston_mode	mode;
	struct wl_event_source *finish_frame_timer;

	xcb_gc_t		gc;
	xcb_shm_seg_t		segment;
	pixman_image_t	       *hw_surface;
	pixman_image_t         *shadow_surface;
	int			shm_id;
	void		       *buf;
	void		       *shadow_buf;
	uint8_t			depth;
};

static struct xkb_keymap *
x11_compositor_get_keymap(struct x11_compositor *c)
{
	xcb_get_property_cookie_t cookie;
	xcb_get_property_reply_t *reply;
	struct xkb_rule_names names;
	struct xkb_keymap *ret;
	const char *value_all, *value_part;
	int length_all, length_part;

	memset(&names, 0, sizeof(names));

	cookie = xcb_get_property(c->conn, 0, c->screen->root,
				  c->atom.xkb_names, c->atom.string, 0, 1024);
	reply = xcb_get_property_reply(c->conn, cookie, NULL);
	if (reply == NULL)
		return NULL;

	value_all = xcb_get_property_value(reply);
	length_all = xcb_get_property_value_length(reply);
	value_part = value_all;

#define copy_prop_value(to) \
	length_part = strlen(value_part); \
	if (value_part + length_part < (value_all + length_all) && \
	    length_part > 0) \
		names.to = value_part; \
	value_part += length_part + 1;

	copy_prop_value(rules);
	copy_prop_value(model);
	copy_prop_value(layout);
	copy_prop_value(variant);
	copy_prop_value(options);
#undef copy_prop_value

	ret = xkb_map_new_from_names(c->base.xkb_context, &names, 0);

	free(reply);
	return ret;
}

static uint32_t
get_xkb_mod_mask(struct x11_compositor *c, uint32_t in)
{
	struct weston_xkb_info *info = &c->core_seat.xkb_info;
	uint32_t ret = 0;

	if ((in & ShiftMask) && info->shift_mod != XKB_MOD_INVALID)
		ret |= (1 << info->shift_mod);
	if ((in & LockMask) && info->caps_mod != XKB_MOD_INVALID)
		ret |= (1 << info->caps_mod);
	if ((in & ControlMask) && info->ctrl_mod != XKB_MOD_INVALID)
		ret |= (1 << info->ctrl_mod);
	if ((in & Mod1Mask) && info->alt_mod != XKB_MOD_INVALID)
		ret |= (1 << info->alt_mod);
	if ((in & Mod2Mask) && info->mod2_mod != XKB_MOD_INVALID)
		ret |= (1 << info->mod2_mod);
	if ((in & Mod3Mask) && info->mod3_mod != XKB_MOD_INVALID)
		ret |= (1 << info->mod3_mod);
	if ((in & Mod4Mask) && info->super_mod != XKB_MOD_INVALID)
		ret |= (1 << info->super_mod);
	if ((in & Mod5Mask) && info->mod5_mod != XKB_MOD_INVALID)
		ret |= (1 << info->mod5_mod);

	return ret;
}

static void
x11_compositor_setup_xkb(struct x11_compositor *c)
{
#ifndef HAVE_XCB_XKB
	weston_log("XCB-XKB not available during build\n");
	c->has_xkb = 0;
	c->xkb_event_base = 0;
	return;
#else
	const xcb_query_extension_reply_t *ext;
	xcb_generic_error_t *error;
	xcb_void_cookie_t select;
	xcb_xkb_use_extension_cookie_t use_ext;
	xcb_xkb_use_extension_reply_t *use_ext_reply;
	xcb_xkb_per_client_flags_cookie_t pcf;
	xcb_xkb_per_client_flags_reply_t *pcf_reply;
	xcb_xkb_get_state_cookie_t state;
	xcb_xkb_get_state_reply_t *state_reply;

	c->has_xkb = 0;
	c->xkb_event_base = 0;

	ext = xcb_get_extension_data(c->conn, &xcb_xkb_id);
	if (!ext) {
		weston_log("XKB extension not available on host X11 server\n");
		return;
	}
	c->xkb_event_base = ext->first_event;

	select = xcb_xkb_select_events_checked(c->conn,
					       XCB_XKB_ID_USE_CORE_KBD,
					       XCB_XKB_EVENT_TYPE_STATE_NOTIFY,
					       0,
					       XCB_XKB_EVENT_TYPE_STATE_NOTIFY,
					       0,
					       0,
					       NULL);
	error = xcb_request_check(c->conn, select);
	if (error) {
		weston_log("error: failed to select for XKB state events\n");
		free(error);
		return;
	}

	use_ext = xcb_xkb_use_extension(c->conn,
					XCB_XKB_MAJOR_VERSION,
					XCB_XKB_MINOR_VERSION);
	use_ext_reply = xcb_xkb_use_extension_reply(c->conn, use_ext, NULL);
	if (!use_ext_reply) {
		weston_log("couldn't start using XKB extension\n");
		return;
	}

	if (!use_ext_reply->supported) {
		weston_log("XKB extension version on the server is too old "
			   "(want %d.%d, has %d.%d)\n",
			   XCB_XKB_MAJOR_VERSION, XCB_XKB_MINOR_VERSION,
			   use_ext_reply->serverMajor, use_ext_reply->serverMinor);
		free(use_ext_reply);
		return;
	}
	free(use_ext_reply);

	pcf = xcb_xkb_per_client_flags(c->conn,
				       XCB_XKB_ID_USE_CORE_KBD,
				       XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT,
				       XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT,
				       0,
				       0,
				       0);
	pcf_reply = xcb_xkb_per_client_flags_reply(c->conn, pcf, NULL);
	if (!pcf_reply ||
	    !(pcf_reply->value & XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT)) {
		weston_log("failed to set XKB per-client flags, not using "
			   "detectable repeat\n");
		free(pcf_reply);
		return;
	}
	free(pcf_reply);

	state = xcb_xkb_get_state(c->conn, XCB_XKB_ID_USE_CORE_KBD);
	state_reply = xcb_xkb_get_state_reply(c->conn, state, NULL);
	if (!state_reply) {
		weston_log("failed to get initial XKB state\n");
		return;
	}

	xkb_state_update_mask(c->core_seat.xkb_state.state,
			      get_xkb_mod_mask(c, state_reply->baseMods),
			      get_xkb_mod_mask(c, state_reply->latchedMods),
			      get_xkb_mod_mask(c, state_reply->lockedMods),
			      0,
			      0,
			      state_reply->group);

	free(state_reply);

	c->has_xkb = 1;
#endif
}

static int
x11_input_create(struct x11_compositor *c, int no_input)
{
	struct xkb_keymap *keymap;

	weston_seat_init(&c->core_seat, &c->base);

	if (no_input)
		return 0;

	weston_seat_init_pointer(&c->core_seat);

	keymap = x11_compositor_get_keymap(c);
	weston_seat_init_keyboard(&c->core_seat, keymap);
	if (keymap)
		xkb_map_unref(keymap);

	x11_compositor_setup_xkb(c);

	return 0;
}

static void
x11_input_destroy(struct x11_compositor *compositor)
{
	weston_seat_release(&compositor->core_seat);
}

static void
x11_output_repaint_gl(struct weston_output *output_base,
		      pixman_region32_t *damage)
{
	struct x11_output *output = (struct x11_output *)output_base;
	struct weston_compositor *ec = output->base.compositor;

	ec->renderer->repaint_output(output_base, damage);

	pixman_region32_subtract(&ec->primary_plane.damage,
				 &ec->primary_plane.damage, damage);

	wl_event_source_timer_update(output->finish_frame_timer, 10);
}

static void
x11_output_repaint_shm(struct weston_output *output_base,
		       pixman_region32_t *damage)
{
	struct x11_output *output = (struct x11_output *)output_base;
	struct weston_compositor *ec = output->base.compositor;
	struct x11_compositor *c = (struct x11_compositor *)ec;
	pixman_box32_t *rects;
	int nrects, i, src_x, src_y, x1, y1, x2, y2, width, height;
	xcb_void_cookie_t cookie;
	xcb_generic_error_t *err;

	pixman_renderer_output_set_buffer(output_base, output->shadow_surface);
	ec->renderer->repaint_output(output_base, damage);

	width = pixman_image_get_width(output->shadow_surface);
	height = pixman_image_get_height(output->shadow_surface);
	rects = pixman_region32_rectangles(damage, &nrects);
	for (i = 0; i < nrects; i++) {
		switch (output_base->transform) {
		default:
		case WL_OUTPUT_TRANSFORM_NORMAL:
			x1 = rects[i].x1;
			x2 = rects[i].x2;
			y1 = rects[i].y1;
			y2 = rects[i].y2;
			break;
		case WL_OUTPUT_TRANSFORM_180:
			x1 = width - rects[i].x2;
			x2 = width - rects[i].x1;
			y1 = height - rects[i].y2;
			y2 = height - rects[i].y1;
			break;
		case WL_OUTPUT_TRANSFORM_90:
			x1 = height - rects[i].y2;
			x2 = height - rects[i].y1;
			y1 = rects[i].x1;
			y2 = rects[i].x2;
			break;
		case WL_OUTPUT_TRANSFORM_270:
			x1 = rects[i].y1;
			x2 = rects[i].y2;
			y1 = width - rects[i].x2;
			y2 = width - rects[i].x1;
			break;
		}
		src_x = x1;
		src_y = y1;

		pixman_image_composite32(PIXMAN_OP_SRC,
			output->shadow_surface, /* src */
			NULL /* mask */,
			output->hw_surface, /* dest */
			src_x, src_y, /* src_x, src_y */
			0, 0, /* mask_x, mask_y */
			x1, y1, /* dest_x, dest_y */
			x2 - x1, /* width */
			y2 - y1 /* height */);
	}

	pixman_region32_subtract(&ec->primary_plane.damage,
				 &ec->primary_plane.damage, damage);
	cookie = xcb_shm_put_image_checked(c->conn, output->window, output->gc,
					pixman_image_get_width(output->hw_surface),
					pixman_image_get_height(output->hw_surface),
					0, 0,
					pixman_image_get_width(output->hw_surface),
					pixman_image_get_height(output->hw_surface),
					0, 0, output->depth, XCB_IMAGE_FORMAT_Z_PIXMAP,
					0, output->segment, 0);
	err = xcb_request_check(c->conn, cookie);
	if (err != NULL) {
		weston_log("Failed to put shm image, err: %d\n", err->error_code);
		free(err);
	}

	wl_event_source_timer_update(output->finish_frame_timer, 10);
}

static int
finish_frame_handler(void *data)
{
	struct x11_output *output = data;
	uint32_t msec;
	struct timeval tv;
	
	gettimeofday(&tv, NULL);
	msec = tv.tv_sec * 1000 + tv.tv_usec / 1000;
	weston_output_finish_frame(&output->base, msec);

	return 1;
}

static void
x11_output_deinit_shm(struct x11_compositor *c, struct x11_output *output)
{
	xcb_void_cookie_t cookie;
	xcb_generic_error_t *err;
	xcb_free_gc(c->conn, output->gc);

	pixman_image_unref(output->hw_surface);
	output->hw_surface = NULL;
	cookie = xcb_shm_detach_checked(c->conn, output->segment);
	err = xcb_request_check(c->conn, cookie);
	if (err) {
		weston_log("xcb_shm_detach failed, error %d\n", err->error_code);
		free(err);
	}
	shmdt(output->buf);

	pixman_image_unref(output->shadow_surface);
	output->shadow_surface = NULL;
	free(output->shadow_buf);
}

static void
x11_output_destroy(struct weston_output *output_base)
{
	struct x11_output *output = (struct x11_output *)output_base;
	struct x11_compositor *compositor =
		(struct x11_compositor *)output->base.compositor;

	wl_list_remove(&output->base.link);
	wl_event_source_remove(output->finish_frame_timer);

	if (compositor->use_shm) {
		pixman_renderer_output_destroy(output_base);
		x11_output_deinit_shm(compositor, output);
	} else
		gl_renderer_output_destroy(output_base);

	xcb_destroy_window(compositor->conn, output->window);

	weston_output_destroy(&output->base);

	free(output);
}

static void
x11_output_set_wm_protocols(struct x11_compositor *c,
			    struct x11_output *output)
{
	xcb_atom_t list[1];

	list[0] = c->atom.wm_delete_window;
	xcb_change_property (c->conn, 
			     XCB_PROP_MODE_REPLACE,
			     output->window,
			     c->atom.wm_protocols,
			     XCB_ATOM_ATOM,
			     32,
			     ARRAY_LENGTH(list),
			     list);
}

struct wm_normal_hints {
    	uint32_t flags;
	uint32_t pad[4];
	int32_t min_width, min_height;
	int32_t max_width, max_height;
    	int32_t width_inc, height_inc;
    	int32_t min_aspect_x, min_aspect_y;
    	int32_t max_aspect_x, max_aspect_y;
	int32_t base_width, base_height;
	int32_t win_gravity;
};

#define WM_NORMAL_HINTS_MIN_SIZE	16
#define WM_NORMAL_HINTS_MAX_SIZE	32

static void
x11_output_set_icon(struct x11_compositor *c,
		    struct x11_output *output, const char *filename)
{
	uint32_t *icon;
	int32_t width, height;
	pixman_image_t *image;

	image = load_image(filename);
	if (!image)
		return;
	width = pixman_image_get_width(image);
	height = pixman_image_get_height(image);
	icon = malloc(width * height * 4 + 8);
	if (!icon) {
		pixman_image_unref(image);
		return;
	}

	icon[0] = width;
	icon[1] = height;
	memcpy(icon + 2, pixman_image_get_data(image), width * height * 4);
	xcb_change_property(c->conn, XCB_PROP_MODE_REPLACE, output->window,
			    c->atom.net_wm_icon, c->atom.cardinal, 32,
			    width * height + 2, icon);
	free(icon);
	pixman_image_unref(image);
}

static void
x11_output_wait_for_map(struct x11_compositor *c, struct x11_output *output)
{
	xcb_map_notify_event_t *map_notify;
	xcb_configure_notify_event_t *configure_notify;
	xcb_generic_event_t *event;
	int mapped = 0;
	uint8_t response_type;

	/* This isn't the nicest way to do this.  Ideally, we could
	 * just go back to the main loop and once we get the map
	 * notify, we add the output to the compositor.  While we do
	 * support output hotplug, we can't start up with no outputs.
	 * We could add the output and then resize once we get the map
	 * notify, but we don't want to start up and immediately
	 * resize the output. */

	xcb_flush(c->conn);

	while (!mapped) {
		event = xcb_wait_for_event(c->conn);
		response_type = event->response_type & ~0x80;

		switch (response_type) {
		case XCB_MAP_NOTIFY:
			map_notify = (xcb_map_notify_event_t *) event;
			if (map_notify->window == output->window)
				mapped = 1;
			break;

		case XCB_CONFIGURE_NOTIFY:
			configure_notify =
				(xcb_configure_notify_event_t *) event;

			output->mode.width = configure_notify->width;
			output->mode.height = configure_notify->height;
			break;
		}
	}
}



static int
x11_output_init_shm(struct x11_compositor *c, struct x11_output *output,
	int width, int height)
{
	xcb_screen_iterator_t iter;
	xcb_visualtype_t *visual_type;
	xcb_format_iterator_t fmt;
	xcb_void_cookie_t cookie;
	xcb_generic_error_t *err;
	int shadow_width, shadow_height;
	pixman_transform_t transform;
	xcb_shm_query_version_reply_t *version;
	int bitsperpixel = 0;
	pixman_format_code_t pixman_format;

	/* Check if SHM is available */
	version = xcb_shm_query_version_reply(c->conn, xcb_shm_query_version(c->conn), 0);
	if (!version)
		/* SHM is missing */
		return -ENOENT;
	weston_log("Found SHM extension version: %d.%d\n", version->major_version, version->minor_version);
	free(version);

	iter = xcb_setup_roots_iterator(xcb_get_setup(c->conn));
	visual_type = xcb_aux_find_visual_by_id(iter.data, iter.data->root_visual);
	if (!visual_type) {
		weston_log("Failed to lookup visual for root window\n");
		return -ENOENT;
	}
	weston_log("Found visual, bits per value: %d, red_mask: %.8x, green_mask: %.8x, blue_mask: %.8x\n",
		visual_type->bits_per_rgb_value,
		visual_type->red_mask,
		visual_type->green_mask,
		visual_type->blue_mask);
	output->depth = xcb_aux_get_depth_of_visual(iter.data, iter.data->root_visual);
	weston_log("Visual depth is %d\n", output->depth);

	for (fmt = xcb_setup_pixmap_formats_iterator(xcb_get_setup(c->conn));
	     fmt.rem;
	     xcb_format_next(&fmt)) {
		if (fmt.data->depth == output->depth) {
			bitsperpixel = fmt.data->bits_per_pixel;
			break;
		}
	}
	weston_log("Found format for depth %d, bpp: %d\n",
		output->depth, bitsperpixel);

	if  (bitsperpixel == 32 &&
	     visual_type->red_mask == 0xff0000 &&
	     visual_type->green_mask == 0x00ff00 &&
	     visual_type->blue_mask == 0x0000ff) {
		weston_log("Will use x8r8g8b8 format for SHM surfaces\n");
		pixman_format = PIXMAN_x8r8g8b8;
	} else {
		weston_log("Can't find appropriate format for SHM pixmap\n");
		return -ENOTSUP;
	}


	/* Create SHM segment and attach it */
	output->shm_id = shmget(IPC_PRIVATE, width * height * (bitsperpixel / 8), IPC_CREAT | S_IRWXU);
	if (output->shm_id == -1) {
		weston_log("x11shm: failed to allocate SHM segment\n");
		return -ENOMEM;
	}
	output->buf = shmat(output->shm_id, NULL, 0 /* read/write */);
	if (-1 == (long)output->buf) {
		weston_log("x11shm: failed to attach SHM segment\n");
		return -ENOMEM;
	}
	output->segment = xcb_generate_id(c->conn);
	cookie = xcb_shm_attach_checked(c->conn, output->segment, output->shm_id, 1);
	err = xcb_request_check(c->conn, cookie);
	if (err) {
		weston_log("x11shm: xcb_shm_attach error %d\n", err->error_code);
		free(err);
		return err->error_code;
	}

	shmctl(output->shm_id, IPC_RMID, NULL);

	/* Now create pixman image */
	output->hw_surface = pixman_image_create_bits(pixman_format, width, height, output->buf,
		width * (bitsperpixel / 8));
	pixman_transform_init_identity(&transform);
	switch (output->base.transform) {
	default:
	case WL_OUTPUT_TRANSFORM_NORMAL:
		shadow_width = width;
		shadow_height = height;
		pixman_transform_rotate(&transform,
			NULL, 0, 0);
		pixman_transform_translate(&transform, NULL,
			0, 0);
		break;
	case WL_OUTPUT_TRANSFORM_180:
		shadow_width = width;
		shadow_height = height;
		pixman_transform_rotate(&transform,
			NULL, -pixman_fixed_1, 0);
		pixman_transform_translate(NULL, &transform,
			pixman_int_to_fixed(shadow_width),
			pixman_int_to_fixed(shadow_height));
		break;
	case WL_OUTPUT_TRANSFORM_270:
		shadow_width = height;
		shadow_height = width;
		pixman_transform_rotate(&transform,
			NULL, 0, pixman_fixed_1);
		pixman_transform_translate(&transform,
			NULL,
			pixman_int_to_fixed(shadow_width),
			0);
		break;
	case WL_OUTPUT_TRANSFORM_90:
		shadow_width = height;
		shadow_height = width;
		pixman_transform_rotate(&transform,
			NULL, 0, -pixman_fixed_1);
		pixman_transform_translate(&transform,
			NULL,
			0,
			pixman_int_to_fixed(shadow_height));
		break;
	}
	output->shadow_buf = malloc(width * height *  (bitsperpixel / 8));
	output->shadow_surface = pixman_image_create_bits(pixman_format, shadow_width, shadow_height,
		output->shadow_buf, shadow_width * (bitsperpixel / 8));
	/* No need in transform for normal output */
	if (output->base.transform != WL_OUTPUT_TRANSFORM_NORMAL)
		pixman_image_set_transform(output->shadow_surface, &transform);

	output->gc = xcb_generate_id(c->conn);
	xcb_create_gc(c->conn, output->gc, output->window, 0, NULL);

	return 0;
}

static struct x11_output *
x11_compositor_create_output(struct x11_compositor *c, int x, int y,
			     int width, int height, int fullscreen,
			     int no_input, char *configured_name,
			     uint32_t transform)
{
	static const char name[] = "Weston Compositor";
	static const char class[] = "weston-1\0Weston Compositor";
	char title[32];
	struct x11_output *output;
	xcb_screen_iterator_t iter;
	struct wm_normal_hints normal_hints;
	struct wl_event_loop *loop;
	uint32_t mask = XCB_CW_EVENT_MASK | XCB_CW_CURSOR;
	xcb_atom_t atom_list[1];
	uint32_t values[2] = {
		XCB_EVENT_MASK_EXPOSURE |
		XCB_EVENT_MASK_STRUCTURE_NOTIFY,
		0
	};

	if (configured_name)
		sprintf(title, "%s - %s", name, configured_name);
	else
		strcpy(title, name);

	if (!no_input)
		values[0] |=
			XCB_EVENT_MASK_KEY_PRESS |
			XCB_EVENT_MASK_KEY_RELEASE |
			XCB_EVENT_MASK_BUTTON_PRESS |
			XCB_EVENT_MASK_BUTTON_RELEASE |
			XCB_EVENT_MASK_POINTER_MOTION |
			XCB_EVENT_MASK_ENTER_WINDOW |
			XCB_EVENT_MASK_LEAVE_WINDOW |
			XCB_EVENT_MASK_KEYMAP_STATE |
			XCB_EVENT_MASK_FOCUS_CHANGE;

	output = malloc(sizeof *output);
	if (output == NULL)
		return NULL;

	memset(output, 0, sizeof *output);

	output->mode.flags =
		WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
	output->mode.width = width;
	output->mode.height = height;
	output->mode.refresh = 60000;
	wl_list_init(&output->base.mode_list);
	wl_list_insert(&output->base.mode_list, &output->mode.link);

	values[1] = c->null_cursor;
	output->window = xcb_generate_id(c->conn);
	iter = xcb_setup_roots_iterator(xcb_get_setup(c->conn));
	xcb_create_window(c->conn,
			  XCB_COPY_FROM_PARENT,
			  output->window,
			  iter.data->root,
			  0, 0,
			  width, height,
			  0,
			  XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  iter.data->root_visual,
			  mask, values);

	if (fullscreen) {
		atom_list[0] = c->atom.net_wm_state_fullscreen;
		xcb_change_property(c->conn, XCB_PROP_MODE_REPLACE,
				    output->window,
				    c->atom.net_wm_state,
				    XCB_ATOM_ATOM, 32,
				    ARRAY_LENGTH(atom_list), atom_list);
	} else {
		/* Don't resize me. */
		memset(&normal_hints, 0, sizeof normal_hints);
		normal_hints.flags =
			WM_NORMAL_HINTS_MAX_SIZE | WM_NORMAL_HINTS_MIN_SIZE;
		normal_hints.min_width = width;
		normal_hints.min_height = height;
		normal_hints.max_width = width;
		normal_hints.max_height = height;
		xcb_change_property(c->conn, XCB_PROP_MODE_REPLACE, output->window,
				    c->atom.wm_normal_hints,
				    c->atom.wm_size_hints, 32,
				    sizeof normal_hints / 4,
				    (uint8_t *) &normal_hints);
	}

	/* Set window name.  Don't bother with non-EWMH WMs. */
	xcb_change_property(c->conn, XCB_PROP_MODE_REPLACE, output->window,
			    c->atom.net_wm_name, c->atom.utf8_string, 8,
			    strlen(title), title);
	xcb_change_property(c->conn, XCB_PROP_MODE_REPLACE, output->window,
			    c->atom.wm_class, c->atom.string, 8,
			    sizeof class, class);

	x11_output_set_icon(c, output, DATADIR "/weston/wayland.png");

	x11_output_set_wm_protocols(c, output);

	xcb_map_window(c->conn, output->window);

	x11_output_wait_for_map(c, output);

	output->base.origin = output->base.current;
	if (c->use_shm)
		output->base.repaint = x11_output_repaint_shm;
	else
		output->base.repaint = x11_output_repaint_gl;
	output->base.destroy = x11_output_destroy;
	output->base.assign_planes = NULL;
	output->base.set_backlight = NULL;
	output->base.set_dpms = NULL;
	output->base.switch_mode = NULL;
	output->base.current = &output->mode;
	output->base.make = "xwayland";
	output->base.model = "none";
	weston_output_init(&output->base, &c->base,
			   x, y, width, height, transform);

	if (c->use_shm) {
		if (x11_output_init_shm(c, output, width, height) < 0)
			return NULL;
		if (pixman_renderer_output_create(&output->base) < 0) {
			x11_output_deinit_shm(c, output);
			return NULL;
		}
	} else {
		if (gl_renderer_output_create(&output->base, output->window) < 0)
			return NULL;
	}

	loop = wl_display_get_event_loop(c->base.wl_display);
	output->finish_frame_timer =
		wl_event_loop_add_timer(loop, finish_frame_handler, output);

	wl_list_insert(c->base.output_list.prev, &output->base.link);

	weston_log("x11 output %dx%d, window id %d\n",
		   width, height, output->window);

	return output;
}

static struct x11_output *
x11_compositor_find_output(struct x11_compositor *c, xcb_window_t window)
{
	struct x11_output *output;

	wl_list_for_each(output, &c->base.output_list, base.link) {
		if (output->window == window)
			return output;
	}

	return NULL;
}

#ifdef HAVE_XCB_XKB
static void
update_xkb_state(struct x11_compositor *c, xcb_xkb_state_notify_event_t *state)
{
	xkb_state_update_mask(c->core_seat.xkb_state.state,
			      get_xkb_mod_mask(c, state->baseMods),
			      get_xkb_mod_mask(c, state->latchedMods),
			      get_xkb_mod_mask(c, state->lockedMods),
			      0,
			      0,
			      state->group);

	notify_modifiers(&c->core_seat,
			 wl_display_next_serial(c->base.wl_display));
}
#endif

/**
 * This is monumentally unpleasant.  If we don't have XCB-XKB bindings,
 * the best we can do (given that XCB also lacks XI2 support), is to take
 * the state from the core key events.  Unfortunately that only gives us
 * the effective (i.e. union of depressed/latched/locked) state, and we
 * need the granularity.
 *
 * So we still update the state with every key event we see, but also use
 * the state field from X11 events as a mask so we don't get any stuck
 * modifiers.
 */
static void
update_xkb_state_from_core(struct x11_compositor *c, uint16_t x11_mask)
{
	uint32_t mask = get_xkb_mod_mask(c, x11_mask);
	struct wl_keyboard *keyboard = &c->core_seat.keyboard;

	xkb_state_update_mask(c->core_seat.xkb_state.state,
			      keyboard->modifiers.mods_depressed & mask,
			      keyboard->modifiers.mods_latched & mask,
			      keyboard->modifiers.mods_locked & mask,
			      0,
			      0,
			      (x11_mask >> 13) & 3);
	notify_modifiers(&c->core_seat,
			 wl_display_next_serial(c->base.wl_display));
}

static void
x11_compositor_deliver_button_event(struct x11_compositor *c,
				    xcb_generic_event_t *event, int state)
{
	xcb_button_press_event_t *button_event =
		(xcb_button_press_event_t *) event;
	uint32_t button;
	struct x11_output *output;

	output = x11_compositor_find_output(c, button_event->event);

	if (state)
		xcb_grab_pointer(c->conn, 0, output->window,
				 XCB_EVENT_MASK_BUTTON_PRESS |
				 XCB_EVENT_MASK_BUTTON_RELEASE |
				 XCB_EVENT_MASK_POINTER_MOTION |
				 XCB_EVENT_MASK_ENTER_WINDOW |
				 XCB_EVENT_MASK_LEAVE_WINDOW,
				 XCB_GRAB_MODE_ASYNC,
				 XCB_GRAB_MODE_ASYNC,
				 output->window, XCB_CURSOR_NONE,
				 button_event->time);
	else
		xcb_ungrab_pointer(c->conn, button_event->time);

	if (!c->has_xkb)
		update_xkb_state_from_core(c, button_event->state);

	switch (button_event->detail) {
	default:
		button = button_event->detail + BTN_LEFT - 1;
		break;
	case 2:
		button = BTN_MIDDLE;
		break;
	case 3:
		button = BTN_RIGHT;
		break;
	case 4:
		/* Axis are measured in pixels, but the xcb events are discrete
		 * steps. Therefore move the axis by some pixels every step. */
		if (state)
			notify_axis(&c->core_seat,
				    weston_compositor_get_time(),
				    WL_POINTER_AXIS_VERTICAL_SCROLL,
				    -DEFAULT_AXIS_STEP_DISTANCE);
		return;
	case 5:
		if (state)
			notify_axis(&c->core_seat,
				    weston_compositor_get_time(),
				    WL_POINTER_AXIS_VERTICAL_SCROLL,
				    DEFAULT_AXIS_STEP_DISTANCE);
		return;
	case 6:
		if (state)
			notify_axis(&c->core_seat,
				    weston_compositor_get_time(),
				    WL_POINTER_AXIS_HORIZONTAL_SCROLL,
				    -DEFAULT_AXIS_STEP_DISTANCE);
		return;
	case 7:
		if (state)
			notify_axis(&c->core_seat,
				    weston_compositor_get_time(),
				    WL_POINTER_AXIS_HORIZONTAL_SCROLL,
				    DEFAULT_AXIS_STEP_DISTANCE);
		return;
	}

	notify_button(&c->core_seat,
		      weston_compositor_get_time(), button,
		      state ? WL_POINTER_BUTTON_STATE_PRESSED :
			      WL_POINTER_BUTTON_STATE_RELEASED);
}

static void
x11_output_transform_coordinate(struct x11_output *x11_output,
						wl_fixed_t *x, wl_fixed_t *y)
{
	struct weston_output *output = &x11_output->base;
	wl_fixed_t tx, ty;
	wl_fixed_t width = wl_fixed_from_int(output->width - 1);
	wl_fixed_t height = wl_fixed_from_int(output->height - 1);

	switch(output->transform) {
	case WL_OUTPUT_TRANSFORM_NORMAL:
	default:
		tx = *x;
		ty = *y;
		break;
	case WL_OUTPUT_TRANSFORM_90:
		tx = *y;
		ty = height - *x;
		break;
	case WL_OUTPUT_TRANSFORM_180:
		tx = width - *x;
		ty = height - *y;
		break;
	case WL_OUTPUT_TRANSFORM_270:
		tx = width - *y;
		ty = *x;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		tx = width - *x;
		ty = *y;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		tx = width - *y;
		ty = height - *x;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		tx = *x;
		ty = height - *y;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		tx = *y;
		ty = *x;
		break;
	}

	tx += wl_fixed_from_int(output->x);
	ty += wl_fixed_from_int(output->y);

	*x = tx;
	*y = ty;
}

static void
x11_compositor_deliver_motion_event(struct x11_compositor *c,
					xcb_generic_event_t *event)
{
	struct x11_output *output;
	wl_fixed_t x, y;
	xcb_motion_notify_event_t *motion_notify =
			(xcb_motion_notify_event_t *) event;

	if (!c->has_xkb)
		update_xkb_state_from_core(c, motion_notify->state);
	output = x11_compositor_find_output(c, motion_notify->event);
	x = wl_fixed_from_int(motion_notify->event_x);
	y = wl_fixed_from_int(motion_notify->event_y);
	x11_output_transform_coordinate(output, &x, &y);

	notify_motion(&c->core_seat, weston_compositor_get_time(), x, y);
}

static void
x11_compositor_deliver_enter_event(struct x11_compositor *c,
					xcb_generic_event_t *event)
{
	struct x11_output *output;
	wl_fixed_t x, y;

	xcb_enter_notify_event_t *enter_notify =
			(xcb_enter_notify_event_t *) event;
	if (enter_notify->state >= Button1Mask)
		return;
	if (!c->has_xkb)
		update_xkb_state_from_core(c, enter_notify->state);
	output = x11_compositor_find_output(c, enter_notify->event);
	x = wl_fixed_from_int(enter_notify->event_x);
	y = wl_fixed_from_int(enter_notify->event_y);
	x11_output_transform_coordinate(output, &x, &y);

	notify_pointer_focus(&c->core_seat, &output->base, x, y);
}

static int
x11_compositor_next_event(struct x11_compositor *c,
			  xcb_generic_event_t **event, uint32_t mask)
{
	if (mask & WL_EVENT_READABLE) {
		*event = xcb_poll_for_event(c->conn);
	} else {
#ifdef HAVE_XCB_POLL_FOR_QUEUED_EVENT
		*event = xcb_poll_for_queued_event(c->conn);
#else
		*event = xcb_poll_for_event(c->conn);
#endif
	}

	return *event != NULL;
}

static int
x11_compositor_handle_event(int fd, uint32_t mask, void *data)
{
	struct x11_compositor *c = data;
	struct x11_output *output;
	xcb_generic_event_t *event, *prev;
	xcb_client_message_event_t *client_message;
	xcb_enter_notify_event_t *enter_notify;
	xcb_key_press_event_t *key_press, *key_release;
	xcb_keymap_notify_event_t *keymap_notify;
	xcb_focus_in_event_t *focus_in;
	xcb_expose_event_t *expose;
	xcb_atom_t atom;
	uint32_t *k;
	uint32_t i, set;
	uint8_t response_type;
	int count;

	prev = NULL;
	count = 0;
	while (x11_compositor_next_event(c, &event, mask)) {
		response_type = event->response_type & ~0x80;

		switch (prev ? prev->response_type & ~0x80 : 0x80) {
		case XCB_KEY_RELEASE:
			/* Suppress key repeat events; this is only used if we
			 * don't have XCB XKB support. */
			key_release = (xcb_key_press_event_t *) prev;
			key_press = (xcb_key_press_event_t *) event;
			if (response_type == XCB_KEY_PRESS &&
			    key_release->time == key_press->time &&
			    key_release->detail == key_press->detail) {
				/* Don't deliver the held key release
				 * event or the new key press event. */
				free(event);
				free(prev);
				prev = NULL;
				continue;
			} else {
				/* Deliver the held key release now
				 * and fall through and handle the new
				 * event below. */
				update_xkb_state_from_core(c, key_release->state);
				notify_key(&c->core_seat,
					   weston_compositor_get_time(),
					   key_release->detail - 8,
					   WL_KEYBOARD_KEY_STATE_RELEASED,
					   STATE_UPDATE_AUTOMATIC);
				free(prev);
				prev = NULL;
				break;
			}

		case XCB_FOCUS_IN:
			assert(response_type == XCB_KEYMAP_NOTIFY);
			keymap_notify = (xcb_keymap_notify_event_t *) event;
			c->keys.size = 0;
			for (i = 0; i < ARRAY_LENGTH(keymap_notify->keys) * 8; i++) {
				set = keymap_notify->keys[i >> 3] &
					(1 << (i & 7));
				if (set) {
					k = wl_array_add(&c->keys, sizeof *k);
					*k = i;
				}
			}

			/* Unfortunately the state only comes with the enter
			 * event, rather than with the focus event.  I'm not
			 * sure of the exact semantics around it and whether
			 * we can ensure that we get both? */
			notify_keyboard_focus_in(&c->core_seat, &c->keys,
						 STATE_UPDATE_AUTOMATIC);

			free(prev);
			prev = NULL;
			break;

		default:
			/* No previous event held */
			break;
		}

		switch (response_type) {
		case XCB_KEY_PRESS:
			key_press = (xcb_key_press_event_t *) event;
			if (!c->has_xkb)
				update_xkb_state_from_core(c, key_press->state);
			notify_key(&c->core_seat,
				   weston_compositor_get_time(),
				   key_press->detail - 8,
				   WL_KEYBOARD_KEY_STATE_PRESSED,
				   c->has_xkb ? STATE_UPDATE_NONE :
						STATE_UPDATE_AUTOMATIC);
			break;
		case XCB_KEY_RELEASE:
			/* If we don't have XKB, we need to use the lame
			 * autorepeat detection above. */
			if (!c->has_xkb) {
				prev = event;
				break;
			}
			key_release = (xcb_key_press_event_t *) event;
			notify_key(&c->core_seat,
				   weston_compositor_get_time(),
				   key_release->detail - 8,
				   WL_KEYBOARD_KEY_STATE_RELEASED,
				   STATE_UPDATE_NONE);
			break;
		case XCB_BUTTON_PRESS:
			x11_compositor_deliver_button_event(c, event, 1);
			break;
		case XCB_BUTTON_RELEASE:
			x11_compositor_deliver_button_event(c, event, 0);
			break;
		case XCB_MOTION_NOTIFY:
			x11_compositor_deliver_motion_event(c, event);
			break;

		case XCB_EXPOSE:
			expose = (xcb_expose_event_t *) event;
			output = x11_compositor_find_output(c, expose->window);
			weston_output_schedule_repaint(&output->base);
			break;

		case XCB_ENTER_NOTIFY:
			x11_compositor_deliver_enter_event(c, event);
			break;

		case XCB_LEAVE_NOTIFY:
			enter_notify = (xcb_enter_notify_event_t *) event;
			if (enter_notify->state >= Button1Mask)
				break;
			if (!c->has_xkb)
				update_xkb_state_from_core(c, enter_notify->state);
			notify_pointer_focus(&c->core_seat, NULL, 0, 0);
			break;

		case XCB_CLIENT_MESSAGE:
			client_message = (xcb_client_message_event_t *) event;
			atom = client_message->data.data32[0];
			if (atom == c->atom.wm_delete_window)
				wl_display_terminate(c->base.wl_display);
			break;

		case XCB_FOCUS_IN:
			focus_in = (xcb_focus_in_event_t *) event;
			if (focus_in->mode == XCB_NOTIFY_MODE_WHILE_GRABBED)
				break;

			prev = event;
			break;

		case XCB_FOCUS_OUT:
			focus_in = (xcb_focus_in_event_t *) event;
			if (focus_in->mode == XCB_NOTIFY_MODE_WHILE_GRABBED ||
			    focus_in->mode == XCB_NOTIFY_MODE_UNGRAB)
				break;
			notify_keyboard_focus_out(&c->core_seat);
			break;

		default:
			break;
		}

#ifdef HAVE_XCB_XKB
		if (c->has_xkb &&
		    response_type == c->xkb_event_base) {
			xcb_xkb_state_notify_event_t *state =
				(xcb_xkb_state_notify_event_t *) event;
			if (state->xkbType == XCB_XKB_STATE_NOTIFY)
				update_xkb_state(c, state);
		}
#endif

		count++;
		if (prev != event)
			free (event);
	}

	switch (prev ? prev->response_type & ~0x80 : 0x80) {
	case XCB_KEY_RELEASE:
		key_release = (xcb_key_press_event_t *) prev;
		update_xkb_state_from_core(c, key_release->state);
		notify_key(&c->core_seat,
			   weston_compositor_get_time(),
			   key_release->detail - 8,
			   WL_KEYBOARD_KEY_STATE_RELEASED,
			   STATE_UPDATE_AUTOMATIC);
		free(prev);
		prev = NULL;
		break;
	default:
		break;
	}

	return count;
}

#define F(field) offsetof(struct x11_compositor, field)

static void
x11_compositor_get_resources(struct x11_compositor *c)
{
	static const struct { const char *name; int offset; } atoms[] = {
		{ "WM_PROTOCOLS",	F(atom.wm_protocols) },
		{ "WM_NORMAL_HINTS",	F(atom.wm_normal_hints) },
		{ "WM_SIZE_HINTS",	F(atom.wm_size_hints) },
		{ "WM_DELETE_WINDOW",	F(atom.wm_delete_window) },
		{ "WM_CLASS",		F(atom.wm_class) },
		{ "_NET_WM_NAME",	F(atom.net_wm_name) },
		{ "_NET_WM_ICON",	F(atom.net_wm_icon) },
		{ "_NET_WM_STATE",	F(atom.net_wm_state) },
		{ "_NET_WM_STATE_FULLSCREEN", F(atom.net_wm_state_fullscreen) },
		{ "STRING",		F(atom.string) },
		{ "UTF8_STRING",	F(atom.utf8_string) },
		{ "CARDINAL",		F(atom.cardinal) },
		{ "_XKB_RULES_NAMES",	F(atom.xkb_names) },
	};

	xcb_intern_atom_cookie_t cookies[ARRAY_LENGTH(atoms)];
	xcb_intern_atom_reply_t *reply;
	xcb_pixmap_t pixmap;
	xcb_gc_t gc;
	unsigned int i;
	uint8_t data[] = { 0, 0, 0, 0 };

	for (i = 0; i < ARRAY_LENGTH(atoms); i++)
		cookies[i] = xcb_intern_atom (c->conn, 0,
					      strlen(atoms[i].name),
					      atoms[i].name);

	for (i = 0; i < ARRAY_LENGTH(atoms); i++) {
		reply = xcb_intern_atom_reply (c->conn, cookies[i], NULL);
		*(xcb_atom_t *) ((char *) c + atoms[i].offset) = reply->atom;
		free(reply);
	}

	pixmap = xcb_generate_id(c->conn);
	gc = xcb_generate_id(c->conn);
	xcb_create_pixmap(c->conn, 1, pixmap, c->screen->root, 1, 1);
	xcb_create_gc(c->conn, gc, pixmap, 0, NULL);
	xcb_put_image(c->conn, XCB_IMAGE_FORMAT_XY_PIXMAP,
		      pixmap, gc, 1, 1, 0, 0, 0, 32, sizeof data, data);
	c->null_cursor = xcb_generate_id(c->conn);
	xcb_create_cursor (c->conn, c->null_cursor,
			   pixmap, pixmap, 0, 0, 0,  0, 0, 0,  1, 1);
	xcb_free_gc(c->conn, gc);
	xcb_free_pixmap(c->conn, pixmap);
}

static void
x11_restore(struct weston_compositor *ec)
{
}

static void
x11_free_configured_output(struct x11_configured_output *output)
{
	free(output->name);
	free(output);
}

static void
x11_destroy(struct weston_compositor *ec)
{
	struct x11_compositor *compositor = (struct x11_compositor *)ec;
	struct x11_configured_output *o, *n;

	wl_list_for_each_safe(o, n, &configured_output_list, link)
		x11_free_configured_output(o);

	wl_event_source_remove(compositor->xcb_source);
	x11_input_destroy(compositor);

	weston_compositor_shutdown(ec); /* destroys outputs, too */

	if (compositor->use_shm)
		pixman_renderer_destroy(ec);
	else
		gl_renderer_destroy(ec);

	XCloseDisplay(compositor->dpy);
	free(ec);
}

static struct weston_compositor *
x11_compositor_create(struct wl_display *display,
		      int fullscreen,
		      int no_input,
		      int use_shm,
		      int argc, char *argv[], const char *config_file)
{
	struct x11_compositor *c;
	struct x11_configured_output *o;
	struct x11_output *output;
	xcb_screen_iterator_t s;
	int i, x = 0, output_count = 0;
	int width, height, count;

	weston_log("initializing x11 backend\n");

	c = malloc(sizeof *c);
	if (c == NULL)
		return NULL;

	memset(c, 0, sizeof *c);

	if (weston_compositor_init(&c->base, display, argc, argv,
				   config_file) < 0)
		goto err_free;

	c->dpy = XOpenDisplay(NULL);
	if (c->dpy == NULL)
		goto err_free;

	c->conn = XGetXCBConnection(c->dpy);
	XSetEventQueueOwner(c->dpy, XCBOwnsEventQueue);

	if (xcb_connection_has_error(c->conn))
		goto err_xdisplay;

	s = xcb_setup_roots_iterator(xcb_get_setup(c->conn));
	c->screen = s.data;
	wl_array_init(&c->keys);

	x11_compositor_get_resources(c);

	c->base.wl_display = display;
	c->use_shm = use_shm;
	if (c->use_shm) {
		if (pixman_renderer_init(&c->base) < 0)
			goto err_xdisplay;
	}
	else {
		if (gl_renderer_create(&c->base, c->dpy, gl_renderer_opaque_attribs,
				NULL) < 0)
			goto err_xdisplay;
	}
	weston_log("Using %s renderer\n", use_shm ? "pixman" : "gl");

	c->base.destroy = x11_destroy;
	c->base.restore = x11_restore;

	if (x11_input_create(c, no_input) < 0)
		goto err_gl;

	width = option_width ? option_width : 1024;
	height = option_height ? option_height : 640;
	count = option_count ? option_count : 1;

	wl_list_for_each(o, &configured_output_list, link) {
		output = x11_compositor_create_output(c, x, 0,
						      option_width ? width :
						      o->width,
						      option_height ? height :
						      o->height,
						      fullscreen, no_input,
						      o->name, o->transform);
		if (output == NULL)
			goto err_x11_input;

		x = pixman_region32_extents(&output->base.region)->x2;

		output_count++;
		if (option_count && output_count >= option_count)
			break;
	}

	for (i = output_count; i < count; i++) {
		output = x11_compositor_create_output(c, x, 0, width, height,
						      fullscreen, no_input, NULL,
						      WL_OUTPUT_TRANSFORM_NORMAL);
		if (output == NULL)
			goto err_x11_input;
		x = pixman_region32_extents(&output->base.region)->x2;
	}

	c->xcb_source =
		wl_event_loop_add_fd(c->base.input_loop,
				     xcb_get_file_descriptor(c->conn),
				     WL_EVENT_READABLE,
				     x11_compositor_handle_event, c);
	wl_event_source_check(c->xcb_source);

	return &c->base;

err_x11_input:
	x11_input_destroy(c);
err_gl:
	gl_renderer_destroy(&c->base);
err_xdisplay:
	XCloseDisplay(c->dpy);
err_free:
	free(c);
	return NULL;
}

static void
x11_output_set_transform(struct x11_configured_output *output)
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
}

static void
output_section_done(void *data)
{
	struct x11_configured_output *output;

	output = malloc(sizeof *output);

	if (!output || !output_name || (output_name[0] != 'X') ||
				(!output_mode && !output_transform)) {
		if (output_name)
			free(output_name);
		output_name = NULL;
		free(output);
		goto err_free;
	}

	output->name = output_name;

	if (output_mode) {
		if (sscanf(output_mode, "%dx%d", &output->width,
						&output->height) != 2) {
			weston_log("Invalid mode \"%s\" for output %s\n",
							output_mode, output_name);
			x11_free_configured_output(output);
			goto err_free;
		}
	} else {
		output->width = 1024;
		output->height = 640;
	}

	x11_output_set_transform(output);

	wl_list_insert(configured_output_list.prev, &output->link);

err_free:
	if (output_mode)
		free(output_mode);
	if (output_transform)
		free(output_transform);
	output_mode = NULL;
	output_transform = NULL;
}

WL_EXPORT struct weston_compositor *
backend_init(struct wl_display *display, int argc, char *argv[],
	     const char *config_file)
{
	int fullscreen = 0;
	int no_input = 0;
	int use_shm = 0;

	const struct weston_option x11_options[] = {
		{ WESTON_OPTION_INTEGER, "width", 0, &option_width },
		{ WESTON_OPTION_INTEGER, "height", 0, &option_height },
		{ WESTON_OPTION_BOOLEAN, "fullscreen", 'f', &fullscreen },
		{ WESTON_OPTION_INTEGER, "output-count", 0, &option_count },
		{ WESTON_OPTION_BOOLEAN, "no-input", 0, &no_input },
		{ WESTON_OPTION_BOOLEAN, "use-shm", 0, &use_shm },
	};

	parse_options(x11_options, ARRAY_LENGTH(x11_options), argc, argv);

	wl_list_init(&configured_output_list);

	const struct config_key x11_config_keys[] = {
		{ "name", CONFIG_KEY_STRING, &output_name },
		{ "mode", CONFIG_KEY_STRING, &output_mode },
		{ "transform", CONFIG_KEY_STRING, &output_transform },
	};

	const struct config_section config_section[] = {
		{ "output", x11_config_keys,
		ARRAY_LENGTH(x11_config_keys), output_section_done },
	};

	parse_config_file(config_file, config_section,
				ARRAY_LENGTH(config_section), NULL);

	return x11_compositor_create(display,
				     fullscreen,
				     no_input,
				     use_shm,
				     argc, argv, config_file);
}
