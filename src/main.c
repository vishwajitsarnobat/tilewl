/*
 * tilewl - a small tiling Wayland compositor built on wlroots.
 *
 * Based on tinywl from wlroots 0.19.3 (CC0, see LICENSES/tinywl-CC0.txt).
 *
 * main.c creates the core wlroots objects, hands them to the modules, runs
 * the event loop, and tears everything down in a safe order afterwards.
 */
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/log.h>

#include "config.h"
#include "server.h"

static void usage(const char *argv0) {
	printf("Usage: %s [-s startup command]\n", argv0);
}

static int handle_terminate_signal(int signal_number, void *data) {
	/*
	 * Runs from the event loop (via a signalfd), not in signal context, so
	 * it is safe to call anything here. Leaving the loop this way, rather
	 * than dying on the signal, means Ctrl+C or `timeout` still go through
	 * the full teardown below.
	 */
	struct tilewl_server *server = data;
	wl_display_terminate(server->wl_display);
	return 0;
}

int main(int argc, char *argv[]) {
	wlr_log_init(WLR_DEBUG, NULL);
	char *startup_cmd = NULL;

	int c;
	while ((c = getopt(argc, argv, "s:h")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}
	if (optind < argc) {
		usage(argv[0]);
		return 1;
	}

	struct tilewl_server server = {0};
	int status = 1;

	/*
	 * The display owns the Unix socket, the client connections, the global
	 * registry, and the event loop everything else plugs into.
	 */
	server.wl_display = wl_display_create();
	if (server.wl_display == NULL) {
		wlr_log(WLR_ERROR, "failed to create wl_display");
		return 1;
	}
	struct wl_event_loop *loop = wl_display_get_event_loop(server.wl_display);

	/*
	 * The backend abstracts input and output hardware. autocreate picks DRM
	 * + libinput on a TTY, or opens a window if WAYLAND_DISPLAY or DISPLAY
	 * points at a running session. On a TTY it also opens a seat session
	 * (logind or seatd) for device access and VT switching.
	 */
	server.backend = wlr_backend_autocreate(loop, &server.session);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		goto out_display;
	}

	/*
	 * The renderer (GLES2, Vulkan or Pixman; override with WLR_RENDERER)
	 * composites the scene. It also tells clients which buffer formats it
	 * can import (wl_shm, linux-dmabuf).
	 */
	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		goto out_backend;
	}
	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	/*
	 * The allocator creates the buffers we render into, in a format both the
	 * renderer can draw to and the backend can scan out (e.g. GBM buffers
	 * for DRM).
	 */
	server.allocator = wlr_allocator_autocreate(server.backend,
		server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		goto out_renderer;
	}

	/*
	 * Protocol globals we do not customise: surfaces, subsurfaces and the
	 * clipboard / drag-and-drop device. They belong to the display.
	 */
	wlr_compositor_create(server.wl_display, 5, server.renderer);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);

	/* Arrangement of outputs in one global coordinate space. */
	server.output_layout = wlr_output_layout_create(server.wl_display);

	server.scene = wlr_scene_create();
	server.scene_layout =
		wlr_scene_attach_output_layout(server.scene, server.output_layout);
	server.tiled = wlr_scene_tree_create(&server.scene->tree);
	server.master_ratio = master_ratio;
	server.workspace = 0;

	output_init(&server);
	xdg_init(&server);
	input_init(&server);

	struct wl_event_source *sigint = wl_event_loop_add_signal(loop, SIGINT,
		handle_terminate_signal, &server);
	struct wl_event_source *sigterm = wl_event_loop_add_signal(loop, SIGTERM,
		handle_terminate_signal, &server);

	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (socket == NULL) {
		wlr_log(WLR_ERROR, "failed to open a Wayland socket");
		goto out_finish;
	}

	/*
	 * Starting the backend enumerates outputs and inputs (firing new_output
	 * and new_input) and, on a TTY, makes us DRM master.
	 */
	if (!wlr_backend_start(server.backend)) {
		wlr_log(WLR_ERROR, "failed to start backend");
		goto out_finish;
	}

	/*
	 * We never wait() for the programs we spawn. Ignoring SIGCHLD makes the
	 * kernel reap them as they exit instead of leaving zombies around.
	 */
	signal(SIGCHLD, SIG_IGN);

	/* Children inherit the environment, so they connect to us. */
	setenv("WAYLAND_DISPLAY", socket, true);
	if (startup_cmd != NULL) {
		const char *const argv[] = {"/bin/sh", "-c", startup_cmd, NULL};
		spawn(&server, &(union tilewl_arg){ .v = argv });
	}

	wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s",
		socket);
	wl_display_run(server.wl_display);
	status = 0;

	/*
	 * Teardown. Order matters:
	 *  1. Disconnect clients first. Their windows get unmapped and destroyed
	 *     while everything those handlers touch (scene, seat, our lists) is
	 *     still alive, and each toplevel unlinks and frees itself.
	 *  2. Unlink our server-level listeners. The objects that own those
	 *     signals are destroyed next, and wlroots asserts that nobody is
	 *     still listening when a signal's owner goes away.
	 *  3. Destroy objects before the things they were created from: scene
	 *     and allocator, then the renderer (it may use the backend's DRM
	 *     fd), then the backend (destroying outputs and input devices, which
	 *     fires our per-output and per-keyboard destroy handlers), and the
	 *     display last, which also destroys the globals (seat, xdg_shell,
	 *     output layout, ...).
	 */
out_finish:
	wl_display_destroy_clients(server.wl_display);
	if (sigint != NULL) {
		wl_event_source_remove(sigint);
	}
	if (sigterm != NULL) {
		wl_event_source_remove(sigterm);
	}

	input_finish(&server);
	xdg_finish(&server);
	output_finish(&server);

	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_allocator_destroy(server.allocator);
out_renderer:
	wlr_renderer_destroy(server.renderer);
out_backend:
	wlr_backend_destroy(server.backend);
out_display:
	wl_display_destroy(server.wl_display);
	return status;
}
