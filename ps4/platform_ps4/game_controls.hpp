// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Per-game launch profiles and controls on PS4, through Playden's shared settings model
// (src/runtime/game_settings.h): a generated recipe, the player's saved choices
// (game-settings.json, as on PS5) and RenderProsperoProfile. Saved choices survive every
// later details visit and launch, which regenerate the profile from them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace playden::ps4
{

struct LaunchSpec
{
    std::string executable;         // relative to the install root, '\' or '/' separated
    std::string working_directory;  // relative to the install root; empty: the program's folder
    std::string arguments;
};

// Builds the game's recipe from Steam's launch option, saves it, and writes
// /data/PLDN10001/profiles/<app_id>.profile with the player's saved choices. prefix_name is the
// Playden Wine prefix the profile names. False, with a reason, when the shared recipe refuses the
// program or it is not installed yet; the caller then writes its plain profile.
bool WriteSharedLaunchProfile(std::uint32_t app_id, const std::string &title, const LaunchSpec &launch,
                              const std::string &prefix_name, std::string *reason);

// For the log when a program is reported missing: what opendir/readdir/lstat see along
// relative ('/' separated) under the game's install folder.
std::string DescribeGamePath(std::uint32_t app_id, const std::string &relative);

// Copies the packaged controller presets (/app0/runtime/input/*.input) to
// /data/PLDN10001/input, where the runtime reads [input] preset = <name>.
void InstallControllerPresets();

}  // namespace playden::ps4

extern "C"
{
// The Game Settings page for one game: its controls (when with_controls; a game with full
// controller support plays with the Xbox controller as it is) and its log detail. open returns 1
// when the game has a saved recipe; otherwise playden_ps4_controls_status() says why.
int playden_ps4_controls_open(std::uint32_t app_id, int with_controls);
std::size_t playden_ps4_controls_count(void);
const char *playden_ps4_controls_label(std::size_t row);
const char *playden_ps4_controls_value(std::size_t row);
// 1 when the player set this row (not the preset or Playden's default).
int playden_ps4_controls_custom(std::size_t row);
// Step a row's value by direction (-1, 1, or a larger jump); saves and rewrites the profile.
int playden_ps4_controls_step(std::size_t row, int direction);
// A row edited as text (step returned 2): opens the system keyboard for user_id and saves.
int playden_ps4_controls_edit(std::size_t row, int user_id);
// The library overrides picker: choices for the text row `row`, each "name=n,b" on or off; the row after
// the last choice is "Type your own" (playden_ps4_controls_edit).
std::size_t playden_ps4_overrides_open(std::size_t row);
const char *playden_ps4_overrides_label(std::size_t item);
int playden_ps4_overrides_checked(std::size_t item);
int playden_ps4_overrides_toggle(std::size_t item);
const char *playden_ps4_overrides_value(void);
// Removes the player's choice for a row.
int playden_ps4_controls_reset(std::size_t row);
const char *playden_ps4_controls_status(void);
}
