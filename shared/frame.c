/*
 * Copyright © 2008 Kristian Høgsberg
 * Copyright © 2012-2013 Collabora, Ltd.
 * Copyright © 2013 Jason Ekstrand
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <wayland-util.h>
#include <linux/input.h>

#include "cairo-util.h"

enum frame_button_flags {
	FRAME_BUTTON_ALIGN_RIGHT = 0x1,
	FRAME_BUTTON_DECORATED = 0x2,
	FRAME_BUTTON_CLICK_DOWN = 0x4,
};

struct frame_button {
	struct frame *frame;
	struct wl_list link;	/* buttons_list */

	cairo_surface_t *icon;
	enum frame_button_flags flags;
	int hover_count;
	int press_count;

	struct {
		int x, y;
		int width, height;
	} allocation;

	enum frame_status status_effect;
};

struct frame_pointer {
	struct wl_list link;
	void *data;

	int x, y;

	struct frame_button *hover_button;
	int active;
};

struct frame_touch {
	struct wl_list link;
	void *data;

	int x, y;

	struct frame_button *button;
};

struct frame {
	int32_t width, height;
	char *title;
	uint32_t flags;
	struct theme *theme;

	struct {
		int32_t x, y;
		int32_t width, height;
	} interior;
	int shadow_margin;
	int opaque_margin;
	int geometry_dirty;

	uint32_t status;

	struct wl_list buttons;
	struct wl_list pointers;
	struct wl_list touches;
};

static struct frame_button *
frame_button_create(struct frame *frame, const char *icon,
		    enum frame_status status_effect,
		    enum frame_button_flags flags)
{
	struct frame_button *button;

	button = calloc(1, sizeof *button);
	if (!button)
		return NULL;

	button->icon = cairo_image_surface_create_from_png(icon);
	if (!button->icon) {
		free(button);
		return NULL;
	}

	button->frame = frame;
	button->flags = flags;
	button->status_effect = status_effect;

	wl_list_insert(frame->buttons.prev, &button->link);

	return button;
}

static void
frame_button_destroy(struct frame_button *button)
{
	cairo_surface_destroy(button->icon);
	free(button);
}

static void
frame_button_enter(struct frame_button *button)
{
	if (!button->hover_count)
		button->frame->status |= FRAME_STATUS_REPAINT;
	button->hover_count++;
}

static void
frame_button_leave(struct frame_button *button, struct frame_pointer *pointer)
{
	button->hover_count--;
	if (!button->hover_count)
		button->frame->status |= FRAME_STATUS_REPAINT;

	/* In this case, we won't get a release */
	if (pointer->active)
		button->press_count--;
}

static void
frame_button_press(struct frame_button *button)
{
	if (!button->press_count)
		button->frame->status |= FRAME_STATUS_REPAINT;
	button->press_count++;

	if (button->flags & FRAME_BUTTON_CLICK_DOWN)
		button->frame->status |= button->status_effect;
}

static void
frame_button_release(struct frame_button *button)
{
	button->press_count--;
	if (!button->press_count)
		button->frame->status |= FRAME_STATUS_REPAINT;

	if (!(button->flags & FRAME_BUTTON_CLICK_DOWN))
		button->frame->status |= button->status_effect;
}

static void
frame_button_repaint(struct frame_button *button, cairo_t *cr)
{
	int x, y;

	if (!button->allocation.width)
		return;
	if (!button->allocation.height)
		return;

	x = button->allocation.x;
	y = button->allocation.y;

	cairo_save(cr);

	if (button->flags & FRAME_BUTTON_DECORATED) {
		cairo_set_line_width(cr, 1);

		cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
		cairo_rectangle(cr, x, y, 25, 16);

		cairo_stroke_preserve(cr);

		if (button->press_count) {
			cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
		} else if (button->hover_count) {
			cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
		} else {
			cairo_set_source_rgb(cr, 0.88, 0.88, 0.88);
		}

		cairo_fill (cr);

		x += 4;
	}

	cairo_set_source_surface(cr, button->icon, x, y);
	cairo_paint(cr);

	cairo_restore(cr);
}

