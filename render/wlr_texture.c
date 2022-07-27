#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <drm_fourcc.h>
#include <wlr/render/interface.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_matrix.h>
#include "types/wlr_buffer.h"
#include "backend/multi.h"
#include "backend/drm/drm.h"
#include "render/drm_format_set.h"
#include "render/wlr_renderer.h"
#include "render/egl.h"

void wlr_texture_init(struct wlr_texture *texture, struct wlr_renderer *renderer,
		const struct wlr_texture_impl *impl, uint32_t width, uint32_t height) {
	assert(renderer);

	memset(texture, 0, sizeof(*texture));
	texture->renderer = renderer;
	texture->impl = impl;
	texture->width = width;
	texture->height = height;
}

void wlr_texture_destroy(struct wlr_texture *texture) {
	if (texture && texture->impl && texture->impl->destroy) {
		texture->impl->destroy(texture);
	} else {
		free(texture);
	}
}

struct wlr_texture *wlr_texture_from_pixels(struct wlr_renderer *renderer,
		uint32_t fmt, uint32_t stride, uint32_t width, uint32_t height,
		const void *data) {
	assert(width > 0);
	assert(height > 0);
	assert(stride > 0);
	assert(data);

	struct wlr_readonly_data_buffer *buffer =
		readonly_data_buffer_create(fmt, stride, width, height, data);
	if (buffer == NULL) {
		return NULL;
	}

	struct wlr_texture *texture =
		wlr_texture_from_buffer(renderer, &buffer->base);

	// By this point, the renderer should have locked the buffer if it still
	// needs to access it in the future.
	readonly_data_buffer_drop(buffer);

	return texture;
}

struct wlr_texture *wlr_texture_from_dmabuf(struct wlr_renderer *renderer,
		struct wlr_dmabuf_attributes *attribs) {
	struct wlr_dmabuf_buffer *buffer = dmabuf_buffer_create(attribs);
	if (buffer == NULL) {
		return NULL;
	}

	struct wlr_texture *texture =
		wlr_texture_from_buffer(renderer, &buffer->base);

	// By this point, the renderer should have locked the buffer if it still
	// needs to access it in the future.
	dmabuf_buffer_drop(buffer);

	return texture;
}

struct wlr_texture *wlr_texture_from_buffer(struct wlr_renderer *renderer,
		struct wlr_buffer *buffer) {
	if (!renderer->impl->texture_from_buffer) {
		return NULL;
	}
	return renderer->impl->texture_from_buffer(renderer, buffer);
}

bool wlr_texture_update_from_buffer(struct wlr_texture *texture,
		struct wlr_buffer *buffer, const pixman_region32_t *damage) {
	if (!texture->impl->update_from_buffer) {
		return false;
	}
	if (texture->width != (uint32_t)buffer->width ||
			texture->height != (uint32_t)buffer->height) {
		return false;
	}
	const pixman_box32_t *extents = pixman_region32_extents(damage);
	if (extents->x1 < 0 || extents->y1 < 0 || extents->x2 > buffer->width ||
			extents->y2 > buffer->height) {
		return false;
	}
	return texture->impl->update_from_buffer(texture, buffer, damage);
}

struct wlr_texture_set *wlr_texture_set_from_dmabuf(struct wlr_renderer *renderer,
	struct wlr_dmabuf_attributes *attribs) {
	struct wlr_dmabuf_buffer *buffer = dmabuf_buffer_create(attribs);
	if (buffer == NULL) {
		return NULL;
	}

	struct wlr_texture_set *set =
		wlr_texture_set_from_buffer(renderer, &buffer->base);

	// By this point, the renderer should have locked the buffer if it still
	// needs to access it in the future.
	dmabuf_buffer_drop(buffer);

	return set;
}

static void handle_renderer_destroy(struct wl_listener *listener, void *data) {
	struct wlr_texture_renderer_pair *pair =
		wl_container_of(listener, pair, renderer_destroy);

	pair->renderer = NULL;
	if (pair->texture) {
		wlr_texture_destroy(pair->texture);
	}
	pair->texture = NULL;
	wl_list_remove(&pair->renderer_destroy.link);
}

