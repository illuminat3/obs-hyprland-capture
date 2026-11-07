// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdlib.h>

#include <obs-module.h>
#include <obs-nix-platform.h>

OBS_DECLARE_MODULE()

extern struct obs_source_info hc_screen_source;
extern struct obs_source_info hc_window_source;

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Zero-copy Hyprland/Wayland screen and window capture (ext-image-copy-capture-v1)";
}

bool obs_module_load(void)
{
	if (obs_get_nix_platform() != OBS_NIX_PLATFORM_WAYLAND && !getenv("WAYLAND_DISPLAY")) {
		blog(LOG_INFO, "[hyprland-capture] not running under Wayland, not loading");
		return false;
	}
	obs_register_source(&hc_screen_source);
	obs_register_source(&hc_window_source);
	blog(LOG_INFO, "[hyprland-capture] loaded (version %s)", PLUGIN_VERSION);
	return true;
}
