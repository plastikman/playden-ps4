// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PLAYDEN_PS4_WINE_RUNTIME_H
#define PLAYDEN_PS4_WINE_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dispatch the relaunched Playden title into its bundled Wine runtime.
 * Returns 0 when the normal Playden UI should start, 1 when a runtime launch
 * was requested but failed, and never returns after a successful handoff. */
int playden_ps4_wine_dispatch(int argc, char **argv);

/* Prepare Playden-owned Wine prefixes and launch Steam games in game mode. */
int playden_ps4_wine_prepare_hl2_install(void);
int playden_ps4_wine_prepare_game_prefix(uint32_t app_id);
/* The game's Wine prefix folder (from its launch profile) into out; 0, or -1 when it has none. */
int playden_ps4_wine_prefix_path(uint32_t app_id, char *out, size_t capacity);
int playden_ps4_wine_launch_hl2(void);
int playden_ps4_wine_launch_game(uint32_t app_id);
/* Relaunch this title into the bundled Wine module for a prepared Playden game.
 * A successful LoadExec does not return. */
int playden_ps4_wine_launch_descriptor(const char *title_id, const char *session_id);
const char *playden_ps4_wine_status(void);
/* 1 when this start is Wine handing back after a game (launcher=1). */
int playden_ps4_wine_returned(void);
/* The Steam app launched last, once (the record is removed); 0 when none. */
uint32_t playden_ps4_wine_take_last_played(void);

#ifdef __cplusplus
}
#endif

#endif
