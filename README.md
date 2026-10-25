# tilewl

A small tiling Wayland compositor written in C on top of
[wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) 0.19. It started
from wlroots' `tinywl` example. That code is now split into modules, with
dwm-style master-stack tiling, workspaces, focus borders and a compile-time
`config.h` added on top.

> Screenshot: coming soon.

## Features

- Master-stack tiling. The master window sits on the left at a configurable
  ratio, and the other windows stack on the right. Windows are re-tiled on
  map, unmap and output resize.
- Workspaces 1-9 on a single output.
- Focus follows click. The focused window gets a coloured border.
- Keybindings and appearance live in `src/config.h`, as in dwm.
- VT switching (Ctrl+Alt+F1..F12) when running on a TTY.
- On exit, every listener is unlinked before the object that owns its
  signal is destroyed. Builds cleanly with `-Wall -Wextra`.

## Building

Fedora:

```sh
sudo dnf install wlroots0.19-devel wayland-devel wayland-protocols-devel \
    libxkbcommon-devel meson ninja-build gcc foot
meson setup build
ninja -C build
```

To build with AddressSanitizer:

```sh
meson setup build-asan -Db_sanitize=address
ninja -C build-asan
```

## Running

Nested inside an existing Wayland session (it opens as a window):

```sh
./build/tilewl -s foot
```

On a TTY, run the same command from a virtual console. tilewl takes over
the display through DRM/KMS and reads input through libinput, using logind
or seatd for device access. You can force a renderer with
`WLR_RENDERER=gles2|vulkan|pixman`.

## Keybindings

Mod is Alt by default, so the bindings still work when tilewl runs nested
under a compositor that grabs Super. Change `MODKEY` in `src/config.h` to
use Super instead.

| Keys                 | Action                                  |
|----------------------|-----------------------------------------|
| Mod+Shift+Return     | Spawn a terminal (`foot`)               |
| Mod+Shift+c          | Close the focused window                |
| Mod+j / Mod+k        | Focus the next / previous window        |
| Mod+h / Mod+l        | Shrink / grow the master area           |
| Mod+Return           | Promote the focused window to master    |
| Mod+1..9             | Switch to workspace N                   |
| Mod+Shift+1..9       | Move the focused window to workspace N  |
| Mod+Shift+q          | Quit                                    |
| Ctrl+Alt+F1..F12     | Switch VT (TTY only)                    |

## Architecture

```
src/main.c    setup, event loop, teardown order
src/server.h  core structs (server, output, toplevel, keyboard)
src/output.c  outputs: modesetting, frame events, layout changes
src/input.c   keyboards, pointer/cursor, seat, keybinding dispatch
src/xdg.c     xdg-shell toplevels and popups, focus, borders
src/layout.c  master-stack tiling and workspaces
src/config.h  keybindings and constants
```

Everything runs on one thread around `wl_display_run()`. The event loop
waits on file descriptors: client sockets, libinput, DRM and timers. When
one of them is ready, wlroots turns it into a `wl_signal`. tilewl subscribes
to these signals by embedding a `wl_listener` in its own structs, and each
callback uses `wl_container_of()` to get back to the struct that holds the
listener. Handlers never draw anything. They only update the `wlr_scene`
graph: they move a window's tree, resize the border rects, or enable and
disable nodes to switch workspaces.

A client draws into a buffer (shared memory or a GPU dmabuf), attaches it to
its `wl_surface` and commits. The scene graph holds a reference to that
buffer and records which region changed. When the output signals `frame`,
`wlr_scene_output_commit()` builds the frame from the damaged regions. If a
single opaque buffer covers the whole output, the scene can hand it straight
to the display (direct scan-out). Otherwise the renderer (GLES2, Vulkan or
Pixman) composites the visible buffers into a swapchain buffer from the
allocator. That buffer is committed atomically to the output. Under the DRM
backend, this commit becomes a KMS atomic commit that page-flips the CRTC to
the new framebuffer at the next vblank. After the commit, tilewl sends
`frame_done` so clients know to draw their next frame.

## Roadmap

- [x] Split tinywl into modules
- [x] Master-stack tiling, master ratio, promote to master
- [x] Workspaces 1-9, focus borders, `config.h` keybindings
- [x] Safe teardown; clean ASan run (the only reports are leaks inside GPU drivers)
- [ ] Multi-monitor: per-output workspaces and re-tiling on output hotplug
- [ ] Floating toggle (Mod+Space)
- [ ] Honour client fullscreen requests
- [ ] wlr-layer-shell support (bars such as waybar), with the exclusive zone
      subtracted from the tiling area
- [ ] Interactive move/resize of floating windows
- [ ] Config reload / runtime configuration
- [ ] CI build (GitHub Actions on Fedora), including an ASan build

`TODO(...)` comments in the source mark where each of these goes.

## License

MIT, see `LICENSE`. Based on tinywl from wlroots, which is released under
CC0 (`LICENSES/tinywl-CC0.txt`). wlroots itself is MIT licensed.

## Contributors

- Vishwajit Sarnobat (@vishwajitsarnobat)
- Paarth Mahadik (@paarth2023)
