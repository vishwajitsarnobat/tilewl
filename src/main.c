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

#include "server.h"

static void usage(const char *argv0) {
	printf("Usage: %s [-s startup command]\n", argv0);
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
		default:
			usage(argv[0]);
			return 0;
		}
	}
	if (optind < argc) {
		usage(argv[0]);
		return 0;
	}

	struct tilewl_server server = {0};

	/*
	 * The display owns the Unix socket, the client connections, the global
	 * registry, and the event loop everything else plugs into.
	 */
	server.wl_display = wl_display_create();

	/*
	 * The backend abstracts input and output hardware. autocreate picks DRM
	 * + libinput on a TTY, or opens a window if WAYLAND_DISPLAY or DISPLAY
	 * points at a running session. On a TTY it also opens a seat session
	 * (logind or seatd) for device access and VT switching.
	 */
	server.backend = wlr_backend_autocreate(
		wl_display_get_event_loop(server.wl_display), &server.session);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		return 1;
	}

	/*
	 * The renderer (GLES2, Vulkan or Pixman; override with WLR_RENDERER)
	 * composites the scene. It also tells clients which buffer formats it
	 * can import (wl_shm, linux-dmabuf).
	 */
	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return 1;
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
		return 1;
	}

	/*
	 * Protocol globals we do not customise: surfaces, subsurfaces and the
	 * clipboard / drag-and-drop device.
	 */
	wlr_compositor_create(server.wl_display, 5, server.renderer);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);

	/* Arrangement of outputs in one global coordinate space. */
	server.output_layout = wlr_output_layout_create(server.wl_display);

	server.scene = wlr_scene_create();
	server.scene_layout =
		wlr_scene_attach_output_layout(server.scene, server.output_layout);

	output_init(&server);
	xdg_init(&server);
	input_init(&server);

	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (socket == NULL) {
		wlr_backend_destroy(server.backend);
		return 1;
	}

	/*
	 * Starting the backend enumerates outputs and inputs (firing new_output
	 * and new_input) and, on a TTY, makes us DRM master.
	 */
	if (!wlr_backend_start(server.backend)) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
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

	/*
	 * Teardown. Order matters:
	 *  1. Disconnect clients first, while everything their destroy handlers
	 *     touch (scene, seat, our lists) is still alive.
	 *  2. Unlink our server-level listeners from the signals of objects we
	 *     are about to destroy.
	 *  3. Destroy the objects, children before the things they depend on.
	 *     wl_display_destroy() comes last because it also destroys the
	 *     globals (seat, xdg_shell, output layout, ...).
	 */
	wl_display_destroy_clients(server.wl_display);

	input_finish(&server);
	xdg_finish(&server);
	output_finish(&server);

	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.wl_display);
	return 0;
}
