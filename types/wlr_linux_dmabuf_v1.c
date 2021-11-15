#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <unistd.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_surface.h>
#include <wlr/util/log.h>
#include "linux-dmabuf-unstable-v1-protocol.h"
#include "render/drm_format_set.h"
#include "util/signal.h"
#include <sys/mman.h>
#include "util/shm.h"

#define LINUX_DMABUF_VERSION 4

struct wlr_linux_dmabuf_v1_surface {
	struct wlr_surface *surface;
	struct wlr_linux_dmabuf_v1 *linux_dmabuf;
	struct wl_list link; // wlr_linux_dmabuf_v1.surfaces

	bool has_feedback;
	struct wlr_linux_dmabuf_feedback_v1 feedback;

	struct wl_list feedback_resources; // wl_resource_get_link
	struct wl_listener surface_destroy;
};

static void buffer_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_buffer_interface wl_buffer_impl = {
	.destroy = buffer_handle_destroy,
};

bool wlr_dmabuf_v1_resource_is_buffer(struct wl_resource *resource) {
	if (!wl_resource_instance_of(resource, &wl_buffer_interface,
			&wl_buffer_impl)) {
		return false;
	}
	return wl_resource_get_user_data(resource) != NULL;
}

struct wlr_dmabuf_v1_buffer *wlr_dmabuf_v1_buffer_from_buffer_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wl_buffer_interface,
		&wl_buffer_impl));
	return wl_resource_get_user_data(resource);
}

static const struct wlr_buffer_impl buffer_impl;

static struct wlr_dmabuf_v1_buffer *dmabuf_v1_buffer_from_buffer(
		struct wlr_buffer *buffer) {
	assert(buffer->impl == &buffer_impl);
	return (struct wlr_dmabuf_v1_buffer *)buffer;
}

static void buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct wlr_dmabuf_v1_buffer *buffer =
		dmabuf_v1_buffer_from_buffer(wlr_buffer);
	if (buffer->resource != NULL) {
		wl_resource_set_user_data(buffer->resource, NULL);
	}
	wlr_dmabuf_attributes_finish(&buffer->attributes);
	wl_list_remove(&buffer->release.link);
	free(buffer);
}

static bool buffer_get_dmabuf(struct wlr_buffer *wlr_buffer,
		struct wlr_dmabuf_attributes *attribs) {
	struct wlr_dmabuf_v1_buffer *buffer =
		dmabuf_v1_buffer_from_buffer(wlr_buffer);
	memcpy(attribs, &buffer->attributes, sizeof(buffer->attributes));
	return true;
}

static const struct wlr_buffer_impl buffer_impl = {
	.destroy = buffer_destroy,
	.get_dmabuf = buffer_get_dmabuf,
};

static void buffer_handle_release(struct wl_listener *listener, void *data) {
	struct wlr_dmabuf_v1_buffer *buffer =
		wl_container_of(listener, buffer, release);
	if (buffer->resource != NULL) {
		wl_buffer_send_release(buffer->resource);
	}
}

static const struct zwp_linux_buffer_params_v1_interface buffer_params_impl;

static struct wlr_linux_buffer_params_v1 *params_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zwp_linux_buffer_params_v1_interface, &buffer_params_impl));
	return wl_resource_get_user_data(resource);
}

static void params_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void params_add(struct wl_client *client,
		struct wl_resource *params_resource, int32_t fd,
		uint32_t plane_idx, uint32_t offset, uint32_t stride,
		uint32_t modifier_hi, uint32_t modifier_lo) {
	struct wlr_linux_buffer_params_v1 *params =
		params_from_resource(params_resource);
	if (!params) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
			"params was already used to create a wl_buffer");
		close(fd);
		return;
	}

	if (plane_idx >= WLR_DMABUF_MAX_PLANES) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
			"plane index %u > %u", plane_idx, WLR_DMABUF_MAX_PLANES);
		close(fd);
		return;
	}

	if (params->attributes.fd[plane_idx] != -1) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
			"a dmabuf with FD %d has already been added for plane %u",
			params->attributes.fd[plane_idx], plane_idx);
		close(fd);
		return;
	}

	uint64_t modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;
	if (params->has_modifier && modifier != params->attributes.modifier) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
			"sent modifier %" PRIu64 " for plane %u, expected"
			" modifier %" PRIu64 " like other planes",
			modifier, plane_idx, params->attributes.modifier);
		close(fd);
		return;
	}

	params->attributes.modifier = modifier;
	params->has_modifier = true;

	params->attributes.fd[plane_idx] = fd;
	params->attributes.offset[plane_idx] = offset;
	params->attributes.stride[plane_idx] = stride;
	params->attributes.n_planes++;
}

