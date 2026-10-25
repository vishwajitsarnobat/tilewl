/*
 * xdg.c - application windows (xdg_toplevel) and their popups.
 *
 * xdg-shell is the protocol regular desktop applications use to get a
 * window. A client creates a wl_surface, gives it the xdg_toplevel role, and
 * then negotiates its size and state with us through configure/ack events.
 */
#include <stdlib.h>
#include <wlr/util/log.h>

#include "server.h"

void focus_toplevel(struct tilewl_toplevel *toplevel) {
	/* Keyboard focus only; pointer focus follows the pointer. */
	if (toplevel == NULL) {
		return;
	}
	struct tilewl_server *server = toplevel->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
	if (prev_surface == surface) {
		return;
	}
	if (prev_surface != NULL) {
		/* Let the old window draw itself as inactive (e.g. hide the caret). */
		struct wlr_xdg_toplevel *prev_toplevel =
			wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
		if (prev_toplevel != NULL) {
			wlr_xdg_toplevel_set_activated(prev_toplevel, false);
		}
	}

	wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	wl_list_remove(&toplevel->link);
	wl_list_insert(&server->toplevels, &toplevel->link);
	wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);

	/*
	 * From here on wlroots routes key events to this surface. The enter
	 * event carries the currently pressed keys and modifiers.
	 */
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes,
			keyboard->num_keycodes, &keyboard->modifiers);
	}
}

struct tilewl_toplevel *toplevel_at(struct tilewl_server *server,
		double lx, double ly, struct wlr_surface **surface,
		double *sx, double *sy) {
	/*
	 * Find the topmost scene node under layout coordinates (lx, ly). If it
	 * is a client buffer, also report the surface and surface-local coords.
	 */
	struct wlr_scene_node *node =
		wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (scene_surface == NULL) {
		return NULL;
	}
	*surface = scene_surface->surface;

	/*
	 * Walk up to the toplevel's root tree; it is the only node whose data
	 * pointer we set.
	 */
	struct wlr_scene_tree *tree = node->parent;
	while (tree != NULL && tree->node.data == NULL) {
		tree = tree->node.parent;
	}
	return tree != NULL ? tree->node.data : NULL;
}

/* ---- toplevels --------------------------------------------------------- */

static void toplevel_handle_map(struct wl_listener *listener, void *data) {
	/* The client committed its first buffer; it can be shown now. */
	struct tilewl_toplevel *toplevel = wl_container_of(listener, toplevel, map);
	wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
	focus_toplevel(toplevel);
}

static void toplevel_handle_unmap(struct wl_listener *listener, void *data) {
	struct tilewl_toplevel *toplevel =
		wl_container_of(listener, toplevel, unmap);
	/* Never leave a grab pointing at a window that is going away. */
	if (toplevel == toplevel->server->grabbed_toplevel) {
		reset_cursor_mode(toplevel->server);
	}
	wl_list_remove(&toplevel->link);
}

static void toplevel_handle_commit(struct wl_listener *listener, void *data) {
	struct tilewl_toplevel *toplevel =
		wl_container_of(listener, toplevel, commit);
	if (toplevel->xdg_toplevel->base->initial_commit) {
		/*
		 * The client cannot map until we answer its first commit with a
		 * configure. A 0x0 size means "pick your own size".
		 */
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
	}
}

static void toplevel_handle_destroy(struct wl_listener *listener, void *data) {
	struct tilewl_toplevel *toplevel =
		wl_container_of(listener, toplevel, destroy);

	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->request_move.link);
	wl_list_remove(&toplevel->request_resize.link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);
	free(toplevel);
}

static void toplevel_handle_request_move(struct wl_listener *listener,
		void *data) {
	/*
	 * The user dragged the client-side title bar. A stricter compositor
	 * would validate the serial against a recent button press.
	 */
	struct tilewl_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_move);
	begin_interactive(toplevel, TILEWL_CURSOR_MOVE, 0);
}

static void toplevel_handle_request_resize(struct wl_listener *listener,
		void *data) {
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct tilewl_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_resize);
	begin_interactive(toplevel, TILEWL_CURSOR_RESIZE, event->edges);
}

