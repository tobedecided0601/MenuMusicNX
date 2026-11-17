#pragma once

#include <switch.h>

namespace config {

// tune shuffle
auto get_shuffle() -> bool;
void set_shuffle(bool value);

// tune repeat
auto get_repeat() -> int;
void set_repeat(int value);

// tune volume
auto get_volume() -> float;
void set_volume(float value);

// per title tune enable
auto has_title_enabled(u64 tid) -> bool;
auto get_title_enabled(u64 tid) -> bool;
void set_title_enabled(u64 tid, bool value);

// default for tune for every title
auto get_title_enabled_default() -> bool;
void set_title_enabled_default(bool value);

// per title volume
auto has_title_volume(u64 tid) -> bool;
auto get_title_volume(u64 tid) -> float;
void set_title_volume(u64 tid, float value);

// default volume for every title
auto get_default_title_volume() -> float;
void set_default_title_volume(float value);

// keep playing in applets opened from the HOME Menu (keyboard, user page,
// controllers, ...). Mii Maker, Album/hbmenu, eShop and browsers stay silent.
auto get_home_applet_music() -> bool;
void set_home_applet_music(bool value);

// sound played when an application (game / homebrew) is launched; WAV file,
// empty path disables it. Not bundled: users provide their own file.
auto get_launch_sound(char* out, int max_len) -> int;
auto get_launch_sound_volume() -> float;

// fade out before sleep/shutdown via psc:m (default on)
auto get_fade_on_sleep() -> bool;

// append pdm focus events and playback decisions to /config/sys-tune/focus.log
auto get_focus_log() -> bool;
void set_focus_log(bool value);

// returns the length of the string
auto get_load_path(char* out, int max_len) -> int;
void set_load_path(const char* path);

}