static struct frame_pointer *
frame_pointer_get(struct frame *frame, void *data)
{
	struct frame_pointer *pointer;

	wl_list_for_each(pointer, &frame->pointers, link)
		if (pointer->data == data)
			return pointer;

	pointer = calloc(1, sizeof *pointer);
	if (!pointer)
		return NULL;

	pointer->data = data;
	wl_list_insert(&frame->pointers, &pointer->link);

	return pointer;
}

static void
frame_pointer_destroy(struct frame_pointer *pointer)
{
	wl_list_remove(&pointer->link);
	free(pointer);
}

static struct frame_touch *
frame_touch_get(struct frame *frame, void *data)
{
	struct frame_touch *touch;

	wl_list_for_each(touch, &frame->touches, link)
		if (touch->data == data)
			return touch;

	touch = calloc(1, sizeof *touch);
	if (!touch)
		return NULL;

	touch->data = data;
	wl_list_insert(&frame->touches, &touch->link);

	return touch;
}

static void
frame_touch_destroy(struct frame_touch *touch)
{
	wl_list_remove(&touch->link);
	free(touch);
}

struct frame *
frame_create(struct theme *t, int32_t width, int32_t height, uint32_t buttons,
	     const char *title)
{
	struct frame *frame;
	struct frame_button *button;

	frame = calloc(1, sizeof *frame);
	if (!frame)
		return NULL;

	frame->width = width;
	frame->height = height;
	frame->flags = 0;
	frame->theme = t;
	frame->status = FRAME_STATUS_REPAINT;
	frame->geometry_dirty = 1;

	if (title) {
		frame->title = strdup(title);
		if (!frame->title)
			goto free_frame;
	}

	wl_list_init(&frame->buttons);
	wl_list_init(&frame->pointers);
	wl_list_init(&frame->touches);

	if (title) {
		button = frame_button_create(frame,
					     DATADIR "/weston/icon_window.png",
					     FRAME_STATUS_MENU,
					     FRAME_BUTTON_CLICK_DOWN);
		if (!button)
			goto free_frame;
	}

	if (buttons & FRAME_BUTTON_CLOSE) {
		button = frame_button_create(frame,
					     DATADIR "/weston/sign_close.png",
					     FRAME_STATUS_CLOSE,
					     FRAME_BUTTON_ALIGN_RIGHT |
					     FRAME_BUTTON_DECORATED);
		if (!button)
			goto free_frame;
	}

	if (buttons & FRAME_BUTTON_MAXIMIZE) {
		button = frame_button_create(frame,
					     DATADIR "/weston/sign_maximize.png",
					     FRAME_STATUS_MAXIMIZE,
					     FRAME_BUTTON_ALIGN_RIGHT |
					     FRAME_BUTTON_DECORATED);
		if (!button)
			goto free_frame;
	}

	if (buttons & FRAME_BUTTON_MINIMIZE) {
		button = frame_button_create(frame,
					     DATADIR "/weston/sign_minimize.png",
					     FRAME_STATUS_MINIMIZE,
					     FRAME_BUTTON_ALIGN_RIGHT |
					     FRAME_BUTTON_DECORATED);
		if (!button)
			goto free_frame;
	}

	return frame;

free_frame:
	free(frame->title);
	free(frame);
	return NULL;
}

void
frame_destroy(struct frame *frame)
{
	struct frame_button *button, *next;

	wl_list_for_each_safe(button, next, &frame->buttons, link)
		frame_button_destroy(button);

	free(frame->title);
	free(frame);
}

int
frame_set_title(struct frame *frame, const char *title)
{
	char *dup = NULL;

	if (title) {
		dup = strdup(title);
		if (!dup)
			return -1;
	}

	free(frame->title);
	frame->title = dup;

	frame->status |= FRAME_STATUS_REPAINT;

	return 0;
}

