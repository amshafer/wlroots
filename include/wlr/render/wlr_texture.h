/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_RENDER_WLR_TEXTURE_H
#define WLR_RENDER_WLR_TEXTURE_H

#include <pixman.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/render/dmabuf.h>

struct wlr_buffer;
struct wlr_renderer;
struct wlr_texture_impl;
struct wlr_multi_gpu;

struct wlr_texture {
	const struct wlr_texture_impl *impl;
	uint32_t width, height;

	struct wlr_renderer *renderer;
};

/**
 * Create a new texture from raw pixel data. `stride` is in bytes. The returned
 * texture is mutable.
 */
struct wlr_texture *wlr_texture_from_pixels(struct wlr_renderer *renderer,
	uint32_t fmt, uint32_t stride, uint32_t width, uint32_t height,
	const void *data);

/**
 * Create a new texture from a DMA-BUF. The returned texture is immutable.
 */
struct wlr_texture *wlr_texture_from_dmabuf(struct wlr_renderer *renderer,
	struct wlr_dmabuf_attributes *attribs);

/**
  * Update a texture with a struct wlr_buffer's contents.
  *
  * The update might be rejected (in case the texture is immutable, the buffer
  * has an unsupported type/format, etc), so callers must be prepared to fall
  * back to re-creating the texture from scratch via wlr_texture_from_buffer().
  *
  * The damage can be used by the renderer as an optimization: only the supplied
  * region needs to be updated.
  */
bool wlr_texture_update_from_buffer(struct wlr_texture *texture,
	struct wlr_buffer *buffer, const pixman_region32_t *damage);

/**
 * Destroys the texture.
 */
void wlr_texture_destroy(struct wlr_texture *texture);

/**
 * Create a new texture from a buffer.
 */
struct wlr_texture *wlr_texture_from_buffer(struct wlr_renderer *renderer,
	struct wlr_buffer *buffer);

struct wlr_texture_renderer_pair {
	struct wlr_renderer *renderer;
	struct wlr_texture *texture;

	struct wl_listener renderer_destroy;
};

/**
 * The texture set provides a mapping between renderers and the texture
 * imported into them. You can use it to query a texture for a particular
 * renderer and it will handle importing and any blitting that needs to
 * take place.
 */
struct wlr_texture_set {
	/* The buffer this texture set was made from */
	struct wlr_buffer *buffer;
	/**
	 * Index into pairings of the device that this texture directly
	 * imports into. This texture is "native" to that device, and
	 * will have to be blitted to other gpus.
	 *
	 * This will be -1 if no buffer has been imported yet.
	 */
	int32_t native_pair;
	struct wlr_multi_gpu *multi_gpu;
	/*
	 * This will cache the result of creating a linear-layout version of
	 * this texture on the native device. This can then be imported into
	 * the other GPUs.
	 */
	uint32_t format;
	void *pixel_data;

	uint32_t width;
	uint32_t height;

	/* This is the size of the pairings array */
	int pairing_count;
	struct wlr_texture_renderer_pair *pairings;
};

/**
 * When setting up our wlr_multi_gpu struct we put all renderers into a list. This lets us
 * iterate them from here. If this request is made on a renderer not in the multi-GPU set,
 * then the list will be of length 1, and the renderer will be the only entry in the set.
 */
struct wlr_texture_set *wlr_texture_set_from_renderer(struct wlr_renderer *renderer,
	struct wlr_buffer *buffer);

/**
 * Create a new texture set from a DMA-BUF. The returned texture is immutable.
 * The dmabuf will be imported on only one of the mgpu renderers in the system,
 * no copies will be made. Returns NULL if the dmabuf could not be imported into
 * any renderer.
 */
struct wlr_texture_set *wlr_texture_set_from_dmabuf(struct wlr_renderer *renderer,
	struct wlr_dmabuf_attributes *attribs);

/**
 * Create a new texture set from a buffer.
 */
struct wlr_texture_set *wlr_texture_set_from_buffer(struct wlr_renderer *renderer,
	struct wlr_buffer *buffer);

/**
 * Request a wlr_texture for this resource that is compatible with the given
 * renderer. This allows for on-demand cross-GPU blits in multi-GPU setups.
 * The texture will have been imported into the renderer that corresponds to
 * its native device. If a texture is requeseted with a different renderer,
 * this function will perform a blit and return the appropriate texture.
 *
 * Textures are cached, so if multiple requests with a non-native renderer
 * are made there will be only one blit.
 */
struct wlr_texture *wlr_texture_set_get_tex_for_renderer(struct wlr_texture_set *set,
	struct wlr_renderer *renderer);

/**
 * Get the wlr_texture corresponding to the primary GPU use for rendered composition.
 */
struct wlr_texture *wlr_texture_set_get_primary_texture(struct wlr_texture_set *set);

/**
 * Get the wlr_texture corresponding to the texture's local GPU. This is the GPU it
 * is directly importable into.
 */
struct wlr_texture *wlr_texture_set_get_native_texture(struct wlr_texture_set *set);

/**
  * Update all textures in a set with the contents of the next buffer. This will call
  * wlr_texture_update_from_buffer for each texture in the set.
  */
bool wlr_texture_set_update_from_buffer(struct wlr_texture_set *set,
		struct wlr_buffer *next, const pixman_region32_t *damage);

void wlr_texture_set_destroy(struct wlr_texture_set *set);
#endif
