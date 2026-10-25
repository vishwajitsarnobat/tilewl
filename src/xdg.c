/*
 * xdg.c - application windows (xdg_toplevel) and their popups.
 *
 * xdg-shell is the protocol regular desktop applications use to get a
 * window. A client creates a wl_surface, gives it the xdg_toplevel role, and
 * then negotiates its size and state with us through configure/ack events.
 *
 * Lifetime of a toplevel:
 *   new_toplevel -> (initial commit, we configure) -> map -> ... -> unmap
 *   -> (possibly map again) -> destroy
 * Our struct lives from new_toplevel to destroy. It is in the tiling list,
 * and its scene node is enabled, only between map and unmap.
 */
#include <stdlib.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#include "config.h"
#include "server.h"

/* ---- focus and geometry ------------------------------------------------ */

static void toplevel_set_border_color(struct tilewl_toplevel *toplevel,
		const float color[static 4]) {
	for (int i = 0; i < 4; i++) {
		wlr_scene_rect_set_color(toplevel->border[i], color);
	}
}

void focus_toplevel(struct tilewl_server *server,
		struct tilewl_toplevel *toplevel) {
	/*
	 * Keyboard focus only; pointer focus follows the pointer. NULL clears
	 * focus, e.g. when the last window on screen goes away.
	 */
	struct tilewl_toplevel *prev = server->focused;
	if (prev == toplevel) {
		return;
	}
	if (prev != NULL) {
		/* Let the old window draw itself as inactive (e.g. hide the caret). */
		wlr_xdg_toplevel_set_activated(prev->xdg_toplevel, false);
		toplevel_set_border_color(prev, border_color_unfocused);
	}

	server->focused = toplevel;
	if (toplevel == NULL) {
		wlr_seat_keyboard_notify_clear_focus(server->seat);
		return;
	}

	/* TODO(floating): raise a focused floating window to the top here. */
	wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
	toplevel_set_border_color(toplevel, border_color_focused);

	/*
	 * From here on wlroots routes key events to this surface. The enter
	 * event carries the currently pressed keys and modifiers.
	 */
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(server->seat,
			toplevel->xdg_toplevel->base->surface, keyboard->keycodes,
			keyboard->num_keycodes, &keyboard->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(server->seat,
			toplevel->xdg_toplevel->base->surface, NULL, 0, NULL);
	}
}

static void toplevel_update_clip(struct tilewl_toplevel *toplevel) {
	/*
	 * A client may draw a buffer larger than the size we configured, e.g.
	 * because of its own minimum size. Crop it to the tile so it cannot
	 * cover its neighbours. The clip box is in the coordinates of the root
	 * surface, where the window geometry starts at (geometry.x, geometry.y);
	 * anything outside it is client-side shadow.
	 */
	struct wlr_box *geo = &toplevel->xdg_toplevel->base->geometry;
	struct wlr_box clip = {
		.x = geo->x,
		.y = geo->y,
		.width = toplevel->geometry.width - 2 * border_width,
		.height = toplevel->geometry.height - 2 * border_width,
	};
	wlr_scene_subsurface_tree_set_clip(&toplevel->surface_tree->node, &clip);
}