void
frame_set_flag(struct frame *frame, enum frame_flag flag)
{
	if (flag & FRAME_FLAG_MAXIMIZED && !(frame->flags & FRAME_FLAG_MAXIMIZED))
		frame->geometry_dirty = 1;

	frame->flags |= flag;
	frame->status |= FRAME_STATUS_REPAINT;
}

void
frame_unset_flag(struct frame *frame, enum frame_flag flag)
{
	if (flag & FRAME_FLAG_MAXIMIZED && frame->flags & FRAME_FLAG_MAXIMIZED)
		frame->geometry_dirty = 1;

	frame->flags &= ~flag;
	frame->status |= FRAME_STATUS_REPAINT;
}

void
frame_resize(struct frame *frame, int32_t width, int32_t height)
{
	frame->width = width;
	frame->height = height;

	frame->geometry_dirty = 1;
	frame->status |= FRAME_STATUS_REPAINT;
}

void
frame_resize_inside(struct frame *frame, int32_t width, int32_t height)
{
	struct theme *t = frame->theme;
	int decoration_width, decoration_height, titlebar_height;

	if (frame->title)
		titlebar_height = t->titlebar_height;
	else
		titlebar_height = t->width;

	if (frame->flags & FRAME_FLAG_MAXIMIZED) {
		decoration_width = t->width * 2;
		decoration_height = t->width + titlebar_height;
	} else {
		decoration_width = (t->width + t->margin) * 2;
		decoration_height = t->width +
			titlebar_height + t->margin * 2;
	}

	frame_resize(frame, width + decoration_width,
		     height + decoration_height);
}

int32_t
frame_width(struct frame *frame)
{
	return frame->width;
}

int32_t
frame_height(struct frame *frame)
{
	return frame->height;
}

static void
frame_refresh_geometry(struct frame *frame)
{
	struct frame_button *button;
	struct theme *t = frame->theme;
	int x_l, x_r, y, w, h, titlebar_height;
	int32_t decoration_width, decoration_height;

	if (!frame->geometry_dirty)
		return;

	if (frame->title)
		titlebar_height = t->titlebar_height;
	else
		titlebar_height = t->width;

	if (frame->flags & FRAME_FLAG_MAXIMIZED) {
		decoration_width = t->width * 2;
		decoration_height = t->width + titlebar_height;

		frame->interior.x = t->width;
		frame->interior.y = titlebar_height;
		frame->interior.width = frame->width - decoration_width;
		frame->interior.height = frame->height - decoration_height;

		frame->opaque_margin = 0;
		frame->shadow_margin = 0;
	} else {
		decoration_width = (t->width + t->margin) * 2;
		decoration_height = t->width + titlebar_height + t->margin * 2;

		frame->interior.x = t->width + t->margin;
		frame->interior.y = titlebar_height + t->margin;
		frame->interior.width = frame->width - decoration_width;
		frame->interior.height = frame->height - decoration_height;

		frame->opaque_margin = t->margin + t->frame_radius;
		frame->shadow_margin = t->margin;
	}

	x_r = frame->width - t->width - frame->shadow_margin;
	x_l = t->width + frame->shadow_margin;
	y = t->width + frame->shadow_margin;
	wl_list_for_each(button, &frame->buttons, link) {
		const int button_padding = 4;
		w = cairo_image_surface_get_width(button->icon);
		h = cairo_image_surface_get_height(button->icon);

		if (button->flags & FRAME_BUTTON_DECORATED)
			w += 10;

		if (button->flags & FRAME_BUTTON_ALIGN_RIGHT) {
			x_r -= w;

			button->allocation.x = x_r;
			button->allocation.y = y;
			button->allocation.width = w + 1;
			button->allocation.height = h + 1;

			x_r -= button_padding;
		} else {
			button->allocation.x = x_l;
			button->allocation.y = y;
			button->allocation.width = w + 1;
			button->allocation.height = h + 1;

			x_l += w;
			x_l += button_padding;
		}
	}

	frame->geometry_dirty = 0;
}

