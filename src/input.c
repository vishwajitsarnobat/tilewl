/*
 * input.c - keyboards, pointers and the cursor.
 *
 * wlroots hands us raw input devices; we decide what each event means. Key
 * presses are either compositor bindings or forwarded to the focused client
 * through the wlr_seat, which tracks focus and speaks the wl_seat protocol.
 */
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "config.h"
#include "server.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/*
 * Lock modifiers are state, not chords: Mod+j must still work with Caps Lock
 * or Num Lock (MOD2) on, so strip them before comparing against keys[].
 */
#define IGNORED_MODIFIERS (WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2)

/* ---- keybinding actions ------------------------------------------------ */

void spawn(struct tilewl_server *server, const union tilewl_arg *arg) {
	const char *const *argv = arg->v;

	pid_t pid = fork();
	if (pid < 0) {
		wlr_log_errno(WLR_ERROR, "fork failed");
		return;
	}
	if (pid == 0) {
		/*
		 * In the child: start a new session so the program is not tied to
		 * our controlling terminal, and undo the SIGCHLD = SIG_IGN set in
		 * main(), since ignored dispositions survive exec and would break
		 * programs (shells) that wait for their own children.
		 */
		setsid();
		signal(SIGCHLD, SIG_DFL);
		execvp(argv[0], (char *const *)argv);
		wlr_log_errno(WLR_ERROR, "failed to exec %s", argv[0]);
		_exit(127);
	}
}

void close_focused(struct tilewl_server *server, const union tilewl_arg *arg) {
	struct wlr_surface *surface = server->seat->keyboard_state.focused_surface;
	if (surface == NULL) {
		return;
	}
	struct wlr_xdg_toplevel *xdg_toplevel =
		wlr_xdg_toplevel_try_from_wlr_surface(surface);
	if (xdg_toplevel != NULL) {
		/* Politely ask; the client decides when (and whether) to close. */
		wlr_xdg_toplevel_send_close(xdg_toplevel);
	}
}

void quit(struct tilewl_server *server, const union tilewl_arg *arg) {
	/* Makes wl_display_run() return; main() then tears down. */
	wl_display_terminate(server->wl_display);
}

/* ---- keyboard ---------------------------------------------------------- */

static bool handle_vt_switch(struct tilewl_server *server,
		const xkb_keysym_t *syms, int nsyms) {
	/*
	 * The default keymap turns Ctrl+Alt+F<n> into XF86Switch_VT_<n>. On a
	 * TTY we must implement switching ourselves: we hold DRM master, so the
	 * kernel's own VT shortcuts are not in effect. Nested, there is no
	 * session and the parent compositor owns the VTs.
	 */
	for (int i = 0; i < nsyms; i++) {
		if (syms[i] >= XKB_KEY_XF86Switch_VT_1 &&
				syms[i] <= XKB_KEY_XF86Switch_VT_12) {
			if (server->session != NULL) {
				unsigned int vt = syms[i] - XKB_KEY_XF86Switch_VT_1 + 1;
				wlr_session_change_vt(server->session, vt);
			}
			return true;
		}
	}
	return false;
}

static bool handle_keybinding(struct tilewl_server *server,
		uint32_t modifiers, xkb_keysym_t sym) {
	modifiers &= ~IGNORED_MODIFIERS;
	for (size_t i = 0; i < ARRAY_LEN(keys); i++) {
		if (keys[i].keysym == sym && keys[i].modifiers == modifiers) {
			keys[i].action(server, &keys[i].arg);
			return true;
		}
	}
	return false;
}