void toplevel_set_geometry(struct tilewl_toplevel *toplevel,
		struct wlr_box box) {
	/* Keep at least 1x1 pixels for the client even in a tiny tile. */
	int bw = border_width;
	if (box.width < 2 * bw + 1) {
		box.width = 2 * bw + 1;
	}
	if (box.height < 2 * bw + 1) {
		box.height = 2 * bw + 1;
	}
	toplevel->geometry = box;
	int inner_width = box.width - 2 * bw;
	int inner_height = box.height - 2 * bw;

	/*
	 * Everything below is relative to the container, so the whole window
	 * moves with this one call.
	 */
	wlr_scene_node_set_position(&toplevel->scene_tree->node, box.x, box.y);

	/* Four solid rects frame the client area. */
	wlr_scene_rect_set_size(toplevel->border[0], box.width, bw);
	wlr_scene_rect_set_size(toplevel->border[1], box.width, bw);
	wlr_scene_node_set_position(&toplevel->border[1]->node,
		0, box.height - bw);
	wlr_scene_rect_set_size(toplevel->border[2], bw, inner_height);
	wlr_scene_node_set_position(&toplevel->border[2]->node, 0, bw);
	wlr_scene_rect_set_size(toplevel->border[3], bw, inner_height);
	wlr_scene_node_set_position(&toplevel->border[3]->node,
		box.width - bw, bw);

	wlr_scene_node_set_position(&toplevel->surface_tree->node, bw, bw);
	wlr_scene_node_set_position(&toplevel->popup_tree->node, bw, bw);

	/*
	 * Ask the client to resize. This only queues a configure event; the
	 * client draws a buffer at the new size whenever it is ready, so for a
	 * frame or two the old buffer may still be shown (clipped).
	 */
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
		inner_width, inner_height);
	toplevel_update_clip(toplevel);
}

struct tilewl_toplevel *toplevel_at(struct tilewl_server *server,
		double lx, double ly, struct wlr_surface **surface,
		double *sx, double *sy) {
	/*
	 * Find the topmost enabled scene node under layout coordinates
	 * (lx, ly). If it is a client buffer, also report the surface and the
	 * surface-local coordinates.
	 */
	*surface = NULL;
	struct wlr_scene_node *node =
		wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
	if (node == NULL) {
		return NULL;
	}
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *scene_buffer =
			wlr_scene_buffer_from_node(node);
		struct wlr_scene_surface *scene_surface =
			wlr_scene_surface_try_from_buffer(scene_buffer);
		if (scene_surface != NULL) {
			*surface = scene_surface->surface;
		}
	}

	/*
	 * Walk up to the toplevel's container tree; it is the only node whose
	 * data pointer we set.
	 */
	struct wlr_scene_tree *tree = node->parent;
	while (tree != NULL && tree->node.data == NULL) {
		tree = tree->node.parent;
	}
	return tree != NULL ? tree->node.data : NULL;
}

/* ---- toplevel events --------------------------------------------------- */

static void toplevel_handle_map(struct wl_listener *listener, void *data) {
	/* The client committed its first buffer; it can be shown now. */
	struct tilewl_toplevel *toplevel = wl_container_of(listener, toplevel, map);
	struct tilewl_server *server = toplevel->server;

	toplevel->mapped = true;
	/*
	 * As in dwm, a new window becomes the master and the previous master
	 * moves to the top of the stack.
	 */
	wl_list_insert(&server->toplevels, &toplevel->link);
	wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);

	arrange(server);
	focus_toplevel(server, toplevel);
}

static void toplevel_handle_unmap(struct wl_listener *listener, void *data) {
	struct tilewl_toplevel *toplevel =
		wl_container_of(listener, toplevel, unmap);
	struct tilewl_server *server = toplevel->server;

	toplevel->mapped = false;
	wl_list_remove(&toplevel->link);
	wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);

	/*
	 * Do not leave server->focused pointing at a window that may be freed
	 * right after this. Hand focus to the master, if any.
	 */
	if (server->focused == toplevel) {
		server->focused = NULL;
		focus_toplevel(server, first_visible(server));
	}
	arrange(server);
}