void
frame_interior(struct frame *frame, int32_t *x, int32_t *y,
		int32_t *width, int32_t *height)
{
	frame_refresh_geometry(frame);

	if (x)
		*x = frame->interior.x;
	if (y)
		*y = frame->interior.y;
	if (width)
		*width = frame->interior.width;
	if (height)
		*height = frame->interior.height;
}

void
frame_input_rect(struct frame *frame, int32_t *x, int32_t *y,
		 int32_t *width, int32_t *height)
{
	frame_refresh_geometry(frame);

	if (x)
		*x = frame->shadow_margin;
	if (y)
		*y = frame->shadow_margin;
	if (width)
		*width = frame->width - frame->shadow_margin * 2;
	if (height)
		*height = frame->height - frame->shadow_margin * 2;
}

void
frame_opaque_rect(struct frame *frame, int32_t *x, int32_t *y,
		  int32_t *width, int32_t *height)
{
	frame_refresh_geometry(frame);

	if (x)
		*x = frame->opaque_margin;
	if (y)
		*y = frame->opaque_margin;
	if (width)
		*width = frame->width - frame->opaque_margin * 2;
	if (height)
		*height = frame->height - frame->opaque_margin * 2;
}

uint32_t
frame_status(struct frame *frame)
{
	return frame->status;
}

void
frame_status_clear(struct frame *frame, enum frame_status status)
{
	frame->status &= ~status;
}

static struct frame_button *
frame_find_button(struct frame *frame, int x, int y)
{
	struct frame_button *button;
	int rel_x, rel_y;

	wl_list_for_each(button, &frame->buttons, link) {
		rel_x = x - button->allocation.x;
		rel_y = y - button->allocation.y;

		if (0 <= rel_x && rel_x < button->allocation.width &&
		    0 <= rel_y && rel_y < button->allocation.height)
			return button;
	}

	return NULL;
}

enum theme_location
frame_pointer_enter(struct frame *frame, void *data, int x, int y)
{
	return frame_pointer_motion(frame, data, x, y);
}

enum theme_location
frame_pointer_motion(struct frame *frame, void *data, int x, int y)
{
	struct frame_pointer *pointer = frame_pointer_get(frame, data);
	struct frame_button *button = frame_find_button(frame, x, y);
	enum theme_location location;

	location = theme_get_location(frame->theme, x, y,
				      frame->width, frame->height,
				      frame->flags & FRAME_FLAG_MAXIMIZED ?
				      THEME_FRAME_MAXIMIZED : 0);
	if (!pointer)
		return location;

	pointer->x = x;
	pointer->y = y;

	if (pointer->hover_button == button)
		return location;

	if (pointer->hover_button)
		frame_button_leave(pointer->hover_button, pointer);

	/* No drags */
	pointer->active = 0;
	pointer->hover_button = button;

	if (pointer->hover_button)
		frame_button_enter(pointer->hover_button);

	return location;
}

void
frame_pointer_leave(struct frame *frame, void *data)
{
	struct frame_pointer *pointer = frame_pointer_get(frame, data);
	if (!pointer)
		return;

	if (pointer->hover_button)
		frame_button_leave(pointer->hover_button, pointer);

	frame_pointer_destroy(pointer);
}

