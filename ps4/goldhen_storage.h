// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PLAYDEN_PS4_GOLDHEN_STORAGE_H
#define PLAYDEN_PS4_GOLDHEN_STORAGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Request GoldHEN's removable-storage view for this title process. Returns 1
 * when enabled, 0 when disabled or unavailable; failure leaves the normal
 * title sandbox in place. */
int playden_ps4_goldhen_storage_enable_if_requested(void);

/* GoldHEN asks must be undone before title exit/relaunch. A failed LoadExec may
 * return; its caller should pass this token to the resume function then. */
int playden_ps4_goldhen_storage_prepare_load_exec(void);
void playden_ps4_goldhen_storage_resume_after_failed_load_exec(int was_enabled);

#ifdef __cplusplus
}
#endif

#endif
