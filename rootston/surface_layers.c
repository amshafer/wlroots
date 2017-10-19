#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>
#include <wayland-server.h>
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_surface.h>
#include <wlr/types/wlr_surface_layers.h>
#include <wlr/util/log.h>
#include "surface-layers-protocol.h"
#include "rootston/desktop.h"
#include "rootston/server.h"
#include "rootston/input.h"

static void handle_commit(struct wl_listener *listener, void *data) {
	struct roots_layer_surface *roots_surface =
		wl_container_of(listener, roots_surface, commit);
	struct roots_desktop *desktop = roots_surface->desktop;

	// Handle keyboard input exclusivity
	struct wlr_layer_surface *surface = wlr_surface_layers_get_exclusive(
		desktop->surface_layers, LAYER_SURFACE_INPUT_DEVICE_KEYBOARD);
	if (surface) {
		wlr_seat_keyboard_notify_enter(desktop->server->input->wl_seat,
			surface->surface);
	}
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct roots_layer_surface *roots_surface =
		wl_container_of(listener, roots_surface, destroy);
	wl_list_remove(&roots_surface->destroy.link);
	wl_list_remove(&roots_surface->commit.link);
	free(roots_surface);
}

void handle_surface_layers_surface(struct wl_listener *listener, void *data) {
	struct roots_desktop *desktop =
		wl_container_of(listener, desktop, surface_layers_surface);

	struct wlr_layer_surface *surface = data;
	wlr_log(L_DEBUG, "new surface_layers surface at layer %d", surface->layer);

	struct roots_layer_surface *roots_surface =
		calloc(1, sizeof(struct roots_layer_surface));
	if (roots_surface == NULL) {
		return;
	}
	roots_surface->desktop = desktop;
	roots_surface->layer_surface = surface;

	roots_surface->destroy.notify = handle_destroy;
	wl_signal_add(&surface->events.destroy, &roots_surface->destroy);
	roots_surface->commit.notify = handle_commit;
	wl_signal_add(&surface->events.commit, &roots_surface->commit);
}
