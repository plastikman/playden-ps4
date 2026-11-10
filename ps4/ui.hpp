// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

extern "C" void playden_ps4_draw_ui(std::uint32_t *frame, int tab, int selected, int page,
                                    int controller_ready, int auth_state, int game_focus,
                                    int game_selected, int delete_choice, const char *challenge_url,
                                    const char *auth_failure, const char *runtime_status);
