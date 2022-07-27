#ifndef BACKEND_MULTI_H
#define BACKEND_MULTI_H

#include <wayland-util.h>
#include <wlr/backend/interface.h>
#include <wlr/backend/multi.h>

/*
 * Helper struct for tracking multiple renderers. This solves the
 * problem of us having many renderers (primary, plus individual
 * secondary GPU drm renderers) but not tracking them in one location.
 * We can use this struct to access renderers for each GPU in
 * the system all from one place. Will be populated by the renderer
 * the compositor makes, plus every time a drm mgpu renderer is made.
 */
struct wlr_multi_gpu {
	struct wlr_renderer *primary;
	struct wl_list renderers;
};

struct wlr_multi_backend {
	struct wlr_backend backend;

	struct wlr_multi_gpu *multi_gpu;
	struct wl_list backends;

	struct wl_listener display_destroy;

	struct {
		struct wl_signal backend_add;
		struct wl_signal backend_remove;
	} events;
};

#endif