static void toplevel_handle_request_maximize(struct wl_listener *listener,
		void *data) {
	/*
	 * We do not maximize, but xdg-shell requires a configure in reply.
	 * Before the initial commit there is nothing to configure yet.
	 */
	struct tilewl_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_maximize);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void toplevel_handle_request_fullscreen(struct wl_listener *listener,
		void *data) {
	struct tilewl_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_fullscreen);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void server_handle_new_toplevel(struct wl_listener *listener,
		void *data) {
	struct tilewl_server *server =
		wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct tilewl_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	if (toplevel == NULL) {
		wlr_log(WLR_ERROR, "out of memory");
		return;
	}
	toplevel->server = server;
	toplevel->xdg_toplevel = xdg_toplevel;

	/*
	 * The helper builds a scene subtree that mirrors the surface and its
	 * subsurfaces, and destroys itself when the xdg_surface is destroyed.
	 * base->data lets popups find the tree to attach to.
	 */
	toplevel->scene_tree =
		wlr_scene_xdg_surface_create(&server->scene->tree, xdg_toplevel->base);
	toplevel->scene_tree->node.data = toplevel;
	xdg_toplevel->base->data = toplevel->scene_tree;

	struct wlr_surface *surface = xdg_toplevel->base->surface;
	toplevel->map.notify = toplevel_handle_map;
	wl_signal_add(&surface->events.map, &toplevel->map);
	toplevel->unmap.notify = toplevel_handle_unmap;
	wl_signal_add(&surface->events.unmap, &toplevel->unmap);
	toplevel->commit.notify = toplevel_handle_commit;
	wl_signal_add(&surface->events.commit, &toplevel->commit);

	toplevel->destroy.notify = toplevel_handle_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);
	toplevel->request_move.notify = toplevel_handle_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
	toplevel->request_resize.notify = toplevel_handle_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize,
		&toplevel->request_resize);
	toplevel->request_maximize.notify = toplevel_handle_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize,
		&toplevel->request_maximize);
	toplevel->request_fullscreen.notify = toplevel_handle_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen,
		&toplevel->request_fullscreen);
}

/* ---- popups ------------------------------------------------------------ */

static void popup_handle_commit(struct wl_listener *listener, void *data) {
	struct tilewl_popup *popup = wl_container_of(listener, popup, commit);
	if (popup->xdg_popup->base->initial_commit) {
		/* Popups also need a configure before they can map. */
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

static void popup_handle_destroy(struct wl_listener *listener, void *data) {
	struct tilewl_popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

static void server_handle_new_popup(struct wl_listener *listener, void *data) {
	struct wlr_xdg_popup *xdg_popup = data;

	/*
	 * A popup is rendered as a child of its parent's scene tree, so it moves
	 * and hides together with the parent window.
	 */
	struct wlr_xdg_surface *parent =
		wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	if (parent == NULL || parent->data == NULL) {
		wlr_log(WLR_ERROR, "popup without an xdg parent, ignoring");
		return;
	}

	struct tilewl_popup *popup = calloc(1, sizeof(*popup));
	if (popup == NULL) {
		wlr_log(WLR_ERROR, "out of memory");
		return;
	}
	popup->xdg_popup = xdg_popup;

	struct wlr_scene_tree *parent_tree = parent->data;
	xdg_popup->base->data =
		wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

	popup->commit.notify = popup_handle_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
	popup->destroy.notify = popup_handle_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

/* ---- setup and teardown ------------------------------------------------ */

void xdg_init(struct tilewl_server *server) {
	wl_list_init(&server->toplevels);
	server->xdg_shell = wlr_xdg_shell_create(server->wl_display, 3);
	server->new_xdg_toplevel.notify = server_handle_new_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel,
		&server->new_xdg_toplevel);
	server->new_xdg_popup.notify = server_handle_new_popup;
	wl_signal_add(&server->xdg_shell->events.new_popup,
		&server->new_xdg_popup);
}

void xdg_finish(struct tilewl_server *server) {
	/* The xdg_shell global itself dies with the display. */
	wl_list_remove(&server->new_xdg_toplevel.link);
	wl_list_remove(&server->new_xdg_popup.link);
}
