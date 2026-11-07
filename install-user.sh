#!/usr/bin/env bash
# Build and install into the per-user OBS plugin directory (no root needed).
#   ./install-user.sh            build + install
#   ./install-user.sh uninstall  remove
set -euo pipefail

cd "$(dirname "$0")"
DEST="${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugins/obs-hyprland-capture"

if [[ ${1:-} == uninstall ]]; then
	rm -rf "$DEST"
	echo "Removed $DEST (restart OBS)"
	exit 0
fi

[[ -d build ]] || meson setup build
ninja -C build
install -Dm755 build/obs-hyprland-capture.so "$DEST/bin/64bit/obs-hyprland-capture.so"
echo "Installed to $DEST (restart OBS)"