static void toplevel_handle_commit(struct wl_listener *listener, void *data) {
	struct tilewl_toplevel *toplevel =
		wl_container_of(listener, toplevel, commit);
	struct wlr_xdg_toplevel *xdg_toplevel = toplevel->xdg_toplevel;

	if (xdg_toplevel->base->initial_commit) {
		/*
		 * The client cannot map until we answer its first commit with a
		 * configure. Tell it that it is tiled on all sides (so it drops
		 * shadows and rounded corners) and let it pick its first size with
		 * 0x0; arrange() gives it the real size once it maps.
		 */
		wlr_xdg_toplevel_set_tiled(xdg_toplevel, WLR_EDGE_TOP |
			WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
		wlr_xdg_toplevel_set_size(xdg_toplevel, 0, 0);
		return;
	}

	/* The window geometry may have changed with this commit. */
	if (toplevel->mapped) {
		toplevel_update_clip(toplevel);
	}
}

static void toplevel_handle_destroy(struct wl_listener *listener, void *data) {
	struct tilewl_toplevel *toplevel =
		wl_container_of(listener, toplevel, destroy);

	/*
	 * Unmap always precedes destroy for a mapped surface, so the toplevel
	 * is no longer in server->toplevels or server->focused here.
	 *
	 * Unlink every listener: the xdg_toplevel and its wl_surface are about
	 * to be freed, and so is this struct.
	 */
	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->request_move.link);
	wl_list_remove(&toplevel->request_resize.link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);

	/*
	 * The container tree is ours. Destroying it also destroys the borders,
	 * the popup tree and the surface tree inside it; the scene helper notices via its own node-destroy
	 * listener and cleans up, so this is safe whether or not wlroots has
	 * already torn the surface tree down.
	 */
	wlr_scene_node_destroy(&toplevel->scene_tree->node);
	/* That freed popup_tree; do not leave base->data dangling. */
	toplevel->xdg_toplevel->base->data = NULL;
	free(toplevel);
}

static void toplevel_handle_request_move(struct wl_listener *listener,
		void *data) {
	/*
	 * The user dragged the client-side title bar. Tiled windows stay put.
	 * TODO(floating): start an interactive move grab for floating windows
	 * (and validate the serial against a recent button press).
	 */
}

static void toplevel_handle_request_resize(struct wl_listener *listener,
		void *data) {
	/*
	 * TODO(floating): start an interactive resize grab from
	 * ((struct wlr_xdg_toplevel_resize_event *)data)->edges for floating
	 * windows. Tiled windows are sized by the layout.
	 */
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
	/*
	 * TODO(fullscreen): honour xdg_toplevel->requested.fullscreen: call
	 * wlr_xdg_toplevel_set_fullscreen(), size the window to the whole
	 * output, raise it above the tiled layer and skip it in arrange().
	 * For now we refuse by replying with an unchanged configure.
	 */
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
	 * Scene layout of one window:
	 *
	 *   scene_tree      (ours; positioned by the layout, data = toplevel)
	 *   |- border[0..3] (solid colour rects around the client area)
	 *   |- surface_tree (wlroots' mirror of the surface + subsurfaces)
	 *   `- popup_tree   (parent for popups, so the clip on surface_tree
	 *                    does not crop menus that extend past the tile)
	 *
	 * Hidden until the client maps.
	 */
	toplevel->scene_tree = wlr_scene_tree_create(server->tiled);
	toplevel->scene_tree->node.data = toplevel;
	wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
	for (int i = 0; i < 4; i++) {
		/* Sized in toplevel_set_geometry(). */
		toplevel->border[i] = wlr_scene_rect_create(toplevel->scene_tree,
			0, 0, border_color_unfocused);
	}
	toplevel->surface_tree = wlr_scene_xdg_surface_create(
		toplevel->scene_tree, xdg_toplevel->base);
	toplevel->popup_tree = wlr_scene_tree_create(toplevel->scene_tree);
	/* Lets new popups find the tree they should attach to. */
	xdg_toplevel->base->data = toplevel->popup_tree;

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
		/*
		 * Popups also need a configure before they can map. A smarter
		 * compositor would constrain the popup to stay on screen here.
		 */
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

static void popup_handle_destroy(struct wl_listener *listener, void *data) {
	/* The popup's scene tree is destroyed by wlroots with the surface. */
	struct tilewl_popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

static void server_handle_new_popup(struct wl_listener *listener, void *data) {
	struct wlr_xdg_popup *xdg_popup = data;

	/*
	 * A popup is drawn as a child of its parent's tree, so it moves and
	 * hides together with the parent window. The parent's xdg_surface data
	 * points at that tree (popup_tree for a toplevel, the popup's own tree
	 * for nested menus).
	 *
	 * TODO(layer-shell): popups of layer surfaces (e.g. bar menus) have a
	 * layer surface as parent, which is not an xdg_surface.
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
	server->focused = NULL;
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
