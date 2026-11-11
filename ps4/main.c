// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// First native PS4 shell. Wine and Steam are added after the display gate.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/Pad.h>

#include "wine_runtime.h"
#include "native/pw_update_ps4.h"

void playden_ps4_draw_ui(uint32_t *frame, int tab, int selected, int page,
                         int controller_ready, int auth_state, int game_focus, int game_selected,
                         int delete_choice, const char *challenge_url,
                         const char *auth_failure, const char *runtime_status);
int playden_ps4_ui_poll_artwork(void);
int playden_ps4_auth_start(void);
int playden_ps4_auth_restore_cached(void);
void playden_ps4_auth_cancel(void);
int playden_ps4_auth_poll(void);
int playden_ps4_auth_state(void);
size_t playden_ps4_library_count(void);
uint32_t playden_ps4_library_app_id(size_t index);
size_t playden_ps4_library_visible_count(int filter);
uint32_t playden_ps4_library_visible_app_id(int filter, size_t index);
int playden_ps4_library_toggle_favorite(uint32_t app_id);
int playden_ps4_library_toggle_hidden(uint32_t app_id);
void playden_ps4_game_details_start(uint32_t app_id);
void playden_ps4_game_install_start(uint32_t app_id);
int playden_ps4_game_prepare_steamworks(uint32_t app_id);
void playden_ps4_game_uninstall_start(uint32_t app_id);
int playden_ps4_game_uninstall_state(void);
uint32_t playden_ps4_game_uninstall_app_id(void);
int playden_ps4_game_details_installed(void);
int playden_ps4_game_details_update(void);
int playden_ps4_game_details_launch_supported(void);
int playden_ps4_game_details_state(void);
int playden_ps4_game_install_state(void);
uint32_t playden_ps4_queue_app_id(void);
int playden_ps4_queue_state(void);
void playden_ps4_queue_cancel(void);
int playden_ps4_saves_state(void);
int playden_ps4_saves_poll(void);
void playden_ps4_saves_start(uint32_t app_id, int resolution);
void playden_ps4_cycle_setting(int setting);
int playden_ps4_edit_log_receiver_ip(int user_id);
const char *playden_ps4_game_store_controller_support(void);
int playden_ps4_controls_open(uint32_t app_id, int with_controls);
size_t playden_ps4_controls_count(void);
int playden_ps4_controls_step(size_t row, int direction);
int playden_ps4_controls_reset(size_t row);
int playden_ps4_controls_edit(size_t row, int user_id);
void playden_ps4_log_text(const char *line);
int playden_ps4_saves_state(void);
void playden_ps4_saves_start(uint32_t app_id, int resolution);
size_t playden_ps4_overrides_open(size_t row);
int playden_ps4_overrides_toggle(size_t item);
int playden_ps4_install_sheet_open(uint32_t app_id);
void playden_ps4_install_sheet_step(int row, int direction);
int playden_ps4_game_details_incomplete(void);
void playden_ps4_game_verify_start(uint32_t app_id);
int playden_ps4_maintenance_start(uint32_t app_id, int action);
int playden_ps4_maintenance_state(void);
const char *playden_ps4_maintenance_message(void);
int playden_ps4_auth_password_start(int user_id);
int playden_ps4_auth_guard_code(int user_id);
int playden_ps4_auth_guard_code_wanted(void);
void playden_ps4_auth_sign_out(void);
int playden_ps4_library_search_edit(int user_id);
int playden_ps4_library_sort_cycle(void);
size_t playden_ps4_library_collection_count(void);
int playden_ps4_library_collection_create(int user_id);
int playden_ps4_library_collection_delete(size_t index);
int playden_ps4_library_collection_toggle(uint32_t app_id, size_t index);
const char *playden_ps4_auth_challenge(void);

/* Delete (page 3): 0 choosing, 1 Steam Cloud sync first, 2 backing up and removing the prefix. ui.cpp shows it,
 * with the reason a delete stopped. */
int playden_ps4_delete_stage = 0;
char playden_ps4_delete_note[160] = "";
/* Settings' Steam account row: signing out takes a second Cross; ui.cpp shows it. */
int playden_ps4_signout_armed = 0;
int playden_ps4_launching = 0;   /* page 7: the game is starting (the launch call blocks) */
/* Maintenance (page 10): the prefix reset takes a second Cross; ui.cpp shows it. */
int playden_ps4_maintenance_armed = 0;
/* The game the collection picker (page 9) is for; ui.cpp draws from it. */
uint32_t playden_ps4_collection_app_id = 0;
/* The Library tab's views (Home uses 0: the installed list, unsearched); ui.cpp has the same rule. */
/* Home's focused shelf; Home's views are filters 3000 + shelf (steam_auth.cpp kHomeShelf). */
int playden_ps4_home_shelf = 0;
size_t playden_ps4_home_shelf_count(void);
int playden_ps4_library_collection_pin_toggle(size_t index);
size_t playden_ps4_queue_waiting_count(void);
int playden_ps4_queue_remove(size_t index);
size_t playden_ps4_update_count(void);
int playden_ps4_queue_all_updates(void);
static int library_filter(int tab, int row) { return tab == 0 ? 3000 + playden_ps4_home_shelf : 1000 + row; }
const char *playden_ps4_auth_failure(void);