static void wlr_texture_set_init_pair(struct wlr_texture_set *set, int pair,
		struct wlr_renderer *renderer) {
	assert(pair < set->pairing_count);

	set->pairings[pair].renderer = renderer;
	set->pairings[pair].renderer_destroy.notify = handle_renderer_destroy;
	wl_signal_add(&renderer->events.destroy, &set->pairings[pair].renderer_destroy);
}

struct wlr_texture_set *wlr_texture_set_from_renderer(struct wlr_renderer *renderer,
		struct wlr_buffer *buffer) {
	struct wlr_texture_set *set = calloc(1, sizeof(struct wlr_texture_set));
	if (!set) {
		return NULL;
	}
	set->buffer = buffer;
	set->native_pair = -1;

	/*
	 * If the renderer is part of a multi-GPU set, then use that list since it contains
	 * all of the renderers. Otherwise only add this renderer.
	 */
	if (renderer->multi_gpu) {
		set->multi_gpu = renderer->multi_gpu;
		set->pairing_count = wl_list_length(&renderer->multi_gpu->renderers);
		set->pairings = calloc(set->pairing_count, sizeof(struct wlr_texture_renderer_pair));
		if (!set->pairings) {
			goto fail;
		}

		/* Now add each mgpu renderer to the set */
		int i = 0;
		struct wlr_renderer *r;
		wl_list_for_each(r, &renderer->multi_gpu->renderers, multi_link) {
			wlr_texture_set_init_pair(set, i, r);
			i++;
		}
	} else {
		set->pairing_count = 1;
		set->pairings = calloc(set->pairing_count, sizeof(struct wlr_texture_renderer_pair));
		if (!set->pairings) {
			goto fail;
		}

		wlr_texture_set_init_pair(set, 0, renderer);
	}

	wlr_buffer_lock(buffer);

	return set;

fail:
	free(set);
	return NULL;
}

/*
 * Helper for importing a buffer into the texture set. This initializes
 * the native_pair internal state.
 */
static bool wlr_texture_set_import_buffer(struct wlr_texture_set *set,
		struct wlr_buffer *buffer) {
	/*
	 * For each renderer, try to create a texture. Go in order, since the first 
	 * entry is always the "primary" renderer that the user created this texture set with.
	 * The odds are highest that it is importable into that renderer, so start with that
	 * one.
	 */
	for (int i = 0; i < set->pairing_count; i++) {
		assert(!set->pairings[i].texture);
		set->pairings[i].texture = wlr_texture_from_buffer(set->pairings[i].renderer, buffer);
		/* If we got a match, mark this renderer as the "native" one the buffer is local to */
		if (set->pairings[i].texture) {
			/* Cache the width and height so other places don't have to search for it in pairings */
			set->width = set->pairings[i].texture->width;
			set->height = set->pairings[i].texture->height;
			set->native_pair = i;
			return true;
		}
	}

	return false;
}

struct wlr_texture_set *wlr_texture_set_from_buffer(struct wlr_renderer *renderer,
		struct wlr_buffer *buffer) {
	/* Get an empty texture set */
	struct wlr_texture_set *set = wlr_texture_set_from_renderer(renderer, buffer);
	if (!set) {
		return NULL;
	}

	if (!wlr_texture_set_import_buffer(set, buffer)) {
		goto fail;
	}

	return set;

fail:
	/* If the buffer couldn't be imported into any renderer in the system, return NULL */
	wlr_texture_set_destroy(set);
	return NULL;
}

static void *read_pixels(struct wlr_renderer *renderer, uint32_t format,
		struct wlr_buffer *src_buffer) {
	int stride = src_buffer->width * 4;
	uint8_t *data = malloc(src_buffer->height * stride);
	if (data == NULL) {
		return false;
	}

	struct wlr_buffer *src = wlr_buffer_lock(src_buffer);
	if (!wlr_renderer_begin_with_buffer(renderer, src)) {
		wlr_buffer_unlock(src);
		return false;
	}

	bool result = wlr_renderer_read_pixels(
		renderer, DRM_FORMAT_ARGB8888,
		stride, src->width, src->height, 0, 0, 0, 0,
		data);

	wlr_renderer_end(renderer);
	wlr_buffer_unlock(src);

	if (!result) {
		free(data);
		return false;
	}

	return data;
}