static void buffer_handle_resource_destroy(struct wl_resource *buffer_resource) {
	struct wlr_dmabuf_v1_buffer *buffer =
		wlr_dmabuf_v1_buffer_from_buffer_resource(buffer_resource);
	buffer->resource = NULL;
	wlr_buffer_drop(&buffer->base);
}

static bool check_import_dmabuf(struct wlr_linux_dmabuf_v1 *linux_dmabuf,
		struct wlr_dmabuf_attributes *attribs) {
	struct wlr_texture *texture =
		wlr_texture_from_dmabuf(linux_dmabuf->renderer, attribs);
	if (texture == NULL) {
		return false;
	}

	// We can import the image, good. No need to keep it since wlr_surface will
	// import it again on commit.
	wlr_texture_destroy(texture);
	return true;
}

static void params_create_common(struct wl_resource *params_resource,
		uint32_t buffer_id, int32_t width, int32_t height, uint32_t format,
		uint32_t flags) {
	struct wlr_linux_buffer_params_v1 *params =
		params_from_resource(params_resource);
	if (!params) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
			"params was already used to create a wl_buffer");
		return;
	}

	struct wlr_dmabuf_attributes attribs = params->attributes;
	struct wlr_linux_dmabuf_v1 *linux_dmabuf = params->linux_dmabuf;

	// Make the params resource inert
	wl_resource_set_user_data(params_resource, NULL);
	free(params);

	if (!attribs.n_planes) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
			"no dmabuf has been added to the params");
		goto err_out;
	}

	if (attribs.fd[0] == -1) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
			"no dmabuf has been added for plane 0");
		goto err_out;
	}

	if ((attribs.fd[3] >= 0 || attribs.fd[2] >= 0) &&
			(attribs.fd[2] == -1 || attribs.fd[1] == -1)) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
			"gap in dmabuf planes");
		goto err_out;
	}

	/* reject unknown flags */
	uint32_t all_flags = ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT |
		ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_INTERLACED |
		ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_BOTTOM_FIRST;
	if (flags & ~all_flags) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
			"Unknown dmabuf flags %"PRIu32, flags);
		goto err_out;
	}

	if (flags != 0) {
		wlr_log(WLR_ERROR, "dmabuf flags aren't supported");
		goto err_failed;
	}

	attribs.width = width;
	attribs.height = height;
	attribs.format = format;

	if (width < 1 || height < 1) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS,
			"invalid width %d or height %d", width, height);
		goto err_out;
	}

	for (int i = 0; i < attribs.n_planes; i++) {
		if ((uint64_t)attribs.offset[i]
				+ attribs.stride[i] > UINT32_MAX) {
			wl_resource_post_error(params_resource,
				ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
				"size overflow for plane %d", i);
			goto err_out;
		}

		if ((uint64_t)attribs.offset[i]
				+ (uint64_t)attribs.stride[i] * height > UINT32_MAX) {
			wl_resource_post_error(params_resource,
				ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
				"size overflow for plane %d", i);
			goto err_out;
		}

		off_t size = lseek(attribs.fd[i], 0, SEEK_END);
		if (size == -1) {
			// Skip checks if kernel does no support seek on buffer
			continue;
		}
		if (attribs.offset[i] > size) {
			wl_resource_post_error(params_resource,
				ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
				"invalid offset %" PRIu32 " for plane %d",
				attribs.offset[i], i);
			goto err_out;
		}

		if (attribs.offset[i] + attribs.stride[i] > size ||
				attribs.stride[i] == 0) {
			wl_resource_post_error(params_resource,
				ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
				"invalid stride %" PRIu32 " for plane %d",
				attribs.stride[i], i);
			goto err_out;
		}

		// planes > 0 might be subsampled according to fourcc format
		if (i == 0 && attribs.offset[i] +
				attribs.stride[i] * height > size) {
			wl_resource_post_error(params_resource,
				ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
				"invalid buffer stride or height for plane %d", i);
			goto err_out;
		}
	}

	/* Check if dmabuf is usable */
	if (!check_import_dmabuf(linux_dmabuf, &attribs)) {
		goto err_failed;
	}

	struct wlr_dmabuf_v1_buffer *buffer = calloc(1, sizeof(*buffer));
	if (!buffer) {
		wl_resource_post_no_memory(params_resource);
		goto err_failed;
	}
	wlr_buffer_init(&buffer->base, &buffer_impl, attribs.width, attribs.height);

	struct wl_client *client = wl_resource_get_client(params_resource);
	buffer->resource = wl_resource_create(client, &wl_buffer_interface,
		1, buffer_id);
	if (!buffer->resource) {
		wl_resource_post_no_memory(params_resource);
		free(buffer);
		goto err_failed;
	}
	wl_resource_set_implementation(buffer->resource,
		&wl_buffer_impl, buffer, buffer_handle_resource_destroy);

	buffer->attributes = attribs;

	buffer->release.notify = buffer_handle_release;
	wl_signal_add(&buffer->base.events.release, &buffer->release);

	/* send 'created' event when the request is not for an immediate
	 * import, that is buffer_id is zero */
	if (buffer_id == 0) {
		zwp_linux_buffer_params_v1_send_created(params_resource,
			buffer->resource);
	}

	return;