static void keyboard_handle_modifiers(struct wl_listener *listener,
		void *data) {
	struct tilewl_keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	/*
	 * The protocol allows one keyboard per seat, so every physical keyboard
	 * shares the seat. Make this one current before forwarding its state.
	 */
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
	struct tilewl_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct tilewl_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_keyboard *wlr_keyboard = keyboard->wlr_keyboard;

	/* libinput keycodes are evdev codes; xkb's are offset by 8. */
	uint32_t keycode = event->keycode + 8;

	bool handled = false;
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* Keysyms with modifiers applied: Ctrl+Alt+F1 -> XF86Switch_VT_1. */
		const xkb_keysym_t *syms;
		int nsyms = xkb_state_key_get_syms(wlr_keyboard->xkb_state,
			keycode, &syms);
		handled = handle_vt_switch(server, syms, nsyms);

		/*
		 * Bindings use the unshifted keysym, so Mod+Shift+1 is reported as
		 * "1" rather than "!" (which differs between layouts).
		 */
		const xkb_keysym_t *base_syms;
		xkb_layout_index_t layout =
			xkb_state_key_get_layout(wlr_keyboard->xkb_state, keycode);
		int nbase = xkb_keymap_key_get_syms_by_level(wlr_keyboard->keymap,
			keycode, layout, 0, &base_syms);
		uint32_t modifiers = wlr_keyboard_get_modifiers(wlr_keyboard);
		for (int i = 0; i < nbase && !handled; i++) {
			handled = handle_keybinding(server, modifiers, base_syms[i]);
		}
	}

	if (!handled) {
		wlr_seat_set_keyboard(server->seat, wlr_keyboard);
		wlr_seat_keyboard_notify_key(server->seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	struct tilewl_keyboard *keyboard =
		wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

static void new_keyboard(struct tilewl_server *server,
		struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	struct tilewl_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	if (keyboard == NULL) {
		wlr_log(WLR_ERROR, "out of memory");
		return;
	}
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	/*
	 * Compile a keymap from the XKB_DEFAULT_* environment (layout "us" if
	 * unset). The keyboard takes its own reference, so we drop ours.
	 */
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_handle_new_input(struct wl_listener *listener, void *data) {
	struct tilewl_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		/*
		 * All pointer handling goes through wlr_cursor, which merges every
		 * attached pointer into one stream of events. It also drops the
		 * device by itself when the device is destroyed.
		 */
		wlr_cursor_attach_input_device(server->cursor, device);
		break;
	default:
		break;
	}

	/* We always draw a cursor, so always advertise a pointer. */
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

/* ---- seat requests from clients ---------------------------------------- */

static void seat_handle_request_cursor(struct wl_listener *listener,
		void *data) {
	struct tilewl_server *server =
		wl_container_of(listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	/* Any client may ask; only the one under the pointer gets its way. */
	if (server->seat->pointer_state.focused_client == event->seat_client) {
		wlr_cursor_set_surface(server->cursor, event->surface,
			event->hotspot_x, event->hotspot_y);
	}
}

static void seat_handle_request_set_selection(struct wl_listener *listener,
		void *data) {
	/* A client copied something; we always allow it. */
	struct tilewl_server *server =
		wl_container_of(listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

/* ---- pointer ----------------------------------------------------------- */

void reset_cursor_mode(struct tilewl_server *server) {
	server->cursor_mode = TILEWL_CURSOR_PASSTHROUGH;
	server->grabbed_toplevel = NULL;
}

void begin_interactive(struct tilewl_toplevel *toplevel,
		enum tilewl_cursor_mode mode, uint32_t edges) {
	/*
	 * Start a compositor-side grab: until the button is released, pointer
	 * motion moves or resizes this window instead of going to clients.
	 */
	struct tilewl_server *server = toplevel->server;
	server->grabbed_toplevel = toplevel;
	server->cursor_mode = mode;

	if (mode == TILEWL_CURSOR_MOVE) {
		server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
		server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
	} else {
		struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;
		double border_x = (toplevel->scene_tree->node.x + geo_box->x) +
			((edges & WLR_EDGE_RIGHT) ? geo_box->width : 0);
		double border_y = (toplevel->scene_tree->node.y + geo_box->y) +
			((edges & WLR_EDGE_BOTTOM) ? geo_box->height : 0);
		server->grab_x = server->cursor->x - border_x;
		server->grab_y = server->cursor->y - border_y;

		server->grab_geobox = *geo_box;
		server->grab_geobox.x += toplevel->scene_tree->node.x;
		server->grab_geobox.y += toplevel->scene_tree->node.y;
		server->resize_edges = edges;
	}
}

static void process_cursor_move(struct tilewl_server *server) {
	struct tilewl_toplevel *toplevel = server->grabbed_toplevel;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		server->cursor->x - server->grab_x,
		server->cursor->y - server->grab_y);
}

static void process_cursor_resize(struct tilewl_server *server) {
	/*
	 * Resizing from the top or left edge also moves the window. For
	 * simplicity we move it immediately rather than waiting for the client
	 * to commit a buffer at the new size.
	 */
	struct tilewl_toplevel *toplevel = server->grabbed_toplevel;
	double border_x = server->cursor->x - server->grab_x;
	double border_y = server->cursor->y - server->grab_y;
	int new_left = server->grab_geobox.x;
	int new_right = server->grab_geobox.x + server->grab_geobox.width;
	int new_top = server->grab_geobox.y;
	int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

	if (server->resize_edges & WLR_EDGE_TOP) {
		new_top = border_y;
		if (new_top >= new_bottom) {
			new_top = new_bottom - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
		new_bottom = border_y;
		if (new_bottom <= new_top) {
			new_bottom = new_top + 1;
		}
	}
	if (server->resize_edges & WLR_EDGE_LEFT) {
		new_left = border_x;
		if (new_left >= new_right) {
			new_left = new_right - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_RIGHT) {
		new_right = border_x;
		if (new_right <= new_left) {
			new_right = new_left + 1;
		}
	}

	struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		new_left - geo_box->x, new_top - geo_box->y);
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
		new_right - new_left, new_bottom - new_top);
}

static void process_cursor_motion(struct tilewl_server *server, uint32_t time) {
	if (server->cursor_mode == TILEWL_CURSOR_MOVE) {
		process_cursor_move(server);
		return;
	} else if (server->cursor_mode == TILEWL_CURSOR_RESIZE) {
		process_cursor_resize(server);
		return;
	}

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct tilewl_toplevel *toplevel = toplevel_at(server,
		server->cursor->x, server->cursor->y, &surface, &sx, &sy);
	if (toplevel == NULL) {
		/* Over the background: show our own default cursor image. */
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
	if (surface != NULL) {
		/*
		 * Pointer focus is separate from keyboard focus. wlroots filters out
		 * duplicate enter/motion events for us.
		 */
		wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(server->seat, time, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(server->seat);
	}
}

static void cursor_handle_motion(struct wl_listener *listener, void *data) {
	/* Relative motion (a delta), e.g. a mouse under DRM/libinput. */
	struct tilewl_server *server =
		wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	/* wlr_cursor clamps the motion to the output layout for us. */
	wlr_cursor_move(server->cursor, &event->pointer->base,
		event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

static void cursor_handle_motion_absolute(struct wl_listener *listener,
		void *data) {
	/* Absolute motion in [0, 1], e.g. when running nested in a window. */
	struct tilewl_server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
		event->x, event->y);
	process_cursor_motion(server, event->time_msec);
}

static void cursor_handle_button(struct wl_listener *listener, void *data) {
	struct tilewl_server *server =
		wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;

	wlr_seat_pointer_notify_button(server->seat, event->time_msec,
		event->button, event->state);
	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		reset_cursor_mode(server);
	} else {
		double sx, sy;
		struct wlr_surface *surface = NULL;
		struct tilewl_toplevel *toplevel = toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);
		focus_toplevel(toplevel);
	}
}

static void cursor_handle_axis(struct wl_listener *listener, void *data) {
	/* Scroll wheel or touchpad scrolling. */
	struct tilewl_server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
		event->orientation, event->delta, event->delta_discrete,
		event->source, event->relative_direction);
}

static void cursor_handle_frame(struct wl_listener *listener, void *data) {
	/* Groups the preceding pointer events into one logical event. */
	struct tilewl_server *server =
		wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

/* ---- setup and teardown ------------------------------------------------ */

void input_init(struct tilewl_server *server) {
	/*
	 * wlr_cursor only draws an image and aggregates attached pointers; it
	 * never moves by itself. We move it in the motion handlers above.
	 */
	server->cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
	/* Loads xcursor themes and keeps images ready for each output scale. */
	server->cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

	server->cursor_mode = TILEWL_CURSOR_PASSTHROUGH;
	server->cursor_motion.notify = cursor_handle_motion;
	wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
	server->cursor_motion_absolute.notify = cursor_handle_motion_absolute;
	wl_signal_add(&server->cursor->events.motion_absolute,
		&server->cursor_motion_absolute);
	server->cursor_button.notify = cursor_handle_button;
	wl_signal_add(&server->cursor->events.button, &server->cursor_button);
	server->cursor_axis.notify = cursor_handle_axis;
	wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
	server->cursor_frame.notify = cursor_handle_frame;
	wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);

	/* A seat groups the input devices a single user operates. */
	wl_list_init(&server->keyboards);
	server->new_input.notify = server_handle_new_input;
	wl_signal_add(&server->backend->events.new_input, &server->new_input);
	server->seat = wlr_seat_create(server->wl_display, "seat0");
	server->request_cursor.notify = seat_handle_request_cursor;
	wl_signal_add(&server->seat->events.request_set_cursor,
		&server->request_cursor);
	server->request_set_selection.notify = seat_handle_request_set_selection;
	wl_signal_add(&server->seat->events.request_set_selection,
		&server->request_set_selection);
}

void input_finish(struct tilewl_server *server) {
	wl_list_remove(&server->cursor_motion.link);
	wl_list_remove(&server->cursor_motion_absolute.link);
	wl_list_remove(&server->cursor_button.link);
	wl_list_remove(&server->cursor_axis.link);
	wl_list_remove(&server->cursor_frame.link);

	wl_list_remove(&server->new_input.link);
	wl_list_remove(&server->request_cursor.link);
	wl_list_remove(&server->request_set_selection.link);

	/*
	 * The seat itself is a Wayland global and is destroyed together with
	 * the display. The cursor and xcursor manager are ours.
	 */
	wlr_xcursor_manager_destroy(server->cursor_mgr);
	wlr_cursor_destroy(server->cursor);
}