static bool get_drm_format(struct wlr_renderer *renderer, struct wlr_buffer *buffer,
		uint32_t *format_out) {
	/* Attach the original buffer for this set before doing renderer operations */
	if (!wlr_renderer_begin_with_buffer(renderer, buffer)) {
		return false;
	}

	*format_out = renderer->impl->preferred_read_format(renderer);

	wlr_renderer_end(renderer);
	return true;
}

static bool wlr_texture_set_get_linear_data(struct wlr_texture_set *set) {
	struct wlr_renderer *native_renderer = set->pairings[set->native_pair].renderer;
	struct wlr_texture *native_texture = set->pairings[set->native_pair].texture;
	assert(native_texture);

	if (set->pixel_data) {
		return true;
	}

	/* Make a buffer with a linear layout and the same format */
	if (!get_drm_format(native_renderer, set->buffer, &set->format)) {
		return false;
	}

	set->pixel_data = read_pixels(native_renderer, set->format, set->buffer);
	if (!set->pixel_data) {
		return false;
	}

	return true;
}

struct wlr_texture *wlr_texture_set_get_tex_for_renderer(struct wlr_texture_set *set,
	struct wlr_renderer *renderer) {
	/*
	 * Because this function will be called on-demand to get textures, it may be called
	 * (such as in sway) while in the middle of the stream of drawing commands. If we
	 * do not save and restore the EGL context then when we return to the user they will
	 * continue calling drawing commends but the current EGL context will have been reset.
	 */
	struct wlr_egl_context egl_context;
	wlr_egl_save_context(&egl_context);

	/*
	 * If we haven't imported the buffer for the first time, do so now.
	 */
	if (set->native_pair < 0) {
		if (!wlr_texture_set_import_buffer(set, set->buffer)) {
			goto fail;
		}
	}

	/* Find the entry for this renderer */
	struct wlr_texture_renderer_pair *pair = NULL;
	for (int i = 0; i < set->pairing_count; i++) {
		if (set->pairings[i].renderer == renderer) {
			pair = &set->pairings[i];
		}
	}

	if (!pair) {
		goto fail;
	}

	/* If we already have a texture for this renderer, return it */
	if (pair->texture) {
		goto success;
	}

	/* first try to directly import the texture */
	pair->texture = wlr_texture_from_buffer(renderer, set->buffer);
	if (pair->texture) {
		goto success;
	}

	/* Get our linear pixel data so we can import it into the target renderer */
	if (!wlr_texture_set_get_linear_data(set)) {
		goto fail;
	}

	/* import the linear texture into our renderer */
	uint32_t stride = set->width * 4;
	pair->texture = wlr_texture_from_pixels(renderer, DRM_FORMAT_ARGB8888, stride, set->width,
			set->height, set->pixel_data);

success:
	wlr_egl_restore_context(&egl_context);
	return pair->texture;

fail:
	wlr_egl_restore_context(&egl_context);
	return NULL;
}

struct wlr_texture *wlr_texture_set_get_primary_texture(struct wlr_texture_set *set) {
	/*
	 * If we have a multi-GPU setup, then use the primary renderer. Otherwise the native
	 * texture is the only one in the set, so return that.
	 */
	if (set->multi_gpu) {
		return wlr_texture_set_get_tex_for_renderer(set, set->multi_gpu->primary);
	} else {
		return wlr_texture_set_get_native_texture(set);
	}
}

struct wlr_texture *wlr_texture_set_get_native_texture(struct wlr_texture_set *set) {
	return set->pairings[set->native_pair].texture;
}

bool wlr_texture_set_update_from_buffer(struct wlr_texture_set *set,
		struct wlr_buffer *next, const pixman_region32_t *damage) {
	/* Call wlr_texture_write_pixels on each valid texture in the set */
	for (int i = 0; i < set->pairing_count; i++) {
		if (set->pairings[i].texture) {
			if (!wlr_texture_update_from_buffer(set->pairings[i].texture,
						next, damage)) {
				return false;
			}
		}
	}

	return true;
}

void wlr_texture_set_destroy(struct wlr_texture_set *set) {
	wlr_buffer_unlock(set->buffer);
	free(set->pixel_data);

	for (int i = 0; i < set->pairing_count; i++) {
		wl_list_remove(&set->pairings[i].renderer_destroy.link);
		if (set->pairings[i].texture) {
			wlr_texture_destroy(set->pairings[i].texture);
		}
	}

	if (set) {
		free(set->pairings);
		free(set);
	}
}