err_failed:
	if (buffer_id == 0) {
		zwp_linux_buffer_params_v1_send_failed(params_resource);
	} else {
		/* since the behavior is left implementation defined by the
		 * protocol in case of create_immed failure due to an unknown cause,
		 * we choose to treat it as a fatal error and immediately kill the
		 * client instead of creating an invalid handle and waiting for it
		 * to be used.
		 */
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
			"importing the supplied dmabufs failed");
	}
err_out:
	wlr_dmabuf_attributes_finish(&attribs);
}

static void params_create(struct wl_client *client,
		struct wl_resource *params_resource,
		int32_t width, int32_t height, uint32_t format, uint32_t flags) {
	params_create_common(params_resource, 0, width, height, format,
		flags);
}

static void params_create_immed(struct wl_client *client,
		struct wl_resource *params_resource, uint32_t buffer_id,
		int32_t width, int32_t height, uint32_t format, uint32_t flags) {
	params_create_common(params_resource, buffer_id, width, height,
		format, flags);
}

static const struct zwp_linux_buffer_params_v1_interface buffer_params_impl = {
	.destroy = params_destroy,
	.add = params_add,
	.create = params_create,
	.create_immed = params_create_immed,
};

static void params_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_linux_buffer_params_v1 *params = params_from_resource(resource);
	if (!params) {
		return;
	}
	wlr_dmabuf_attributes_finish(&params->attributes);
	free(params);
}

static const struct zwp_linux_dmabuf_v1_interface linux_dmabuf_impl;

static struct wlr_linux_dmabuf_v1 *linux_dmabuf_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zwp_linux_dmabuf_v1_interface,
			&linux_dmabuf_impl));

	struct wlr_linux_dmabuf_v1 *dmabuf = wl_resource_get_user_data(resource);
	assert(dmabuf);
	return dmabuf;
}

static void linux_dmabuf_create_params(struct wl_client *client,
		struct wl_resource *linux_dmabuf_resource,
		uint32_t params_id) {
	struct wlr_linux_dmabuf_v1 *linux_dmabuf =
		linux_dmabuf_from_resource(linux_dmabuf_resource);

	struct wlr_linux_buffer_params_v1 *params = calloc(1, sizeof(*params));
	if (!params) {
		wl_resource_post_no_memory(linux_dmabuf_resource);
		return;
	}

	for (int i = 0; i < WLR_DMABUF_MAX_PLANES; i++) {
		params->attributes.fd[i] = -1;
	}

	params->linux_dmabuf = linux_dmabuf;

	uint32_t version = wl_resource_get_version(linux_dmabuf_resource);
	params->resource = wl_resource_create(client,
		&zwp_linux_buffer_params_v1_interface, version, params_id);
	if (!params->resource) {
		free(params);
		wl_resource_post_no_memory(linux_dmabuf_resource);
		return;
	}
	wl_resource_set_implementation(params->resource,
		&buffer_params_impl, params, params_handle_resource_destroy);
}

