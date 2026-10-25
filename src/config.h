/*
 * config.h - compile-time configuration, in the style of dwm.
 *
 * Edit this file and rebuild to change keybindings and appearance.
 */
#ifndef TILEWL_CONFIG_H
#define TILEWL_CONFIG_H

#include <xkbcommon/xkbcommon.h>

#include "server.h"

/*
 * Modifier for all bindings. Alt is the default so tilewl is usable nested
 * inside another compositor that already grabs the Super/Logo key. Use
 * WLR_MODIFIER_LOGO for Super.
 */
#define MODKEY WLR_MODIFIER_ALT
#define SHIFT WLR_MODIFIER_SHIFT

static const char *const termcmd[] = {"foot", NULL};

/*
 * Keys are matched on the unshifted keysym (level 1 of the keymap), so
 * write lowercase letters and plain digits even for Shift bindings:
 * {MODKEY | SHIFT, XKB_KEY_c, ...} is Mod+Shift+c.
 */
static const struct tilewl_key keys[] = {
	/* modifiers      keysym           action          argument */
	{ MODKEY | SHIFT, XKB_KEY_Return,  spawn,          { .v = termcmd } },
	{ MODKEY | SHIFT, XKB_KEY_c,       close_focused,  { 0 } },
	{ MODKEY | SHIFT, XKB_KEY_q,       quit,           { 0 } },
};

#endif
