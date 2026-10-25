project(
	'tilewl',
	'c',
	version: '0.1.0',
	license: 'MIT',
	meson_version: '>=0.60.0',
	default_options: [
		'c_std=c11',
		'warning_level=2',
		'buildtype=debugoptimized',
	],
)

cc = meson.get_compiler('c')

# wlroots does not promise a stable API, so its headers refuse to build
# unless we explicitly opt in. _POSIX_C_SOURCE exposes setenv(), fork(), etc.
add_project_arguments(
	'-DWLR_USE_UNSTABLE',
	'-D_POSIX_C_SOURCE=200809L',
	# Every wl_listener callback takes a `void *data` argument that most
	# handlers ignore. Like wlroots itself, silence that one -Wextra warning
	# instead of littering every handler with (void)data.
	'-Wno-unused-parameter',
	language: 'c',
)

wlroots = dependency('wlroots-0.19')
wayland_server = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')

# wlroots' xdg-shell header includes "xdg-shell-protocol.h", which is not
# shipped by any package: it is generated from the protocol XML at build time.
wayland_protocols = dependency('wayland-protocols')
wayland_scanner_dep = dependency('wayland-scanner', native: true)
wayland_scanner = find_program(
	wayland_scanner_dep.get_variable('wayland_scanner'),
	native: true,
)

wl_protocol_dir = wayland_protocols.get_variable('pkgdatadir')
xdg_shell_header = custom_target(
	'xdg-shell-protocol.h',
	input: wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
	output: 'xdg-shell-protocol.h',
	command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

executable(
	'tilewl',
	['src/tinywl.c', xdg_shell_header],
	dependencies: [wlroots, wayland_server, xkbcommon],
	install: true,
)
