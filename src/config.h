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

/* Initial fraction of the output width given to the master window. */
static const float master_ratio = 0.55f;
/* How much Mod+h / Mod+l change it per press. */
#define RATIO_STEP 0.05f

static const char *const termcmd[] = {"foot", NULL};

/*
 * Keys are matched on the unshifted keysym (level 1 of the keymap), so
 * write lowercase letters and plain digits even for Shift bindings:
 * {MODKEY | SHIFT, XKB_KEY_c, ...} is Mod+Shift+c.
 */
static const struct tilewl_key keys[] = {
	/* modifiers      keysym           action            argument */
	{ MODKEY | SHIFT, XKB_KEY_Return,  spawn,            { .v = termcmd } },
	{ MODKEY | SHIFT, XKB_KEY_c,       close_focused,    { 0 } },
	{ MODKEY,         XKB_KEY_j,       focus_stack,      { .i = +1 } },
	{ MODKEY,         XKB_KEY_k,       focus_stack,      { .i = -1 } },
	{ MODKEY,         XKB_KEY_h,       set_master_ratio, { .f = -RATIO_STEP } },
	{ MODKEY,         XKB_KEY_l,       set_master_ratio, { .f = +RATIO_STEP } },
	{ MODKEY,         XKB_KEY_Return,  zoom,             { 0 } },
	/* TODO(floating): { MODKEY, XKB_KEY_space, toggle_floating, { 0 } }, */
	{ MODKEY | SHIFT, XKB_KEY_q,       quit,             { 0 } },
};

#endif
