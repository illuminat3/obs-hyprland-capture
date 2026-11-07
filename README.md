# obs-hyprland-capture

Zero-copy screen and window capture for OBS Studio on Hyprland (and any Wayland
compositor that implements `ext-image-copy-capture-v1`).

It adds two sources:

- **Hyprland Screen Capture**: a whole monitor, picked by connector name (`DP-3`).
- **Hyprland Window Capture**: a single window, matched by app class and title.
  The window is captured even when it is covered or on another workspace.

No xdg-desktop-portal, no PipeWire and no picker dialog, so there's nothing to
"forget". The monitor or window is stored in the source settings and
reconnected automatically when it comes back (monitor hot-plug, app restart).

## Why

`wlrobs`' screencopy source copies every frame **GPU → system RAM → GPU**,
creates and destroys a shared-memory file and a texture per frame, and blocks
the OBS render thread until the compositor answers. The PipeWire portal path
avoids the copies, but it adds the portal's session/restore-token state, which
can lose the selected output or show stale frames.

This plugin:

| | |
|---|---|
| Frames stay on the GPU | The compositor blits into GBM-allocated dma-bufs in the GPU's native tiled layout; OBS imports them directly (`gs_texture_create_from_dmabuf`). |
| OBS never waits | A dedicated thread with its own Wayland connection drives capture; the render thread only samples the newest *finished* buffer. |
| No per-frame allocation | A ring of 3 buffers is allocated once per size/format; textures are imported once per buffer. |
| Paced to OBS | One capture is requested per OBS video tick, and only while the source is visible. Nothing is captured when it's hidden. |
| Correct sync | Waits on the buffer's implicit fence (`DMA_BUF_IOCTL_EXPORT_SYNC_FILE`) before publishing, so half-written frames are never shown. |
| No stale frames | When the captured monitor/window goes away the source goes blank instead of freezing on the last image. |

### Measured (RTX 5070, OBS at 1080p50, capturing a 1080p monitor)

| | wlrobs (screencopy) | obs-hyprland-capture |
|---|---|---|
| OBS GPU (SM) | 4–5 % | ~0 % |
| OBS render time | 0.18–0.20 ms | 0.15–0.19 ms |
| Hyprland GPU | ~18 % | ~18 % (same blit) |

The compositor still has to copy the frame once on the GPU; that cost is the
same for every capture method. For Vulkan/OpenGL games, `obs-vkcapture` is
still cheaper because it bypasses the compositor entirely.

## Requirements

- Hyprland (or another compositor) with `ext-image-copy-capture-v1`,
  `ext-output-image-capture-source-manager-v1` and `linux-dmabuf` v3+.
  Window capture also needs `ext-foreign-toplevel-list-v1` and
  `ext-foreign-toplevel-image-capture-source-manager-v1`.
- OBS Studio 30+ using the EGL renderer (the default on Linux).
- Build: `meson`, `ninja`, `wayland-protocols`, `wayland-scanner`, `libgbm`,
  `libdrm`, OBS headers (`obs-studio` on Arch).

## Install

Per-user (no root):

```sh
./install-user.sh            # builds and installs to ~/.config/obs-studio/plugins/
./install-user.sh uninstall
```

System-wide:

```sh
meson setup build --prefix=/usr
ninja -C build
sudo ninja -C build install   # -> /usr/lib/obs-plugins/
```

Restart OBS, then add **Hyprland Screen Capture** or **Hyprland Window
Capture** from the Sources "+" menu.

## Troubleshooting

OBS log lines are prefixed with `[hyprland-capture]`. On start a session logs
the device and buffer layout, e.g.

```
[hyprland-capture] capturing output DP-3
[hyprland-capture] allocating buffers on /dev/dri/renderD128
[hyprland-capture] allocated 3 x 1920x1080 XR24 dma-bufs (modifier 0x300000000e08014, 1 plane(s))
```

- *"compositor does not support ext-image-copy-capture-v1"*: your compositor
  is too old.
- *"no dma-buf format/modifier is shared by the compositor and OBS"* or
  *"OBS failed to import the captured dma-buf"*: OBS and the compositor are
  likely on different GPUs. Run OBS on the GPU the compositor renders with.

## License

GPL-2.0-or-later