static void linux_dmabuf_feedback_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwp_linux_dmabuf_feedback_v1_interface
		linux_dmabuf_feedback_impl = {
	.destroy = linux_dmabuf_feedback_destroy,
};

static bool feedback_tranche_init_with_renderer(struct wlr_linux_dmabuf_v1 *linux_dmabuf,
		struct wlr_linux_dmabuf_feedback_v1_tranche *tranche,
		struct wlr_renderer *renderer) {
	memset(tranche, 0, sizeof(*tranche));

	int drm_fd = wlr_renderer_get_drm_fd(renderer);
	if (drm_fd < 0) {
		wlr_log(WLR_ERROR, "Failed to get DRM FD from renderer");
		return false;
	}

	struct stat stat;
	if (fstat(drm_fd, &stat) != 0) {
		wlr_log_errno(WLR_ERROR, "fstat failed");
		return false;
	}
	tranche->target_device = stat.st_rdev;

	const struct wlr_drm_format_set *formats =
		wlr_renderer_get_dmabuf_texture_formats(renderer);
	if (formats == NULL) {
		wlr_log(WLR_ERROR, "Failed to get renderer DMA-BUF texture formats");
		return false;
	}

	return true;
}

static bool feedback_init_with_renderer(struct wlr_linux_dmabuf_v1 *linux_dmabuf,
		struct wlr_linux_dmabuf_feedback_v1 *feedback,
		struct wlr_renderer *renderer) {
	memset(feedback, 0, sizeof(*feedback));

	struct wlr_linux_dmabuf_feedback_v1_tranche *tranche =
		calloc(1, sizeof(*tranche));
	if (tranche == NULL) {
		return false;
	}

	if (!feedback_tranche_init_with_renderer(linux_dmabuf, tranche, renderer)) {
		free(tranche);
		return false;
	}

	feedback->main_device = tranche->target_device;
	feedback->tranches = tranche;
	feedback->tranches_len = 1;

	return true;
}

static void feedback_finish(struct wlr_linux_dmabuf_feedback_v1 *feedback) {
	free(feedback->tranches);
}

static bool feedback_copy(struct wlr_linux_dmabuf_feedback_v1 *dst,
		const struct wlr_linux_dmabuf_feedback_v1 *src) {
	memset(dst, 0, sizeof(*dst));

	dst->main_device = src->main_device;
	dst->tranches_len = src->tranches_len;

	dst->tranches = calloc(src->tranches_len,
		sizeof(struct wlr_linux_dmabuf_feedback_v1_tranche));
	if (dst->tranches == NULL) {
		return false;
	}

	for (size_t i = 0; i < src->tranches_len; i++) {
		dst->tranches[i].target_device = src->tranches[i].target_device;
		dst->tranches[i].flags = src->tranches[i].flags;
	}

	return true;
}

static void feedback_tranche_send(
		struct wlr_linux_dmabuf_v1 *linux_dmabuf,
		struct wlr_linux_dmabuf_feedback_v1_tranche *tranche,
		struct wl_resource *resource) {
	struct wl_array dev_array = {
		.size = sizeof(tranche->target_device),
		.data = (void *)&tranche->target_device,
	};
	zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(resource, &dev_array);

	zwp_linux_dmabuf_feedback_v1_send_tranche_flags(resource, tranche->flags);

	zwp_linux_dmabuf_feedback_v1_send_tranche_formats(resource, &linux_dmabuf->format_table.indices);

	zwp_linux_dmabuf_feedback_v1_send_tranche_done(resource);
}

static void feedback_send(struct wlr_linux_dmabuf_v1 *linux_dmabuf,
	const struct wlr_linux_dmabuf_feedback_v1 *feedback,
		struct wl_resource *resource) {
	struct wl_array dev_array = {
		.size = sizeof(feedback->main_device),
		.data = (void *)&feedback->main_device,
	};
	zwp_linux_dmabuf_feedback_v1_send_main_device(resource, &dev_array);

	zwp_linux_dmabuf_feedback_v1_send_format_table(resource,
			linux_dmabuf->format_table.ro_fd,
			linux_dmabuf->format_table.len * sizeof(struct wlr_dmabuf_format_table_entry));

	for (size_t i = 0; i < feedback->tranches_len; i++) {
		feedback_tranche_send(linux_dmabuf, &feedback->tranches[i], resource);
	}

	zwp_linux_dmabuf_feedback_v1_send_done(resource);
}