enum { WIDTH = 1920, HEIGHT = 1080, BUFFERS = 3 };
enum { FRAME_BYTES = 0x800000, DIRECT_ALIGN = 0x200000 };

static void log_line(const char *text) {
    sceKernelDebugOutText(0, text);
    sceKernelDebugOutText(0, "\n");
}

static void draw_screen(uint32_t *frame, int tab, int selected, int page, int controller_ready,
                        int auth_state, int game_focus, int game_selected,
                        int delete_choice, const char *challenge_url, const char *auth_failure,
                        const char *runtime_status) {
    playden_ps4_draw_ui(frame, tab, selected, page, controller_ready,
                        auth_state, game_focus, game_selected, delete_choice, challenge_url, auth_failure,
                        runtime_status);
}

int playden_ps4_main(int argc, char **argv) {
    if (playden_ps4_wine_dispatch(argc, argv) != 0)
        log_line("PLDN10001 runtime: launch failed; returning to library");
    log_line("PLDN10001: native shell starting");
    int handle = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN,
                                 0, NULL);
    if (handle < 0) {
        OrbisUserServiceUserId user = -1;
        if (sceUserServiceInitialize(NULL) >= 0 &&
            sceUserServiceGetInitialUser(&user) >= 0 && user >= 0)
            handle = sceVideoOutOpen(user, ORBIS_VIDEO_OUT_BUS_MAIN, 0, NULL);
    }
    if (handle < 0) { log_line("PLDN10001: video open failed"); return 1; }

    int64_t physical = -1;
    size_t bytes = (size_t)FRAME_BYTES * BUFFERS;
    if (sceKernelAllocateDirectMemory(0, (int64_t)sceKernelGetDirectMemorySize(),
                                      bytes, DIRECT_ALIGN, 3, &physical) < 0) {
        log_line("PLDN10001: video allocation failed"); return 1;
    }
    void *memory = NULL;
    if (sceKernelMapDirectMemory(&memory, bytes, 0x33, 0, physical,
                                 DIRECT_ALIGN) < 0) {
        log_line("PLDN10001: video map failed"); return 1;
    }
    void *buffers[BUFFERS];
    // Start on Connect Steam in the empty, signed-out library. Successful sign-in
    // moves focus to All games below when the library finishes loading.
    int tab = 1, selected = 4, page = 0, current = 0;
    int game_focus = 0, game_selected = 0;
    uint32_t detail_app_id = 0;
    uint32_t saves_app_id = 220;
    int previous_saves_state = 0;
    int previous_uninstall_state = playden_ps4_game_uninstall_state();
    int delete_choice = 0;
    int settings_refresh = 0;  /* Game Settings waits for a details refresh, then reloads */
    int sync_waiting = 0;      /* page 7: a Cloud sync before play is running */
    /* Back from a game: its saves go to Steam Cloud once the session is restored. */
    uint32_t post_play_sync = playden_ps4_wine_returned() ? playden_ps4_wine_take_last_played() : 0;
    int settings_row = 0;      /* the Game Settings row the overrides picker returns to */
    int override_count = 0;    /* the picker's choices; "Type your own" follows them */
    // Restore a saved Steam session silently. With no cached credentials the
    // launcher remains on Connect Steam and does not start a QR flow.
    if (playden_ps4_auth_restore_cached()) page = 1;
    selected = 4;
    for (int i = 0; i < BUFFERS; ++i) {
        buffers[i] = (uint8_t *)memory + (size_t)i * FRAME_BYTES;
        draw_screen(buffers[i], tab, selected, page, 0, 0, game_focus, game_selected, delete_choice, "", "",
                    playden_ps4_wine_status());
    }
    OrbisVideoOutBufferAttribute attribute;
    memset(&attribute, 0, sizeof(attribute));
    // Matches the BGRX bytes used by the proven orbis-win PS4 presenter.
    sceVideoOutSetBufferAttribute(&attribute, 0x80000000u,
                                  ORBIS_VIDEO_OUT_TILING_MODE_LINEAR,
                                  ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9, WIDTH, HEIGHT, WIDTH);
    if (sceVideoOutRegisterBuffers(handle, 0, buffers, BUFFERS, &attribute) < 0) {
        log_line("PLDN10001: video registration failed"); return 1;
    }
    sceSystemServiceHideSplashScreen();
    sceVideoOutSubmitFlip(handle, 0, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0);
    log_line("PLDN10001: interactive launcher shell displayed");

    int user = -1, pad = -1, pad_ready = 0;
    if (sceUserServiceInitialize(NULL) >= 0 &&
        sceUserServiceGetInitialUser(&user) >= 0 && user >= 0 &&
        scePadInit() >= 0) {
        for (int attempt = 0; attempt < 20 && pad < 0; ++attempt) {
            pad = scePadOpen(user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);
            if (pad < 0) usleep(50000);
        }
        pad_ready = pad >= 0;
    }
    if (!pad_ready) log_line("PLDN10001: controller unavailable; retrying");
    /* A newer Playden release on GitHub, checked once on its own thread
     * (/data/PLDN10001/no-update-check turns it off). */
    pw_update_ps4_start("/data/PLDN10001", playden_ps4_log_text);
    PwUpdateDownloadState update_shown = PW_UPDATE_DOWNLOAD_NONE;
    uint64_t update_tick = 0;

    uint32_t previous = 0;
    int rendered_pad_ready = 0;
    for (;;) {
        if (!pad_ready) {
            if (sceUserServiceGetInitialUser(&user) >= 0 && user >= 0 && scePadInit() >= 0) {
                pad = scePadOpen(user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);
                pad_ready = pad >= 0;
            }
        }
        OrbisPadData sample;
        memset(&sample, 0, sizeof(sample));
        uint32_t buttons = pad_ready && scePadReadState(pad, &sample) >= 0 ? sample.buttons : 0;
        uint32_t pressed = buttons & ~previous;
        previous = buttons;
        int changed = pad_ready != rendered_pad_ready;
        rendered_pad_ready = pad_ready;
        if (page == 1) {
            if (pressed & ORBIS_PAD_BUTTON_CIRCLE) {
                playden_ps4_auth_cancel();
                page = 0;
                changed = 1;
            }
            /* Account name and password instead of the QR code; Steam Guard's code when it asks for one. */
            const int auth_now = playden_ps4_auth_state();
            if ((pressed & ORBIS_PAD_BUTTON_SQUARE) && auth_now != 1 && auth_now != 8) {
                playden_ps4_auth_password_start(user);
                changed = 1;
            }
            if ((pressed & ORBIS_PAD_BUTTON_TRIANGLE) && playden_ps4_auth_guard_code_wanted()) {
                playden_ps4_auth_guard_code(user);
                changed = 1;
            }
            /* Steam Guard prompts and results arrive on their own. */
            static int shown_auth = -1;
            if (auth_now != shown_auth) { shown_auth = auth_now; changed = 1; }
        } else if (page == 2) {
            if (pressed & ORBIS_PAD_BUTTON_CIRCLE) {
                page = 0;
                game_focus = 1;
                changed = 1;
            }
            if ((pressed & ORBIS_PAD_BUTTON_SQUARE) &&
                playden_ps4_game_details_installed() && playden_ps4_game_uninstall_state() != 1) {
                page = 3;
                delete_choice = 0;
                playden_ps4_delete_stage = 0;
                playden_ps4_delete_note[0] = '\0';
                changed = 1;
            }
            // Game settings, saved per game: Wine log detail, and controller mode and button mapping
            // unless Steam lists full controller support (the Xbox controller works as it is).
            if ((pressed & ORBIS_PAD_BUTTON_TRIANGLE) && playden_ps4_game_details_installed() &&
                playden_ps4_game_details_state() == 2) {
                playden_ps4_controls_open(detail_app_id,
                                          strcmp(playden_ps4_game_store_controller_support(), "full") != 0);
                page = 5;
                delete_choice = 0;
                changed = 1;
            }
            /* Maintenance: check and repair, save backups, prefix reset. */
            if ((pressed & ORBIS_PAD_BUTTON_OPTIONS) && playden_ps4_game_details_installed() &&
                playden_ps4_game_details_state() == 2 && playden_ps4_game_install_state() != 1) {
                page = 10;
                delete_choice = 0;
                playden_ps4_maintenance_armed = 0;
                changed = 1;
            }
            if ((pressed & ORBIS_PAD_BUTTON_CROSS) &&
                playden_ps4_game_uninstall_state() != 1 &&
                playden_ps4_game_details_state() == 2 &&
                playden_ps4_game_install_state() != 1) {
                if (!playden_ps4_game_details_installed() && !playden_ps4_game_details_incomplete() &&
                    playden_ps4_install_sheet_open(detail_app_id)) {
                    /* Not installed: the install sheet (version, language, sizes) first. */
                    delete_choice = 2;
                    page = 8;
                    changed = 1;
                } else if (!playden_ps4_game_details_installed() || playden_ps4_game_details_update()) {
                    /* A download to resume, or an update on Steam: straight on, as the install recorded it. */
                    playden_ps4_game_install_start(detail_app_id);
                    changed = 1;
                } else if (playden_ps4_game_details_launch_supported()) {
                    /* Steam Cloud first: the saves on Steam and in the game's prefix agree before it starts. */
                    playden_ps4_saves_start(detail_app_id, 0);
                    sync_waiting = 1;
                    delete_choice = 0;
                    page = 7;
                    changed = 1;
                }
            }
        } else if (page == 3) {
            /* 0 delete the game and keep its prefix and saves, 1 delete the prefix too (saves backed up first),
             * 2 keep the game. Signed in, Steam Cloud syncs first; a conflict stops the delete. */
            static int remove_prefix = 0;
            /* Not while the prefix is being removed: the delete finishes first. */
            if ((pressed & ORBIS_PAD_BUTTON_CIRCLE) && playden_ps4_delete_stage != 3) {
                page = 2;
                playden_ps4_delete_stage = 0;
                changed = 1;
            }
            if (playden_ps4_delete_stage == 0) {
                if ((pressed & ORBIS_PAD_BUTTON_UP) && delete_choice > 0) { --delete_choice; changed = 1; }
                if ((pressed & ORBIS_PAD_BUTTON_DOWN) && delete_choice < 2) { ++delete_choice; changed = 1; }
                if ((pressed & ORBIS_PAD_BUTTON_CROSS) && delete_choice == 2) {
                    page = 2;
                    changed = 1;
                } else if (pressed & ORBIS_PAD_BUTTON_CROSS) {
                    remove_prefix = delete_choice == 1;
                    playden_ps4_delete_note[0] = '\0';
                    const int auth_now = playden_ps4_auth_state();
                    if ((auth_now == 6 || auth_now == 7) && playden_ps4_saves_state() != 1) {
                        playden_ps4_saves_start(detail_app_id, 0);
                        playden_ps4_delete_stage = 1;
                    } else {
                        playden_ps4_delete_stage = 2;
                    }
                    changed = 1;
                }
            }
            if (playden_ps4_delete_stage == 1) {
                const int saves = playden_ps4_saves_state();
                if (saves == 3) {
                    snprintf(playden_ps4_delete_note, sizeof(playden_ps4_delete_note),
                             "Steam Cloud and the PS4 have different saves. Choose which to keep in Saves, then delete.");
                    playden_ps4_delete_stage = 0;
                    changed = 1;
                } else if (saves != 1) {
                    playden_ps4_delete_stage = 2;   /* synced, nothing to sync, or offline (logged) */
                    changed = 1;
                }
            }
            if (playden_ps4_delete_stage == 2) {
                if (!remove_prefix) {
                    playden_ps4_game_uninstall_start(detail_app_id);
                    playden_ps4_delete_stage = 0;
                    page = 2;
                } else if (playden_ps4_maintenance_start(detail_app_id, 4) == 0) {
                    playden_ps4_delete_stage = 3;
                } else {
                    snprintf(playden_ps4_delete_note, sizeof(playden_ps4_delete_note),
                             "Another maintenance job is running. Try again when it finishes.");
                    playden_ps4_delete_stage = 0;
                }
                changed = 1;
            }
            if (playden_ps4_delete_stage == 3) {
                const int job = playden_ps4_maintenance_state();
                if (job == 2) {
                    playden_ps4_game_uninstall_start(detail_app_id);
                    playden_ps4_delete_stage = 0;
                    page = 2;
                    changed = 1;
                } else if (job == 3) {
                    snprintf(playden_ps4_delete_note, sizeof(playden_ps4_delete_note), "%s",
                             playden_ps4_maintenance_message());
                    playden_ps4_delete_stage = 0;
                    changed = 1;
                }
            }
            if (playden_ps4_saves_poll()) changed = 1;
        } else if (page == 5) {
            const int rows = (int)playden_ps4_controls_count();
            if (pressed & ORBIS_PAD_BUTTON_CIRCLE) {
                page = 2;
                settings_refresh = 0;
                changed = 1;
            }
            /* Refresh: check the game again with Steam, which rebuilds its recipe and profile. */
            if ((pressed & ORBIS_PAD_BUTTON_CROSS) && !settings_refresh) {
                playden_ps4_game_details_start(detail_app_id);
                settings_refresh = 1;
                changed = 1;
            }
            if (settings_refresh && playden_ps4_game_details_state() >= 2) {
                settings_refresh = 0;
                playden_ps4_controls_open(detail_app_id,
                                          strcmp(playden_ps4_game_store_controller_support(), "full") != 0);
                if (delete_choice >= (int)playden_ps4_controls_count()) delete_choice = 0;
                changed = 1;
            }
            if ((pressed & ORBIS_PAD_BUTTON_UP) && delete_choice > 0) { --delete_choice; changed = 1; }
            if ((pressed & ORBIS_PAD_BUTTON_DOWN) && delete_choice + 1 < rows) { ++delete_choice; changed = 1; }
            if (rows > 0) {
                int step = 0;
                if (pressed & ORBIS_PAD_BUTTON_LEFT) step = -1;
                if (pressed & ORBIS_PAD_BUTTON_RIGHT) step = 1;
                if (pressed & ORBIS_PAD_BUTTON_L1) step = -10;
                if (pressed & ORBIS_PAD_BUTTON_R1) step = 10;
                if (step) {
                    if (playden_ps4_controls_step((size_t)delete_choice, step) == 2) {
                        /* A text row: library overrides open the picker; anything else the keyboard. */
                        settings_row = delete_choice;
                        override_count = (int)playden_ps4_overrides_open((size_t)settings_row);
                        if (override_count > 0) {
                            page = 6;
                            delete_choice = 0;
                        } else {
                            playden_ps4_controls_edit((size_t)settings_row, user);
                        }
                    }
                    changed = 1;
                }
                if (pressed & ORBIS_PAD_BUTTON_SQUARE) {
                    playden_ps4_controls_reset((size_t)delete_choice);
                    changed = 1;
                }
            }
        } else if (page == 6) {
            /* Library overrides: the game's DLLs and common ones, then "Type your own". */
            if (pressed & ORBIS_PAD_BUTTON_CIRCLE) {
                page = 5;
                delete_choice = settings_row;
                changed = 1;
            }
            if ((pressed & ORBIS_PAD_BUTTON_UP) && delete_choice > 0) { --delete_choice; changed = 1; }
            if ((pressed & ORBIS_PAD_BUTTON_DOWN) && delete_choice < override_count) { ++delete_choice; changed = 1; }
            if (pressed & ORBIS_PAD_BUTTON_CROSS) {
                if (delete_choice < override_count) playden_ps4_overrides_toggle((size_t)delete_choice);
                else playden_ps4_controls_edit((size_t)settings_row, user);
                changed = 1;
            }
        } else if (page == 8) {
            /* The install sheet: version and language rows, then Install. */
            if (pressed & ORBIS_PAD_BUTTON_CIRCLE) { page = 2; changed = 1; }
            if ((pressed & ORBIS_PAD_BUTTON_UP) && delete_choice > 0) { --delete_choice; changed = 1; }
            if ((pressed & ORBIS_PAD_BUTTON_DOWN) && delete_choice < 2) { ++delete_choice; changed = 1; }
            if (delete_choice < 2 && (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT))) {
                playden_ps4_install_sheet_step(delete_choice, (pressed & ORBIS_PAD_BUTTON_LEFT) ? -1 : 1);
                changed = 1;
            }
            if (pressed & ORBIS_PAD_BUTTON_CROSS) {
                if (delete_choice < 2) {
                    playden_ps4_install_sheet_step(delete_choice, 1);
                } else {
                    playden_ps4_game_install_start(detail_app_id);
                    page = 2;
                }
                changed = 1;
            }
        } else if (page == 7) {
            /* Cloud sync before play: launch when done; a conflict asks which saves to keep. */
            const int state = playden_ps4_saves_state();
            int launch = 0;
            if (sync_waiting && (state == 2 || state == 4 || state == 5 || state == 0)) {
                launch = 1;   /* synced, nothing to sync, or no network: play (the reason is logged) */
            } else if (state == 3) {
                sync_waiting = 0;
                if ((pressed & ORBIS_PAD_BUTTON_UP) && delete_choice > 0) { --delete_choice; changed = 1; }
                if ((pressed & ORBIS_PAD_BUTTON_DOWN) && delete_choice < 3) { ++delete_choice; changed = 1; }
                if (pressed & ORBIS_PAD_BUTTON_CROSS) {
                    if (delete_choice == 0 || delete_choice == 1) {
                        /* 2: the Cloud's saves; 1: the PS4's. Then play. */
                        playden_ps4_saves_start(detail_app_id, delete_choice == 0 ? 2 : 1);
                        sync_waiting = 1;
                    } else if (delete_choice == 2) {
                        launch = 1;
                    } else {
                        page = 2;
                    }
                    changed = 1;
                }
            }
            if (pressed & ORBIS_PAD_BUTTON_CIRCLE) {
                sync_waiting = 0;
                page = 2;
                changed = 1;
            }
            if (launch) {
                sync_waiting = 0;
                /* The launch blocks (Steam setup, a first-launch prefix copy of about a minute): show why first. */
                playden_ps4_launching = 1;
                current = (current + 1) % BUFFERS;
                draw_screen(buffers[current], tab, selected, page, pad_ready,
                            playden_ps4_auth_state(), game_focus, game_selected, delete_choice,
                            playden_ps4_auth_challenge(),
                            playden_ps4_auth_failure(), playden_ps4_wine_status());
                sceVideoOutSubmitFlip(handle, current, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0);
                if (!playden_ps4_game_prepare_steamworks(detail_app_id) ||
                    playden_ps4_wine_launch_game(detail_app_id) != 0) {
                    log_line("PLDN10001 runtime: could not launch the selected Steam game");
                    page = 2;
                }
                playden_ps4_launching = 0;
                changed = 1;
            }
            if (playden_ps4_saves_poll()) changed = 1;
        } else if (page == 4) {
            if (pressed & ORBIS_PAD_BUTTON_CIRCLE) {
                page = 0;
                game_focus = 1;
                changed = 1;
            }
            if (pressed & ORBIS_PAD_BUTTON_UP) {
                if (delete_choice > 0) --delete_choice;
                changed = 1;
            }
            if (pressed & ORBIS_PAD_BUTTON_DOWN) {
                if (delete_choice < 3) ++delete_choice;
                changed = 1;
            }
            if ((pressed & ORBIS_PAD_BUTTON_CROSS) && delete_choice == 2) {
                /* Collections: the picker keeps this game even when the view below changes. */
                playden_ps4_collection_app_id = playden_ps4_library_visible_app_id(library_filter(tab, selected),
                                                                                   (size_t)game_selected);
                page = 9;
                delete_choice = 0;
                changed = 1;
            } else if (pressed & ORBIS_PAD_BUTTON_CROSS) {
                const int filter = library_filter(tab, selected);
                const uint32_t app_id = playden_ps4_library_visible_app_id(filter, (size_t)game_selected);
                int saved = 1;
                if (delete_choice == 0)
                    saved = playden_ps4_library_toggle_favorite(app_id);
                else if (delete_choice == 1)
                    saved = playden_ps4_library_toggle_hidden(app_id);
                if (!saved) log_line("PLDN10001: could not save library organization change");
                page = 0;
                game_focus = delete_choice != 3;
                const int count = (int)playden_ps4_library_visible_count(filter);
                if (count <= 0) { game_selected = 0; game_focus = 0; }
                else if (game_selected >= count) game_selected = count - 1;
                changed = 1;
            }
        } else if (page == 10) {
            const int working = playden_ps4_maintenance_state() == 1;
            if (pressed & ORBIS_PAD_BUTTON_CIRCLE) { page = 2; playden_ps4_maintenance_armed = 0; changed = 1; }
            if ((pressed & ORBIS_PAD_BUTTON_UP) && delete_choice > 0) {
                --delete_choice; playden_ps4_maintenance_armed = 0; changed = 1;
            }
            if ((pressed & ORBIS_PAD_BUTTON_DOWN) && delete_choice < 5) {
                ++delete_choice; playden_ps4_maintenance_armed = 0; changed = 1;
            }
            if ((pressed & ORBIS_PAD_BUTTON_CROSS) && !working) {
                if (delete_choice == 0) {
                    /* The check runs as an install job: its progress shows on the details screen. */
                    playden_ps4_game_verify_start(detail_app_id);
                    page = 2;
                } else if (delete_choice == 1 || delete_choice == 2) {
                    playden_ps4_maintenance_start(detail_app_id, delete_choice);
                } else if (delete_choice == 3) {
                    if (playden_ps4_maintenance_armed) {
                        playden_ps4_maintenance_start(detail_app_id, 3);
                        playden_ps4_maintenance_armed = 0;
                    } else {
                        playden_ps4_maintenance_armed = 1;
                    }
                } else if (delete_choice == 4) {
                    playden_ps4_maintenance_start(detail_app_id, 5);
                } else {
                    page = 2;
                }
                changed = 1;
            }
            /* A running job's message and the backup line change on their own. */
            static int shown_state = -1;
            if (playden_ps4_maintenance_state() != shown_state) {
                shown_state = playden_ps4_maintenance_state();
                changed = 1;
            }
        } else if (page == 9) {
            /* Collections for one game: Cross ticks a collection or makes a new one; Square deletes one. */
            const int count = (int)playden_ps4_library_collection_count();
            if (pressed & ORBIS_PAD_BUTTON_CIRCLE) {
                page = 0;
                const int visible = (int)playden_ps4_library_visible_count(library_filter(tab, selected));
                game_focus = visible > 0;
                if (game_selected >= visible) game_selected = visible > 0 ? visible - 1 : 0;
                changed = 1;
            }
            if ((pressed & ORBIS_PAD_BUTTON_UP) && delete_choice > 0) { --delete_choice; changed = 1; }
            if ((pressed & ORBIS_PAD_BUTTON_DOWN) && delete_choice < count) { ++delete_choice; changed = 1; }
            if (pressed & ORBIS_PAD_BUTTON_CROSS) {
                if (delete_choice < count) {
                    if (!playden_ps4_library_collection_toggle(playden_ps4_collection_app_id, (size_t)delete_choice))
                        log_line("PLDN10001: could not save collection change");
                } else {
                    const int made = playden_ps4_library_collection_create(user);
                    if (made >= 0) {
                        playden_ps4_library_collection_toggle(playden_ps4_collection_app_id, (size_t)made);
                        delete_choice = made;
                    }
                }
                changed = 1;
            }
            if ((pressed & ORBIS_PAD_BUTTON_TRIANGLE) && delete_choice < count) {
                /* Pin to Home: the collection becomes a Home shelf. */
                if (!playden_ps4_library_collection_pin_toggle((size_t)delete_choice))
                    log_line("PLDN10001: could not save collection pin");
                changed = 1;
            }
            if ((pressed & ORBIS_PAD_BUTTON_SQUARE) && delete_choice < count) {
                if (playden_ps4_library_collection_delete((size_t)delete_choice)) {
                    /* The Library tab's row for it, or one after it, moves up. */
                    if (tab == 1 && selected >= 4 + delete_choice) selected = selected > 4 + delete_choice ? selected - 1 : 1;
                    if (delete_choice > 0 && delete_choice >= count - 1) --delete_choice;
                }
                changed = 1;
            }
        } else {
            if ((tab == 1 || tab == 0) && game_focus) {
                if (pressed & ORBIS_PAD_BUTTON_CIRCLE) {
                    game_focus = 0;
                    changed = 1;
                }
                const int game_count = (int)playden_ps4_library_visible_count(library_filter(tab, selected));
                /* Library search and sort, from the grid. */
                if (tab == 1 && (pressed & (ORBIS_PAD_BUTTON_TRIANGLE | ORBIS_PAD_BUTTON_R1))) {
                    if (pressed & ORBIS_PAD_BUTTON_TRIANGLE) playden_ps4_library_search_edit(user);
                    else playden_ps4_library_sort_cycle();
                    game_selected = 0;
                    if (playden_ps4_library_visible_count(library_filter(tab, selected)) == 0) game_focus = 0;
                    changed = 1;
                }
                if (tab == 0) {
                    /* Up / Down: the shelf above or below, keeping the column where it can. */
                    const int shelves = (int)playden_ps4_home_shelf_count();
                    const int before = playden_ps4_home_shelf;
                    if ((pressed & ORBIS_PAD_BUTTON_UP) && playden_ps4_home_shelf > 0) --playden_ps4_home_shelf;
                    if ((pressed & ORBIS_PAD_BUTTON_DOWN) && playden_ps4_home_shelf + 1 < shelves) ++playden_ps4_home_shelf;
                    if (playden_ps4_home_shelf >= shelves) playden_ps4_home_shelf = shelves > 0 ? shelves - 1 : 0;
                    if (playden_ps4_home_shelf != before) {
                        const int size = (int)playden_ps4_library_visible_count(library_filter(tab, selected));
                        if (game_selected >= size) game_selected = size > 0 ? size - 1 : 0;
                        changed = 1;
                    }
                    if ((pressed & ORBIS_PAD_BUTTON_LEFT) && game_selected > 0) {
                        --game_selected; changed = 1;
                    } else if ((pressed & ORBIS_PAD_BUTTON_LEFT) && game_selected == 0) {
                        game_focus = 0; changed = 1;
                    }
                    if ((pressed & ORBIS_PAD_BUTTON_RIGHT) && game_selected + 1 < game_count) {
                        ++game_selected; changed = 1;
                    }
                } else {
                    if ((pressed & ORBIS_PAD_BUTTON_UP) && game_selected >= 6) {
                        game_selected -= 6; changed = 1;
                    }
                    if ((pressed & ORBIS_PAD_BUTTON_DOWN) && game_selected + 6 < game_count) {
                        game_selected += 6; changed = 1;
                    }
                    if ((pressed & ORBIS_PAD_BUTTON_LEFT) && game_selected % 6 != 0) {
                        --game_selected; changed = 1;
                    } else if ((pressed & ORBIS_PAD_BUTTON_LEFT) && game_selected % 6 == 0) {
                        game_focus = 0; changed = 1;
                    }
                    if ((pressed & ORBIS_PAD_BUTTON_RIGHT) && game_selected + 1 < game_count &&
                        game_selected % 6 != 5) {
                        ++game_selected; changed = 1;
                    }
                }
                if ((pressed & ORBIS_PAD_BUTTON_CROSS) && game_count > 0) {
                    const int filter = library_filter(tab, selected);
                    detail_app_id = playden_ps4_library_visible_app_id(filter, (size_t)game_selected);
                    playden_ps4_game_details_start(detail_app_id);
                    page = 2; changed = 1;
                }
                if ((pressed & ORBIS_PAD_BUTTON_SQUARE) && game_count > 0) {
                    const int filter = library_filter(tab, selected);
                    const uint32_t app_id = playden_ps4_library_visible_app_id(filter, (size_t)game_selected);
                    if (!playden_ps4_library_toggle_favorite(app_id))
                        log_line("PLDN10001: could not save favorite change");
                    const int count = (int)playden_ps4_library_visible_count(filter);
                    if (count <= 0) { game_selected = 0; game_focus = 0; }
                    else if (game_selected >= count) game_selected = count - 1;
                    changed = 1;
                }
                if ((pressed & ORBIS_PAD_BUTTON_OPTIONS) && game_count > 0) {
                    page = 4;
                    delete_choice = 0;
                    changed = 1;
                }
            } else if ((pressed & ORBIS_PAD_BUTTON_LEFT) && tab > 0) {
                --tab; selected = 0; changed = 1;
                game_focus = 0;
            }
            if (!game_focus && (pressed & ORBIS_PAD_BUTTON_RIGHT) && tab < 4) {
                ++tab; selected = 0; changed = 1;
                game_focus = 0;
            }
            if (!game_focus) {
                if (pressed & (ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_DOWN | ORBIS_PAD_BUTTON_LEFT |
                               ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CIRCLE)) {
                    if (playden_ps4_signout_armed) changed = 1;
                    playden_ps4_signout_armed = 0;
                }
                if ((pressed & ORBIS_PAD_BUTTON_UP) && selected > 0) { --selected; changed = 1; }
                const int saves_state = playden_ps4_saves_state();
                const int auth_state = playden_ps4_auth_state();
                const int steam_connected = auth_state == 6 || auth_state == 7;
                const int installed_games = (int)playden_ps4_library_visible_count(0);
                const int library_rows = 4 + (int)playden_ps4_library_collection_count();
                const int max_selection = tab == 1 ? library_rows - (steam_connected ? 1 : 0) : tab == 3 && saves_state == 3 ? 2 :
                                          tab == 3 && saves_state == 1 ? 0 :
                                          tab == 3 ? (installed_games > 0 ? installed_games - 1 : 0) :
                                          tab == 4 ? 8 :
                                          tab == 2 ? (int)playden_ps4_queue_waiting_count() +
                                                     (playden_ps4_update_count() > 0 ? 1 : 0) : 0;
                if ((pressed & ORBIS_PAD_BUTTON_DOWN) && selected < max_selection) { ++selected; changed = 1; }
                /* Library search and sort from the views menu too. */
                if (tab == 1 && (pressed & (ORBIS_PAD_BUTTON_TRIANGLE | ORBIS_PAD_BUTTON_R1))) {
                    if (pressed & ORBIS_PAD_BUTTON_TRIANGLE) playden_ps4_library_search_edit(user);
                    else playden_ps4_library_sort_cycle();
                    changed = 1;
                }
                if ((pressed & ORBIS_PAD_BUTTON_CROSS) && tab == 1 && selected < library_rows &&
                    playden_ps4_library_visible_count(library_filter(tab, selected)) > 0) {
                    game_focus = 1;
                    game_selected = 0;
                    changed = 1;
                } else if ((pressed & ORBIS_PAD_BUTTON_CROSS) && tab == 1 &&
                           selected == library_rows && !steam_connected) {
                    playden_ps4_auth_start();
                    page = 1;
                    changed = 1;
                } else if ((pressed & ORBIS_PAD_BUTTON_CROSS) && tab == 0) {
                    playden_ps4_home_shelf = 0;
                    if (playden_ps4_home_shelf_count() > 0) {
                        game_focus = 1;
                        game_selected = 0;
                    } else {
                        tab = 1;
                        selected = 1;
                    }
                    changed = 1;
                } else if ((pressed & ORBIS_PAD_BUTTON_CROSS) && tab == 2) {
                    /* Row 0: the running download (cancel); then the waiting games (remove); then Update all. */
                    const int waiting = (int)playden_ps4_queue_waiting_count();
                    if (selected == 0 && playden_ps4_queue_state() == 1) playden_ps4_queue_cancel();
                    else if (selected >= 1 && selected <= waiting) {
                        playden_ps4_queue_remove((size_t)(selected - 1));
                        if (selected > (int)playden_ps4_queue_waiting_count()) --selected;
                    } else if (selected == waiting + 1 && playden_ps4_update_count() > 0) {
                        playden_ps4_queue_all_updates();
                    }
                    changed = 1;
                } else if ((pressed & ORBIS_PAD_BUTTON_CROSS) && tab == 3) {
                    if (saves_state == 3) {
                        playden_ps4_saves_start(saves_app_id, selected);
                        changed = 1;
                    } else if (installed_games > selected) {
                        saves_app_id = playden_ps4_library_visible_app_id(0, (size_t)selected);
                        playden_ps4_saves_start(saves_app_id, 0);
                        changed = 1;
                    }
                } else if ((pressed & ORBIS_PAD_BUTTON_CROSS) && tab == 4 && selected == 7) {
                    /* Steam account: sign out (second Cross), or sign in when signed out. */
                    if (!steam_connected) {
                        playden_ps4_signout_armed = 0;
                        playden_ps4_auth_start();
                        page = 1;
                    } else if (playden_ps4_signout_armed) {
                        playden_ps4_auth_sign_out();
                        playden_ps4_signout_armed = 0;
                    } else {
                        playden_ps4_signout_armed = 1;
                    }
                    changed = 1;
                } else if ((pressed & ORBIS_PAD_BUTTON_CROSS) && tab == 4 && selected == 6) {
                    /* Playden update: download, cancel, or try again. Installing stays GoldHEN's. */
                    PwUpdateDownload d;
                    pw_update_ps4_download_status(&d);
                    if (d.state == PW_UPDATE_DOWNLOAD_RUNNING) pw_update_ps4_download_cancel();
                    else if (d.state == PW_UPDATE_DOWNLOAD_READY || d.state == PW_UPDATE_DOWNLOAD_FAILED)
                        pw_update_ps4_download_start();
                    changed = 1;
                } else if ((pressed & ORBIS_PAD_BUTTON_CROSS) && tab == 4) {
                    if (selected == 5) {
                        const int edit_result = playden_ps4_edit_log_receiver_ip(user);
                        if (edit_result < 0)
                            log_line("PLDN10001: receiver IP edit cancelled, invalid, or not saved");
                    } else {
                        playden_ps4_cycle_setting(selected);
                    }
                    changed = 1;
                }
            }
        }
        if (playden_ps4_auth_poll() || playden_ps4_saves_poll()) changed = 1;
        /* The update's state and a running download's progress, twice a second. */
        if (++update_tick % 250 == 0) {
            PwUpdateDownload d;
            pw_update_ps4_download_status(&d);
            if (d.state != update_shown || d.state == PW_UPDATE_DOWNLOAD_RUNNING) changed = 1;
            update_shown = d.state;
        }
        /* The game just played: send its saves to Steam Cloud in the background once signed in. */
        if (post_play_sync && (playden_ps4_auth_state() == 6 || playden_ps4_auth_state() == 7) &&
            playden_ps4_saves_state() != 1) {
            log_line("PLDN10001: syncing Steam Cloud saves after play");
            playden_ps4_saves_start(post_play_sync, 0);
            post_play_sync = 0;
            changed = 1;
        }
        if (tab == 1 && playden_ps4_ui_poll_artwork()) changed = 1;
        const int saves_state_now = playden_ps4_saves_state();
        if (tab == 3 && saves_state_now == 3 && previous_saves_state != 3) {
            selected = 0;
            changed = 1;
        }
        previous_saves_state = saves_state_now;
        const int uninstall_state_now = playden_ps4_game_uninstall_state();
        if (uninstall_state_now != previous_uninstall_state) changed = 1;
        previous_uninstall_state = uninstall_state_now;
        if (page == 1 && (playden_ps4_auth_state() == 6 || playden_ps4_auth_state() == 7)) {
            page = 0;
            selected = 1;
            game_focus = playden_ps4_auth_state() == 6;
            game_selected = 0;
            changed = 1;
        }
        if (changed) {
            current = (current + 1) % BUFFERS;
            draw_screen(buffers[current], tab, selected, page, pad_ready,
                        playden_ps4_auth_state(), game_focus, game_selected, delete_choice,
                        playden_ps4_auth_challenge(),
                        playden_ps4_auth_failure(), playden_ps4_wine_status());
            sceVideoOutSubmitFlip(handle, current, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0);
        }
        usleep(2000);
    }
}
