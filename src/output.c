/*
 * output.c - monitors.
 *
 * An output is anything we can present frames on: a real monitor under the
 * DRM backend, or a window when we run nested inside another compositor.
 */
#include <stdlib.h>
#include <time.h>
#include <wlr/util/log.h>

#include "server.h"

static void output_handle_frame(struct wl_listener *listener, void *data) {
	/*
	 * Fired once per vblank-ish interval when the output is ready for a new
	 * frame. wlr_scene_output_commit() renders only if something in the
	 * scene is damaged, then submits the buffer (a page flip under DRM).
	 */
	struct tilewl_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
		output->server->scene, output->wlr_output);

	wlr_scene_output_commit(scene_output, NULL);

	/*
	 * Tell clients whose surfaces are visible on this output that now is a
	 * good time to draw their next frame (wl_surface.frame callbacks).
	 */
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_handle_request_state(struct wl_listener *listener,
		void *data) {
	/*
	 * The backend asks for a new state, e.g. the nested Wayland/X11 backend
	 * when the user resizes our window.
	 */
	struct tilewl_output *output =
		wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_handle_destroy(struct wl_listener *listener, void *data) {
	struct tilewl_output *output = wl_container_of(listener, output, destroy);

	/*
	 * The wlr_output is going away, and with it the signals our listeners
	 * are linked into. Unlink all of them before freeing the struct that
	 * contains them.
	 */
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);
}

static void server_handle_new_output(struct wl_listener *listener, void *data) {
	struct tilewl_server *server =
		wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	/* Use our renderer and allocator for this output's buffers. */
	if (!wlr_output_init_render(wlr_output, server->allocator,
			server->renderer)) {
		wlr_log(WLR_ERROR, "failed to init rendering on %s", wlr_output->name);
		return;
	}

	/*
	 * Output configuration is atomic: build up a wlr_output_state and commit
	 * it in one go. Under DRM a mode must be set before anything can be
	 * shown; we simply take the monitor's preferred one.
	 */
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	struct tilewl_output *output = calloc(1, sizeof(*output));
	if (output == NULL) {
		wlr_log(WLR_ERROR, "out of memory");
		return;
	}
	output->wlr_output = wlr_output;
	output->server = server;

	output->frame.notify = output_handle_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = output_handle_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = output_handle_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	/* Append, so the first output plugged in stays first in the list. */
	wl_list_insert(server->outputs.prev, &output->link);

	/*
	 * Place the output in the global layout (left to right in hotplug order;
	 * this also advertises a wl_output global to clients) and give the scene
	 * graph a viewport onto it.
	 */
	struct wlr_output_layout_output *l_output =
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	struct wlr_scene_output *scene_output =
		wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, l_output,
		scene_output);
}

static void server_handle_layout_change(struct wl_listener *listener,
		void *data) {
	/*
	 * Fired when an output is added to or removed from the layout, or
	 * changes mode, scale or transform (e.g. the nested window was resized).
	 * Any of these changes the space we tile into.
	 *
	 * TODO(multi-monitor): on hotplug, move the windows of a removed
	 * output to a remaining one and re-tile every output.
	 */
	struct tilewl_server *server =
		wl_container_of(listener, server, layout_change);
	arrange(server);
}

void output_init(struct tilewl_server *server) {
	wl_list_init(&server->outputs);
	server->new_output.notify = server_handle_new_output;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);
	server->layout_change.notify = server_handle_layout_change;
	wl_signal_add(&server->output_layout->events.change,
		&server->layout_change);
}

void output_finish(struct tilewl_server *server) {
	wl_list_remove(&server->new_output.link);
	wl_list_remove(&server->layout_change.link);
}
