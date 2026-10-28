/*
 * server.h - core data structures shared by every module.
 *
 * tilewl is a single-threaded, event-driven program. All state hangs off one
 * struct tilewl_server that lives on main()'s stack for the lifetime of the
 * process. Per-object state (outputs, windows, keyboards) is heap-allocated
 * when wlroots announces the object and freed when wlroots says it is gone.
 *
 * A note on wl_listener and wl_container_of, since they appear everywhere:
 * wlroots objects expose events as `struct wl_signal`, which is just an
 * intrusive linked list of `struct wl_listener`. We embed a wl_listener
 * inside our own struct, register it with wl_signal_add(), and when the
 * signal fires libwayland calls listener->notify(listener, data). The
 * callback only receives a pointer to the embedded listener, so it uses
 * wl_container_of(listener, ptr, member) - the same trick as the Linux
 * kernel's container_of() - to subtract the member's offset and recover a
 * pointer to the enclosing struct. No allocation, no void *user_data.
 *
 * The flip side is ownership: the signal's list holds raw pointers into our
 * structs. Before one of our structs is freed, every listener embedded in it
 * MUST be unlinked with wl_list_remove(), otherwise the next emit of that
 * signal walks into freed memory. wlroots 0.19 also asserts that a signal's
 * listener list is empty when its owner is destroyed.
 */
#ifndef TILEWL_SERVER_H
#define TILEWL_SERVER_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <xkbcommon/xkbcommon.h>

// TODO: create a toplevel prev_focus to store previously focused window
struct tilewl_server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	/* Logind/seatd session; NULL when running nested (no VT to switch). */
	struct wlr_session *session;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	/*
	 * The scene graph is a tree of nodes (trees, surface buffers, solid
	 * rects). We only ever position, show and hide nodes; wlroots works out
	 * damage and does the actual rendering.
	 *
	 * Children of a tree are drawn in order, so sibling trees act as
	 * stacking layers. For now there is a single layer for tiled windows.
	 *
	 * TODO(layer-shell): add trees for the background, bottom, top and
	 * overlay layers of wlr-layer-shell around `tiled`, in that order.
	 * TODO(floating): add a `floating` tree above `tiled`.
	 */
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_scene_tree *tiled;

	/* Outputs (output.c) */
	struct wlr_output_layout *output_layout;
	struct wl_list outputs; /* tilewl_output.link */
	struct wl_listener new_output;
	struct wl_listener layout_change;

	/* Application windows (xdg.c) */
	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	/*
	 * Mapped toplevels in tiling order: the first entry is the master.
	 * Keyboard focus is tracked separately so focusing a window does not
	 * reorder the layout.
	 */
	struct wl_list toplevels; /* tilewl_toplevel.link */
	struct tilewl_toplevel *focused;

	/* Layout (layout.c) */
	float master_ratio; /* fraction of the width given to the master */
	/*
	 * Index of the workspace being shown, 0 .. NUM_WORKSPACES - 1. A window
	 * is visible when its own workspace index equals this one.
	 * TODO(multi-monitor): move this into struct tilewl_output.
	 */
	unsigned int workspace;

	/* Pointer (input.c) */
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	/*
	 * TODO(floating): interactive move/resize. tinywl kept a grab mode
	 * here (cursor_mode, grabbed toplevel, grab offset and geometry,
	 * resize edges). It only makes sense for floating windows, so it was
	 * removed; bring it back together with the floating layer.
	 */

	/* Seat and keyboards (input.c) */
	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	struct wl_list keyboards; /* tilewl_keyboard.link */
};

struct tilewl_output {
	struct wl_list link;
	struct tilewl_server *server;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
	/*
	 * TODO(multi-monitor): give each output its own current workspace and
	 * tile its windows independently.
	 * TODO(layer-shell): keep a usable_area here (the output box minus
	 * exclusive zones of panels) and tile into that instead.
	 */
};

// TODO: arrange the windows in a tree format rather than a linked list.
struct tilewl_toplevel {
	struct wl_list link; /* server->toplevels, only while mapped */
	struct tilewl_server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;

	/*
	 * Our own container tree, owned by us and destroyed in the destroy
	 * handler. It holds the client's surface tree, so moving the container
	 * moves everything inside it.
	 */
	struct wlr_scene_tree *scene_tree;
	/*
	 * Created by wlr_scene_xdg_surface_create(), owned by wlroots: it
	 * destroys itself when the xdg_surface goes away.
	 */
	struct wlr_scene_tree *surface_tree;
	struct wlr_scene_tree *popup_tree;
	/* Top, bottom, left, right. Children of scene_tree, so ours to keep. */
	struct wlr_scene_rect *border[4];

	struct wlr_box geometry; /* last tile from the layout, incl. border */
	unsigned int workspace;
	bool mapped;
	/* TODO(floating): bool floating; skipped by the tiler. */

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
};

struct tilewl_popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct tilewl_keyboard {
	struct wl_list link;
	struct tilewl_server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

/* Enum class to hold direction of focus (in future can be extended to just direction)
 */
enum focus_direction {
	FOCUS_UP,
	FOCUS_DOWN,
	FOCUS_LEFT,
	FOCUS_RIGHT
};

/*
 * Keybindings. config.h holds a table of these; input.c looks a key press
 * up in it and calls the action with its argument.
 */
union tilewl_arg {
	enum focus_direction direction;
	int i;
	float f;
	unsigned int ui;
	const void *v;
};

struct tilewl_key {
	uint32_t modifiers; /* WLR_MODIFIER_* mask */
	xkb_keysym_t keysym;
	void (*action)(struct tilewl_server *server, const union tilewl_arg *arg);
	union tilewl_arg arg;
};

/*
 * Each module registers its server-wide listeners in *_init() and unlinks
 * them again in *_finish(). main() calls the finish functions before
 * destroying the wlroots objects those listeners are attached to.
 */

/* output.c */
void output_init(struct tilewl_server *server);
void output_finish(struct tilewl_server *server);

/* input.c */
void input_init(struct tilewl_server *server);
void input_finish(struct tilewl_server *server);
void cursor_rebase(struct tilewl_server *server);

/* xdg.c */
void xdg_init(struct tilewl_server *server);
void xdg_finish(struct tilewl_server *server);
void focus_toplevel(struct tilewl_server *server,
	struct tilewl_toplevel *toplevel);
void toplevel_set_geometry(struct tilewl_toplevel *toplevel,
	struct wlr_box box);
struct tilewl_toplevel *toplevel_at(struct tilewl_server *server,
	double lx, double ly, struct wlr_surface **surface,
	double *sx, double *sy);

/* layout.c */
void arrange(struct tilewl_server *server);
bool toplevel_is_visible(const struct tilewl_toplevel *toplevel);
struct tilewl_toplevel *first_visible(struct tilewl_server *server);

/* Keybinding actions, bound in config.h */
void spawn(struct tilewl_server *server, const union tilewl_arg *arg);
void close_focused(struct tilewl_server *server, const union tilewl_arg *arg);
void quit(struct tilewl_server *server, const union tilewl_arg *arg);
void focus_stack(struct tilewl_server *server, const union tilewl_arg *arg);
void zoom(struct tilewl_server *server, const union tilewl_arg *arg);
void set_master_ratio(struct tilewl_server *server,
	const union tilewl_arg *arg);
void view_workspace(struct tilewl_server *server, const union tilewl_arg *arg);
void move_to_workspace(struct tilewl_server *server,
	const union tilewl_arg *arg);

#endif