enum theme_location
frame_pointer_button(struct frame *frame, void *data,
		     uint32_t button, enum frame_button_state state)
{
	struct frame_pointer *pointer = frame_pointer_get(frame, data);
	enum theme_location location;

	location = theme_get_location(frame->theme, pointer->x, pointer->y,
				      frame->width, frame->height,
				      frame->flags & FRAME_FLAG_MAXIMIZED ?
				      THEME_FRAME_MAXIMIZED : 0);

	if (!pointer)
		return location;

	if (button == BTN_RIGHT) {
		if (state == FRAME_BUTTON_PRESSED &&
		    location == THEME_LOCATION_TITLEBAR)
			frame->status |= FRAME_STATUS_MENU;

	} else if (button == BTN_LEFT && state == FRAME_BUTTON_PRESSED) {
		if (pointer->hover_button) {
			pointer->active = 1;
			frame_button_press(pointer->hover_button);
			return location;
		} else {
			switch (location) {
			case THEME_LOCATION_TITLEBAR:
				frame->status |= FRAME_STATUS_MOVE;
				break;
			case THEME_LOCATION_RESIZING_TOP:
			case THEME_LOCATION_RESIZING_BOTTOM:
			case THEME_LOCATION_RESIZING_LEFT:
			case THEME_LOCATION_RESIZING_RIGHT:
			case THEME_LOCATION_RESIZING_TOP_LEFT:
			case THEME_LOCATION_RESIZING_TOP_RIGHT:
			case THEME_LOCATION_RESIZING_BOTTOM_LEFT:
			case THEME_LOCATION_RESIZING_BOTTOM_RIGHT:
				frame->status |= FRAME_STATUS_RESIZE;
				break;
			default:
				break;
			}
		}
	} else if (button == BTN_LEFT && state == FRAME_BUTTON_RELEASED) {
		if (pointer->hover_button && pointer->active)
			frame_button_release(pointer->hover_button);

		pointer->active = 0;
	}

	return location;
}

void
frame_touch_down(struct frame *frame, void *data, int32_t id, int x, int y)
{
	struct frame_touch *touch = frame_touch_get(frame, data);
	struct frame_button *button = frame_find_button(frame, x, y);
	enum theme_location location;

	if (id > 0)
		return;

	if (button) {
		touch->button = button;
		frame_button_press(touch->button);
		return;
	}

	location = theme_get_location(frame->theme, x, y,
				      frame->width, frame->height,
				      frame->flags & FRAME_FLAG_MAXIMIZED ?
				      THEME_FRAME_MAXIMIZED : 0);

	switch (location) {
	case THEME_LOCATION_TITLEBAR:
		frame->status |= FRAME_STATUS_MOVE;
		break;
	case THEME_LOCATION_RESIZING_TOP:
	case THEME_LOCATION_RESIZING_BOTTOM:
	case THEME_LOCATION_RESIZING_LEFT:
	case THEME_LOCATION_RESIZING_RIGHT:
	case THEME_LOCATION_RESIZING_TOP_LEFT:
	case THEME_LOCATION_RESIZING_TOP_RIGHT:
	case THEME_LOCATION_RESIZING_BOTTOM_LEFT:
	case THEME_LOCATION_RESIZING_BOTTOM_RIGHT:
		frame->status |= FRAME_STATUS_RESIZE;
		break;
	default:
		break;
	}
}

void
frame_touch_up(struct frame *frame, void *data, int32_t id)
{
	struct frame_touch *touch = frame_touch_get(frame, data);

	if (id > 0)
		return;

	if (touch->button) {
		frame_button_release(touch->button);
		frame_touch_destroy(touch);
		return;
	}
}

void
frame_repaint(struct frame *frame, cairo_t *cr)
{
	struct frame_button *button;
	uint32_t flags = 0;

	frame_refresh_geometry(frame);

	if (frame->flags & FRAME_FLAG_MAXIMIZED)
		flags |= THEME_FRAME_MAXIMIZED;

	if (frame->flags & FRAME_FLAG_ACTIVE)
		flags |= THEME_FRAME_ACTIVE;

	cairo_save(cr);
	theme_render_frame(frame->theme, cr, frame->width, frame->height,
			   frame->title, flags);
	cairo_restore(cr);

	wl_list_for_each(button, &frame->buttons, link)
		frame_button_repaint(button, cr);

	frame_status_clear(frame, FRAME_STATUS_REPAINT);
}