static void linux_dmabuf_get_default_feedback(struct wl_client *client,
		struct wl_resource *resource, uint32_t id) {
	struct wlr_linux_dmabuf_v1 *linux_dmabuf =
		linux_dmabuf_from_resource(resource);

	uint32_t version = wl_resource_get_version(resource);
	struct wl_resource *feedback_resource = wl_resource_create(client,
		&zwp_linux_dmabuf_feedback_v1_interface, version, id);
	if (feedback_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(feedback_resource, &linux_dmabuf_feedback_impl,
		NULL, NULL);

	feedback_send(linux_dmabuf, &linux_dmabuf->default_feedback, feedback_resource);
}

static void surface_destroy(struct wlr_linux_dmabuf_v1_surface *surface) {
	struct wl_resource *resource, *resource_tmp;
	wl_resource_for_each_safe(resource, resource_tmp, &surface->feedback_resources) {
		struct wl_list *link = wl_resource_get_link(resource);
		wl_list_remove(link);
		wl_list_init(link);
	}

	if (surface->has_feedback) {
		feedback_finish(&surface->feedback);
	}

	wl_list_remove(&surface->surface_destroy.link);
	wl_list_remove(&surface->link);
	free(surface);
}

static void surface_handle_surface_destroy(struct wl_listener *listener,
		void *data) {
	struct wlr_linux_dmabuf_v1_surface *surface =
		wl_container_of(listener, surface, surface_destroy);
	surface_destroy(surface);
}

static struct wlr_linux_dmabuf_v1_surface *surface_get_or_create(
		struct wlr_linux_dmabuf_v1 *linux_dmabuf,
		struct wlr_surface *wlr_surface) {
	struct wlr_linux_dmabuf_v1_surface *surface;
	wl_list_for_each(surface, &linux_dmabuf->surfaces, link) {
		if (surface->surface == wlr_surface) {
			return surface;
		}
	}

	surface = calloc(1, sizeof(*surface));
	if (surface == NULL) {
		return NULL;
	}

	surface->surface = wlr_surface;
	surface->linux_dmabuf = linux_dmabuf;
	wl_list_init(&surface->feedback_resources);

	surface->surface_destroy.notify = surface_handle_surface_destroy;
	wl_signal_add(&wlr_surface->events.destroy, &surface->surface_destroy);

	wl_list_insert(&linux_dmabuf->surfaces, &surface->link);

	return surface;
}

static const struct wlr_linux_dmabuf_feedback_v1 *surface_get_feedback(
		struct wlr_linux_dmabuf_v1_surface *surface) {
	if (surface->has_feedback) {
		return &surface->feedback;
	}
	return &surface->linux_dmabuf->default_feedback;
}

static void surface_feedback_handle_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

static void linux_dmabuf_get_surface_feedback(struct wl_client *client,
		struct wl_resource *resource, uint32_t id,
		struct wl_resource *surface_resource) {
	struct wlr_linux_dmabuf_v1 *linux_dmabuf =
		linux_dmabuf_from_resource(resource);
	struct wlr_surface *wlr_surface = wlr_surface_from_resource(surface_resource);

	struct wlr_linux_dmabuf_v1_surface *surface =
		surface_get_or_create(linux_dmabuf, wlr_surface);
	if (surface == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	uint32_t version = wl_resource_get_version(resource);
	struct wl_resource *feedback_resource = wl_resource_create(client,
		&zwp_linux_dmabuf_feedback_v1_interface, version, id);
	if (feedback_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(feedback_resource, &linux_dmabuf_feedback_impl,
		NULL, surface_feedback_handle_resource_destroy);
	wl_list_insert(&surface->feedback_resources, wl_resource_get_link(feedback_resource));

	feedback_send(linux_dmabuf, surface_get_feedback(surface), feedback_resource);
}

static void linux_dmabuf_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwp_linux_dmabuf_v1_interface linux_dmabuf_impl = {
	.destroy = linux_dmabuf_destroy,
	.create_params = linux_dmabuf_create_params,
	.get_default_feedback = linux_dmabuf_get_default_feedback,
	.get_surface_feedback = linux_dmabuf_get_surface_feedback,
};

static void linux_dmabuf_send_modifiers(struct wl_resource *resource,
		const struct wlr_drm_format *fmt) {
	if (wl_resource_get_version(resource) < ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION) {
		zwp_linux_dmabuf_v1_send_format(resource, fmt->format);
		return;
	}

	for (size_t i = 0; i < fmt->len; i++) {
		uint64_t mod = fmt->modifiers[i];
		zwp_linux_dmabuf_v1_send_modifier(resource, fmt->format,
			mod >> 32, mod & 0xFFFFFFFF);
	}

	// We always support buffers with an implicit modifier
	zwp_linux_dmabuf_v1_send_modifier(resource, fmt->format,
		DRM_FORMAT_MOD_INVALID >> 32, DRM_FORMAT_MOD_INVALID & 0xFFFFFFFF);
}

static void linux_dmabuf_send_formats(struct wlr_linux_dmabuf_v1 *linux_dmabuf,
		struct wl_resource *resource) {
	const struct wlr_drm_format_set *formats =
		wlr_renderer_get_dmabuf_texture_formats(linux_dmabuf->renderer);
	if (formats == NULL) {
		return;
	}

	for (size_t i = 0; i < formats->len; i++) {
		const struct wlr_drm_format *fmt = formats->formats[i];
		linux_dmabuf_send_modifiers(resource, fmt);
	}
}

static void linux_dmabuf_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wlr_linux_dmabuf_v1 *linux_dmabuf = data;

	struct wl_resource *resource = wl_resource_create(client,
		&zwp_linux_dmabuf_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &linux_dmabuf_impl,
		linux_dmabuf, NULL);
	linux_dmabuf_send_formats(linux_dmabuf, resource);
}

static void linux_dmabuf_v1_destroy(struct wlr_linux_dmabuf_v1 *linux_dmabuf) {
	wlr_signal_emit_safe(&linux_dmabuf->events.destroy, linux_dmabuf);

	struct wlr_linux_dmabuf_v1_surface *surface, *surface_tmp;
	wl_list_for_each_safe(surface, surface_tmp, &linux_dmabuf->surfaces, link) {
		surface_destroy(surface);
	}

	feedback_finish(&linux_dmabuf->default_feedback);

	wl_list_remove(&linux_dmabuf->display_destroy.link);
	wl_list_remove(&linux_dmabuf->renderer_destroy.link);

	wl_global_destroy(linux_dmabuf->global);
	free(linux_dmabuf);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_linux_dmabuf_v1 *linux_dmabuf =
		wl_container_of(listener, linux_dmabuf, display_destroy);
	linux_dmabuf_v1_destroy(linux_dmabuf);
}

static void handle_renderer_destroy(struct wl_listener *listener, void *data) {
	struct wlr_linux_dmabuf_v1 *linux_dmabuf =
		wl_container_of(listener, linux_dmabuf, renderer_destroy);
	linux_dmabuf_v1_destroy(linux_dmabuf);
}

static bool dmabuf_format_table_init(struct wlr_linux_dmabuf_v1 *linux_dmabuf,
	struct wlr_renderer *renderer) {
	const struct wlr_drm_format_set *formats =
		wlr_renderer_get_dmabuf_texture_formats(renderer);
	if (formats == NULL) {
		wlr_log(WLR_ERROR, "Failed to get renderer DMA-BUF texture formats");
		return false;
	}

	/*
	 * The format list we were given doesn't include the modifiers in its
	 * len. To calculate format_table size we need to go through and count
	 * format+modifier pairs.
	 */
	int len = 0;
	for (size_t i = 0; i < formats->len; i++) {
		const struct wlr_drm_format *fmt = formats->formats[i];
		len += fmt->len;
	}
	size_t size = len * sizeof(struct wlr_dmabuf_format_table_entry);

	/* Make a temp file to hold the format table so we can share the fd */
	int fd, ro_fd;
	if (!allocate_shm_file_pair(size, &fd, &ro_fd)) {
		wlr_log(WLR_ERROR, "failed to create anonymous file\n");
		return false;
	}

	struct wlr_dmabuf_format_table_entry *data =
		mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		wlr_log(WLR_ERROR, "failed to mmap()\n");
		close(fd);
		return false;
	}

	wl_array_init(&linux_dmabuf->format_table.indices);
	if (!wl_array_add(&linux_dmabuf->format_table.indices, len * sizeof(uint16_t))) {
		wlr_log(WLR_ERROR, "failed to grow the size of the format indices array\n");
		close(fd);
		wl_array_release(&linux_dmabuf->format_table.indices);
		return false;
	}

	int idx = 0;
	for (size_t i = 0; i < formats->len; i++) {
		const struct wlr_drm_format *fmt = formats->formats[i];
		for (size_t j = 0; j < fmt->len; j++) {
			data[idx].format = fmt->format;
			data[idx].modifier = fmt->modifiers[j];
			idx++;
		}
	}
	assert(idx == len);

	/* Now calculate the indices (in order, nothing fancy) */
	idx = 0;
	uint16_t *index_ptr;
	wl_array_for_each(index_ptr, &linux_dmabuf->format_table.indices) {
		*index_ptr = idx++;
	}

	linux_dmabuf->format_table.fd = fd;
	linux_dmabuf->format_table.ro_fd = ro_fd;
	linux_dmabuf->format_table.len = formats->len;
	linux_dmabuf->format_table.data = data;
	return true;
}

static void wlr_dmabuf_format_table_free(struct wlr_linux_dmabuf_v1 *linux_dmabuf) {
	wl_array_release(&linux_dmabuf->format_table.indices);
	free(linux_dmabuf->format_table.data);
	close(linux_dmabuf->format_table.fd);
}

struct wlr_linux_dmabuf_v1 *wlr_linux_dmabuf_v1_create(struct wl_display *display,
		struct wlr_renderer *renderer) {
	struct wlr_linux_dmabuf_v1 *linux_dmabuf =
		calloc(1, sizeof(struct wlr_linux_dmabuf_v1));
	if (linux_dmabuf == NULL) {
		wlr_log(WLR_ERROR, "could not create simple dmabuf manager");
		return NULL;
	}
	linux_dmabuf->renderer = renderer;

	wl_list_init(&linux_dmabuf->surfaces);
	wl_signal_init(&linux_dmabuf->events.destroy);

	/* Fill in the format table */
	if (!dmabuf_format_table_init(linux_dmabuf, renderer)) {
		wlr_log(WLR_ERROR, "Failed to init linux-dmabuf format table");
		wl_global_destroy(linux_dmabuf->global);
		free(linux_dmabuf);
		return NULL;
	}

	linux_dmabuf->global =
		wl_global_create(display, &zwp_linux_dmabuf_v1_interface,
			LINUX_DMABUF_VERSION, linux_dmabuf, linux_dmabuf_bind);
	if (!linux_dmabuf->global) {
		wlr_log(WLR_ERROR, "could not create linux dmabuf v1 wl global");
		free(linux_dmabuf);
		wlr_dmabuf_format_table_free(linux_dmabuf);
		return NULL;
	}

	if (!feedback_init_with_renderer(linux_dmabuf, &linux_dmabuf->default_feedback, renderer)) {
		wlr_log(WLR_ERROR, "Failed to init default linux-dmabuf feedback");
		wl_global_destroy(linux_dmabuf->global);
		free(linux_dmabuf);
		wlr_dmabuf_format_table_free(linux_dmabuf);
		return NULL;
	}

	linux_dmabuf->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &linux_dmabuf->display_destroy);

	linux_dmabuf->renderer_destroy.notify = handle_renderer_destroy;
	wl_signal_add(&renderer->events.destroy, &linux_dmabuf->renderer_destroy);

	return linux_dmabuf;
}

bool wlr_linux_dmabuf_v1_set_surface_feedback(
		struct wlr_linux_dmabuf_v1 *linux_dmabuf,
		struct wlr_surface *wlr_surface,
		const struct wlr_linux_dmabuf_feedback_v1 *feedback) {
	struct wlr_linux_dmabuf_v1_surface *surface =
		surface_get_or_create(linux_dmabuf, wlr_surface);
	if (surface == NULL) {
		return false;
	}

	if (surface->has_feedback) {
		feedback_finish(&surface->feedback);
		surface->has_feedback = false;
	}

	if (feedback != NULL) {
		if (!feedback_copy(&surface->feedback, feedback)) {
			return false;
		}
		surface->has_feedback = true;
	}

	struct wl_resource *resource;
	wl_resource_for_each(resource, &surface->feedback_resources) {
		feedback_send(linux_dmabuf, surface_get_feedback(surface), resource);
	}

	return true;
}
