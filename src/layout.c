/*
 * layout.c - master-stack tiling.
 *
 *   +-------------+--------+
 *   |             |   2    |
 *   |   master    +--------+
 *   |     1       |   3    |
 *   |             +--------+
 *   |             |   4    |
 *   +-------------+--------+
 *    <- ratio * W ->
 *
 * The first visible window in server->toplevels is the master; the rest
 * share the right-hand column equally. arrange() is cheap and stateless, so
 * we simply call it after anything that could change the layout: map,
 * unmap, output resize, workspace switches and the layout keybindings.
 *
 * Workspaces are just an index per window. Switching workspaces does not
 * touch the clients at all: arrange() disables the scene nodes of windows
 * on other workspaces, so the scene graph skips them when rendering and
 * when looking up what is under the pointer.
 */
#include <wlr/util/box.h>

#include "config.h"
#include "server.h"

bool toplevel_is_visible(const struct tilewl_toplevel *toplevel) {
	/* TODO(floating): floating windows are visible but not tiled. */
	return toplevel->mapped &&
		toplevel->workspace == toplevel->server->workspace;
}

struct tilewl_toplevel *first_visible(struct tilewl_server *server) {
	struct tilewl_toplevel *toplevel;
	wl_list_for_each(toplevel, &server->toplevels, link) {
		if (toplevel_is_visible(toplevel)) {
			return toplevel;
		}
	}
	return NULL;
}

/*
 * The next visible toplevel after `from` in tiling order, going forwards
 * (dir > 0) or backwards (dir < 0) and wrapping around. Returns NULL if
 * `from` is the only visible one.
 */
static struct tilewl_toplevel *visible_after(struct tilewl_server *server,
		struct tilewl_toplevel *from, int dir) {
	struct wl_list *pos = &from->link;
	for (;;) {
		pos = dir > 0 ? pos->next : pos->prev;
		if (pos == &from->link) {
			return NULL;
		}
		if (pos == &server->toplevels) {
			/* The list head is a sentinel, not a toplevel; step over it. */
			continue;
		}
		struct tilewl_toplevel *toplevel =
			wl_container_of(pos, toplevel, link);
		if (toplevel_is_visible(toplevel)) {
			return toplevel;
		}
	}
}

static struct tilewl_output *primary_output(struct tilewl_server *server) {
	if (wl_list_empty(&server->outputs)) {
		return NULL;
	}
	struct tilewl_output *output =
		wl_container_of(server->outputs.next, output, link);
	return output;
}

void arrange(struct tilewl_server *server) {
	/*
	 * TODO(multi-monitor): this tiles every window onto the first output.
	 * Loop over server->outputs instead and tile each output's windows into
	 * its own box.
	 */
	struct tilewl_output *output = primary_output(server);
	if (output == NULL) {
		return;
	}

	struct wlr_box area;
	wlr_output_layout_get_box(server->output_layout, output->wlr_output,
		&area);
	if (wlr_box_empty(&area)) {
		return;
	}
	/*
	 * TODO(layer-shell): shrink `area` by the exclusive zones of panels
	 * (output->usable_area) before tiling.
	 */

	/* Show this workspace's windows, hide everything else. */
	int n = 0;
	struct tilewl_toplevel *toplevel;
	wl_list_for_each(toplevel, &server->toplevels, link) {
		bool visible = toplevel_is_visible(toplevel);
		wlr_scene_node_set_enabled(&toplevel->scene_tree->node, visible);
		if (visible) {
			n++;
		}
	}

	if (n > 0) {
		int master_width = n > 1 ?
			(int)(area.width * server->master_ratio) : area.width;
		int stack_count = n - 1;
		int stack_y = area.y;
		int i = 0;

		wl_list_for_each(toplevel, &server->toplevels, link) {
			if (!toplevel_is_visible(toplevel)) {
				continue;
			}
			struct wlr_box box;
			if (i == 0) {
				box = (struct wlr_box){
					.x = area.x,
					.y = area.y,
					.width = master_width,
					.height = area.height,
				};
			} else {
				/*
				 * Divide what is left of the column by the windows still to
				 * place, so rounding leftovers are spread out and the last
				 * window ends exactly at the bottom edge.
				 */
				int remaining = stack_count - (i - 1);
				int height = (area.y + area.height - stack_y) / remaining;
				box = (struct wlr_box){
					.x = area.x + master_width,
					.y = stack_y,
					.width = area.width - master_width,
					.height = height,
				};
				stack_y += height;
			}
			toplevel_set_geometry(toplevel, box);
			i++;
		}
	}

	/* Windows moved under a stationary pointer: refresh pointer focus. */
	cursor_rebase(server);
}

/* ---- keybinding actions ------------------------------------------------ */

void focus_stack(struct tilewl_server *server, const union tilewl_arg *arg) {
	/* Mod+j / Mod+k: move focus down/up the stack, wrapping around. */
	if (server->focused == NULL) {
		focus_toplevel(server, first_visible(server));
		return;
	}
	struct tilewl_toplevel *next =
		visible_after(server, server->focused, arg->i);
	if (next != NULL) {
		focus_toplevel(server, next);
	}
}

void zoom(struct tilewl_server *server, const union tilewl_arg *arg) {
	/*
	 * Mod+Return: promote the focused window to master. If it already is
	 * the master, promote the next one instead (dwm behaviour), so pressing
	 * it twice swaps the top two windows.
	 */
	struct tilewl_toplevel *toplevel = server->focused;
	struct tilewl_toplevel *master = first_visible(server);
	if (toplevel == NULL || master == NULL) {
		return;
	}
	if (toplevel == master) {
		toplevel = visible_after(server, master, +1);
		if (toplevel == NULL) {
			return;
		}
	}
	wl_list_remove(&toplevel->link);
	wl_list_insert(&server->toplevels, &toplevel->link);
	focus_toplevel(server, toplevel);
	arrange(server);
}

void set_master_ratio(struct tilewl_server *server,
		const union tilewl_arg *arg) {
	/* Mod+h / Mod+l: shrink or grow the master column. */
	float ratio = server->master_ratio + arg->f;
	if (ratio < 0.1f || ratio > 0.9f) {
		return;
	}
	server->master_ratio = ratio;
	arrange(server);
}

void view_workspace(struct tilewl_server *server, const union tilewl_arg *arg) {
	if (arg->ui >= NUM_WORKSPACES || arg->ui == server->workspace) {
		return;
	}
	server->workspace = arg->ui;
	arrange(server);
	/* Focus the master of the new workspace, or nothing if it is empty. */
	focus_toplevel(server, first_visible(server));
}

void move_to_workspace(struct tilewl_server *server,
		const union tilewl_arg *arg) {
	struct tilewl_toplevel *toplevel = server->focused;
	if (toplevel == NULL || arg->ui >= NUM_WORKSPACES ||
			arg->ui == toplevel->workspace) {
		return;
	}
	toplevel->workspace = arg->ui;
	/*
	 * The window is no longer visible here, so first_visible() skips it
	 * and focus moves to the master of the current workspace.
	 */
	focus_toplevel(server, first_visible(server));
	arrange(server);
}
